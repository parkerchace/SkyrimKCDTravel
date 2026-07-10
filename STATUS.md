# Wayfarer — Status & Master Plan

KCD-inspired immersive travel for Skyrim SE/AE. Fast travel becomes a *journey*:
a marker crosses the map, time passes, and you're stopped on the road by real,
choosable events. Everything is generative/runtime — **no hand-authored world
data**, so external space mods work with zero patches.

_Encounter chance is at **100% (TESTING)** — revert `fBaseChance` to ~5–8 to ship._

---

_Re-evaluated 2026-07-07 after the navmesh breakthrough. Mid-route + placement
are now SOLVED; focus has shifted to content quality (scenes) and shipping polish._

## 1. Snapshot

### Working (verified in-game)
- **Interception & journey:** hook the map fast-travel confirm, cancel native
  teleport, run our journey; icons stay put; distance-scaled time.
- **Own map marker:** drawn + projected through the world-map camera; camera
  follows, centered via screen-space feedback.
- **GENUINE mid-route stop (Phase 2 ✅):** the player is physically teleported to
  a point ALONG the route (not the destination). Handles cross-worldspace via the
  city↔Tamriel ONAM transform; refuses city/interior-like shared spaces. ONE load
  (no double-load); commits to the stop, never re-teleports.
- **NAVMESH placement (the breakthrough ✅):** settle snaps the player to the
  nearest **road** (`kPreferred` navmesh triangle = Skyrim's own designer road
  annotation) or walkable triangle across the whole loaded cell grid. Guarantees
  standing-ground (no mountainside/floating/water), terrain-referenced Z (no
  sky-fall from high→low), elevation-capped (no cliff-switchback yank). Roads
  detected generically from the engine's pathing data — works vanilla/DLC/mods.
- **Mid-journey choice (KCD):** arrow stops, dialogue asks how to approach. ONE
  clean roll now: passive events (corpse/camp/debris) leave cleanly if you decline;
  only combat gets a Sneak-scaled slip-past. No more "you slipped by → ambushed."
- **Events:** combat (coherent single-type groups + 8 formations, spawn at a
  SEE-IT-COMING distance ~900-1600), camp (discovered varied scene + NPC, below),
  corpse (varied body, bloodstain, delayed proximity ambush), debris (bandit-only
  trap ambush), danger (animals/monsters/bandits/robbers/dragons, dragon at
  altitude). MCM weights per category + per danger. Solo predators default to 1.
- **Destructible debris:** static blockers batter down as ONE cluster; barrels
  Fus-movable.
- **Hygiene & safety:** props cleaned on next event; state reset on save-load
  (kPreLoadGame/kNewGame — was a stale-handle crash risk); leveled lists resolve
  to level-scaled NPCs; time-of-day gating; anti-repeat.

### Phase status (re-evaluated)
- **Phase 1 — Spawn/Placement Engine:** ✅ **DONE.** Navmesh made this solid.
  Objects/actors land on real walkable ground; obstacle spheres (loaded-3D OR
  OBND fallback) keep spawns out of walls/rocks; bound-aware Z lift.
- **Phase 2 — Mid-route + road detection:** ✅ **DONE (core).** Genuine on-route
  stop that lands on real roads via navmesh. Remaining refinement is quality, not
  capability (see Frontier below).
- **Phase 3 — Encounter Director:** 🟡 **PARTIAL.** Time-of-day gating + anti-repeat
  + coherent groups + solo-predator logic done. REMAINING: region gating (Forsworn
  Reach-only, snow-appropriate content — now feasible via cell/region data),
  stats→travel-success influence (Sneak already factors avoid; expand to a toggle),
  leader+minions, player-state reactivity (bounty→hunters).
- **Phase 4 — Presentation/Scenes:** 🔴 **THE ACTIVE FRONTIER.** Scenes need to
  feel real: discovered varied camps WITH npcs (v2 shipped — iterate), and debris
  that's genuine "cluttered clutter" (many grounded pieces, not floating meshes).
  A proper **scene composer** built on the navmesh flat-ground guarantee.

