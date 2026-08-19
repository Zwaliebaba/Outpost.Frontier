# ADR-016 — Procedural Universe, Anchors, and Warp

**Status:** Accepted · 2026-08-19 (owner design session) · amended by
[ADR-017](ADR-017-station-docking.md) (2026-08-19): station anchors gain an undock point
and facing (§3); the transfer bus lands with the station phase, before U3a (§4);
`StationRoster` becomes the summary family's first resident (§6); presence includes docked
ships (§7) · further amended by [ADR-018](ADR-018-scaling-baseline.md) (2026-08-19): the
target is an MMO shard and a topology ADR blocks U2 (D1); §3 arrival contention answered by
deterministic per-order offsets (D18); §4 gains registry-owned ship-id allocation (D6a),
the `(applyTick, transferId)` total order (D17), the empty-world quiescence invariant and
hash-domain rule (D8), and the world-isolation invariant (D1a); §7's presence edges defined
and marked revocable (D16); per-commander event record joins the universe layer (D19)
· §4 further amended by [ADR-019](ADR-019-shard-topology.md) (2026-08-19): the registry
becomes host-aware (`HostForAnchor`) and **the world's tick becomes shard-global** rather
than a per-world counter; §5's transit durations gain a `TRANSFER_FLOOR_TICKS` floor
**Depends on:** ADR-001 (plane, grids), ADR-002 (tick), ADR-004 (wire), ADR-005 (orders,
determinism), ADR-009 (universe model), ADR-012 (JSON), ADR-014 (seam), ADR-015 (contact)
**Supersedes:** the corpus scale figure F13 (~300 systems across ~6 regions → **2,500 across
~50**); ADR-009 §9's "no gates traversal in MVP" fence (the MVP is over); ADR-009 §6's
"closed at eleven" hull roster (**`Gate` is appended as the twelfth value**); ADR-001 §3's
"one play area per session" (a session now hosts many grids; the client views one at a time).
**Delivery plan:** [Universe-Build-Order.md](../Universe-Build-Order.md) (U1–U6, milestones
W0–W2).

## Context

Owner directive at the close of the MVP: the universe is **procedurally generated, 2,500
solar systems to start**; every system has a name, a sun, multiple planets, and a station
close to one or two of them; fleets **warp** between systems and, within a system, between
planets; mining and PVE locations join the warp destinations later; the UI is part of the
design, not an afterthought.

Most of the ground was prepared. ADR-009 built the universe model *for* this: the int64-metre
plane, `GridAnchor`, exact anchor↔local conversion, systems/celestials/stations/gates parsed
and content-hash-guarded, and the sentence this ADR now cashes in — *"warp/gate arrival later
= anchor swap + local re-origin — exact and deterministic by construction."* The corpus fixed
more than the code did: `strategic-map.png` designs the whole universe-map screen, names the
generation pipeline ("**the bake**"), states its layout requirement (clustered constellations
with clear gaps), fixes the naming scheme (proper names for regions and constellations,
`ROOT-N` for systems), and confronts the one execution question this ADR must answer (§8).
`session-surfaces.png` requires authored starter systems. What no document had decided:
how 2,500 systems get made, what a warp *is* mechanically, how one session simulates many
grids without giving up the replay contract, and where the player's eyes go when their fleets
are in three places at once.

