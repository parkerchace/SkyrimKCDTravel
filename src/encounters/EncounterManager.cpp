#include "PCH.h"
#include "EncounterManager.h"
#include "config/Settings.h"
#include "journey/JourneyController.h"
#include "ui/TravelOverlay.h"

#include <chrono>
#include <thread>
#include <unordered_map>

namespace EncounterManager {

namespace {
    // Transient event "dressing" (campfires, props, rubble, bodies, spawned
    // foes). We clear the previous batch whenever a new event fires so revisiting
    // a spot doesn't accumulate piles of duplicate campfires/rocks.
    std::vector<RE::ObjectRefHandle> g_dressing;
    void Track(RE::TESObjectREFR* r) {
        if (r) g_dressing.push_back(r->CreateRefHandle());
    }
    // Destructible static debris: some blockers (fallen log, cart) can't be
    // physics-shoved, so instead the player batters them down — each melee/magic
    // hit chips away a chunk (with a notification) until the piece is removed.
    // A whole stack/pile of static debris is ONE clearable cluster: hitting any
    // piece chips the shared total, and when it's spent the entire stack is
    // removed together — so a heap of boxes clears in a few swings, not per-box.
    class DebrisClearSink : public RE::BSTEventSink<RE::TESHitEvent> {
    public:
        static DebrisClearSink* Get() { static DebrisClearSink s; return &s; }
        struct Cluster { float remaining; std::vector<RE::ObjectRefHandle> members; };
        std::unordered_map<RE::FormID, size_t> formToCluster;
        std::vector<Cluster> clusters;
        std::mutex mtx;

