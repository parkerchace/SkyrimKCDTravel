#include "PCH.h"
#include "JourneyController.h"
#include "RouteMath.h"
#include "config/Settings.h"
#include "encounters/EncounterManager.h"
#include "ui/MapMarkerDraw.h"
#include "ui/TravelOverlay.h"

#include <chrono>
#include <thread>

namespace JourneyController {

namespace {
    // ── Active journey state ─────────────────────────────────────────────────
    struct ActiveJourney {
        RE::NiPoint3        origin{};
        RE::NiPoint3        dest{};
        RE::ObjectRefHandle destHandle{};
        bool                byCart{ false };
        int                 waylays{ 0 };
        float               t{ 0.0f };         // 0..1 animation progress
        float               duration{ 6.0f };  // real seconds for the arrow slide
        bool                cameraFollow{ true };

        // Mid-journey event (the KCD "how do you approach this?" decision, shown
        // on the map while the arrow travels).
        bool                hasEvent{ false };
        bool                prompted{ false };     // dialogue already shown
        float               encounterFraction{ 0.6f };
        EncounterManager::EventKind eventKind{ EncounterManager::EventKind::Combat };
    };

    std::atomic<bool> s_traveling{ false };  // whole journey (anim + resolve)
    std::atomic<bool> s_animating{ false };  // arrow currently sliding
    std::atomic<bool> s_paused{ false };     // frozen while the mid-route dialogue is up
    ActiveJourney     s_active{};

    std::mt19937& Rng() {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    bool IsNight() {
        auto* cal = RE::Calendar::GetSingleton();
        if (!cal) return false;
        const float h = cal->GetHour();
        return (h < 6.0f || h >= 18.0f);
    }

    void AdvanceGameHours(float hours) {
        auto* cal = RE::Calendar::GetSingleton();
        if (cal && cal->gameHour) {
            cal->gameHour->value += hours;
        }
    }

    int RollEncounterCount(const Settings& cfg) {
        float chance = cfg.baseEncounterChance;
        if (IsNight()) chance *= cfg.nightMultiplier;
        chance = std::clamp(chance, 0.0f, 100.0f);
        int hits = 0;
        for (int i = 0; i < cfg.checkpoints; ++i) {
            if (Roll100() < chance) ++hits;
        }
        return (std::min)(hits, cfg.maxEncountersPerJourney);
    }

    constexpr float kPi = 3.14159265f;

    float EaseInOut(float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);  // smoothstep
    }

    // Pan the world-map camera to keep our marker centred (KCD follows the
    // traveller). The map camera is TILTED, so simply setting its focus to the
    // marker's world XY mis-centres along north/south. Instead we measure where
    // the marker actually lands on screen and nudge the camera focus to drive
    // that toward screen-centre — a small feedback loop that self-corrects for
    // the tilt and zoom in every direction. Guarded by an exact vtable-identity
    // check so we only ever write a genuine World camera state.
    void FollowCamera(RE::MapMenu* a_mapMenu, RE::GFxMovieView* a_mv, const RE::NiPoint3& target) {
        if (!a_mv) return;
        auto& cam   = a_mapMenu->GetRuntimeData2().camera;
        auto* state = cam.currentState.get();
        if (!state) return;
        static REL::Relocation<std::uintptr_t> worldVtbl{ RE::MapCameraStates::World::VTABLE[0] };
        if (*reinterpret_cast<std::uintptr_t*>(state) != worldVtbl.address()) return;
        auto* world = static_cast<RE::MapCameraStates::World*>(state);

        const RE::GRectF r = a_mv->GetVisibleFrameRect();
        const float cx = (r.left + r.right) * 0.5f;
        const float cy = (r.top + r.bottom) * 0.5f;

        // Local screen-space Jacobian of the projection (screen units per world
        // unit) by sampling the marker and two small world offsets.
        constexpr float K = 500.0f;
        float mx, my, ax, ay, bx, by;
        if (!MapMarkerDraw::Project(a_mv, target, mx, my)) return;
        if (!MapMarkerDraw::Project(a_mv, { target.x + K, target.y, target.z }, ax, ay)) return;
        if (!MapMarkerDraw::Project(a_mv, { target.x, target.y + K, target.z }, bx, by)) return;

        const float a = (ax - mx) / K, c = (ay - my) / K;  // d(screen)/d(worldX)
        const float b = (bx - mx) / K, d = (by - my) / K;  // d(screen)/d(worldY)
        const float det = a * d - b * c;
        if (std::abs(det) < 1e-6f) return;

        // World move that brings the marker from its screen spot to centre.
        const float ex = mx - cx, ey = my - cy;
        const float dWx = ( d * ex - b * ey) / det;
        const float dWy = (-c * ex + a * ey) / det;

        constexpr float gain = 0.5f;  // eased for smoothness
        world->currentPosition.x += dWx * gain;
        world->currentPosition.y += dWy * gain;
    }