### Active frontier / remaining work (priority order)
1. **Scene composer — DEBRIS redesign:** road-blocks are still sparse/floating-
   feeling. Want MANY grounded, believable pieces that truly block, arranged
   generatively. (Camps got v2 this pass; debris is next.)
2. **Scene composer — CAMP iteration:** v2 = discovered + NPC + 3 variants +
   navmesh-grounded, no forced rest. Iterate on variety/props/believability.
3. **MCM ESP + Papyrus scripts:** deliver the actual plugin + scripts (only INI
   config exists). User explicitly wants this.
4. **Region gating:** Forsworn Reach-only, snow-appropriate debris/enemies.
5. **Stats → safe-travel success (toggle):** expand Sneak-on-avoid into a real,
   toggleable skill/stat influence on encounter/avoid outcomes.
6. **Ship config:** revert `fBaseChance` 100→~6; final MCM defaults.

### Frontier note — "the stop point itself following the road"
Navmesh snap lands you ON a real road, but the stop POINT is still chosen by
straight-line interpolation, so on very mountainous routes it can pick a spot off
the natural road path before snapping. Making the route itself follow roads needs
true route knowledge (offline pre-gen of a road-point DB, or a runtime pathing
query). Deferred — current behavior is good for most trips.

### Recently fixed (this era)
Sky-fall high→low ✅ · mountainside embedding ✅ · double-load ✅ · slipped-by-then-
ambushed ✅ · roadblock-became-a-spider ✅ · 2-sabre-cats ✅ · save-load crash
(was another mod, confirmed) ✅ · Havok-raycast crash storm (removed) ✅.

---

## 2. Systems to build (the plan is organized around these, not one-off fixes)

### System A — Spawn Engine  `(P0)`
A single, reliable placement API used by every event.
- **Ground snap by object bounds:** query the object's Z-extent and place its
  *base* on the surface (fixes floating pot / bodies).
- **Surface validation:** reject water, roofs, steep slopes, and points with no
  headroom (raycast up = inside a building) or an object already there (clutter/
  fauna) — retry nearby until a clear spot is found or give up gracefully.
- **Corpse spawns:** spawn already-dead reliably — try KillImmediate before
  Disable, or spawn far/behind and ragdoll settle; verify with a placed-ref log.
- **Persistence policy:** transient dressing tracked + cleaned (done); decide
  what persists (lootable bodies until looted vs. pure decoration).
- Deliverable: `PlaceGrounded(base, pos, opts)` returning a valid ref or nothing,
  with clearance/water/building checks and bounds-aware Z.

### System B — Scene Composer  `(P0/P1)`
Generative arrangement of *related* props into a believable scene.
- Define **scene templates** as relative layouts around an anchor (e.g. camp:
  fire at center → cooking spit *with rack* offset, bedrolls at a comfortable
  radius facing the fire, a log seat, a satchel, embers/light), with **min-spacing**
  so nothing overlaps and nothing sits in the flames.
- Pick **region/biome-appropriate variants** (snow bedroll vs grass, etc.).
- Convincing **road obstructions:** an overturned cart + spilled cargo + a fallen
  tree/boulder that genuinely spans the road (movable where it should be), not a
  loose wheel.
- Deliverable: `ComposeScene(template, anchor, region)` driving System A.

### System C — Road & Region Detection (generative)  `(P1 — the big unlock)`
Runtime, data-free understanding of *where the road is* and *what biome/region*
you're in — works in any worldspace/mod.
- Sample **land textures** (and/or navmesh/roadmap) along the route to find road
  tiles and approximate the road's **direction**; steer arrow + spawns along it.
- Derive **region/biome** (snow / forest / reach / tundra / volcanic) from land
  texture + weather + worldspace for content gating.
- **Mid-route placement:** drop the player (or just the event) at a real *road*
  point, not the destination → fixes building spawns, makes on-road events sensible,
  lets the arrow follow the road.
- Deliverable: `DetectRoad(pos, dir)` + `DetectRegion(pos)`; mid-route resolve path.