        size_t NewCluster(float hp) {
            std::lock_guard l(mtx);
            clusters.push_back({ hp, {} });
            return clusters.size() - 1;
        }
        void AddToCluster(size_t cid, RE::TESObjectREFR* ref) {
            if (!ref) return;
            std::lock_guard l(mtx);
            if (cid >= clusters.size()) return;
            formToCluster[ref->GetFormID()] = cid;
            clusters[cid].members.push_back(ref->CreateRefHandle());
        }
        void Reset() { std::lock_guard l(mtx); formToCluster.clear(); clusters.clear(); }
        RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* e,
                                              RE::BSTEventSource<RE::TESHitEvent>*) override {
            if (!e || !e->target || !e->cause) return RE::BSEventNotifyControl::kContinue;
            if (e->cause->GetFormID() != 0x14) return RE::BSEventNotifyControl::kContinue;  // player only
            auto* ref = e->target.get();
            if (!ref) return RE::BSEventNotifyControl::kContinue;
            std::lock_guard l(mtx);
            auto it = formToCluster.find(ref->GetFormID());
            if (it == formToCluster.end()) return RE::BSEventNotifyControl::kContinue;
            Cluster& c = clusters[it->second];
            c.remaining -= 20.0f;
            if (c.remaining <= 0.0f) {
                RE::DebugNotification("You clear the way through.");
                for (auto& h : c.members) {
                    if (auto m = h.get()) { m->Disable(); m->SetDelete(true); formToCluster.erase(m->GetFormID()); }
                }
                c.members.clear();
            } else {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "You clear the debris... %.0f%%", 100.0f - c.remaining);
                RE::DebugNotification(buf);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void ClearDressing() {
        for (auto& h : g_dressing) {
            if (auto ref = h.get()) {
                ref->Disable();
                ref->SetDelete(true);
            }
        }
        g_dressing.clear();
        DebrisClearSink::Get()->Reset();
    }

    // Bridges the game's message-box result back to a std::function. Lifetime is
    // managed by the BSTSmartPointer the MessageBoxData holds (intrusive refcount).
    class ChoiceCallback : public RE::IMessageBoxCallback {
    public:
        std::function<void(unsigned)> fn;
        explicit ChoiceCallback(std::function<void(unsigned)> f) : fn(std::move(f)) {}
        ~ChoiceCallback() override = default;
        void Run(Message a_msg) override {
            if (fn) fn(static_cast<unsigned>(a_msg));
        }
    };

    // Queue a message box with arbitrary buttons and a result callback. Falls
    // back to invoking cb(fallbackBtn) synchronously if the UI factory is
    // unavailable so callers never deadlock waiting on a choice.
    void ShowChoice(const std::string& body,
                    const std::vector<std::string>& buttons,
                    unsigned fallbackBtn,
                    std::function<void(unsigned)> cb) {
        auto* fm = RE::MessageDataFactoryManager::GetSingleton();
        auto* is = RE::InterfaceStrings::GetSingleton();
        if (!fm || !is) { cb(fallbackBtn); return; }

        auto* creator = fm->GetCreator<RE::MessageBoxData>(is->messageBoxData);
        if (!creator) { cb(fallbackBtn); return; }

        auto* mb = creator->Create();
        if (!mb) { cb(fallbackBtn); return; }

        mb->bodyText = body.c_str();
        for (const auto& b : buttons) {
            RE::BSString bs;
            bs = b.c_str();
            mb->buttonText.push_back(bs);
        }
        mb->callback = RE::BSTSmartPointer<RE::IMessageBoxCallback>{
            new ChoiceCallback(std::move(cb))
        };
        mb->QueueMessage();
    }

    // Look up a configured base form (may be a TESNPC or a leveled character).
    RE::TESForm* LookupConfiguredForm(unsigned int localFormID, const std::string& plugin) {
        if (localFormID == 0) return nullptr;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return nullptr;
        return dh->LookupForm(static_cast<RE::FormID>(localFormID), plugin);
    }

    // Resolve a form to a concrete TESNPC. PlaceObjectAtMe does NOT resolve
    // leveled character lists (it places the list as a bodiless ref with no 3D),
    // so we walk the list ourselves: pick a random entry whose level requirement
    // the player meets, recursing through nested lists until we reach an NPC.
    RE::TESNPC* ResolveToNPC(RE::TESForm* form, std::uint16_t level, std::mt19937& rng, int depth = 0) {
        if (!form || depth > 12) return nullptr;
        if (auto* npc = form->As<RE::TESNPC>()) return npc;
        auto* lev = form->As<RE::TESLevCharacter>();
        if (!lev) return nullptr;

        std::vector<RE::TESForm*> cand;
        for (auto& e : lev->entries) {
            if (e.form && e.level <= level) cand.push_back(e.form);
        }
        if (cand.empty()) {  // player below the list's floor — take anything
            for (auto& e : lev->entries) {
                if (e.form) cand.push_back(e.form);
            }
        }
        if (cand.empty()) return nullptr;
        auto* picked = cand[std::uniform_int_distribution<size_t>(0, cand.size() - 1)(rng)];
        return ResolveToNPC(picked, level, rng, depth + 1);
    }

    // Tactical arrangements for where the ambush comes from, relative to the
    // player's travel direction (forward).
    enum class Ambush { Ahead, Behind, Flanking, Pincer, Scattered, Ring, Line, Vanguard };
    static constexpr int kNumArrangements = 8;

    const char* AmbushLine(Ambush a, bool byCart) {
        switch (a) {
            case Ambush::Ahead:     return byCart ? "The road ahead is blocked!" : "Figures step into the road ahead!";
            case Ambush::Behind:    return "You hear something behind you...";
            case Ambush::Flanking:  return "They rush in from both sides!";
            case Ambush::Pincer:    return "You're surrounded — front and back!";
            case Ambush::Scattered: return "Ambush! They close in from the trees!";
            case Ambush::Ring:      return "They circle you on all sides!";
            case Ambush::Line:      return "A line of them bars the road!";
            case Ambush::Vanguard:  return "Scouts ahead — and more behind!";
            default: return "You are waylaid!";
        }
    }

    // A coherent hostile group. One group is chosen per encounter so we never
    // mix incompatible enemies (e.g. wolves alongside bandits, who are actually
    // hostile to each other). All FormIDs are Skyrim.esm leveled lists.
    struct EncGroup {
        const char*               name;
        std::vector<unsigned int> forms;
        int                       minCount;
        int                       maxCount;
        bool                      nightOnly{ false };  // e.g. vampires — only after dark
        bool                      solo{ false };       // solitary predator: comes alone, rarely a pair
    };

    // Group size: solitary predators come alone (rarely a pair, per pairChance);
    // everyone else rolls their coherent min..max. Capped by the MCM count.
    int GroupCount(const EncGroup& g, std::mt19937& rng) {
        auto& cfg = Settings::GetSingleton();
        int count = g.solo ? (1 + (JourneyController::Roll100() < cfg.pairChance ? 1 : 0))
                           : std::uniform_int_distribution<int>(g.minCount, g.maxCount)(rng);
        return std::clamp(count, 1, (std::max)(1, cfg.encounterActorCount));
    }

    bool IsNight() {
        auto* cal = RE::Calendar::GetSingleton();
        if (!cal) return false;
        const float h = cal->GetHour();
        return (h < 6.0f || h >= 20.0f);
    }
    const std::vector<EncGroup>& Groups() {
        // TODO: weight these by region (Reach->Forsworn, snow->Falmer, barrows->
        // Draugr, wilds->predators). For now one is chosen at random per fight.
        // Above-ground, road-appropriate groups only. Falmer (underground) and
        // draugr (barrows) are intentionally excluded until region-gating can put
        // them near Dwemer ruins / Nordic tombs specifically.
        static const std::vector<EncGroup> g = {
            { "bandits",   { 0x00039CFC, 0x0003DEC8, 0x0001A348 }, 2, 4 },  // melee1H/2H + archer
            { "wolves",    { 0x000B83C2 },                         3, 5 },  // wolf pack
            { "a sabre cat", { 0x000FE2D5 },                       1, 1, false, true },  // solitary
            { "a bear",    { 0x00042266 },                         1, 1, false, true },  // solitary
            { "vampires",  { 0x00033930, 0x00033942 },             2, 3, true },  // night hunters
            { "warlocks",  { 0x000D5C49, 0x000D5C48 },             2, 3 },  // necromancer/conjurer
            // Forsworn removed from the general pool — their skin armor makes them
            // absurd in snow. Reintroduce Reach-only when region-gating lands.
        };
        return g;
    }

    // The Z to place something at (x,y): terrain height when it's near the
    // reference Z (open ground), otherwise the reference Z itself — so spawns
    // stay on the player's walkable level on roads/bridges/paving rather than
    // dropping into the terrain below. Returns false if under water.
    //
    // NOTE: a havok downward-raycast would be more accurate, but calling
    // TES::Pick from here races the physics job thread and crashes (havok has no
    // lock here). A safe, locked raycast is future work; heuristic for now.
    bool SurfaceAt(RE::TES* tes, RE::TESObjectCELL* cell, float x, float y, float refZ, float& outZ) {
        if (!tes) { outZ = refZ; return true; }
        float land;
        if (!tes->GetLandHeight({ x, y, refZ }, land)) { outZ = refZ; return true; }
        if (tes->GetWaterHeight({ x, y, refZ }, cell) > land + 24.0f) return false;
        outZ = (std::abs(land - refZ) < 250.0f) ? land : refZ;
        return true;
    }

    // "Am I a ball touching anything?" — bounding spheres of nearby solid objects
    // (buildings, walls, trees, furniture, containers, doors). Refreshed at the
    // start of each spawn so a candidate point can be rejected if it overlaps one
    // (i.e. it's inside a wall/building/object). Pure gameplay, no havok = no crash.
    struct Obstacle { RE::NiPoint3 c; float r; };
    std::vector<Obstacle> g_obstacles;

    void RefreshObstacles(float radius) {
        g_obstacles.clear();
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* tes = RE::TES::GetSingleton();
        if (!player || !tes) return;
        tes->ForEachReferenceInRange(player, radius, [&](RE::TESObjectREFR* r) {
            if (r && r != player && !r->IsDisabled() && !r->IsDeleted()) {
                if (auto* base = r->GetBaseObject()) {
                    switch (base->GetFormType()) {
                        case RE::FormType::Static:
                        case RE::FormType::MovableStatic:
                        case RE::FormType::Tree:
                        case RE::FormType::Furniture:
                        case RE::FormType::Container:
                        case RE::FormType::Door: {
                            // Prefer the loaded 3D's world bound (the real mesh
                            // extent). But right after a teleport/cell-load — the
                            // exact moment we're placing things — a nearby rock's
                            // 3D may not have streamed in yet, so Get3D() silently
                            // returns null and the obstacle is invisible to us
                            // (this is why things spawned INSIDE rocks: the rock
                            // just wasn't in the obstacle list at all). Fall back
                            // to the base object's own OBND record instead — it's
                            // always available regardless of 3D load state. Center
                            // the sphere at the ref's actual position and size it
                            // to the farthest corner, so it's rotation-safe (a
                            // sphere doesn't care about orientation) even though
                            // OBND is in unrotated local space.
                            RE::NiPoint3 c{};
                            float rad = 0.0f;
                            if (auto* n = r->Get3D(); n && n->worldBound.radius > 1.0f) {
                                c = n->worldBound.center;
                                rad = n->worldBound.radius;
                            } else if (auto* bound = base->As<RE::TESBoundObject>()) {
                                const auto& bd = bound->boundData;
                                const float ex = (std::max)(std::abs(static_cast<float>(bd.boundMin.x)), std::abs(static_cast<float>(bd.boundMax.x)));
                                const float ey = (std::max)(std::abs(static_cast<float>(bd.boundMin.y)), std::abs(static_cast<float>(bd.boundMax.y)));
                                const float ez = (std::max)(std::abs(static_cast<float>(bd.boundMin.z)), std::abs(static_cast<float>(bd.boundMax.z)));
                                rad = std::sqrt(ex * ex + ey * ey + ez * ez);
                                c = r->GetPosition();
                            }
                            if (rad > 1.0f) g_obstacles.push_back({ c, rad });
                            break;
                        }
                        default: break;
                    }
                }
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
    }

    bool TouchesObstacle(float x, float y, float z, float ballR) {
        for (const auto& o : g_obstacles) {
            const float dx = x - o.c.x, dy = y - o.c.y, dz = z - o.c.z;
            const float need = o.r + ballR;
            if (dx * dx + dy * dy + dz * dz < need * need) return true;
        }
        return false;
    }

    // Cast a ray straight down through Havok and return the REAL collision
    // surface — not the raw terrain heightmap, which mountains routinely sit
    // ABOVE (big mountain faces are often separate rock meshes/large references
    // layered on top of the heightmap, so "ground height" at that (x,y) column
    // can be far below the actual visible/walkable rock surface). Placing
    // anything using only the heightmap can bury it inside that overlay — you
    // can walk OUT (the surface blocks entry from outside normally; physics
    // ejects you once you're already inside) but never should have been in it.
    //
    // A previous attempt at this crashed the game: it called the havok world
    // without any lock, racing the physics job thread. bhkWorld exposes the
    // SAME lock (worldLock) the engine's own queries use — BSReadLockGuard
    // acquires it properly here, which is the actual fix, not avoiding havok.
    bool HavokGroundZ(RE::TESObjectCELL* cell, float x, float y, float fromZ, float toZ, float& outZ) {
        if (!cell) return false;
        auto* world = cell->GetbhkWorld();
        if (!world) return false;
        RE::bhkPickData pick{};
        const float s = RE::bhkWorld::GetWorldScale();
        const RE::NiPoint3 a{ x, y, fromZ }, b{ x, y, toZ };
        pick.rayInput.from = RE::hkVector4(a * s);
        pick.rayInput.to   = RE::hkVector4(b * s);
        {
            RE::BSReadLockGuard lock(world->worldLock);
            world->PickObject(pick);
        }
        if (!pick.rayOutput.HasHit()) return false;
        outZ = a.z + (b.z - a.z) * pick.rayOutput.hitFraction;
        return true;
    }

    // A denser terrain scan than a handful of fixed points — the closest thing to
    // "lidar" we can safely do. A single 4-point ring at a fixed radius can call a
    // spot "flat" right up to the lip of a cliff that starts just past the sampled
    // ring — this scans 8 directions at THREE radii: an immediate-footing ring, a
    // mid ring for ordinary slope, and a wider ring specifically to catch a nearby
    // drop-off (the actual cause of "walked out and fell to my death" — locally
    // flat spot, cliff edge just past what was being checked). ALWAYS trusts real
    // terrain height directly (no "must be near some reference Z" gate) — that
    // gate is right for placing a prop beside an already-grounded player, but
    // wrong for finding ground in the first place, which is what every caller
    // here needs. The center point is then refined with a locked Havok raycast so
    // a mountain overlay above the heightmap doesn't bury whatever we place.
    struct GroundSpot { float z{ 0.0f }; float roughness{ 1e9f }; bool ok{ false }; };

    GroundSpot ScanGround(RE::TES* tes, RE::TESObjectCELL* cell, float x, float y, float refZ) {
        GroundSpot gc;
        if (!tes) return gc;
        float land;
        if (!tes->GetLandHeight({ x, y, refZ }, land)) return gc;               // no terrain data here
        if (tes->GetWaterHeight({ x, y, refZ }, cell) > land + 24.0f) return gc; // submerged

        // NOTE: a Havok raycast refinement lived here briefly. Pulled back out —
        // this function is called from an escalating-radius search (up to ~150
        // candidate points per settle attempt), which meant 100+ locked Havok
        // queries fired back-to-back within single-digit milliseconds. Each call
        // is individually safe (properly locked), but hammering Havok's lock that
        // hard, that often, turned out to correlate with serious instability.
        // Heightmap-only is the safe, previously-verified baseline; a real fix
        // for mountain-overlay embedding needs Havok called AT MOST once — on the
        // single winning candidate after the cheap scan below picks it — not
        // inside the search loop. That's future work, done carefully and tested
        // in isolation, not stacked onto an already-shaky session.

        static constexpr float kNearR = 90.0f, kMidR = 220.0f, kFarR = 420.0f;
        float roughness = 0.0f;
        for (int i = 0; i < 8; ++i) {
            const float ang = (2.0f * 3.14159265f / 8.0f) * static_cast<float>(i);
            const float cx = std::cos(ang), sx = std::sin(ang);
            float h;
            if (tes->GetLandHeight({ x + cx * kNearR, y + sx * kNearR, refZ }, h))
                roughness = (std::max)(roughness, std::abs(h - land));
            if (tes->GetLandHeight({ x + cx * kMidR, y + sx * kMidR, refZ }, h))
                roughness = (std::max)(roughness, std::abs(h - land) * 0.6f);  // less weight, further out
            // Cliff guard: if a wider-ring sample is much LOWER than here, there's
            // a drop-off nearby even though the immediate footing tested flat.
            if (tes->GetLandHeight({ x + cx * kFarR, y + sx * kFarR, refZ }, h) && (land - h) > 350.0f)
                return gc;  // reject outright — this is a cliff lip
        }
        if (roughness > 200.0f) return gc;  // too rough/steep to stand on comfortably

        gc.z = land;
        gc.roughness = roughness;
        gc.ok = true;
        return gc;
    }

    // Spawn a coherent hostile group around the player in a chosen tactical
    // arrangement. Placement stays on the player's own ground plane (Z) plus a
    // small drop so the game settles them onto the navmesh — no terrain-height
    // snap, which was burying spawns on roads/city markers. No-op (but logged)
    // when no valid forms are configured, so a clean install stays crash-free.
    void SpawnHostiles(const RE::NiPoint3& forward, bool byCart, const EncGroup* forced = nullptr) {
        auto& cfg = Settings::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        std::mt19937 rng{ std::random_device{}() };

        // Choose ONE coherent group (or use the forced one, e.g. a danger group).
        // Filter by time of day, and avoid repeating the last group back-to-back.
        static const char* s_lastGroup = nullptr;
        const auto& groups = Groups();
        const EncGroup* chosen = forced;
        if (!chosen) {
            const bool night = IsNight();
            std::vector<const EncGroup*> eligible;
            for (const auto& g : groups) {
                if (g.nightOnly && !night) continue;
                if (g.name == s_lastGroup && groups.size() > 2) continue;  // anti-repeat
                eligible.push_back(&g);
            }
            if (eligible.empty()) for (const auto& g : groups) eligible.push_back(&g);
            chosen = eligible[std::uniform_int_distribution<size_t>(0, eligible.size() - 1)(rng)];
            s_lastGroup = chosen->name;
        }
        const EncGroup& group = *chosen;

        std::vector<RE::TESForm*> pool;
        for (unsigned int id : group.forms) {
            if (auto* f = LookupConfiguredForm(id, cfg.encounterActorPlugin)) {
                pool.push_back(f);
            }
        }
        if (pool.empty()) {
            logger::info("Encounter: group '{}' had no valid forms — narrative-only ambush", group.name);
            RE::DebugNotification("You are ambushed on the road!");
            return;
        }
        const std::uint16_t plvl = player->GetLevel();
        const int count = GroupCount(group, rng);

        const RE::NiPoint3 center = player->GetPosition();
        auto* pcell = player->GetParentCell();
        logger::info("Spawn: player center=({:.0f},{:.0f},{:.0f}) cell={} 3D={}",
            center.x, center.y, center.z,
            pcell ? pcell->GetFormEditorID() : "(none)", player->Is3DLoaded());

        // Build a forward/right frame. Fall back to the player's facing if the
        // travel direction is degenerate (origin == dest).
        RE::NiPoint3 fwd = forward;
        float flen = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y);
        if (flen < 0.01f) {
            const float yaw = player->GetAngleZ();
            fwd = { std::sin(yaw), std::cos(yaw), 0.0f };
            flen = 1.0f;
        }
        fwd.x /= flen; fwd.y /= flen; fwd.z = 0.0f;
        const RE::NiPoint3 right{ fwd.y, -fwd.x, 0.0f };  // 90° clockwise

        const Ambush arr = static_cast<Ambush>(std::uniform_int_distribution<int>(0, kNumArrangements - 1)(rng));
        std::uniform_int_distribution<size_t> pick(0, pool.size() - 1);
        std::uniform_real_distribution<float> angDist(0.0f, 2.0f * 3.14159265f);
        auto* tes = RE::TES::GetSingleton();

        // Enemies appear at a distance you can SEE coming (closer to how a dragon
        // announces itself), not on top of you — you spot them down the road and
        // close the gap. Scales up with group size so packs fan out even wider.
        const float baseNear = (count <= 2) ? 900.0f : 1100.0f;
        const float baseFar  = baseNear + static_cast<float>(count) * 250.0f;
        std::uniform_real_distribution<float> distDist(baseNear, baseFar);
        std::uniform_real_distribution<float> jitter(-baseFar * 0.25f, baseFar * 0.25f);

        // Keep spawned enemies apart from each other too.
        std::vector<RE::NiPoint2> placedEnemies;
        const float enemySep = (count <= 2) ? 200.0f : 300.0f;

        RefreshObstacles(1400.0f);
        auto validate = [&](float x, float y) -> std::optional<RE::NiPoint3> {
            float z;
            if (!SurfaceAt(tes, pcell, x, y, center.z, z)) return std::nullopt;
            // Check the whole column, not one height — a single test point can
            // clear a rock at head height while the base still overlaps it (or
            // vice versa), which is exactly how enemies ended up INSIDE rocks.
            for (float h = 0.0f; h <= 320.0f; h += 80.0f)
                if (TouchesObstacle(x, y, z + h, 65.0f)) return std::nullopt;
            for (const auto& q : placedEnemies) {
                const float dx = x - q.x, dy = y - q.y;
                if (dx * dx + dy * dy < enemySep * enemySep) return std::nullopt;  // too clumped
            }
            return RE::NiPoint3{ x, y, z + 8.0f };
        };

        // Name what's attacking, so a wolf pack doesn't read as "armed figures".
        {
            std::string note = std::string("You're set upon by ") + group.name + "!";
            RE::DebugNotification(note.c_str());
        }
        logger::info("Encounter: group='{}' count={} arrangement={}", group.name, count, static_cast<int>(arr));

        int spawned = 0;
        for (int i = 0; i < count; ++i) {
            // Resolve the group's (possibly leveled) form to a concrete NPC.
            RE::TESNPC* base = ResolveToNPC(pool[pick(rng)], plvl, rng);
            if (!base) { logger::warn("Encounter: could not resolve a form to an NPC"); continue; }
            auto ref = player->PlaceObjectAtMe(base, false);
            if (!ref) continue;

            const float d = distDist(rng);
            // Lateral spread so they don't line up single-file.
            const float lateral = jitter(rng);
            float alongF = 0.0f, alongR = 0.0f;

            switch (arr) {
                case Ambush::Ahead:
                    alongF = d; alongR = lateral; break;
                case Ambush::Behind:
                    alongF = -d; alongR = lateral; break;
                case Ambush::Flanking:
                    alongR = (i % 2 == 0 ? d : -d); alongF = lateral; break;
                case Ambush::Pincer:
                    alongF = (i % 2 == 0 ? d : -d); alongR = lateral; break;
                case Ambush::Scattered: {
                    const float ang = angDist(rng);
                    alongF = std::cos(ang) * d; alongR = std::sin(ang) * d; break;
                }
                case Ambush::Ring: {
                    const float ang = (static_cast<float>(i) / (std::max)(1, count)) * 2.0f * 3.14159265f;
                    alongF = std::cos(ang) * d; alongR = std::sin(ang) * d; break;
                }
                case Ambush::Line:
                    alongF = d; alongR = (static_cast<float>(i) - count * 0.5f) * 150.0f; break;
                case Ambush::Vanguard:
                    alongF = (i < count / 2 ? d : -d * 0.8f); alongR = lateral; break;
            }

            // Desired offset point, then fan out in angle / pull in distance
            // looking for dry, valid ground; fall back to the player's own spot
            // (guaranteed walkable where they're standing) if nothing qualifies.
            const float baseX = center.x + fwd.x * alongF + right.x * alongR;
            const float baseY = center.y + fwd.y * alongF + right.y * alongR;
            const float baseAng = std::atan2(baseY - center.y, baseX - center.x);
            const float baseDist = std::sqrt((baseX - center.x) * (baseX - center.x) +
                                             (baseY - center.y) * (baseY - center.y));
            // Fallback = the intended offset point at the player's level (NOT the
            // player's own spot — that made enemies spawn on top of the player).
            RE::NiPoint3 pos{ baseX, baseY, center.z };
            for (int attempt = 0; attempt < 10; ++attempt) {
                const float da = (attempt == 0) ? 0.0f
                    : ((attempt % 2) ? 1.0f : -1.0f) * 0.35f * static_cast<float>((attempt + 1) / 2);
                // Widen the search angle on failed attempts instead of pulling the
                // distance in — retries used to collapse toward the player, which
                // is exactly what made "failed validation" spots feel too close.
                const float dd = (std::max)(baseNear, baseDist * (1.0f - 0.02f * attempt));
                if (auto v = validate(center.x + std::cos(baseAng + da) * dd,
                                      center.y + std::sin(baseAng + da) * dd)) {
                    pos = *v;
                    break;
                }
            }
            ref->SetPosition(pos);
            placedEnemies.push_back({ pos.x, pos.y });  // not tracked for deletion — actors are gameplay
            auto actual = ref->GetPosition();
            auto* rc = ref->GetParentCell();
            ++spawned;
            logger::info("Encounter: spawned {:08X} baseType={} arr={} want=({:.0f},{:.0f},{:.0f}) got=({:.0f},{:.0f},{:.0f}) cell={} 3D={}",
                ref->GetFormID(), static_cast<int>(base->GetFormType()), static_cast<int>(arr),
                pos.x, pos.y, pos.z, actual.x, actual.y, actual.z,
                rc ? rc->GetFormEditorID() : "(none)", ref->Is3DLoaded());
        }
        logger::info("Encounter: {} hostiles placed (arrangement {})", spawned, static_cast<int>(arr));
    }

    // The wilderness danger you run into when you leave the road / avoid an
    // event. Which kind is MCM-weighted (animals / monsters / bandits / robbers /
    // dragons); each can be disabled by zeroing its weight.
    void SpawnDanger(const RE::NiPoint3& forward, bool byCart) {
        auto& cfg = Settings::GetSingleton();
        std::mt19937 catRng{ std::random_device{}() };

        // Each category is a set of COHERENT sub-groups (never mixed). Solitary
        // predators (bear, sabre cat, frost troll) come alone; wolves/spiders/
        // bandits come in numbers.
        static const EncGroup animals[] = {
            { "wolves",      { 0x000B83C2 }, 3, 5 },
            { "a bear",      { 0x00042266 }, 1, 1, false, true },
            { "a sabre cat", { 0x000FE2D5 }, 1, 1, false, true },
        };
        static const EncGroup monsters[] = {
            { "frostbite spiders", { 0x0001E77C }, 1, 3 },
            { "a frost troll",     { 0x00106386 }, 1, 1, false, true },
            { "mudcrabs",          { 0x0002183E }, 1, 2 },
        };
        static const EncGroup bandits = { "bandits", { 0x00039CFC, 0x0003DEC8, 0x0001A348 }, 2, 4 };
        static const EncGroup robbers = { "robbers", { 0x00039CFC, 0x0001A348 }, 1, 2 };
        auto pickFrom = [&](const EncGroup* arr, size_t n) { return &arr[std::uniform_int_distribution<size_t>(0, n - 1)(catRng)]; };

        const float ws = cfg.wDangerAnimals + cfg.wDangerMonsters + cfg.wDangerBandits +
                         cfg.wDangerRobbers + cfg.wDangerDragons;
        if (ws <= 0.0f) { SpawnHostiles(forward, byCart); return; }  // all disabled — fall back
        float r = JourneyController::Roll100() / 100.0f * ws;
        const EncGroup* g = nullptr;
        bool dragon = false;
        if      ((r -= cfg.wDangerAnimals)  < 0.0f) g = pickFrom(animals, 3);
        else if ((r -= cfg.wDangerMonsters) < 0.0f) g = pickFrom(monsters, 3);
        else if ((r -= cfg.wDangerBandits)  < 0.0f) g = &bandits;
        else if ((r -= cfg.wDangerRobbers)  < 0.0f) g = &robbers;
        else                                        dragon = true;

        logger::info("Danger: {}", dragon ? "a dragon" : g->name);
        if (dragon) {
            // Spawn high and ahead so it flies in and dives, not walks up.
            auto* player = RE::PlayerCharacter::GetSingleton();
            std::mt19937 rng{ std::random_device{}() };
            if (auto* npc = ResolveToNPC(LookupConfiguredForm(0x0005EACF, "Skyrim.esm"), player ? player->GetLevel() : 1, rng)) {
                if (auto ref = player->PlaceObjectAtMe(npc, false)) {
                    const auto p = player->GetPosition();
                    ref->SetPosition({ p.x + 600.0f, p.y + 600.0f, p.z + 2600.0f });
                    Track(ref.get());
                    RE::DebugNotification("A dragon descends upon you!");
                }
            }
            return;
        }
        RE::DebugNotification("The wilds are no safer than the road...");
        SpawnHostiles(forward, byCart, g);
    }

    // ── Outcome helpers ──────────────────────────────────────────────────────
    void GiveGold(int amount) {
        if (amount <= 0) return;  // removal handled elsewhere; add-only here
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* form   = RE::TESForm::LookupByID(0x0000000F);  // Gold001
        auto* gold   = form ? form->As<RE::TESBoundObject>() : nullptr;
        if (player && gold) player->AddObjectToContainer(gold, nullptr, amount, nullptr);
    }
    void HealPlayer(float amt) {
        if (auto* p = RE::PlayerCharacter::GetSingleton()) {
            p->AsActorValueOwner()->RestoreActorValue(
                RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, amt);
        }
    }
    void AddGameHours(float h) {
        auto* cal = RE::Calendar::GetSingleton();
        if (cal && cal->gameHour) cal->gameHour->value += h;
    }
    int RollInt(int lo, int hi) {
        return lo + static_cast<int>(JourneyController::Roll100() / 100.0f * static_cast<float>(hi - lo + 1));
    }

    RE::TESBoundObject* LookupBound(unsigned int id) {
        auto* form = RE::TESForm::LookupByID(id);
        return form ? form->As<RE::TESBoundObject>() : nullptr;
    }

    // Place a bound object on dry, valid ground a short distance from the player
    // (used for campfires, road debris, bodies). Returns the placed reference.
    RE::TESObjectREFR* PlaceObjectOnGround(RE::TESBoundObject* base, const RE::NiPoint3& center,
                                           float dist, RE::TES* tes, RE::TESObjectCELL* cell,
                                           std::mt19937& rng) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !base) return nullptr;
        auto ref = player->PlaceObjectAtMe(base, false);
        if (!ref) return nullptr;
        RE::NiPoint3 pos = center;
        std::uniform_real_distribution<float> ang(0.0f, 2.0f * 3.14159265f);
        for (int i = 0; i < 10; ++i) {
            const float a = ang(rng);
            RE::NiPoint3 c{ center.x + std::cos(a) * dist, center.y + std::sin(a) * dist, center.z };
            if (!tes) { pos = c; break; }
            float land;
            if (tes->GetLandHeight(c, land)) {
                const float w = tes->GetWaterHeight(c, cell);
                if (w <= land + 24.0f) { c.z = land + 2.0f; pos = c; break; }
            }
        }
        ref->SetPosition(pos);
        Track(ref.get());
        return ref.get();
    }

    // ── Combat event (the original waylay) ───────────────────────────────────
    // Common world-state for spawn events.
    struct SpawnCtx {
        RE::PlayerCharacter* player;
        RE::NiPoint3         center;
        RE::TES*             tes;
        RE::TESObjectCELL*   cell;
    };
    SpawnCtx MakeCtx() {
        auto* p = RE::PlayerCharacter::GetSingleton();
        return { p, p ? p->GetPosition() : RE::NiPoint3{}, RE::TES::GetSingleton(),
                 p ? p->GetParentCell() : nullptr };
    }

    // A placed prop's footprint (world XY + its bounding radius) so later props
    // can be kept clear of it — respecting each object's actual SIZE.
    struct PlacedObj { float x, y, radius; };

    float RefRadiusXY(RE::TESObjectREFR* r) {
        const RE::NiPoint3 mn = r->GetBoundMin(), mx = r->GetBoundMax();
        const float dx = mx.x - mn.x, dy = mx.y - mn.y;
        return 0.5f * std::sqrt(dx * dx + dy * dy);
    }

    // Place a prop ~radius from `hub` on flat, dry ground, kept clear of every
    // already-placed prop by BOTH objects' bounding radii + `margin`. Rejects
    // steep slopes and water. The Scene Composer's atom.
    RE::TESObjectREFR* PlaceProp(unsigned int formID, const RE::NiPoint3& hub, float radius,
                                 float margin, std::vector<PlacedObj>& placed,
                                 RE::TES* tes, RE::TESObjectCELL* cell, std::mt19937& rng) {
        auto* base   = LookupBound(formID);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!base || !player) return nullptr;
        std::uniform_real_distribution<float> ang(0.0f, 2.0f * 3.14159265f);
        std::uniform_real_distribution<float> rad(radius * 0.75f, radius * 1.25f);
        for (int attempt = 0; attempt < 24; ++attempt) {
            const float a = ang(rng), r = (radius <= 0.1f ? 0.0f : rad(rng));
            const float x = hub.x + std::cos(a) * r, y = hub.y + std::sin(a) * r;

            float z;
            if (!SurfaceAt(tes, cell, x, y, hub.z, z)) continue;  // dry surface at player's level
            bool blocked = false;
            for (float h = 0.0f; h <= 320.0f; h += 80.0f)
                if (TouchesObstacle(x, y, z + h, 55.0f)) { blocked = true; break; }
            if (blocked) continue;  // inside a wall/building/rock — checked the whole column
            if (tes) {
                // Slope check — sample around and reject steep ground.
                float h0;
                if (tes->GetLandHeight({ x, y, hub.z }, h0)) {
                    const float ox[4] = { 90.0f, -90.0f, 0.0f, 0.0f }, oy[4] = { 0.0f, 0.0f, 90.0f, -90.0f };
                    float maxd = 0.0f;
                    for (int k = 0; k < 4; ++k) { float h; if (tes->GetLandHeight({ x + ox[k], y + oy[k], hub.z }, h)) maxd = (std::max)(maxd, std::abs(h - h0)); }
                    if (maxd > 130.0f) continue;
                }
            }
            // Lift by the object's OWN bound so its bottom touches the surface,
            // not its origin — a flat offset buried anything whose model pivot
            // sits above its base (big rocks especially).
            z += -static_cast<float>(base->boundData.boundMin.z) + 2.0f;
            auto ref = player->PlaceObjectAtMe(base, false);
            if (!ref) return nullptr;
            ref->SetPosition({ x, y, z });
            const float rr = RefRadiusXY(ref.get());
            bool overlap = false;
            for (const auto& q : placed) {
                const float dx = x - q.x, dy = y - q.y;
                const float need = q.radius + rr + margin;
                if (dx * dx + dy * dy < need * need) { overlap = true; break; }
            }
            if (overlap) { ref->Disable(); ref->SetDelete(true); continue; }
            Track(ref.get());
            placed.push_back({ x, y, rr });
            return ref.get();
        }
        return nullptr;
    }

