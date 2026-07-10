#pragma once
#include <mutex>

// Central tuning store for Wayfarer. Loaded from the default INI first, then
// overlaid with MCM Helper's user overrides so the MCM always wins. Every knob
// the design exposes lives here so the hook / journey / encounter code reads a
// single source of truth.
class Settings {
public:
    static Settings& GetSingleton() noexcept {
        static Settings instance;
        return instance;
    }

    void Load();
    void Save();

    // ── Master ────────────────────────────────────────────────────────────
    // Master on/off. When false the hook stands down and vanilla fast travel
    // happens untouched (maximum-compatibility escape hatch).
    bool enabled{ true };

    // Force the Papyrus arrive-then-waylay fallback even if the C++ confirm
    // hook installed successfully. Troubleshooting switch.
    bool forceFallback{ false };

    // ── Journey ───────────────────────────────────────────────────────────
    // Player travel speed in game-world units per real second of simulated
    // travel. Only affects how long the journey overlay lasts, not distance.
    float onFootSpeed{ 12000.0f };
    float cartSpeed{ 20000.0f };

    // In-game hours that pass per 100,000 world units travelled. Vanilla-ish
    // pacing; tune to taste. Total time = distance/100000 * hoursPer100k.
    float hoursPer100k{ 2.0f };

    // How many encounter checks are rolled across a full journey.
    int checkpoints{ 4 };

    // Block starting a journey while over-encumbered (KCD parity).
    bool blockWhenOverEncumbered{ true };

    // ── Encounters ────────────────────────────────────────────────────────
    // Base chance (0-100) that any single checkpoint produces an encounter,
    // before region / time-of-day / level multipliers. With 4 checkpoints, 5%
    // per check ≈ a ~1-in-5 chance of an encounter per journey (a bit higher at
    // night); tune to taste.
    float baseEncounterChance{ 5.0f };

    // Multiply encounter chance at night (18:00-06:00).
    float nightMultiplier{ 1.5f };

    // Cap on encounters per journey so a long trip can't become a gauntlet.
    int maxEncountersPerJourney{ 1 };

    // Encounter source mix: 0 = own pool only, 1 = vanilla Story Manager only,
    // 2 = hybrid (roll between them).
    int encounterSource{ 2 };

    // Avoid-success base chance (0-100). The player's Sneak/level shift this.
    float avoidBaseChance{ 45.0f };

    // Relative weights for what KIND of event fires when an encounter triggers.
    // Not percentages — picked proportionally. Set any to 0 to disable that kind.
    // 50/50 combat vs non-combat (TESTING). Non-combat splits camp/corpse/debris.
    float wCombat{ 50.0f };
    float wTraveler{ 15.0f };   // traveler's camp (rest, sometimes a corpse)
    float wDiscovery{ 15.0f };  // roadside corpse
    float wMoral{ 0.0f };       // beggar — deferred until a proper NPC form
    float wHazard{ 20.0f };     // road debris (+35% ambush)

    // Leaving the road (choosing to avoid/go around an event) is dangerous. This
    // is the % chance that "keep travelling / find another way" drops you into a
    // wilderness danger instead of a clean pass.
    float wildernessDangerChance{ 85.0f };
    // Relative weights for WHAT the wilderness danger is (0 disables that kind).
    float wDangerAnimals{ 25.0f };   // wolves, bears, sabre cats
    float wDangerMonsters{ 20.0f };  // spiders, trolls, mudcrabs
    float wDangerBandits{ 25.0f };   // bandit gang
    float wDangerRobbers{ 20.0f };   // a small shakedown
    float wDangerDragons{ 10.0f };   // a dragon

    // Upper cap on hostiles spawned per combat encounter (the coherent group
    // picks its own size within this cap).
    int encounterActorCount{ 5 };

    // Solitary predators (bears, sabre cats) come alone by default; this is the
    // % chance one brings a second. Wolves (packs) and humanoid gangs ignore it.
    // Low by default — a lone sabre cat/bear is the norm; a pair is a rare scare.
    float pairChance{ 10.0f };

    // Up to 4 base-form FormIDs (in the plugin named by encounterActorPlugin)
    // resolved and spawned for combat encounters. 0 = unused. Defaults are the
    // vanilla bandit/wolf leveled-character lists (Skyrim.esm), verified FormIDs:
    //   0x00039CFC LCharBanditMelee1H, 0x0003DEC8 LCharBanditMelee2H,
    //   0x0001A348 LCharBanditMissileNordM (archer), 0x000B83C2 LCharWolf.
    // PlaceObjectAtMe resolves a leveled character to a real, level-scaled NPC.
    unsigned int encounterActor[4]{ 0x00039CFC, 0x0003DEC8, 0x0001A348, 0x000B83C2 };
    std::string  encounterActorPlugin{ "Skyrim.esm" };

    // ── Presentation ──────────────────────────────────────────────────────
    // Show the on-screen travel overlay while a journey resolves.
    bool showOverlay{ true };

    // Max real seconds the player arrow slide takes (for the longest journeys);
    // shorter trips scale down proportionally to distance.
    float mapTravelSeconds{ 12.0f };

    // Pan the world-map camera to follow our travel marker (KCD-style). Safe now
    // that we draw our own marker instead of moving the native one, so the map
    // pans with every location icon staying glued to the terrain.
    bool mapCameraFollow{ true };

    static constexpr auto kIniPath    = L"Data/SKSE/Plugins/Wayfarer.ini";
    static constexpr auto kMcmIniPath = L"Data/MCM/Settings/Wayfarer.ini";

    std::mutex iniMutex;

private:
    Settings() = default;
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
};
