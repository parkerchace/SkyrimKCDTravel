#pragma once

namespace JourneyController {
    // A request captured by the fast-travel hook: where the player is now and
    // which map marker they confirmed travel to.
    struct Request {
        RE::NiPoint3        origin;
        RE::ObjectRefHandle destination;
        bool                byCart{ false };
    };

    // Begin a *direct* journey (no map animation): advance time, teleport, and
    // resolve encounters immediately. Used by carriage rides and the Papyrus
    // fallback, where there is no open map to slide the arrow across.
    bool Begin(const Request& req);

    // Begin a *map-animated* journey: the map stays open and the player arrow
    // slides origin->destination (driven per-frame by the MapMenu AdvanceMovie
    // hook) before we advance time / teleport / resolve encounters. Called from
    // the fast-travel confirm hook. Returns false if it can't start (caller then
    // lets vanilla fast travel proceed). Does NOT teleport or close the map.
    bool BeginMapJourney(const Request& req);

    // Per-frame animation tick, called from MapMenu::AdvanceMovie (main thread)
    // AFTER the original runs. Advances the arrow and, on completion, closes the
    // map and dispatches arrival resolution.
    void TickMapAnimation(RE::MapMenu* a_mapMenu, float a_interval);

    // True while a journey (animation or unresolved encounter) is in progress.
    bool IsTraveling();

    // True while the map arrow is actively animating (used by the MapMenu hook).
    bool IsAnimating();

    // Shared RNG for journey/encounter rolls (0-100).
    float Roll100();

    // Abort any in-flight journey bookkeeping. Call on every save load (and new
    // game) — s_active can hold a handle into whatever world was loaded when a
    // journey started; after a DIFFERENT save loads, that handle is stale and
    // must never be teleported to / touched.
    void ResetForSaveLoad();
}