    void Finish() {
        s_animating.store(false);
        s_paused.store(false);
        s_traveling.store(false);
    }

    RE::NiPoint3 TravelForward() {
        return { s_active.dest.x - s_active.origin.x, s_active.dest.y - s_active.origin.y, 0.0f };
    }

    // Advance game time for the trip and teleport the player to the destination.
    bool TeleportToDest() {
        auto* player  = RE::PlayerCharacter::GetSingleton();
        auto  destPtr = s_active.destHandle.get();
        if (!player || !destPtr) return false;
        const float distance = RouteMath::Distance2D(s_active.origin, s_active.dest);
        const float hours    = RouteMath::JourneyHours(distance, Settings::GetSingleton().hoursPer100k);
        AdvanceGameHours(hours);
        player->MoveTo(destPtr.get());
        return true;
    }

    // Clean arrival — no event fired, or the player chose to keep travelling.
    void ArriveClean() {
        TeleportToDest();
        TravelOverlay::ShowArrival();
        Finish();
    }

    // The arrow finished its journey and the player engaged the event. Teleport
    // to the destination, wait in REAL time for the cell to load (MoveTo is
    // async), then spawn the event there. The obstacle check keeps spawns out of
    // buildings; true on-the-road placement is the deferred mid-route step.
    void ArriveAndEngage() {
        if (!TeleportToDest()) { Finish(); return; }
        const auto forward = TravelForward();
        const auto kind    = s_active.eventKind;
        const bool byCart  = s_active.byCart;
        std::thread([forward, kind, byCart]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(2800));
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([forward, kind, byCart]() {
                    EncounterManager::EngageEvent(kind, byCart, forward, []() {
                        TravelOverlay::ShowArrival();
                        Finish();
                    });
                });
            }
        }).detach();
    }

    // Skyrim's walled cities (Solitude, Whiterun, Windhelm, ...) are each their
    // OWN worldspace with their own coordinate system, a CHILD of Tamriel
    // (TESWorldSpace::parentWorld), related by a fixed transform Bethesda stores
    // for map-projection purposes (worldMapOffsetData / ONAM): parent = local *
    // mapScale + mapOffset. This lets us convert a position from a city's local
    // coordinates into Tamriel's, so a trip that STARTS or ENDS inside a city can
    // still be interpolated in the shared (Tamriel) space instead of just being
    // refused outright.
    RE::NiPoint3 ToParentSpace(RE::TESWorldSpace* ws, const RE::NiPoint3& local) {
        if (!ws || !ws->parentWorld) return local;
        const auto& o = ws->worldMapOffsetData;
        return { local.x * o.mapScale + o.mapOffsetX,
                 local.y * o.mapScale + o.mapOffsetY,
                 local.z * o.mapScale + o.mapOffsetZ };
    }

    bool IsCitylikeWorld(RE::TESWorldSpace* ws) {
        using Flag = RE::TESWorldSpace::Flag;
        return !ws || ws->flags.any(Flag::kSmallWorld) || ws->flags.any(Flag::kNoLandscape);
    }

    // PHASE 2 — the real "stopped along the way". Teleport the player to the STOP
    // POINT on the route (where the arrow halted), not the destination, so the
    // native map arrow ends up exactly where the player physically is.
    //
    // How: figure out a SHARED worldspace to interpolate in (same worldspace, or
    // one endpoint transformed into the other's space via the city<->Tamriel
    // transform above), reject it if that shared space is a city/interior-like
    // compound (no real terrain — nothing to ground on), interpolate the stop
    // point there, spawn a temp marker from a ref ALREADY in that worldspace,
    // reposition it to the stop coords, and MoveTo it. The player's position is
    // then corrected onto real nearby ground (SettlePlayerOnGround). Anything
    // that can't be resolved safely falls back to the destination so nobody is
    // stranded floating, buried, or lost in unrelated worldspaces (e.g. Solstheim).
    void MidRouteStopThenEngage() {
        auto* player  = RE::PlayerCharacter::GetSingleton();
        auto  destPtr = s_active.destHandle.get();
        if (!player) { Finish(); return; }

        const float dist = RouteMath::Distance2D(s_active.origin, s_active.dest);
        AdvanceGameHours(RouteMath::JourneyHours(dist, Settings::GetSingleton().hoursPer100k) *
                         s_active.encounterFraction);  // time for the distance actually covered

        auto* wsPlayer = player->GetWorldspace();
        auto* wsDest   = destPtr ? destPtr->GetWorldspace() : nullptr;

        // Resolve a SHARED worldspace to interpolate in, transforming whichever
        // endpoint is local to a city into the other's space via the ONAM
        // transform. `anchor` is a reference already sitting in that shared
        // worldspace — required because repositioning a marker never changes
        // which worldspace/cell it belongs to; it must be spawned from a ref
        // that's already there.
        RE::NiPoint3 originForLerp = s_active.origin;
        RE::NiPoint3 destForLerp   = s_active.dest;
        RE::TESObjectREFR* anchor  = nullptr;
        RE::TESWorldSpace* sharedWs = nullptr;

        if (wsPlayer && !wsDest) {
            // Destination cell not currently loaded — assume it shares the
            // player's worldspace (true for the overwhelming majority of trips).
            sharedWs = wsPlayer;
            anchor   = player;
        } else if (wsPlayer && wsDest && wsPlayer == wsDest) {
            sharedWs = wsPlayer;
            anchor   = player;
        } else if (wsPlayer && wsDest && wsPlayer->parentWorld == wsDest) {
            originForLerp = ToParentSpace(wsPlayer, s_active.origin);  // city -> Tamriel
            sharedWs = wsDest;
            anchor   = destPtr.get();
        } else if (wsPlayer && wsDest && wsDest->parentWorld == wsPlayer) {
            destForLerp = ToParentSpace(wsDest, s_active.dest);        // Tamriel -> city (rare mid-trip)
            sharedWs = wsPlayer;
            anchor   = player;
        }
        // else: genuinely unrelated worldspaces (e.g. Solstheim) — no shared
        // space found; sharedWs stays null and we fall back to the destination.

        const bool citylike = IsCitylikeWorld(sharedWs);
        logger::info("MidRoute: wsPlayer={} wsDest={} shared={} citylike={} frac={:.2f}",
            wsPlayer ? wsPlayer->GetFormEditorID() : "(null)",
            wsDest   ? wsDest->GetFormEditorID()   : "(null)",
            sharedWs ? sharedWs->GetFormEditorID() : "(none)", citylike, s_active.encounterFraction);

        bool attemptedStop = false;
        if (sharedWs && !citylike && anchor) {
            const RE::NiPoint3 stop = RouteMath::Lerp(originForLerp, destForLerp, s_active.encounterFraction);
            logger::info("MidRoute: interpolated stop=({:.0f},{:.0f},{:.0f}) in {}", stop.x, stop.y, stop.z, sharedWs->GetFormEditorID());
            if (auto* mkForm = RE::TESForm::LookupByID(0x00000034)) {           // XMarkerHeading
                if (auto* base = mkForm->As<RE::TESBoundObject>()) {
                    if (auto marker = anchor->PlaceObjectAtMe(base, false)) {   // created IN the shared worldspace
                        marker->SetPosition(stop);                             // then relocated to the stop
                        player->MoveTo(marker.get());                          // teleport + load that cell
                        marker->Disable();
                        marker->SetDelete(true);
                        attemptedStop = true;
                    } else {
                        logger::warn("MidRoute: PlaceObjectAtMe(marker) failed");
                    }
                }
            } else {
                logger::warn("MidRoute: could not look up XMarkerHeading (0x34)");
            }
        } else {
            logger::info("MidRoute: not attempting a stop ({})",
                !sharedWs ? "unrelated worldspaces" : citylike ? "city/interior-like — no terrain to ground on" : "no anchor");
        }
        if (!attemptedStop && destPtr) {
            logger::info("MidRoute: cross-worldspace or unresolvable — falling back to destination");
            player->MoveTo(destPtr.get());
        }

        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));  // load + physics settle
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([]() {
                    // We've committed to ONE teleport. The area is now loaded, so
                    // finding good ground is a LOCAL move (instant, no load screen)
                    // — never a second MoveTo to the destination. That second
                    // teleport was the jarring "arrive, then a second closer load"
                    // double-load, and it dumped the player at the destination
                    // anyway instead of a genuine stop on the way. SettlePlayer-
                    // OnGround now always commits to the best real ground nearby.
                    EncounterManager::SettlePlayerOnGround();
                    const auto forward = TravelForward();
                    EncounterManager::EngageEvent(s_active.eventKind, s_active.byCart, forward, []() {
                        RE::DebugNotification("You're stopped on the road.");
                        Finish();
                    });
                });
            }
        }).detach();
    }

    // Direct (no-map) resolution for cart / Papyrus-fallback journeys: teleport,
    // wait for load, then roll+prompt+engage at the destination.
    void ResolveDirect() {
        if (!TeleportToDest()) { Finish(); return; }
        const auto forward = TravelForward();
        const bool byCart  = s_active.byCart;
        const bool hasEvent = s_active.waylays > 0;
        std::thread([forward, byCart, hasEvent]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(2800));
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([forward, byCart, hasEvent]() {
                    if (hasEvent) {
                        EncounterManager::ResolveEncounter(byCart, forward, []() {
                            TravelOverlay::ShowArrival();
                            Finish();
                        });
                    } else {
                        TravelOverlay::ShowArrival();
                        Finish();
                    }
                });
            }
        }).detach();
    }
}

