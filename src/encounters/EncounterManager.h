#pragma once
#include <functional>

namespace EncounterManager {
    enum class EventKind { Combat, Camp, Corpse, Debris, Danger };

    // The single, resolved outcome of the player's "how do you approach it?"
    // choice — decided entirely inside PromptEvent so there is never a second,
    // contradictory roll afterwards ("you slip past" ... then ambushed anyway).
    //   Engage    = deal with the shown event right here.
    //   CleanPass = you got away / kept to the road — travel on, no encounter.
    //   Waylaid   = your detour off the road ran you into a wilderness danger.
    enum class Approach { Engage, CleanPass, Waylaid };

    // True when avoiding an event / leaving the road drops you into a wilderness
    // danger (per the MCM chance). If so, resolve the event as EventKind::Danger.
    bool WildernessDangerStrikes();

    // Install the hit-event sink that lets the player batter down static road
    // debris. Call once after data load.
    void InstallHitSink();

    // Drop all of Wayfarer's process-lifetime bookkeeping (tracked decorative
    // props, the obstacle cache, debris-cluster state) WITHOUT touching the refs
    // themselves. Call on every save load (and new game) — those handles point
    // into whatever world was loaded when they were created, and after a
    // DIFFERENT save loads they're dangling; touching them (e.g. Disable/
    // SetDelete, as the normal same-session cleanup does) is unsafe once the
    // underlying save context has changed. Just clearing our own lists is safe.
    void ResetForSaveLoad();

    // After teleporting the player to an arbitrary point (mid-route stop), find
    // real, walkable, dry, unobstructed ground near their CURRENT position and
    // correct their placement onto it (fixes floating-in-the-sky / buried-
    // underground / stuck-in-a-mountain-wall from a naive coordinate guess).
    // Returns false if nothing valid was found nearby, so the caller can fall
    // back to a known-safe location instead of leaving the player stranded.
    bool SettlePlayerOnGround(float searchRadius = 700.0f);

    // Pick an event kind by the configured category weights.
    EventKind RollEventKind();

    // Show the "something's ahead — how do you approach it?" choice for this kind
    // (this is the KCD mid-journey decision). Calls onDecision(true) to ENGAGE
    // (or when a combat "avoid" roll fails), or onDecision(false) to wave it off
    // and keep travelling. Must run on the main thread (queues a message box).
    void PromptEvent(EventKind kind, bool byCart, std::function<void(Approach)> onDecision);

    // Spawn/resolve the event at the player's current location. Main thread.
    void EngageEvent(EventKind kind, bool byCart, const RE::NiPoint3& forward, std::function<void()> onResolved);

    // Convenience for paths without a map animation (Papyrus fallback): roll,
    // prompt, and engage-or-skip in one call.
    void ResolveEncounter(bool byCart, const RE::NiPoint3& forward, std::function<void()> onResolved);
}