Decisions taken in the session, recorded here with their reasons: **timed warp** (not
instant); **fleets split across systems from day one** (the server ticks every grid with
presence); **the client feeds routes one jump at a time** (the map print's own lean); **~50
regions of ~50 systems** (the region screen keeps working as printed); **no avatar ship**
(the commander stays disembodied); **anchors, never coordinates, as warp destinations**.

## Decision

### 1. Scale, organisation, names, security

2,500 systems in **~50 regions of ~50 systems**, each region **~4 constellations** laid out
as clusters with clear gaps — the strategic map's stated legibility requirement, so a whole
region stays one legible screen exactly as printed (F13's ~300/~6 is superseded; the map's
*design* is not). Constellations remain UI grouping with zero mechanics (ADR-009 §4).
Naming follows the corpus: regions and constellations take proper names from curated root
lists; systems are **`ROOT-N`** of their constellation (`VEI-4`, `TALIS-7`) — the print warns
that long proper names break its label strategy, so "every system has a name" is satisfied
the way the map can draw. Security is bake-authored content: a band per region, a value per
system (the route planner and the character-create screen both already display exactly this),
and **~3 designated starter systems** close the "starter systems are unauthored" gap.

### 2. The bake

Generation is **offline, and the artifact is the truth**. `GenerateUniverse(seed, config)` is
a pure GameLogic function — integer arithmetic and NeuronCore's PCG32 only, no floats in
universe placement (ADR-009's rule holds through generation) — invoked by a **bake mode of
the executable** (ADR-012 machinery: a config directory selects it; the JSON writer emits the
canonical file). The output is **committed** to `GameData/Universe/`, hash-guarded end to end
exactly as authored content is today, and remains hand-editable: the generator supports
**curated inserts**, and the start system stays hand-authored (Vesta-3 keeps its polish, the
generated galaxy grows around it).

Why baked and not seeded at runtime: the committed file is reviewable, diffable, curatable,
and covered by the existing fail-closed `universeHash` — while a runtime generator would make
"the universe" a property of generator version and floating toolchains, exactly the class of
cross-build promise ADR-005 §6 refuses to make. The corpus already used the word "bake";
this ADR agrees with it.

Bake-time validation is part of the bake, not a hope: gate graph connected, gate pairs
symmetric, ids and names unique, every station orbiting a planet, 1–2 stations per system,
security in range, cluster separation (the map's requirement made measurable), starter
systems valid, every anchor's warp-in point inside its grid bound. A `GameLogicTests` suite
holds all of it against the committed file. Scale costs are measured, not guessed: ~2,500
systems ≈ 3–5 MB of JSON parsed at boot by both halves; U1's acceptance records the number,
and per-region files are the reserved fallback if it disappoints.

### 3. Anchors — warp destinations are authored, never coordinates

A **grid anchor** becomes a first-class authored record:
`{ kind : Station | Planet | Gate | Site(reserved), owner id, grid origin : UniversePos,
warp-in point : local offset, warp-in facing }`. Ships warp **to anchors only** — never to
arbitrary space. That single rule bounds the number of grids a system can ever host, makes
"where can I go?" a finite, pickable list for the UI, and gives future content its extension
point: mining fields and PVE encounters are new `Site` anchor rows, zero new architecture.

Placement rules: a station's grid **doubles as its planet's primary anchor** — "warp to
Kessler" and "warp to the Anchorage" land on the same grid, warp-in at a few kilometres'
standoff from the structure, so the busy place stays one place. A planet without a station
gets a bare anchor of its own (empty space for now — ADR-009 §9a's "celestials are data, not
geometry" stands, and the cost is re-stated below). A gate's warp-in point sits inside its
own jump radius, so route hops chain without a crawl. Arrival is **never random**: the
formation solve centres on the warp-in point with the authored facing, and ADR-015's
separation guarantees clean water even when two fleets arrive the same tick.

### 4. Many worlds, one universe runtime

A session hosts **many concurrent tactical grids** — one `World` per anchor with presence —
under a **universe runtime** that owns the registry: spin a world up when ships (or a viewer,
§7) arrive, spawn its authored occupants (station, gate), tear it down when the last ship
leaves and nobody is watching. `World` itself is untouched: per-grid determinism, orders,
formations, contact — everything ADR-005 and ADR-015 built carries over verbatim, and each
world seeds its PCG32 from (session seed, anchor id) so teardown and recreation reproduce
bit-identically.

Ships move between worlds on the **transfer bus**: a warp departure removes the ships from
their world and files a tick-stamped transit record at the universe layer; arrivals apply
**between ticks**, in a fixed order — (arrival tick, then order id) — which keeps the CI
guard "no `UniversePos` in per-tick code" true by construction. The replay contract extends
accordingly: a session replay is the per-grid order logs **plus the transfer log**, and the
double-run suite covers both. In-warp fleets have no positions and are not simulated — the
map animates them along their route line, presentation only, which is the "distances between
systems are map fiction" rule applied to time as well as space.

### 5. The warp order

Warp is an order like any other: **`OrderKind::Warp`**, appended after `Abilities` (the enum
grows by append, value 4 — a schema bump the fail-closed hash turns into an `UpdateRequired`,
never a silent skew). One group, the existing 64-ship cap, validated by **the same function
on both halves** — the client pre-checks, the server decides, bounce parity holds (ADR-005
§4). New refusal reasons arrive with it (`UnknownAnchor`, and `NotAtGate` for jumps).

Three phases, all in ticks, because the tick is the only clock:

- **Spool** — a per-class charge time (capitals slower; two new class-table columns:
  `warpSpeedMetresPerSec`, `spoolSeconds`). Cancellable: a replacing order during spool
  simply wins.
- **Transit** — committed. In-system: `base + universe distance / warp speed`, governed by
  the **slowest member** (the same philosophy as `GroupTravelSeconds`: the arrival that
  matters is the last one — a fleet warps together and arrives together). Gate jumps: a
  fixed short duration, because inter-system distance is map fiction (ADR-009 §3) and must
  not price travel.
- **Arrival** — formation solve at the warp-in point, authored facing, ADR-015 separation as
  the floor. The replicated `etaSeconds` that already exists on order records now also
  reports warp, so the HUD's number is the authority's own.

**Gate jumps** additionally require every member within a jump radius of the gate structure
on the gate's own grid — warp-in lands inside that radius, so the common case chains
seamlessly, and the gate keeps a physical presence that later gameplay (camping, interdiction)
can hang off. The timed model is not negotiable ornament: the route planner print prices an
11-jump route at 4m 10s, ~23 seconds a hop, and this ADR is what makes that arithmetic true.

### 6. The wire

Snapshots become **per-grid**: the header carries the grid's identity, and a client is
subscribed to exactly one grid's stream — its **view** (§7). Three additions, all under the
existing schema hash: a **view request** (client → server: point my view at this grid of
mine), a **grid-switch notice** (server → client: your view now streams grid X, with its
anchor — the `Welcome` fields, generalised to mid-session), and **fleet summaries** — a
low-rate (~1 Hz) record per fleet the player owns: where (anchor or transit), how many ships,
what state, `etaSeconds`. Summaries are what the roster and the maps read for everything the
player is *not* watching. ADR-004's budgets are unchanged and per-grid: the viewed grid's
snapshot obeys the same datagram cap as today, and summaries are a few dozen bytes per fleet.
The snapshot's grid identity is also the smear guard: a late datagram from the previous view
names the wrong grid and is dropped, never blended.

### 7. The player, fleets, and focus

The player remains what the corpus made them: a **disembodied commander** (F3 — one
character; the reconnect print's "station commander held your standing orders"). **No avatar
or active ship is introduced**, and none is needed: presence in a system *is* the player's
ships there, and separately the player's **view** is a subscription to one grid.

A **fleet is emergent, not authored state**: your ships sharing a location (a grid, or one
transit record). Derived on both halves from replicated state, so there is nothing new to
desync; wings (TALON, ANVIL, …) stay what they are — grouping and naming *within* a location.
The roster becomes location-grouped blocks and is the primary focus switch; the full set:

1. **Roster click** — jump the view to that fleet's grid, camera centred on it.
2. **Fleet cycling** on a key (bindings live in the settings screen).
3. **The maps** — fleet markers and systems-with-presence carry a VIEW action.
4. **Toasts** — warp arrivals feed the alerts rail (`alerts-and-toasts.png`), and the toast
   is clickable.

Rules: the view may point at **any grid where the player has ships** — and nowhere else.
Seeing without presence is the intel overlay's territory (`strategic-map.png`'s INTEL PINGS:
"the shape of your ignorance"), deliberately not given away for free now. Within a grid the
camera is exactly as free as today. **Auto-follow**: a watched fleet that warps takes the
view with it — the map is the between-surface, tactical resumes on arrival — unless the
camera is pinned. A grid stays alive while it has ships *or a viewer*, so a world is never
torn down under the player's camera. A view switch costs one interpolation-buffer refill
(~100 ms, ADR-002), so the transition is a designed ~200 ms settle rather than a pretended
instant with one janky frame. And **focus never gates command**: queued orders run
everywhere, selection and input apply to the viewed grid, and the client's route feeding
(§8) continues for fleets nobody is watching.

### 8. Route execution — the client feeds

The strategic map print poses this as its one real open question: the order queue holds four
legs and a route can be fourteen jumps. Decided: **the map plans, the client feeds** — the
route lives client-side (Dijkstra over the gate graph; fastest first, safest and
avoid-hostile after; the avoid-list persists in the user settings layer, ADR-012), and the
client submits **one warp/jump order per completed hop** into the existing queue. No schema
change, no server-side planner to keep in agreement with the client's. The named, accepted
cost, exactly as the print prices it: a disconnected player's fleet halts at the next gate —
the future AI-commander feature (the reconnect print already assumes one) is where that gap
closes, not here.

### 9. UI surfaces

- **Strategic map** — built to `strategic-map.png`, first slice deliberately a subset:
  region / constellation / system pinch levels, gate links, `ROOT-N` labels, the **security**
  overlay (its content exists from the bake) with the region band badge, search, the
  selected-system panel, fleet markers from summaries, route line with SET DESTINATION /
  ADD WAYPOINT, VIEW on systems with presence, and the TACTICAL ⇄ MAP handoff. Sovereignty,
  activity heat, intel pings and the history scrubber remain **stubs** — their content does
  not exist yet, which the print itself anticipates ("depends on content that does not
  exist").
- **System view** — the screen the corpus names (pinch level: SYSTEM) **but never drew**;
  it needs its own print before its slice builds (a named deliverable in the build order).
  Contents: the sun, orbit rings at **presentation scale** (real orbital distances are not
  linearly renderable — "layout is legibility" applies inside a system too), anchor icons
  (planets, stations, gates), fleet markers, in-warp fleets sliding along route lines. This
  is where "warp to that planet" is clicked, through the existing grammar: pending ghost,
  ETA label, bounce on refusal — the whole S9 pipeline, reused.
- **Tactical HUD** — roster location blocks with IN WARP / off-grid states and ETAs; a ~1 s
  departure/arrival treatment (no new render pass — the Ui/overlay vocabulary suffices);
  warp ghosts and refusal toasts through the existing surfaces.

### 10. The gate hull

Gates on their grids follow the one pattern the tree has for static presence: **a ship-table
entry that never moves** — stations proved it ("one path instead of two" is written where
`Structure` spawns). So **`HullClass::Gate` joins as value 11**, the twelfth entry. ADR-009
§6's closure exists so wire values, icons and palettes never *renumber* — appending renumbers
nothing, and that clause is amended, not violated. The class takes a contact radius in
ADR-015's table, a STATIC-family icon, zero speed, and eventually **`Gate.obj`** — a
radially symmetric ring/portal in the shared five-material palette. Until the art lands the
Structure mesh stands in: the Fighter/Cruiser precedent, run in reverse — a named content
gap, not a design gap.

### What this deliberately does not do, so nobody mistakes it for covered

- **No grid persistence.** A torn-down world forgets everything but its authored occupants.
  The policy question (wrecks, mined-out fields) arrives with the content that needs it —
  `Site` anchors — and is named here so it is a decision then, not an accident.
- **No intel / fog of war.** Presence-gated viewing is the placeholder posture; the INTEL
  PINGS overlay owns the real design later.
- **No combat interaction with warp.** Interdiction, scramble, in-warp damage — all waiting
  on combat existing at all. The spool/transit split was shaped so a hold-down mechanic has
  somewhere to attach.
- **Celestials stay undrawn** (ADR-009 §9a stands). A bare planet anchor is visually empty
  space, and warping there will feel like it. Accepted, with the same eyes-open note as 9a:
  the authored content already contains the hard case when someone reopens rendering.
- **No AI commander.** The disconnect-halts-at-gate cost stands until that feature exists.

## Alternatives rejected

- **Runtime generation from a seed** — makes the universe a property of generator version and
  toolchain, weakens the hash guard to a promise, and produces content nobody can review or
  curate. The bake keeps every existing guarantee. Rejected.
- **One grid per solar system** — a system is hundreds of millions of kilometres wide;
  float32 at metre resolution dies six orders of magnitude earlier. This is precisely the
  precision layering ADR-009 §2 exists for. Rejected without regret.
- **Free-space warp (arbitrary coordinates)** — unbounded grid population, an unpickable
  "where to?" UI, and it forfeits the anchor list as the content extension point. Rejected.
- **Server-owned route execution** — survives disconnects, but widens the order schema and
  replication, and demands a server-side planner kept in perfect agreement with the client's
  presentation of it. The print leaned client-feeds; so does the architecture. Rejected,
  revisitable when the AI commander lands.
- **An avatar / active ship** — re-opens "what happens to *you* when that hull dies
  mid-warp", contradicts F3's commander identity, and buys nothing command needs. A
  *flagship designation* remains available later as gameplay, not attachment. Rejected.
- **Instant warp with cooldown** — cheaper, but it deletes the travel weight the route
  planner's own arithmetic promises, and it leaves nothing for interdiction to hold onto.
  Rejected.

## Consequences

- The delivery plan is **[Universe-Build-Order.md](../Universe-Build-Order.md)**: U1–U6 with
  milestones W0 (first warp), W1 (first crossing), W2 (the universe on screen). U5 depends
  only on U1 and runs in parallel.
- **Schema bumps, enumerated once** so they cluster per slice instead of dribbling:
  `OrderKind` +Warp; order-state values for spool/transit; per-grid snapshot header; view
  request / grid-switch / fleet-summary messages; `hull{11→12 classes}`. Every one rides the
  existing fail-closed hash.
- The **replay contract grows the transfer log**; the harness and `selfTest` extend with it
  (Risk R16). Two new class-table columns (warp speed, spool) join the envelope suite.
- The **content pipeline joins the tree**: bake mode, curated inserts, invariants suite, and
  a committed multi-megabyte content file whose parse time is measured at U1 (Risk R17).
- **Named content deliverables**: the system-view print (blocks U6), `Gate.obj` + icons
  (stand-in until landed), curated name-root lists (part of U1).
- ADR-001, ADR-009 and the corpus's F13 carry amendment notes pointing here; the README's
  supersession list grows one line. `Ids.h`'s scale comment is corrected with the code that
  lands U1.