float Roll100() {
    std::uniform_real_distribution<float> dist(0.0f, 100.0f);
    return dist(Rng());
}

void ResetForSaveLoad() {
    s_traveling.store(false);
    s_animating.store(false);
    s_paused.store(false);
    s_active = ActiveJourney{};  // drops destHandle — it may be stale after a load
    logger::info("JourneyController: reset for save load");
}

bool IsTraveling() { return s_traveling.load(); }
bool IsAnimating() { return s_animating.load(); }

namespace {
    // Shared validation + s_active setup for both entry points. Claims the
    // traveling flag on success (caller owns clearing it via ResolveArrival).
    bool PrepareActive(const Request& req) {
        auto& cfg = Settings::GetSingleton();
        if (!cfg.enabled) return false;

        auto destPtr = req.destination.get();
        if (!destPtr) { logger::warn("Journey: PrepareActive abort — destination handle empty"); return false; }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) { logger::warn("Journey: PrepareActive abort — no player"); return false; }

        // KCD parity guard — refuse to set out overloaded.
        if (cfg.blockWhenOverEncumbered && player->IsOverEncumbered()) {
            logger::info("Journey: blocked — over-encumbered");
            RE::DebugNotification("You are carrying too much to travel.");
            return false;
        }
        // NOTE: an IsInCombat() guard used to live here, refusing to start the
        // journey while "in combat" — but reaching this callback at all means
        // vanilla's OWN fast-travel confirm dialog was already accepted, which
        // Skyrim doesn't allow while genuinely fighting. IsInCombat() can stay
        // true for a moment after a fight actually ends, so this guard was a
        // false positive in practice — and worse, refusing here fell through to
        // s_origRun (vanilla), which teleported the player anyway, just without
        // the map animation. That silent "sometimes it just teleports with no
        // arrow" was this guard doing exactly what it was told, not a bug in the
        // animation itself. Removed rather than patched around.

