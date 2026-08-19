# Universe Build Order — Post-MVP Phase One

**Status:** Session output 2026-08-19 · **no slice started.** The design it delivers is
[ADR-016](ADR/ADR-016-procedural-universe-and-warp.md); where this document and that one
disagree, the ADR wins on *what* and this one on *when*.

**Interleave (owner sequencing, 2026-08-19):** the station phase
([Station-Build-Order.md](Station-Build-Order.md), ADR-017) runs **after U2 and before
U3a**. It introduces the transfer bus and `World`'s transfer seam, so U3a inherits both
rather than building them; U1's anchor table carries ADR-017's undock fields from the
first bake (see U1's acceptance).

**Scaling baseline (owner decisions, 2026-08-19):**
[ADR-018](ADR/ADR-018-scaling-baseline.md) rides this plan. Its action register (§A) adds
acceptance items to U1–U3b below, three design deliverables (D5–D7 in the list at the end),
and one new slice — **U3c, the second-commander gate**. The four actions that gated U1 and U2
are done: **A2** (CI is a Debug/Release matrix and spike 2 is a standing job), **A3** (the
shader build is dxc/SM 6.x in both configurations), **A1**
([ADR-019](ADR/ADR-019-shard-topology.md)), and **A4** — the soak, measured: a grid at the
1,024 cap ticks in **10.6 ms mean / 21.9 ms worst, 21 % of the budget**, so the tick has ~5×
headroom at the cap and **no broadphase is owed before U2**. **A4's in-repo soak landed the
same day** (`Outpost/TickSoak.h/.cpp`): the self test runs the 41 / 256 / 512 / 1,024 ladder
in the shipping binary on every push, CI tables it per configuration, and the capped-grid
figure D1c wants — how many grids fit one core — is printed beside it. So the number is now
re-taken rather than remembered — **and the first run closed A4's last owed item**, the
authoritative MSVC **Release** figure: a capped grid ticks in **7.728 ms mean / 13.6 ms worst,
15 % of the budget, ~6.5 grids per core**, better than the cross-build indicated. (Debug is
78.3 ms, 10.1× that, which is D11 in one number.) **U2 inherits the
instrument, not the chore:** the ladder measures one grid, and the question U2 makes
interesting is M grids — extending it is a line in the soak rather than a new harness.
**A24 also landed**, so the build invariants the determinism story cites are stated in two
property sheets and held by CI rather than by prose.

The rules are the MVP build order's, unchanged: each slice is independently testable, lands
green (`Tests/` + `selfTest` where applicable), is sized at "a few days" or less, and later
slices assume earlier ones. Landed slices will carry a **Built** line naming what is in the
tree and what is still owed — none exists yet, and this sentence is the reminder to add them.
Test placement follows the Dependency Map: bake and warp logic prove themselves in
`GameLogicTests`, wire changes in `NeuronCoreTests`/`NeuronServerTests`, map and view math in
`NeuronClientTests`, and anything needing the real loopback in `selfTest`.

Milestones: **W0** — *first warp* (a fleet warps between two anchors of one system,
replay-deterministic, watched end to end) · **W1** — *first crossing* (a routed multi-system
trip through gates) · **W2** — *the universe on screen* (plan on the map, watch the crossing,
command at both ends).

---

### U1 — The bake
`GenerateUniverse(seed, config)` in GameLogic — pure, integer-only, PCG32 the sole
randomness: region/constellation/system layout with clustered constellations and clear gaps
(the strategic map's stated requirement, made a measurable invariant); connected gate graph
with symmetric pairs (1–4 gates per system, ~2.4 average); curated-root naming (regions and
constellations proper, systems `ROOT-N`, unique); security bands per region + per-system
values; per system one star and 2–8 planets at real orbital scales; 1–2 stations, each
orbiting a planet at an Anchorage-style standoff; the **anchor table** (station / planet /
gate, each with grid origin, warp-in point, warp-in facing — ADR-016 §3 — and, for station
anchors, the undock point and undock facing, ~800 m off the structure facing outward —
ADR-017 §3); ~3 starter systems
designated. **Curated inserts**: Vesta-3 stays hand-authored as the start, the galaxy grows
around it. Bake mode in the exe (config-selected, ADR-012) writes the canonical JSON; the
2,500-system file is **committed** as the authored universe.
**Accept:** same seed + config ⇒ byte-identical file, run twice in CI; the invariants suite
(connectivity, symmetry, uniqueness, station-orbits-planet, security ranges, cluster
separation, starter validity, warp-in-inside-grid — plus ADR-017's: station warp-in inside
the dock radius **as amended by ADR-018 D7 (footprint-derived — the invariant holds for the
base radius; fleet-scaled radii are validation-time)**, undock point clear of the
structure's contact radius, parking rings inside the grid) is green against the *committed*
file; parse + `universeHash` of that file **measured and the number recorded here** (in
**Release**, per ADR-018 D11, with the Debug ratio noted; per-region split is the reserved
fallback if it exceeds ~1 s); headless boot from it; `Ids.h`'s scale comment corrected.
**ADR-018 additions (A5):** every authored occupant's **deterministic u32 ship id, derived
from its anchor** (D6a), is a bake output the invariants suite checks for uniqueness; the
anchor record carries what the **deterministic per-order arrival offset** rule needs (D18)
so contention never forces an anchor-schema migration.

### U2 — Anchors and the world registry
**Gate (ADR-018): both cleared — A1 is delivered
([ADR-019](ADR/ADR-019-shard-topology.md)) and A4's soak is measured** (a capped grid costs
~⅕ of a core, so the registry is not racing a broadphase); the registry is shaped
location-transparent by the first and budgeted by the second.
**ADR-019 §6 is part of this slice's acceptance**, and every item costs a shape rather than
a mechanism at `HostId = 0`: worlds addressed by `AnchorId` (no pointer held across a tick);
transfers naming a destination anchor and never touching the destination world;
`TransferId = (HostId, counter)` with the `(applyTick, hostId, counter)` total order;
`HostForAnchor()` existing and returning 0; **the shard tick stated as a contract** — the
registry drives every world with one number (`World::Tick(_tick)` already takes it), a world
spun up at shard tick *N* begins there, and the teardown/recreate hash comparison is made at
equal shard ticks rather than equal ages; player-scoped queries
going through the index rather than walking the registry; ship ids from a host-held block;
and no live grid migration.
The universe runtime in GameLogic: a registry of `World`s keyed by anchor — spin-up spawns
the anchor's authored occupants, teardown when the last ship leaves and nobody views it
(the viewer half of that rule lands with U3b; U2 exposes the hold). Per-world PCG32 seeded
from (session seed, anchor id). The registry **owns ship-id allocation** (ADR-018 D6a):
`World::Spawn` takes an injected id, authored occupants take their bake-derived ids, and
the registry keeps the session-wide ship→location index. The exe's `Simulation`
implementation hosts the registry while the wire still serves exactly the start grid —
**no visible change**.
**Accept:** a session boots into the start anchor exactly as today; registry double-run
determinism suite green, including a teardown/recreate cycle reproducing spawned state
bit-exactly **and bit-identical occupant ids**; the "no `UniversePos` in per-tick code" CI
guard extended to the new files. **ADR-018 additions (A6–A8):** the **empty-world
quiescence test** (a world holding only authored occupants ticked N times hashes
identically to its recreation) and the registry hash/replay domain excluding ship-less
viewer-held worlds (D8); the **world-isolation invariant** stated in the registry API
(worlds share no mutable state during `Tick`; the bus and extract are the only crossings)
with a **permuted-world-tick-order bit-identity test** holding it (D1a); ADR-007 §7's
owner-assert built and armed on every world.

### U3a — In-system warp (sim)
`OrderKind::Warp` (appended; schema bump) with an anchor-reference payload; validation shared
by both halves with the new reasons (`UnknownAnchor`; ordering added to the check-order
contract, ADR-005 §4a); class table gains `warpSpeedMetresPerSec` and `spoolSeconds` (shape
asserted by the envelope suite: capitals spool slower, warp slower). Spool (cancellable by
replacement) → committed transit via the **transfer bus** (tick-stamped records, applied
between ticks in (arrival tick, order id) order — **inherited from T1**, which lands it with
dock/undock records and the transfer seam on `World`; U3a adds transit records, not the
mechanism) → arrival by formation solve at the warp-in
point with ADR-015 separation. Transit duration: base + universe distance / slowest member's
warp speed. Replay harness and `WorldHash`-level suites extend to cover transfers.
**Accept:** `GameLogicTests`: double-run bit-identity across a scenario with concurrent
warps in both directions; spool cancellation; slowest-member timing; arrival lands every
ship inside tolerance of its station with no contact pair; validation-parity matrix over the
new reasons; `etaSeconds` on the order record tracks transit to zero.

### U3b — Warp on the wire and on screen
Per-grid snapshots (grid identity in the header — the smear guard), the view request,
the grid-switch notice, and **fleet summaries** (~1 Hz: anchor-or-transit, count, state,
`etaSeconds`) — one clustered schema bump with U3a's. Client: view subscription, auto-follow
on warp (v1: switch on arrival + roster chip; the map-as-between-surface completes in U6),
roster location blocks with IN WARP / off-grid states, warp ghosts and bounces through the
existing S9 pipeline, ~200 ms designed view-switch settle over the interpolation refill,
~1 s depart/arrive treatment in the existing overlay vocabulary.
**Accept 🏁 W0:** two fleets on different anchors; roster click switches view to smooth
motion in under half a second **at zero added RTT — the target is stated as RTT + settle
and re-run with an injected-delay shim on the loopback (ADR-018 A15)**; a watched fleet
warps and the view follows it to arrival; the unwatched fleet's roster block tracks its
summary; `selfTest` drives a headless warp over the real loopback and observes the grid
switch and the summaries; every pre-existing suite green.
**ADR-018 additions:** `SnapshotSender` is **per-client from its first line** (A13 — the
per-grid stream and per-player summaries are already per-client facts); the presence-edge
rules render (D16: presence lost under a pinned camera → the map; every fleet in transit →
the map); warp events emit into the **per-commander event record** (A17) and the alerts
taxonomy gains its universe rows with the toast **action payload** (A18); summaries and
view rights key on `PlayerId` (D5, minted in T2's cluster).

### U3c — The second-commander gate *(ADR-018 A25, new)*
Two real clients against one shard: distinct `PlayerId`s, each commanding its own fleets on
**disjoint grids** — the shared-grid case stays gated behind the interest/delta slice (D6
in the deliverables below), because two full fleets on one grid exceed the full-snapshot
cap by arithmetic. No new mechanism: this slice is the proof that U3b/T2's per-client
machinery, identity keying, and privacy rules actually hold with a second commander on the
wire.
**Accept:** `selfTest` (or a scripted twin-client run) drives both clients through the full
loop — handshake with distinct ids, orders attributed correctly, per-player summaries and
rosters private (client B never receives A's `StationRoster` or summaries), view rights
enforced (B cannot subscribe to A's grid), disconnect + reconnect resumes B's session under
the grace window with the fleet intact; every pre-existing suite green.

### U4 — Gates: the twelfth hull and the jump
`HullClass::Gate = 11` (append; `hull{}` schema bump; ADR-015 contact-radius row; STATIC
icon; Structure mesh stands in until `Gate.obj` lands — a named content gap). The bake's gate
anchors get their gate entity; jumping requires every member inside the jump radius of the
structure (`NotAtGate` otherwise, both halves); a jump is a fixed-duration transit on the
transfer bus to the paired gate's anchor. Client route planner v1 — Dijkstra over the gate
graph, fastest mode — **feeding one order per completed hop** (ADR-016 §8); route progress
surfaced on the HUD.
**Accept 🏁 W1:** a routed A→B→C crossing completes watched and unwatched; killing the
client mid-route halts the fleet at the next gate and reconnecting resumes the feed;
`NotAtGate` bounces with parity; the strategic-map print's §3 question is answered in the
tree and its OPEN note updated.

### U5 — Strategic map v1 *(depends only on U1 — runs in parallel with U2–U4)*
**Gate (ADR-018): D7 is delivered — [ADR-020](ADR/ADR-020-ui-architecture.md) — and A20's
instruments still owe their run** (spike 3, the S5 frame check), with the upload ring and
fixed GPU budgets re-sized from the corpus caps (1,024 entities / 2,500 nodes) so this slice
measures the map, not the MVP's constants. The screen is built as an **engine surface fed neutral
data** (ADR-018 D14): the baked topology crosses the seam once at boot as a neutral graph,
search and route-solve are GameLogic pure functions.
The screen from `strategic-map.png`, deliberately the subset whose content exists: region /
constellation / system pinch levels over the real bake, gate links, `ROOT-N` labels, security
overlay + region band badge, search, selected-system panel, fleet markers from summaries
(once U3b lands; markers degrade gracefully to "no data" before that), route line with SET
DESTINATION / ADD WAYPOINT driving the U4 planner, VIEW on systems with presence, TACTICAL ⇄
MAP navigation. Sovereignty, heat, intel and the history scrubber are visible stubs, exactly
as the print anticipates for content that does not exist.
**Accept:** visual checkpoint against the print at region level *over the real baked
content* — constellation hulls disjoint, labels legible, which is U1's clustering invariant
paying off on screen; a destination set on the map produces a real crossing; the full 2,500
render inside the frame budget with the `Ui` span proving it.

### U6 — System view and focus polish
**Prerequisite: the system-view print (D1) — designed and agreed before this slice builds.**
The screen: sun, orbit rings at presentation scale, anchor icons (planets, stations, gates),
fleet markers, in-warp fleets sliding along route lines (presentation-only interpolation).
Warp orders issued from it through the existing grammar (ghost, ETA, bounce). Focus polish:
clickable warp toasts on the alerts rail, fleet cycling on a key, camera pinning, and the
transit view completing auto-follow (the map as the between-surface, fleet highlighted).
**Accept 🏁 W2:** click a planet in the system view and the fleet warps there with a pending
ghost and honest ETA; a toast click jumps focus across systems; cycling visits every owned
fleet; the whole loop — plan on the map, watch the crossing, command at both ends — runs in
one sitting.

---

## Content & design deliverables (not slices — tracked so they cannot be quietly dropped)

- **D1 — System-view print.** The corpus names the pinch level and never drew it; U6 must not
  invent it ad hoc. Owner-reviewed like every other print.
- **D2 — `Gate.obj` + icons.** Ring/portal silhouette, radially symmetric, the shared
  five-material palette; STATIC-family tactical icon and map glyph. Structure stands in from
  U4 until this lands.
- **D3 — Name root lists.** The curated region/constellation vocabularies the bake draws
  from — content authoring inside U1, named here because curation is a task, not a fallout.
- **D4 — Warp audio cues.** Spool/depart/arrive; lands only after S15 gives audio a home in
  the bank format. Deliberately last.
- ~~**D5 — The topology ADR** *(ADR-018 A1 — blocks U2)*.~~ **Delivered 2026-08-19:
  [ADR-019](ADR/ADR-019-shard-topology.md).** Three roles (SimHost, SessionHost,
  Directory) in one process today; the anchor is the placement unit with region affinity
  and no live migration; transfers filed at departure with a
  `(applyTick, hostId, counter)` total order and a `TRANSFER_FLOOR_TICKS` floor on transit
  durations (which U3a and U4 must respect when they set spool, transit and jump times);
  one client connection through the session front door, so **the client wire does not
  change**. Its §6 is U2's acceptance.
- **D6 — The interest/delta ADR** *(ADR-018 A14 — drafted during the station phase; its
  implementation slice follows U3c and gates shared grids)*. Scope fixed by ADR-018 D4:
  snapshot-ack and baseline ownership, the keyframe/initial-sync path, `Simulation`'s
  relevance hook, the degradation rule, the interest guarantee (owned + selected never
  culled; unreplicated presence stated via counted chips), `lastOrderSeqProcessed` out of
  the world hash, `EntityRecord` → u32 id, the ownership field.
- ~~**D7 — The UI-architecture ADR** *(ADR-018 A19 — blocks U5 here and T3 in the station
  order)*.~~ **Delivered 2026-08-19: [ADR-020](ADR/ADR-020-ui-architecture.md).** A surface
  is a value on a small stack (pushing one already present pops back to it, so `◀ TACTICAL`
  and `◀ BACK` are one mechanism); a full-screen surface skips the three world passes and the
  resolve rather than adding a pass, which is where U5's node budget comes from; input is
  claimed once by one router across three independent channels, with the printable-key rule
  that makes "W" type *or* pan; the screen-data contract is three shapes, not three methods.
  A surface switch is **not** a view switch — the network half runs on every surface, so
  returning to tactical costs no interpolation refill.

## Sequencing rationale

- **U1 before everything**: every slice consumes the bake's output, and the map's legibility
  gate (U5) is really a property of U1's layout invariants — prove it where it is cheap.
- **U2 before U3**: the registry must exist before anything transfers between its entries;
  U2 lands invisible so its determinism suite is judged without wire noise.
- **U3a/U3b split** along the seam the repo already respects: sim truth first, then its
  replication and presentation — the same order S6/S7 used, for the same reason (replication
  bugs must never masquerade as sim bugs).
- **Gates after in-system warp**: a jump is a transit record with a precondition — it reuses
  everything U3a built and adds only the hull class and the radius rule.
- **U5 in parallel**: it reads committed content and stubs the rest, so it is the one slice
  that can overlap the sim track without either blocking the other.
- **The print before U6**: retrofitting a screen design after its screen exists is how the
  corpus stops being the governing artefact.