    // Spawn a lootable body on dry ground BEHIND the player (out of their forward
    // view) so they don't watch it appear and die. Returns its resting position.
    RE::NiPoint3 SpawnBody(const RE::NiPoint3& center, float dist, RE::TES* tes, std::mt19937& rng) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return center;
        // Vary who the dead traveler was — hunter, commoner, bandit, soldier, mage.
        static const unsigned int kBodies[] = { 0x00073FC2, 0x0001A319, 0x00039CFC, 0x000D5C48 };
        RE::TESNPC* npc = ResolveToNPC(LookupConfiguredForm(kBodies[RollInt(0, 3)], "Skyrim.esm"), player->GetLevel(), rng);
        if (!npc) { logger::warn("Body: could not resolve NPC"); return center; }
        auto ref = player->PlaceObjectAtMe(npc, false);
        if (!ref) { logger::warn("Body: PlaceObjectAtMe failed"); return center; }

        // Behind the player (opposite their facing), on clear ground not inside
        // a wall/object — uses the same "mini-lidar" scan as player-settling
        // (rejects steep/cliff spots, always trusts real terrain height).
        RefreshObstacles(1000.0f);
        const float yaw = player->GetAngleZ();
        std::uniform_real_distribution<float> jit(-120.0f, 120.0f);
        RE::NiPoint3 c = center;  // ultimate fallback: right by the player, ground already known-good
        bool placed = false;
        for (int attempt = 0; attempt < 12; ++attempt) {
            RE::NiPoint3 cand{ center.x - std::sin(yaw) * dist + jit(rng),
                               center.y - std::cos(yaw) * dist + jit(rng), center.z };
            const auto gc = ScanGround(tes, player->GetParentCell(), cand.x, cand.y, center.z);
            if (!gc.ok) continue;
            cand.z = gc.z + 4.0f;
            bool blocked = false;
            for (float h = 0.0f; h <= 320.0f; h += 80.0f)
                if (TouchesObstacle(cand.x, cand.y, cand.z + h, 65.0f)) { blocked = true; break; }
            if (blocked) continue;
            c = cand;
            placed = true;
            break;
        }
        if (!placed) {
            // Nothing valid behind the player (cliff, wall, water) — settle for
            // right beside them instead of an ungrounded guess that leaves the
            // body invisible (buried/underwater/behind unreachable terrain).
            c = { center.x + 80.0f, center.y, center.z + 4.0f };
            logger::info("Body: no valid spot behind player — placing beside them instead");
        }
        ref->SetPosition(c);
        if (auto* gform = RE::TESForm::LookupByID(0x0000000F))
            if (auto* gb = gform->As<RE::TESBoundObject>())
                ref->AddObjectToContainer(gb, nullptr, RollInt(20, 90), nullptr);
        if (auto* actor = ref->As<RE::Actor>()) actor->KillImmediate();
        // The body itself is left for the game to manage (loot it); only the
        // decorative bloodstain is tracked for cleanup.
        // A bloodstain (and sometimes a stray bone) sells that it was violent.
        if (auto* blood = LookupBound(0x000B8B0B)) {  // FXValthumeBloodPool
            if (auto b = player->PlaceObjectAtMe(blood, false)) {
                b->SetPosition({ c.x, c.y, c.z - 2.0f });
                Track(b.get());
            }
        }
        logger::info("Body: placed {:08X} at ({:.0f},{:.0f},{:.0f})", ref->GetFormID(), c.x, c.y, c.z);
        return c;
    }