        bool expected = false;
        if (!s_traveling.compare_exchange_strong(expected, true)) {
            logger::warn("Journey: PrepareActive abort — already traveling");
            return false;  // already traveling
        }

        s_active = ActiveJourney{};
        s_active.origin       = req.origin;
        s_active.dest         = destPtr->GetPosition();
        s_active.destHandle   = req.destination;
        s_active.byCart       = req.byCart;
        s_active.waylays      = RollEncounterCount(cfg);
        s_active.t            = 0.0f;
        s_active.cameraFollow = cfg.mapCameraFollow;

        // If an event fires this journey, pre-roll its kind and pick where along
        // the road the player will be stopped and asked how to approach it.
        s_active.hasEvent = (s_active.waylays > 0);
        s_active.prompted = false;
        s_paused.store(false);
        if (s_active.hasEvent) {
            s_active.eventKind = EncounterManager::RollEventKind();
            std::uniform_real_distribution<float> frac(0.35f, 0.75f);
            s_active.encounterFraction = frac(Rng());
        }

        const float distance = RouteMath::Distance2D(s_active.origin, s_active.dest);

        // Scale the arrow-slide time to distance so short hops feel quick and
        // long treks feel long, capped at mapTravelSeconds. Cart is a touch
        // faster than on foot.
        const float speed = (req.byCart ? cfg.cartSpeed : cfg.onFootSpeed);
        s_active.duration = std::clamp(distance / (std::max)(speed, 1.0f),
                                       2.5f, (std::max)(cfg.mapTravelSeconds, 2.5f));

