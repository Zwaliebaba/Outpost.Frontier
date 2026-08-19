# ADR-009 — Universe Model: int64×int64 Plane, Systems, Celestials, Stations

**Status:** Accepted · 2026-08-17 (session follow-up, owner directive) · extended and partly
superseded by [ADR-016](ADR-016-procedural-universe-and-warp.md) (2026-08-19): the scale
figure grows (~300 systems → 2,500 across ~50 regions), §9's no-traversal fence expires,
§6's hull roster gains an appended `Gate` class, and grid anchors become authored warp
destinations · [ADR-017](ADR-017-station-docking.md) (2026-08-19) expires the docking half
of §9's fence — stations dock, hold, and relaunch fleets now
**Depends on:** ADR-001 (planar sim), ADR-004 (wire), ADR-005 (GameLogic structure)
**Feeds:** Build Order S5/S6 content, strategic map (post-MVP), Dependency Map (GameLogic)

## Context

Owner directive: the universe is an **int64 × int64** coordinate setup with room for future
growth; it consists of **solar systems** containing **planets** and **1–2 stations** each; the
meshes under `GameData/Meshes` are the **standard ships in use** (with `Structure` serving as
the station mesh). The corpus already fixes the surrounding shape: the client holds a
*complete universe copy* (F15), launch is ~300 systems across ~6 regions (F13), the hierarchy
is region ⊃ constellation ⊃ system with **constellation as UI grouping only**, systems are
graph nodes joined by **gates**, and *"constellation layout is a legibility problem, not a
physics one"* (`strategic-map.png`). ADR-001 promised "many grids, each with a local origin"
without saying what anchors them. This ADR says it.

## Decision

### Coordinates
1. **One universe plane, `UniversePos { int64 x, int64 y }`, unit = 1 metre.** The universe is
   the same 2D plane as the sim (ADR-001), addressed globally in integer metres. Range
   ±9.2 × 10¹⁸ m ≈ ±975 light-years — room for millions of systems; growth is content, never
   a coordinate migration. Integer coordinates are exact, hashable, and deterministic —
   no float drift in world placement, ever.
   *Unit trade recorded:* cm resolution was rejected (caps the plane at ±0.097 ly — roomy for
   one system, not a universe); metres match the sim's local unit, and nothing at universe
   scale needs sub-metre — sub-metre lives in the grids.
2. **Precision layering (normative).**
   - Universe level: every persistent placement — system centres, star, planets, stations,
     gates, grid anchors — is a `UniversePos`.
   - Tactical level: simulation runs in **local float32 metres** on a grid whose origin is a
     `UniversePos` anchor (station, site, or fleet position); grids obey ADR-001's ~40 km
     bound so float32 keeps ~mm precision; the wire stays cm-quantised local (ADR-004).
   - Exact absolute position = `anchor + quantised local` — reconstructible without loss.
   - **Long-range rule:** never subtract distant `UniversePos` values expecting sub-metre
     meaning. Relative math is done between *nearby* anchors (any in-system pair fits int64
     centimetres with 10 orders of magnitude to spare); distances between systems are map
     data, not navigation input.
3. **Inter-system space is not flown.** Systems are graph nodes; travel between them is gate
   traversal (post-MVP). Distances *between* systems on the plane are map fiction laid out
   for legibility (corpus rule); positions *within* a system are gameplay-real.

### Structure & identity
4. `Universe ⊃ Region ⊃ [Constellation — label only, zero mechanics] ⊃ SolarSystem`.
   A `SolarSystem` owns: one star, 0..N planets (celestials: landmarks now, economy anchors
   later), **1–2 stations**, 0..N gates (edges to other systems), and any active tactical
   grids. Stations use the `Structure` hull class/mesh — static and radially symmetric, per
   the icon sheet's STATIC glyph rules.
5. **Ids are data-stable:** `SystemId u16` (300 at launch — **2,500 as of ADR-016**, still
   26× headroom), `CelestialId u16`, `StationId u16`, `GateId u16` — assigned in the universe
   definition, never at runtime; they are the wire and save identifiers.
6. **Ship roster ruling (supersedes the README "content gap" observation):** the standard
   ship set *is* the `GameData/Meshes` content — Interceptor, Corvette, Frigate, Bomber,
   Miner, Hauler, Carrier, Battleship, plus Structure for stations. The `HullClass` enum
   keeps the icon sheet's 11-value closed taxonomy so wire, icons, and palettes never
   renumber: **Fighter and Cruiser remain reserved ids, defined but unused until content
   exists.**
   *Amended by ADR-016 §10 (2026-08-19):* the taxonomy grows by **append**, never by
   insertion — `Gate = 11` joins as the twelfth value for the gate structure on gate grids,
   with the Structure mesh standing in until `Gate.obj` lands. Appending renumbers nothing,
   which is the property this clause was protecting; Fighter and Cruiser stay reserved.

### Data & loading
7. **`GameData/Universe/`** holds the authored universe definition. *(Format amended by
   ADR-012: **JSON**, parsed by NeuronCore's custom parser — one format and one parser for
   config, universe, and sound banks. The parser keeps integral tokens as exact `int64`,
   which is what makes `UniversePos` authorable at all.)* Parsing is a **pure GameLogic
   function (bytes → `UniverseDef`)**; file IO stays in the hosts, so GameLogic stays OS-free
   (ADR-005 discipline).
