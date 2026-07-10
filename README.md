# Wayfarer

A Kingdom Come: Deliverance–inspired immersive travel system for Skyrim SE/AE.
Fast travel becomes an actual **journey**: when you confirm travel on the map,
Wayfarer intercepts the teleport, passes in-game time as if you walked (or rode),
and can **waylay you on the road** — bandits, predators, ambushes — with a
KCD-style **engage / avoid** choice where avoiding can fail. Cart/carriage travel
runs through the same system.

Built as a single CommonLibSSE-NG SKSE plugin, reusing the build scaffold and the
"render-into-a-GFx-movie" presentation technique from **Skyrim Loading Percent**.
Designed for **maximum compatibility**.

---

## Status — v1 scaffold

This is the v1 build. What works in the DLL today:

- **True mid-route interception (C++):** hooks `FastTravelConfirmCallback::Run`
  (verified vtable in CommonLibSSE-NG) so the journey is decided *before* the
  native teleport fires — not after arrival.
- **Moving player arrow (KCD-style):** on confirm the map stays open and the
  native player arrow slides origin→destination — driven each frame by a hook on
  `MapMenu::AdvanceMovie` writing `MapMenu::playerMarkerPosition` (the field that
  positions the arrow; confirmed manipulable by Flat Map Markers). The world-map
  camera pans to follow. Only when the arrow arrives does the map close and the
  teleport/encounter resolve. Arrow-slide time and camera-follow are MCM-tunable.
- **Journey controller:** straight-line distance → in-game hours; game time
  advanced accordingly; encounter rolls across N checkpoints; over-encumbered and
  in-combat guards (KCD parity).
- **Encounters:** KCD engage/avoid message box; avoid rolls against Sneak and can
  fail; combat encounters spawn a configurable hostile pool at the player.
- **Cart travel:** `Wayfarer.StartCartJourney(marker)` routes carriage rides
  through the same controller at cart speed.
- **Fallback:** `Wayfarer.TriggerWaylayFallback()` gives an arrive-then-waylay
  path (driven by a player-alias `OnPlayerFastTravelEnd`) if the hook is disabled.
- **Full MCM** (MCM Helper `config.json`) + INI tuning for every knob.

### Deliberately v1-scoped (documented, not hidden)

- **Encounter placement:** v1 resolves the encounter *on the approach*, at the
  destination arrival point (the true mid-route *interception* is real; arbitrary
  mid-road *placement* needs road/navmesh data). `RouteMath::Lerp` is the ready
  seam for v1.1 interpolated placement.
- **Arrow drive needs one in-game check:** the per-frame `playerMarkerPosition`
  write is the mechanism; if a runtime rewrites it after our write so the arrow
  won't budge, the fallback is drawing our own marker into the MapMenu GFx movie
  with the Loading Percent Scaleform technique (guaranteed-visible, ENB-safe).
  Camera-follow (`bMapCameraFollow`) is the more fragile half — toggle it off if
  it misbehaves; the arrow still slides.
- **Encounters resolve at arrival:** you *watch* the arrow travel, then the
  encounter (engage/avoid) resolves at the destination. Freezing the arrow
  mid-map for the choice is the next refinement (needs no new tech).
- **HUD narration** (`src/ui/TravelOverlay.cpp`) still backs the journey with
  text; it shares the signatures a richer Scaleform overlay would drive.
- **Default spawn pool is empty** (`iActorForm*=0`) so a clean install never
  spawns an unintended form — combat encounters narrate until you set real
  FormIDs in the INI/MCM. Vanilla Story Manager sourcing (`iSource=1/2`) is the
  hybrid hook point for encounter-mod content.
- **Cross-worldspace trips** (e.g. Solstheim) are treated as clean arrivals in v1
  (straight-line interpolation isn't valid across worldspaces).

---

## Building

Prerequisites: Visual Studio 2022 (v143 toolset), CMake ≥ 3.21, and vcpkg with
`VCPKG_ROOT` set.

```sh
cmake --preset release
cmake --build build/release --config Release
```

Output: `build/release/Release/Wayfarer.dll`.

To auto-deploy on build, pass your Skyrim `Data` folder:

```sh
cmake --preset release -DSKYRIM_PATH="C:/.../Skyrim Special Edition/Data"
```

---

## Installing (manual)

1. `Wayfarer.dll` → `Data/SKSE/Plugins/`
2. `data/SKSE/Plugins/Wayfarer.ini` → `Data/SKSE/Plugins/`
3. `data/MCM/Config/Wayfarer/config.json` → `Data/MCM/Config/Wayfarer/`
4. Compile the `.psc` files in `Scripts/Source/` and deploy the `.pex` to
   `Data/Scripts/` (needs the Papyrus compiler / Creation Kit).
5. **ESP (data step):** create a small ESL-flagged plugin with a start-enabled
   quest that has a `ReferenceAlias` pointing at `PlayerRef` running
   `WayfarerPlayerAlias`. This wires the fallback path and lets MCM Helper show
   the menu. No vanilla records are edited.

Requires: SKSE64, Address Library, and (for the menu/fallback) MCM Helper.

---

## Architecture

| Module | Responsibility |
|---|---|
| `hooks/FastTravelHook` | Intercept `FastTravelConfirmCallback::Run`; capture origin + destination marker; suppress native teleport; hand off. |
| `journey/JourneyController` | State/flow: distance → hours, time advance, encounter rolls, dispatch to main thread, arrival. |
| `journey/RouteMath` | Distance / interpolation / hours (the mid-route placement seam). |
| `encounters/EncounterManager` | Engage/avoid message box, avoid roll, hostile spawning. |
| `ui/TravelOverlay` | v1 HUD narration; v1.1 Scaleform map overlay seam. |
| `papyrus/PapyrusAPI` | Native functions for MCM, cart glue, and the fallback. |
| `config/Settings` | INI + MCM-overlaid tuning store. |

## Compatibility

Single NG DLL (SE/AE/VR via Address Library); a narrow, reversible vtable hook
that only triggers on the travel-confirm "Yes"; no D3D hooking; encounters can be
sourced from the vanilla Story Manager so encounter mods extend Wayfarer for
free; master toggle and forced Papyrus fallback as escape hatches.

## Credits

Built on [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) and
techniques from Skyrim Loading Percent.