    // After lingering near a body, the "victim's" killers may spring an ambush.
    void MaybeAmbushNearBody(RE::NiPoint3 bodyPos, bool byCart, RE::NiPoint3 forward) {
        if (JourneyController::Roll100() >= 40.0f) return;  // 40% it's a set-up
        std::thread([bodyPos, byCart, forward]() {
            std::this_thread::sleep_for(std::chrono::seconds(7));
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([bodyPos, byCart, forward]() {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    if (!player) return;
                    const auto pp = player->GetPosition();
                    const float dx = pp.x - bodyPos.x, dy = pp.y - bodyPos.y;
                    if (dx * dx + dy * dy < 700.0f * 700.0f) {  // still lingering by the body
                        RE::DebugNotification("An ambush! You lingered too long.");
                        SpawnHostiles(forward, byCart);
                    }
                });
            }
        }).detach();
    }

    // ── Spawn logic (no dialogue — the choice lives in PromptEvent) ──────────

    // A traveler's camp composed as a scene: a fire hub with a cooking spit,
    // bedrolls spaced back from the flames, and a log to sit on — all grounded
    // and spaced so nothing overlaps or floats.
    // Place a LIVING NPC (camp occupant) on dry ground near a hub. Not tracked
    // for deletion (actors are gameplay). Returns the actor or nullptr.
    RE::Actor* PlaceCampNPC(unsigned int formID, const RE::NiPoint3& hub, float dist,
                            RE::TES* tes, std::mt19937& rng) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return nullptr;
        auto* npc = ResolveToNPC(LookupConfiguredForm(formID, "Skyrim.esm"), player->GetLevel(), rng);
        if (!npc) return nullptr;
        auto ref = player->PlaceObjectAtMe(npc, false);
        if (!ref) return nullptr;
        std::uniform_real_distribution<float> ang(0.0f, 2.0f * 3.14159265f);
        const float a = ang(rng);
        RE::NiPoint3 p{ hub.x + std::cos(a) * dist, hub.y + std::sin(a) * dist, hub.z };
        if (tes) { float land; if (tes->GetLandHeight(p, land)) p.z = land + 16.0f; }
        ref->SetPosition(p);
        return ref->As<RE::Actor>();
    }

    // A camp you DISCOVER by the road — built on the flat, walkable ground the
    // player is already settled on (navmesh), populated and varied: a hunter's
    // camp with its hunter, a traveler resting, or an abandoned camp with its
    // slain owners. No forced "you rest a while" — the bedrolls are there to
    // actually sleep in if you choose.
    void SpawnCamp() {
        auto ctx = MakeCtx();
        if (!ctx.player) return;
        std::mt19937 rng{ std::random_device{}() };
        std::vector<PlacedObj> placed;

        // Fire hub, right in the flat clearing the player settled on.
        static const unsigned int kFires[] = { 0x00101A51, 0x000FB9B0, 0x000DB682 };
        auto* fire = PlaceProp(kFires[RollInt(0, 2)], ctx.center, 170.0f, 0.0f, placed, ctx.tes, ctx.cell, rng);
        const RE::NiPoint3 hub = fire ? fire->GetPosition() : ctx.center;
        placed.clear();
        placed.push_back({ hub.x, hub.y, 70.0f });  // clearing around the flames

        // Shared dressing — a lived-in camp, grounded around the fire.
        PlaceProp(0x0001018E3, hub,  95.0f, 20.0f, placed, ctx.tes, ctx.cell, rng);  // cooking spit
        const int beds = 1 + RollInt(0, 2);
        for (int i = 0; i < beds; ++i)
            PlaceProp(0x0001015F5, hub, 175.0f, 45.0f, placed, ctx.tes, ctx.cell, rng);  // bedrolls
        PlaceProp(0x000287BD, hub, 150.0f, 40.0f, placed, ctx.tes, ctx.cell, rng);       // log seat
        PlaceProp(0x000EEE0C, hub, 205.0f, 30.0f, placed, ctx.tes, ctx.cell, rng);       // supply barrel

        const char* note = "A traveler's camp sits by the road.";
        switch (RollInt(0, 2)) {
            case 0:  // occupied hunter's camp — a neutral hunter tends the fire
                note = "You come upon a hunter's camp, its keeper by the fire.";
                PlaceCampNPC(0x00073FC2, hub, 120.0f, ctx.tes, rng);  // LCharHunter (neutral)
                PlaceProp(0x000EEE0C, hub, 190.0f, 30.0f, placed, ctx.tes, ctx.cell, rng);  // extra supplies
                break;
            case 1:  // a fellow traveler resting at the roadside
                note = "A weary traveler rests at a roadside camp.";
                PlaceCampNPC(0x00073FC2, hub, 120.0f, ctx.tes, rng);
                break;
            default:  // abandoned — its owners lie dead, something happened here
                note = "You find a camp, cold and abandoned — its owners slain.";
                for (int i = 0, n = 1 + RollInt(0, 1); i < n; ++i) SpawnBody(hub, 130.0f, ctx.tes, rng);
                break;
        }

        logger::info("Camp: '{}' fire={} at hub=({:.0f},{:.0f},{:.0f})", note, fire != nullptr, hub.x, hub.y, hub.z);
        RE::DebugNotification(note);
    }

    // A dead traveler by the road — a real lootable body. Lingering near it can
    // spring the ambush that left them there.
    void SpawnCorpse(bool byCart, const RE::NiPoint3& forward) {
        auto ctx = MakeCtx();
        if (!ctx.player) return;
        std::mt19937 rng{ std::random_device{}() };
        const RE::NiPoint3 bodyPos = SpawnBody(ctx.center, 200.0f, ctx.tes, rng);
        RE::DebugNotification("A dead traveler lies by the roadside.");
        MaybeAmbushNearBody(bodyPos, byCart, forward);
    }

    // ── Scene layouts (Fallout 4 "Sim Settlements" idea) ────────────────────
    // A layout is a HAND-DESIGNED arrangement of pieces at fixed positions in the
    // ROAD's frame (fwd = along travel, side = across the road), each with its own
    // rotation — so the scene tells a coherent story (a tipped merchant cart with
    // its cargo fanned out, a deliberate bandit barricade) instead of random junk.
    // The whole layout is then oriented to the actual road direction and grounded.
    struct LayoutPiece {
        unsigned int form;
        float        fwd, side;  // offset in the road frame (units)
        float        yaw;        // extra rotation on top of the road orientation (rad)
        bool         blocker;    // joins the clearable cluster (else movable/scenery)
    };
    struct SceneLayout { const char* line; const LayoutPiece* pieces; std::size_t count; };

    // Place one designed piece: transform its road-frame offset to world, ground
    // it (bound-aware lift), orient it to the road + its own yaw, register it.
    void PlaceLayoutPiece(const LayoutPiece& pc, const RE::NiPoint3& anchor,
                          const RE::NiPoint3& fwdDir, const RE::NiPoint3& rightDir, float roadYaw,
                          std::vector<PlacedObj>& placed, std::size_t cluster,
                          RE::TES* tes, RE::TESObjectCELL* cell) {
        auto* base = LookupBound(pc.form);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!base || !player) return;
        const float wx = anchor.x + fwdDir.x * pc.fwd + rightDir.x * pc.side;
        const float wy = anchor.y + fwdDir.y * pc.fwd + rightDir.y * pc.side;
        float z;
        if (!SurfaceAt(tes, cell, wx, wy, anchor.z, z)) return;  // off a cliff/in water — skip
        z += -static_cast<float>(base->boundData.boundMin.z) + 2.0f;  // sit the base on the ground
        auto ref = player->PlaceObjectAtMe(base, false);
        if (!ref) return;
        ref->data.angle.z = roadYaw + pc.yaw;          // face the road frame
        ref->SetPosition({ wx, wy, z });
        Track(ref.get());
        placed.push_back({ wx, wy, RefRadiusXY(ref.get()) });
        if (cluster != static_cast<std::size_t>(-1) && pc.blocker)
            DebrisClearSink::Get()->AddToCluster(cluster, ref.get());
    }

    // A composed road obstruction, placed from a designed layout, oriented to the
    // road. 35% of the time it's a bandit-staged trap.
    void SpawnDebris(bool byCart, const RE::NiPoint3& forward) {
        // FormID shorthands used across layouts.
        constexpr unsigned int CART1 = 0x0001C0C0, CART2 = 0x0001C0C5;
        constexpr unsigned int CRATE = 0x000DAE82, CRATE2 = 0x000DAE80, OPENCR = 0x0004138D;
        constexpr unsigned int BARREL = 0x000EEE0C, TREE = 0x000B788A;
        constexpr unsigned int ROCKL = 0x00020F37, ROCKM = 0x0001EF89, ROCKM2 = 0x000BBA00;
        constexpr unsigned int RUBB1 = 0x000F23F5, RUBB2 = 0x000F1F7B, LEAVES = 0x000F5EB6, DIRT = 0x000B723D;

        // OVERTURNED MERCHANT CART — a trader's wagon on its side, cargo fanned out.
        static const LayoutPiece kCartWreck[] = {
            { CART1,   0,    0,    1.45f, true }, { CRATE,  85, -45, 0.4f, true },
            { CRATE,  105,  55,   1.0f,  true }, { CRATE2, 55,  95, 0.6f, true },
            { OPENCR,  65, -95,   1.2f,  false }, { OPENCR, 120, 20, 0.2f, false },
            { BARREL,  15, -135,  0.0f,  false }, { BARREL,-30, 125, 0.0f, false },
            { BARREL, 135, -25,   0.0f,  false }, { RUBB1,  45, -60, 0.0f, false },
            { LEAVES,  95,  35,   0.0f,  false }, { RUBB2, -10,  75, 0.0f, false },
            { DIRT,   125,  85,   0.0f,  false }, { LEAVES,-20, -80, 0.0f, false },
        };
        // ROCKSLIDE — boulders tumbled across the road from the uphill side.
        static const LayoutPiece kRockslide[] = {
            { ROCKL,   0,  -120, 0.0f, true }, { ROCKL,  25,   5,  0.0f, true },
            { ROCKM,  -5,  125,  0.0f, true }, { ROCKM2, 60, -55,  0.0f, true },
            { ROCKM,  70,   70,  0.0f, true }, { RUBB1,  40, -30, 0.0f, false },
            { RUBB2,  30,  55,   0.0f, false }, { RUBB1, -25, -70, 0.0f, false },
            { DIRT,  100,   20,  0.0f, false }, { RUBB2, 110, -80, 0.0f, false },
            { RUBB1, -30,  95,   0.0f, false }, { DIRT,   85,  -95, 0.0f, false },
            { RUBB2,  55,  110,  0.0f, false }, { RUBB1,  10, -110, 0.0f, false },
        };
        // BANDIT BARRICADE — deliberately built: cart across, crates stacked, log,
        // barrels for cover. Reads as man-made (pairs with the trap ambush).
        static const LayoutPiece kBarricade[] = {
            { CART1,   0,  -55, 0.0f,  true }, { CRATE,  0,  35, 0.0f, true },
            { CRATE,  18,  35,  0.15f, true }, { CRATE2, 9,  70, 0.0f, true },
            { TREE,   -5,   0,  1.57f, true }, { CRATE,  35,  -20, 0.0f, true },
            { BARREL, -35, 90,  0.0f,  false }, { BARREL,-35, -95, 0.0f, false },
            { BARREL,  40, 100,  0.0f,  false }, { RUBB1,  50, -60, 0.0f, false },
            { LEAVES,  20,  15,  0.0f,  false }, { RUBB2, -20, 55, 0.0f, false },
        };
        // FALLEN TREE — a storm-felled pine laid across, foliage and rock around it.
        static const LayoutPiece kFallenTree[] = {
            { TREE,    0,   0,  1.57f, true }, { ROCKM, -45,  85, 0.0f, true },
            { ROCKM2,  40, -90, 0.0f,  true }, { LEAVES, 60, -40, 0.0f, false },
            { LEAVES, -50, -30, 0.0f,  false }, { LEAVES, 30, 60, 0.0f, false },
            { RUBB1,   20, -20, 0.0f,  false }, { RUBB2, -30, 40, 0.0f, false },
            { LEAVES,  80,  25, 0.0f,  false }, { RUBB1, -70, 10, 0.0f, false },
            { LEAVES,  10, 100, 0.0f,  false }, { DIRT,   55,  75, 0.0f, false },
        };

        static const SceneLayout kLayouts[] = {
            { "An overturned cart blocks the road, its cargo spilled everywhere.", kCartWreck,  std::size(kCartWreck) },
            { "A rockslide has buried the road in stone.",                          kRockslide,  std::size(kRockslide) },
            { "Someone has barricaded the road ahead.",                             kBarricade,  std::size(kBarricade) },
            { "A fallen tree blocks the way, branches strewn across the road.",      kFallenTree, std::size(kFallenTree) },
        };

        auto ctx = MakeCtx();
        const SceneLayout& scene = kLayouts[RollInt(0, static_cast<int>(std::size(kLayouts)) - 1)];
        bool barricade = (scene.pieces == kBarricade);
        if (ctx.player) {
            std::vector<PlacedObj> placed;
            // Road frame: forward (travel dir) + right (across the road).
            RE::NiPoint3 fwd = forward;
            const float fl = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y);
            if (fl > 0.01f) { fwd.x /= fl; fwd.y /= fl; } else { fwd = { 0, 1, 0 }; }
            const RE::NiPoint3 right{ fwd.y, -fwd.x, 0.0f };
            const float roadYaw = std::atan2(fwd.x, fwd.y);  // Skyrim yaw (0 = +Y)
            // Anchor the scene a short way ahead, on the road.
            const RE::NiPoint3 anchor{ ctx.center.x + fwd.x * 170.0f, ctx.center.y + fwd.y * 170.0f, ctx.center.z };

            const std::size_t cluster = DebrisClearSink::Get()->NewCluster(120.0f);
            for (std::size_t i = 0; i < scene.count; ++i)
                PlaceLayoutPiece(scene.pieces[i], anchor, fwd, right, roadYaw, placed, cluster, ctx.tes, ctx.cell);
            logger::info("Debris: layout '{}' ({} pieces) at ({:.0f},{:.0f})", scene.line, scene.count, anchor.x, anchor.y);
        }

        RE::DebugNotification(scene.line);
        // A deliberate barricade is ALWAYS a trap; other blocks 35% of the time.
        if (barricade || JourneyController::Roll100() < 35.0f) {
            static const EncGroup banditAmbush = { "bandits", { 0x00039CFC, 0x0003DEC8, 0x0001A348 }, 2, 4 };
            RE::DebugNotification("It's a trap — bandits set the roadblock!");
            SpawnHostiles(forward, byCart, &banditAmbush);
        }
    }
}