8. **Both halves load the identical definition** (F15: the client owns a full copy).
   A `universeHash` (FNV-1a over the canonicalised *parsed content* — so comments, whitespace
   and key order never affect it, ADR-012 §D) travels in `Hello`/`Welcome`
   beside the schema hash and **fails closed** on mismatch — same posture, same
   `UpdateRequired` path (ADR-004). `Welcome.worldMeta` is now concrete:
   `{ systemId, gridAnchor : UniversePos, universeHash }`.

   **As built (S5b), with two deliberate departures:**
   - **The hash is not carried twice.** `Hello`/`Welcome` already had a `contentHash` field,
     and it is the field the handshake refuses on. The universe hash *is* that value, rather
     than a second copy inside `worldMeta`: two fields carrying the same number are two fields
     that can disagree, and the one that decides whether to refuse would win silently.
   - **The remaining two are named in engine terms** — `worldId`, `anchorX`, `anchorY` — because
     they live in `NeuronCore/Wire.h`, which must stay plausible in an unrelated networked sim
     (Dependency Map ruling 4). "Which world, and where is its origin" is generic; "which solar
     system" is not. The mapping is one-to-one: `worldId` is the `SystemId`, and the anchor is
     the `GridAnchor`'s `UniversePos`. The engine carries them and never reads them, which is
     ADR-014's rule holding at the one place it was most tempting to bend.

   `worldMeta` earns its place in the wire now rather than at S5c because `mode: "client"`
   already exists: a client in another process shares no configuration with the server, so
   without the anchor it cannot place a single replicated position.
9. **MVP content:** one authored system — **Vesta-3** (the system on the prints): star, two
   planets, one station. The MVP tactical grid is anchored at the station; the fleet flies
   there; the station (Structure mesh) is on-grid scenery/landmark. No docking, no gates
   traversal, no second system in MVP — but the MVP boots *from the universe definition*, not
   from a hardcoded scene, so "more universe" is authoring, not engineering.
   *(The no-traversal fence expired with the MVP: ADR-016 lands gate traversal, in-system
   warp between authored anchors, and the 2,500-system bake — with Vesta-3 kept as the
   hand-authored start the generated galaxy grows around.)*

9a. **Celestials are data, not geometry** (owner decision, 2026-08-18, replacing this clause's
   original promise that they "render as distant backdrop").

   Nothing in the corpus draws a celestial body. `tactical-hud.png` is near-black space, an
   ambient haze behind the fleet and sparse star points — no star, no planet, no limb.
   `strategic-map.png` draws systems as graph nodes and reports "STATIONS 2 · GATES 4" as
   panel text. Neither screen has a celestial in it, and S5's acceptance was measured against
   the first of them, so the prints are the governing artefact and this ADR's clause was the
   outlier. It was written here before rendering had an owner for the claim.

   So a `Celestial` earns its place in `UniverseDef` as **content the game reads, not content
   the frame draws**: system identity and layout, landmarks for naming and orientation,
   economy anchors later, and real coordinates for the strategic map to place a node from.
   Parsing them, hashing them, and having both halves load the identical definition is the
   requirement (§8 — they are never replicated; each side reads the file and the hash refuses a
   mismatch at the door). Drawing them is not the requirement, and no slice owes it.

   *Do not re-derive this from ADR-006's pass list.* Its reserved `Nebula` node is an ambient
   haze composited **after** `Opaque` and before `Tonemap` — the green field the tactical print
   shows behind the fleet, which is why `overlay-pass.png` §1 worries about "HDR drift between
   a bright nebula and empty space". It is not a celestial renderer and was never a scheduled
   home for one; reading it as one is the mistake this paragraph exists to stop.

   **What this costs, stated so it is a decision rather than an omission:** if celestials are
   ever drawn, the authored content already contains the hard case. The Anchorage holds a
   2,000 km orbit over Kessler, so the planet is 8,051 km away and 6,051 km in radius — about
   a hundred degrees of sky — while Vesta (107 million km) and Halgren (234 million km) are
   points. A backdrop built only for points would be built to the wrong requirement. The orbit
   stays: it is better content than a station anchored to nothing, and it keeps that
   requirement visible in the data for whoever reopens this.

## Alternatives rejected

- **float64 universe coordinates** — no exact equality, drift under accumulation, hashes and
  replication get fuzzy edges. Integers are the only defensible currency for persistent
  placement. Rejected.
- **Per-system coordinate spaces with no global frame** (address = `{systemId, local}`) —
  simpler per system, but the strategic map, future seamless-adjacent-grid tricks, and any
  cross-system reasoning then need a second, invented geometry; the owner directive asks for
  the global frame. Rejected.
- **Hierarchical chunk/sector addressing** (sector ids + offsets) — machinery to dodge a
  problem int64 metres doesn't have at our scale. Rejected.
- **Physically scaled universe distances** (real light-years between systems) — overflows any
  fixed-point plane and buys nothing: travel is graph-based and the corpus already declares
  layout a legibility concern. Rejected.

## Consequences

- ADR-001's growth story is now concrete: grids anchored at `UniversePos`, warp/gate arrival
  later = anchor swap + local re-origin — exact and deterministic by construction.
- The strategic map (post-MVP) reads real coordinates from the same `UniverseDef` the server
  simulates against; no second world description ever exists.
- Universe content is hand-editable JSON under `GameData/`, hash-guarded end to end; a
  content mismatch is caught at the door, not mid-session.
- `GameLogicTests` gains: parser round-trip/rejection suites, universeHash stability, and an
  anchor+local reconstruction property test (no loss across quantisation).
- Wire impact is confined to `Hello`/`Welcome` (`universeHash`, `worldMeta`) — snapshots and
  orders are untouched; ADR-004's budgets stand.