        const float hours = RouteMath::JourneyHours(distance, cfg.hoursPer100k);
        TravelOverlay::ShowJourneyStart(distance, hours, req.byCart);
        return true;
    }
}

bool Begin(const Request& req) {
    // Direct (non-animated) journey: no open map to slide the arrow across
    // (carriage rides, Papyrus fallback). Resolve arrival on the main thread.
    if (!PrepareActive(req)) return false;
    s_animating.store(false);
    if (auto* task = SKSE::GetTaskInterface()) {
        task->AddTask([]() { ResolveDirect(); });
    } else {
        ResolveDirect();
    }
    return true;
}

bool BeginMapJourney(const Request& req) {
    // Animated journey: keep the map open and slide the arrow before arrival.
    if (!PrepareActive(req)) return false;
    s_animating.store(true);
    logger::info("Journey: map animation started dist={:.0f} waylays={} dur={:.1f}s",
        RouteMath::Distance2D(s_active.origin, s_active.dest), s_active.waylays, s_active.duration);
    return true;
}

// Close the open map and hand off to a main-thread task.
static void CloseMapThen(void (*fn)()) {
    if (auto* msgQ = RE::UIMessageQueue::GetSingleton()) {
        msgQ->AddMessage(RE::BSFixedString(RE::MapMenu::MENU_NAME),
                         RE::UI_MESSAGE_TYPE::kHide, nullptr);
    }
    if (auto* task = SKSE::GetTaskInterface()) {
        task->AddTask([fn]() { fn(); });
    } else {
        fn();
    }
}

void TickMapAnimation(RE::MapMenu* a_mapMenu, float a_interval) {
    if (!s_animating.load() || !a_mapMenu) return;
    auto* mv = a_mapMenu->uiMovie.get();

    // While the mid-route dialogue is up, freeze the arrow (keep it drawn) and
    // wait for the player's decision.
    if (!s_paused.load()) {
        s_active.t += a_interval / s_active.duration;
    }
    const float p = EaseInOut((std::min)(s_active.t, 1.0f));
    const RE::NiPoint3 pos = RouteMath::Lerp(s_active.origin, s_active.dest, p);

    // Draw OUR marker over the map, projected through the world-map camera. The
    // native player arrow and every location icon are left completely untouched.
    if (mv) {
        float sx = 0.0f, sy = 0.0f, dsx = 0.0f, dsy = 0.0f;
        const bool okCur = MapMarkerDraw::Project(mv, pos, sx, sy);
        const bool okDst = MapMarkerDraw::Project(mv, s_active.dest, dsx, dsy);
        float heading = 0.0f;
        if (okCur && okDst) heading = std::atan2(dsy - sy, dsx - sx) * 180.0f / kPi;
        if (okCur) MapMarkerDraw::DrawArrow(mv, sx, sy, heading);
    }
    if (s_active.cameraFollow && mv && !s_paused.load()) {
        FollowCamera(a_mapMenu, mv, pos);
    }

    if (s_paused.load()) return;  // dialogue up — nothing else to do

    // The arrow reaches the stop point along the road — you're stopped HERE (the
    // arrow does NOT run on to Solitude). Reveal the event and ask how you approach.
    if (s_active.hasEvent && !s_active.prompted && s_active.t >= s_active.encounterFraction) {
        s_active.prompted = true;
        s_paused.store(true);
        TravelOverlay::ShowWaylaid(s_active.byCart);
        EncounterManager::PromptEvent(s_active.eventKind, s_active.byCart, [](EncounterManager::Approach outcome) {
            using Approach = EncounterManager::Approach;
            if (outcome == Approach::CleanPass) {
                // Got away clean — the arrow resumes and you carry on. (No second
                // roll: PromptEvent already decided the whole outcome.)
                s_active.hasEvent = false;
                s_paused.store(false);
                return;
            }
            if (outcome == Approach::Waylaid)
                s_active.eventKind = EncounterManager::EventKind::Danger;  // detour ran into danger
            s_animating.store(false);
            CloseMapThen(&MidRouteStopThenEngage);  // stopped on the road (engage or waylaid)
        });
        return;
    }

    if (s_active.t >= 1.0f) {
        s_animating.store(false);
        if (mv) MapMarkerDraw::Clear(mv);
        CloseMapThen(&ArriveClean);  // reached the destination
    }
}

}