### System D — Encounter Director  `(P1/P2)`
Composition intelligence on top of the event/group system.
- **Region/time/weather gating:** Forsworn in the Reach, vampires/undead at night,
  Falmer only near Dwemer ruins, draugr near barrows, ice wraiths in blizzards.
- **Leader + minions**, level-scaled rosters, **anti-repeat** cooldown.
- **Player-state reactivity:** bounty → hunters; vampire/werewolf → reactions.
- More groups (vanilla + composed-from-vanilla), per-category MCM frequency.

### System E — Carriage System  `(P2)`
The full carriage vision, built on carriage integration (ESP + scripts).
- Route real carriage rides through the journey (cart speed, cart events).
- Carriage-stopped events: driver may **ditch you** in danger; the carriage
  **spawns near you** where there's space; **defend the driver + horse or they die**
  → can't **re-board** to continue/redirect; if they live, re-board to go on.

### System F — Presentation & Tuning  `(P2/P3)`
- **Load-in hold** (reuse Loading Percent tech): don't reveal/unpause the
  destination until spawns are placed, so you load into a populated scene.
- Arrow polish (follow road via System C), richer travel overlay.
- Full **MCM suite** (frequency per category, region/time toggles, rates, camera).

---

## 3. Phased roadmap

**Phase 1 — Make what exists solid (P0)**
1. Spawn Engine (System A): grounding-by-bounds, clearance/water/building checks,
   reliable corpse spawn.
2. Scene Composer v1 (System B): fix camp (spit+rack, spacing, shadow), a real
   road obstruction.
3. Verify each event actually spawns (logging already in place).

**Phase 2 — Put events on the road (P1)**
4. Road & Region Detection (System C): road direction + region.
5. Mid-route placement: resolve events on the road, not the destination
   (kills the building-spawn problem; enables road-following arrow).

**Phase 3 — Depth (P1/P2)**
6. Encounter Director (System D): region/time gating, anti-repeat, leader+minions.
7. Camp/corpse depth: living non-hostile NPC at camps (sit AI); key-in-a-corpse
   that unlocks a nearby locked safe; survivor variants; region loot.

**Phase 4 — Carriage & polish (P2/P3)**
8. Carriage System (System E) incl. the defend/ditch/re-board vision.
9. Load-in hold, arrow-follows-road, full MCM suite (System F).

---

## 4. Backlog (specific asks, mapped to systems)

| Item | System | Notes |
|---|---|---|
| Corpse not spawning | A | Regression from Disable/Enable; fix reliably |
| Floating cooking pot (no rack) | A+B | Ground by bounds + use a spit-with-rack form |
| Bedroll too close to fire / shadow / on fauna | A+B | Min-spacing + clearance + proper grounding |
| Weak road block (loose cart) | B | Overturned cart + fallen tree/boulder spanning road |
| Enemies inside buildings | A+C | Building check now; real fix = mid-route |
| Snowy debris outside snow | C | Region-appropriate variants |
| Falmer/draugr on roads | D | Region-gate (ruins/barrows) to re-enable |
| Living NPC at camp | B+D | Non-hostile actor + sit package |
| Loot: key → nearby locked safe | B | Spawn corpse-with-key + locked container |
| Ambush after lingering by a body | ✓ | Done (7s / 700u proximity → 40%) |
| More combat groups | D | 7 now; add + compose-from-vanilla; region weight |
| Arrow follows real roads | C | Generative road direction |
| Load into populated scene | F | Loading-hold gating |
| Carriage stop / ditch / defend / re-board | E | Needs ESP foundation |
| MCM per-category frequency + region/time | F | |

---

## 5. Current settings & known limits
- INI `[Encounters]`: `fBaseChance=100` (TESTING), weights Combat 50 / Camp 15 /
  Corpse 15 / Debris 20 / Beggar 0 (deferred).
- Events resolve **at the destination** (Phase 2 moves them to the road).
- Spawned dressing is transient (deleted on next event).
- Deploy: builds copy the DLL to game `Data` + Vortex staging; **must copy after
  the game closes** (a running game locks the DLL).