// Snap (x,y) to the nearest point on the NAVMESH — ground the engine itself
// guarantees a person can stand and walk on (never inside a mountain, floating,
// or in deep water). Strongly prefers triangles flagged kPreferred, which are
// Skyrim's own designer-annotated main paths / roads — the exact triangles NPCs
// prefer to walk, in vanilla, DLC, and any navmeshed mod. This is both the road
// detector and the walkability guarantee, from one authoritative source. Purely
// read-only (no Havok, no writes) so it's crash-safe. Searches the given cell's
// navmesh(es); returns ok=false if none (caller falls back to heightmap search).
namespace {
    struct NavSpot { RE::NiPoint3 pos{}; bool onRoad{ false }; bool ok{ false }; };

    // Scan ONE cell's navmesh, updating the running best road/walkable candidate.
    void ScanCellNavmesh(RE::TESObjectCELL* cell, float x, float y, float refZ,
                         float& bestRoad, RE::NiPoint3& road, bool& haveRoad,
                         float& bestAny, RE::NiPoint3& any, bool& haveAny) {
        if (!cell) return;
        auto* navArr = cell->GetRuntimeData().navMeshes;
        if (!navArr) return;
        using Flag = RE::BSNavmeshTriangle::TriangleFlag;
        for (auto& navPtr : navArr->navMeshes) {
            RE::NavMesh* nav = navPtr.get();
            if (!nav) continue;
            const auto& verts = nav->vertices;
            for (const auto& tri : nav->triangles) {
                if (tri.triangleFlags.any(Flag::kDeleted)) continue;
                if (tri.vertices[0] >= verts.size() || tri.vertices[1] >= verts.size() || tri.vertices[2] >= verts.size()) continue;
                const auto& a = verts[tri.vertices[0]].location;
                const auto& b = verts[tri.vertices[1]].location;
                const auto& c = verts[tri.vertices[2]].location;
                const RE::NiPoint3 cen{ (a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f };
                const float dz = cen.z - refZ;
                if (std::abs(dz) > 1000.0f) continue;  // don't snap up/down a cliff face to a switchback overhead
                const float dx = cen.x - x, dy = cen.y - y;
                const float d2 = dx * dx + dy * dy + dz * dz * 3.0f;  // strongly prefer the reference elevation
                if (d2 < bestAny) { bestAny = d2; any = cen; haveAny = true; }
                if (tri.triangleFlags.any(Flag::kPreferred) && d2 < bestRoad) { bestRoad = d2; road = cen; haveRoad = true; }
            }
        }
    }

    // Find the nearest ROAD (kPreferred) — or failing that, nearest walkable —
    // navmesh point across the WHOLE loaded cell grid, not just the player's cell.
    // This matters because a straight-line stop cuts through mountains where roads
    // wind around them: the actual road is often a cell or two away, and the exact
    // interpolated point frequently has no navmesh at all (unwalkable slope). refZ
    // should be the real TERRAIN height at (x,y), never a falling player's Z.
    NavSpot SnapToNavmesh(float x, float y, float refZ) {
        NavSpot out;
        auto* tes = RE::TES::GetSingleton();
        if (!tes || !tes->gridCells) return out;
        auto* grid = tes->gridCells;

        float bestRoad = 1e30f, bestAny = 1e30f;
        RE::NiPoint3 road{}, any{};
        bool haveRoad = false, haveAny = false;

        const std::uint32_t n = grid->length;
        for (std::uint32_t gx = 0; gx < n; ++gx)
            for (std::uint32_t gy = 0; gy < n; ++gy)
                ScanCellNavmesh(grid->GetCell(gx, gy), x, y, refZ, bestRoad, road, haveRoad, bestAny, any, haveAny);

        // Prefer a real road anywhere within ~4000 units (≈1 cell); else nearest walkable.
        if (haveRoad && bestRoad < 4000.0f * 4000.0f) { out.pos = road; out.onRoad = true; out.ok = true; }
        else if (haveAny) { out.pos = any; out.onRoad = false; out.ok = true; }
        return out;
    }
}

bool SettlePlayerOnGround(float searchRadius) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* tes    = RE::TES::GetSingleton();
    if (!player || !tes) return false;
    const auto pos = player->GetPosition();
    auto* cell = player->GetParentCell();

