# Universe Build Order — Post-MVP Phase One

**Status:** Session output 2026-08-19 · **no slice started.** The design it delivers is
[ADR-016](ADR/ADR-016-procedural-universe-and-warp.md); where this document and that one
disagree, the ADR wins on *what* and this one on *when*.

**Interleave (owner sequencing, 2026-08-19):** the station phase
([Station-Build-Order.md](Station-Build-Order.md), ADR-017) runs **after U2 and before
U3a**. It introduces the transfer bus and `World`'s transfer seam, so U3a inherits both
rather than building them; U1's anchor table carries ADR-017's undock fields from the
first bake (see U1's acceptance).

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
the dock radius, undock point clear of the structure's contact radius, parking rings inside
the grid) is green against the *committed* file;
parse + `universeHash` of that file **measured and the number recorded here** (per-region
split is the reserved fallback if it exceeds ~1 s Debug); headless boot from it; `Ids.h`'s
scale comment corrected.

### U2 — Anchors and the world registry
The universe runtime in GameLogic: a registry of `World`s keyed by anchor — spin-up spawns
the anchor's authored occupants, teardown when the last ship leaves and nobody views it
(the viewer half of that rule lands with U3b; U2 exposes the hold). Per-world PCG32 seeded
from (session seed, anchor id). The exe's `Simulation` implementation hosts the registry
while the wire still serves exactly the start grid — **no visible change**.
**Accept:** a session boots into the start anchor exactly as today; registry double-run
determinism suite green, including a teardown/recreate cycle reproducing spawned state
bit-exactly; the "no `UniversePos` in per-tick code" CI guard extended to the new files.

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
motion in under half a second; a watched fleet warps and the view follows it to arrival; the
unwatched fleet's roster block tracks its summary; `selfTest` drives a headless warp over the
real loopback and observes the grid switch and the summaries; every pre-existing suite green.

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