    // Reference elevation = the real TERRAIN height at the player's (x,y), NOT
    // the player's own Z — which may be mid-fall after teleporting to a wrong
    // interpolated height (that was the "fall out of the sky" from a high place).
    float refZ = pos.z;
    if (float land; tes->GetLandHeight({ pos.x, pos.y, pos.z }, land)) refZ = land;

    // PRIMARY: snap to the nearest road/walkable navmesh across the loaded grid —
    // provably walkable, prefers real roads (kPreferred), and terrain-referenced.
    if (auto nav = SnapToNavmesh(pos.x, pos.y, refZ); nav.ok) {
        player->SetPosition({ nav.pos.x, nav.pos.y, nav.pos.z + 8.0f }, true);
        logger::info("MidRoute: settled on {} at ({:.0f},{:.0f},{:.0f}) [refZ={:.0f}; player was ({:.0f},{:.0f},{:.0f})]",
            nav.onRoad ? "ROAD" : "walkable ground", nav.pos.x, nav.pos.y, nav.pos.z, refZ, pos.x, pos.y, pos.z);
        return true;
    }
    logger::info("MidRoute: no navmesh in loaded grid — falling back to heightmap search");

    std::mt19937 rng{ std::random_device{}() };

    // Score candidates by flatness+safety via the shared "mini-lidar" scan (see
    // ScanGround) — catches nearby cliff edges, not just immediate footing.
    float bestScore = -1.0f;
    RE::NiPoint3 best{};
    bool found = false;

    // strict = full "mini-lidar" quality bar (flat, no cliff, clear). lenient =
    // just real, dry, unobstructed ground at ANY roughness. We prefer strict, but
    // if the whole area is rugged we STILL commit to the flattest real ground we
    // can find rather than leaving the player floating/embedded — because we no
    // longer fall back to a second teleport (that was the double-load).
    auto tryPoint = [&](float x, float y, bool strict) {
        float land;
        if (!tes->GetLandHeight({ x, y, pos.z }, land)) return;
        if (tes->GetWaterHeight({ x, y, pos.z }, cell) > land + 24.0f) return;  // dry only
        for (float h = 0.0f; h <= 320.0f; h += 80.0f)
            if (TouchesObstacle(x, y, land + 8.0f + h, 60.0f)) return;          // clear column
        float roughness = 0.0f;
        if (strict) {
            const auto gc = ScanGround(tes, cell, x, y, pos.z);
            if (!gc.ok) return;
            roughness = gc.roughness;
        } else {
            // cheap local roughness so "flattest available" still means something
            const float ox[4] = { 128.0f, -128.0f, 0.0f, 0.0f }, oy[4] = { 0.0f, 0.0f, 128.0f, -128.0f };
            for (int k = 0; k < 4; ++k) { float h; if (tes->GetLandHeight({ x + ox[k], y + oy[k], pos.z }, h)) roughness = (std::max)(roughness, std::abs(h - land)); }
        }
        const float score = 1000.0f - roughness;
        if (score > bestScore) { bestScore = score; best = { x, y, land + 8.0f }; found = true; }
    };

    std::uniform_real_distribution<float> ang(0.0f, 2.0f * 3.14159265f);
    for (bool strict : { true, false }) {           // strict first; lenient only if strict finds nothing
        for (float radiusMult : { 1.0f, 2.0f, 3.2f }) {
            const float radius = searchRadius * radiusMult;
            RefreshObstacles(radius + 400.0f);
            if (strict && radiusMult == 1.0f) tryPoint(pos.x, pos.y, strict);  // where we landed, cheapest win
            for (int ring = 1; ring <= 6; ++ring) {
                const float r = radius * (static_cast<float>(ring) / 6.0f);
                for (int i = 0; i < 8; ++i) {
                    const float a = ang(rng);
                    tryPoint(pos.x + std::cos(a) * r, pos.y + std::sin(a) * r, strict);
                }
                if (found && bestScore > 900.0f) break;  // good enough (near-flat) — stop early
            }
            if (found) break;
        }
        if (found) break;  // strict succeeded — don't bother with the lenient pass
    }

    if (!found) return false;  // truly no dry land in range (e.g. mid-ocean) — leave as-is
    player->SetPosition(best, true);
    logger::info("MidRoute: settled player at ({:.0f},{:.0f},{:.0f}) score={:.0f}", best.x, best.y, best.z, bestScore);
    return true;
}

EventKind RollEventKind() {
    auto& cfg = Settings::GetSingleton();
    const float wsum = cfg.wCombat + cfg.wTraveler + cfg.wDiscovery + cfg.wHazard;
    if (wsum <= 0.0f) return EventKind::Combat;
    static EventKind s_last = EventKind::Danger;  // sentinel
    auto roll = [&]() -> EventKind {
        float r = JourneyController::Roll100() / 100.0f * wsum;
        if ((r -= cfg.wCombat)    < 0.0f) return EventKind::Combat;
        if ((r -= cfg.wTraveler)  < 0.0f) return EventKind::Camp;
        if ((r -= cfg.wDiscovery) < 0.0f) return EventKind::Corpse;
        return EventKind::Debris;
    };
    EventKind k = roll();
    if (k == s_last) k = roll();  // one reroll to avoid the same event twice running
    s_last = k;
    return k;
}

void PromptEvent(EventKind kind, bool byCart, std::function<void(Approach)> onDecision) {
    std::string body;
    std::vector<std::string> buttons;
    switch (kind) {
        case EventKind::Combat:
            body = byCart ? "The cart slows — something's on the road ahead."
                          : "Something's on the road ahead, and it's seen you.";
            buttons = { "Confront it", "Try to avoid it" };
            break;
        case EventKind::Camp:
            body = "There's a camp just off the road ahead.";
            buttons = { "Stop and look", "Keep travelling" };
            break;
        case EventKind::Corpse: {
            static const char* lines[] = {
                "You spot a body by the road.",
                "Someone lies dead ahead.",
                "There's a corpse off the path.",
            };
            body = lines[JourneyController::Roll100() < 33.0f ? 0 : (JourneyController::Roll100() < 50.0f ? 1 : 2)];
            buttons = { "Investigate it", "Keep travelling" };
            break;
        }
        case EventKind::Debris:
            body = "The way ahead is blocked.";
            buttons = { "Stop and deal with it", "Find another way" };
            break;
    }
    logger::info("Event prompt: kind={}", static_cast<int>(kind));
    ShowChoice(body, buttons, /*fallback=*/1u,
        [kind, onDecision = std::move(onDecision)](unsigned choice) {
            if (choice == 0) { onDecision(Approach::Engage); return; }  // chose to deal with it

            // PASSIVE events (a corpse, a camp, a roadblock) are OPTIONAL. Choosing
            // to leave them means you LEAVE them — cleanly, no surprise ambush.
            // Getting attacked for ignoring a corpse, or a "roadblock" turning into
            // a spider, was the bait-and-switch that felt bad. Only true COMBAT
            // (something already hunting you on the road) gets a sneak roll.
            if (kind != EventKind::Combat) {
                RE::DebugNotification("You travel on, leaving it behind.");
                onDecision(Approach::CleanPass);
                return;
            }

            // Combat "avoid" = try to slip past unseen (scales with Sneak). Fail and
            // the SAME threat catches you — never a different, incongruous enemy.
            auto& cfg = Settings::GetSingleton();
            float chance = cfg.avoidBaseChance;
            if (auto* p = RE::PlayerCharacter::GetSingleton())
                chance += (p->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSneak) - 25.0f) * 0.5f;
            chance = std::clamp(chance, 5.0f, 95.0f);
            if (JourneyController::Roll100() < chance) {
                RE::DebugNotification("You slip past unseen and travel on.");
                onDecision(Approach::CleanPass);
            } else {
                RE::DebugNotification("They spot you — no escape!");
                onDecision(Approach::Engage);
            }
        });
}

void EngageEvent(EventKind kind, bool byCart, const RE::NiPoint3& forward, std::function<void()> onResolved) {
    logger::info("Event engage: kind={}", static_cast<int>(kind));
    ClearDressing();  // remove the previous event's props so nothing accumulates
    switch (kind) {
        case EventKind::Combat: SpawnHostiles(forward, byCart); break;
        case EventKind::Camp:   SpawnCamp(); break;
        case EventKind::Corpse: SpawnCorpse(byCart, forward); break;
        case EventKind::Debris: SpawnDebris(byCart, forward); break;
        case EventKind::Danger: SpawnDanger(forward, byCart); break;
    }
    if (onResolved) onResolved();
}

bool WildernessDangerStrikes() {
    return JourneyController::Roll100() < Settings::GetSingleton().wildernessDangerChance;
}

void InstallHitSink() {
    if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
        holder->AddEventSink(DebrisClearSink::Get());
        logger::info("EncounterManager: debris hit-sink installed");
    }
}

void ResetForSaveLoad() {
    g_dressing.clear();     // just drop the handles — do NOT Disable/SetDelete;
                            // they may point into a save that's no longer loaded
    g_obstacles.clear();
    DebrisClearSink::Get()->Reset();
    logger::info("EncounterManager: reset for save load");
}

void ResolveEncounter(bool byCart, const RE::NiPoint3& forward, std::function<void()> onResolved) {
    const EventKind kind = RollEventKind();
    PromptEvent(kind, byCart,
        [kind, byCart, forward, onResolved = std::move(onResolved)](Approach outcome) mutable {
            if (outcome == Approach::Engage) {
                EngageEvent(kind, byCart, forward, std::move(onResolved));
            } else if (outcome == Approach::Waylaid) {
                EngageEvent(EventKind::Danger, byCart, forward, std::move(onResolved));  // detour into danger
            } else if (onResolved) {
                onResolved();  // clean pass
            }
        });
}

}
