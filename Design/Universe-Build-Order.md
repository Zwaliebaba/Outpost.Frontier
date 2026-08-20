# Universe Build Order — Post-MVP Phase One

**Status:** Session output 2026-08-19 · **U1, U2, U3a, U3b's sim and wire halves, U4's sim
half and U5's pure half built** (2026-08-20). What is left in this plan is, with one
exception, screen work: U3b's client half, U4's route feeder and icons, U5's map itself and U6
need a GPU and a person. The exception is **U3c**, and its blockers have since cleared: it
needed T2's per-client `SnapshotSender` (A13, built 2026-08-20) and U3b's view subscription
(built 2026-08-20), so what remains there is the second commander's identity rather than the
machinery to serve one. The design this plan delivers is
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
slices assume earlier ones. Landed slices carry a **Built** line naming what is in the
tree and what is still owed.
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

**Built (U1, 2026-08-19).** `GameLogic/UniverseGen.h/.cpp` generates and writes; bake mode
lives in `Outpost/UniverseBake.h/.cpp` and is selected from config; `Universe.h` grew the
`Anchor` and `Constellation` records and `UniverseParse.cpp` reads them back.
`GameData/Universe/Frontier.json` **is the committed universe**: 50 regions, 250
constellations, 2,500 systems, 12,453 planets, 3,356 stations, 6,000 gates and **18,618
anchors** in ~14.2 MB, `universeHash db10606904062335`. Parse + hash of that file measured in
**Release: 167 ms** — against ADR-018 D11's ~1 s ceiling, so **no per-region content split is
owed** (Debug is ~10× that, which is the same ratio A4's soak found and the reason the number
is quoted from Release). `Tests/GameLogicTests/UniverseGenTests.cpp` holds the invariants,
including the round-trip against the *committed* file rather than a freshly generated one.
Two bugs are worth remembering because neither was visible by eye: a `Member(key, "star")`
call binding to the `bool` overload and writing `true` into every celestial (caught only by
the round-trip, and fixed by giving `JsonWriter` a `const char*` overload), and int64
overflow squaring universe-plane deltas — 1.2e16² does not fit — which made "nearest" mean
"furthest" **and was then reintroduced in the test written to check the property it broke**.
`DistanceSquared` now takes an explicit shift and the scales are named constants.
**Still owed:** `Ids.h`'s scale comment is corrected, but the deterministic per-order arrival
offset (D18) has only the anchor fields reserved for it — the rule itself is U3a's.

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

**Built (U2, 2026-08-19).** `GameLogic/WorldRegistry.h/.cpp` is the runtime: worlds keyed by
`AnchorId`, borrowed and never held, spun up with their anchor's authored occupants at the
bake-derived ids, torn down when the last ship leaves and nobody is watching, ticked with one
shard number in anchor-id order. `NeuronCore/OwnerThread.h/.cpp` is ADR-007 §7's owner-assert,
armed on `World::Tick`/`Spawn`/`Despawn`/`SubmitOrder`. `World::Spawn` now takes an injected
id (D6a) and `World` no longer mints one. `Outpost/Main.cpp`'s `UniverseSimulation` hosts the
registry and borrows the served grid on every use; the exe no longer spawns stations, because
the start anchor's station is its *authored occupant*.
`Tests/GameLogicTests/RegistryTests.cpp` holds the nine invariants, including A7's empty-world
quiescence, D8's viewer-held exclusion from the hash, and D1a's permuted-tick-order bit
identity. The CI determinism guard was **split rather than extended**: `UniversePos` stays
banned everywhere outside the universe files, and only the *content* name `UniverseDef` gains
the registry as an exception — it looks an anchor up and does no math on a position.
One thing the id-space arithmetic caught before it shipped: giving every anchor a 64-id block
put the highest authored id at 1.19M, far past the u16 window D6 keeps. Blocks now go only to
anchors that author something (3,356 of 18,618) and are 8 wide, so the highest authored id is
26,848 against a dynamic base of 32,768.
**Still owed:** the viewer hold exists and is exercised by the tests, but nothing calls
`AddViewer`/`RemoveViewer` for a *player's* view until U3b; `HostForAnchor` returns 0 and
`TransferId` has no bus behind it until T1.

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


**Built (U3a, 2026-08-19).** `OrderKind::Warp` stopped being reserved and filled in the
number ADR-016 published, with `OrderReason::UnknownAnchor = 14` after the station phase's
five. The class table grew `warpSpeedMetresPerSec` and `spoolSeconds`, scaled the way speed
and turn rate already are; the envelope claim is asserted as a comparison (a battleship
spools longer and travels slower) rather than against values, so retuning is not rewriting
tests.

`ValidationView` gained `reachableAnchors` — **a list of ids, not the universe**. Both halves
load the identical definition and could each look a destination up, but naming that type in
the validator would put the universe inside the one function that must round the same way on
both machines. The registry resolves reachability (same system, itself excluded — leaving a
system is what gates are for, and that is U4's) and hands the world the list once at spin-up,
because it is content and never changes.

The three phases are the ADR's. A warp **spools where it stands**: it takes the group table
like any order, so it is acked, replaced by the next order and drawn as a ghost, but it has
no legs — the fleet holds, which is what makes "cancelled by a replacing order" mean
something. When the spool runs out the ships leave through the same seam a dock uses, and the
registry stamps the record's `applyTick` at the **arrival** tick: base plus universe distance
over the slowest member's warp speed. In between the fleet is *nowhere* — `LocationOf` says
so — and the crossing is on the bus, which is why the in-flight bus is in the hash. Arrival is
a formation solve at the anchor's authored warp-in point.

One thing that needed care rather than cleverness: `TransferOut` despawns, despawning forgets
the ship in its group, and forgetting the last member **erases the group**. Departing while
iterating the group table would have deleted the entry being read, so the departures are
collected first and executed after.

The distance is computed in `double` and the reason is arithmetic: two anchors in one system
are up to ~1e12 metres apart and squaring that overflows `int64`, while `double` counts
integers exactly to 9e15. What it must never become is `float`, which stops counting metres
exactly at about sixteen million of them.

**Also built with U3a:** ADR-019 §4b's `TRANSFER_FLOOR_TICKS` (20 ticks, one second), which
that ADR names as a constraint U3a and U4 must respect when they set their timing tables. It
is not a game-feel number — it is the slack a cross-host transfer needs to be delivered in,
and there is one host, which is exactly when a timing table gets tuned under a floor without
anybody noticing. A test asks the nearest pair of anchors in a system, which is the case that
would breach it.

**Still owed by U3a:** nothing. `etaSeconds` during transit landed with U3b's summaries
below, which is where it belongs: a fleet mid-crossing is in no world, so no grid's order
records can carry it.

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
per-grid stream and per-player summaries are already per-client facts; **built 2026-08-20**,
along with the summary family's frame, so `FleetSummary` already reaches its viewer at ~1 Hz
and what U3b's client half inherits is a feed rather than a thing to build); the presence-edge
rules render (D16: presence lost under a pinned camera → the map; every fleet in transit →
the map); warp events emit into the **per-commander event record** (A17) and the alerts
taxonomy gains its universe rows with the toast **action payload** (A18); summaries and
view rights key on `PlayerId` (D5, minted in T2's cluster).

**Built (U3b's sim half, 2026-08-19).** `GameLogic/FleetSummary.h/.cpp` and
`WorldRegistry::Summaries()` — the summary family ADR-016 §6 named, with `StationRoster` as
its first resident and this as its second.

One row per (place, state) over the three places a ship can be: standing on a grid, docked at
a station, crossing to somewhere. A fleet is emergent, so a row is a *count grouped by place*
rather than a record of an entity — there is no fleet id, and nothing to keep in step with the
snapshot. Ordered by anchor and then state, so two runs of one script produce the same bytes
and a client can diff two messages without sorting.

The `InTransit` row carries the anchor a fleet is **going to** and the ETA no grid could give.
That is the number U3a owed: a fleet mid-crossing is in no world at all, so the estimate is a
fact about the bus rather than about a grid's order records.

Two things the summary deliberately does *not* say. It does not report what a fleet is doing
on its grid — that is the snapshot's business, and a summary that tried would be a second
source of truth. And a station standing on its own grid is not a fleet: authored occupants are
subtracted, or every station in the universe would read as a parked one-ship fleet.

**Built (U3b's wire half, 2026-08-20).** Per-grid snapshots, the view request and the
grid-switch notice — the three wire pieces the previous note filed under "on screen" and
which were not screen work at all.

`SnapshotHeader` carries `gridAnchor`, and `ReplicatedView` drops its history when that
number changes. That is **the smear guard**, and the failure it prevents needs only two
ordinary facts: ids are allocated per registry, so two grids can each hold a ship 1, and the
view interpolates between its last two frames. Together, unguarded, a switch walks every hull
from where it stood on the grid you left to where a ship of the same id stands on the grid you
arrived at. The check runs **before** the staleness test, because a frame from elsewhere is
never stale — checked after, a switch to a world whose tick was lower would be dropped as old
news and the player would keep watching the world they left. Two header bytes, and no ships:
the cap arithmetic had the slack, asserted so the next field finds out.

`ViewRequest`/`ViewChanged` are reliable and ordered, because a switch is something the player
did once. The gate is ADR-014's usual split: `Simulation::MayView` decides — presence, which
is a fact about where ships are and which ADR-017 §7 already folded docked ships into — and
the session role enforces. A refusal **leaves the feed exactly where it was**, which is the
half a naive implementation gets wrong by switching first and validating after. `NoPresence`
is its own reason rather than a reused one, because the refusal is a sentence the player
reads and `NotAtStation` would name a different problem with a different action.

**Still owed by U3b:** the screen — auto-follow, roster location blocks with IN WARP and
off-grid states, warp ghosts, the ~200 ms settle over the interpolation refill — plus A15's
RTT-parameterised acceptance, A16's presence edges and A18's toast rows. The wire underneath
all of it is in the tree, and `selfTest` drives the view gate end to end in the shipping
binary. **What it cannot yet prove is a switch:** that needs two grids with presence on both,
which is U3c's scenario and not this one's.

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

**Unblocked 2026-08-20, and worth naming what that leaves.** Both things this slice was
waiting on are in the tree — the per-client `SnapshotSender` (A13) and U3b's view request with
its `MayView` gate — so the machinery to serve *a* commander per connection exists. What does
not is the second commander's identity: `ServerHost` mints `SOLE_PLAYER_ID` for every session,
and `WorldRegistry::Summaries()` and `Roster()` answer for everyone because there has only
ever been one of them. That is the work, and the shape ADR-018 D5 gives it — key on
`PlayerId`, filter rather than restructure — has not moved.

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
tree and its OPEN note updated. **That question was answered by
[ADR-016 §8](ADR/ADR-016-procedural-universe-and-warp.md) when this phase was designed** —
the map plans, the client feeds, one order per completed hop — and re-ruled unchanged on
2026-08-20 (§9a.1). So what U4 owes is not a decision but the *behaviour*: the feeder, and
the halt emitted into ADR-018 D19's event record so "your fleet stopped at KIL-7 while you
were away" is something the away-log can say rather than something the player discovers.

**Built (U4's sim half, 2026-08-20).** A fleet can leave its system. `HullClass::Gate = 11`
is the twelfth hull (ADR-016 §10), the bake gives every gate anchor its entity, a gate grid's
reachable list carries the far side of its own gate, a `Warp` naming that anchor is judged on
where the fleet is standing, and the crossing is priced flat.

Four things are worth reading rather than inferring from the diff.

**A jump is not a new verb.** It is `OrderKind::Warp` with a destination on the other side of
a gate, so it inherits the spool, the group table, the ghost, the bus, the arrival solve and
the hash without any of them learning that systems exist. What distinguishes it is one field —
`ValidationView::jumpAnchor`, the single reachable anchor that is reached by crossing — and
the rule that field selects: every member inside `JUMP_RADIUS_METRES` of the gate, or
`NotAtGate` (reason 16, appended after `NoPresence`). `UnknownAnchor` still means "not from
here", which is what keeps the two refusals from having to be told apart by the player: they
are the same pair `UnknownStation` and `NotAtStation` already are.

**Flat, and stated in ticks.** `GATE_JUMP_TICKS = 400` — twenty seconds at 20 Hz, which puts a
light fleet's hop (spool plus crossing) on the print's ~23 seconds. It is a tick count and not
a duration because the conversion is where a flat number stops being flat: `20.0 / 0.05` is
399.99999 in binary floating point and truncates to 399, so the number the design states would
not have been the number the bus used. Two crossings of very different map lengths are
asserted to cost the same, which is ADR-009 §3's "between systems is map fiction" made
mechanical.

**The block shrank from eight ids to two, and U4 is what measured it.** Gate anchors author an
entity now, and there are 6,000 of them against 3,356 stations; at eight ids each that is
74,848 authored ids, past the u16 ceiling and far past the 32,767 below `DYNAMIC_SHIP_ID_BASE`.
Two fits the worst case a recipe at this scale can ask for (~30,000) with the committed
universe using 18,712, and the bake now refuses rather than wrapping if one ever does not.
**The content is re-baked** — `Frontier.json` moves only in `occupantIdBase` and
`occupantCount`, nothing geometric, which the re-bake was diffed to confirm.

**The mesh arrived with the slice** (see ADR-016 §10's amendment): `Stargate.obj`, registered
behind `Structure.obj` in the content list, with the class's pick and contact radii taken from
its silhouette rather than guessed ahead of it. Its export carried a sixth material the
five-material palette does not have; the two faces were authored onto `accent`, whose albedo
it already matched exactly.

**Still owed by U4, and it is the client half:** the route feeder (Dijkstra over the gate
graph, one order per completed hop — the pure half of it, search and route-solve, is
`UniverseRoute` and already built), route progress on the HUD, the STATIC-family tactical icon
and map glyph, and the halt in the event record, which is a *client* fact today — the server
sees a fleet arrive at a gate and cannot know a route existed. The client's pre-check cannot
judge a jump either, but that is not new: `ReplicatedWorldView::PreCheck` builds a view with
ids alone, so no `Dock` or `Warp` has been pre-checkable since either landed, and the fields
those need arrive together with the surfaces that raise them.

**Not driven over the wire, on purpose.** A jump is 400 ticks by design, so a loopback
scenario would add twenty seconds of wall clock to a gate that runs on every push in two
configurations, to exercise an order path the dock already covers. The crossing runs instead
in `selfTest`'s device-free half, ticked as fast as the CPU allows in the shipping binary:
the gate stands on its grid, a fleet at it is let through, a fleet across the grid is refused
`NotAtGate`, and the crossing lands in the system on the far side.

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
**Three of the print's §4 decisions landed 2026-08-20
([ADR-016 §9a](ADR/ADR-016-procedural-universe-and-warp.md)) and they shape this slice:** the
history scrubber keeps its rail — drawn, inert and labelled — because the irreversible thing
is the layout rather than the feature, and build-or-cut waits on the strategic stream existing
rather than on U5; intel-ping provenance is deferred behind a named trigger (the first
information one commander sees because another reported it), so the overlay shows nothing and
promises nothing; and the screen is **landscape only**, because aspect is a property of the
display envelope and not of a surface — which is also the envelope this slice must not lay
zone tables against blind (UI-4).
**Accept:** visual checkpoint against the print at region level *over the real baked
content* — constellation hulls disjoint, labels legible, which is U1's clustering invariant
paying off on screen; a destination set on the map produces a real crossing; the full 2,500
render inside the frame budget with the `Ui` span proving it.

**Built (U5's pure half, 2026-08-19).** `GameLogic/UniverseRoute.h/.cpp`: the two questions
the strategic map asks that are not about drawing — "which system did you mean" and "how do I
get there" — as pure functions over the baked universe, which is where ADR-018 D14 puts them.
A search that lived in the client would be a second search the day a route is planned from
anywhere else, and a route solver that lived there could not be replayed.

Breadth-first, because a gate is a gate; when jumps stop being equal (a security-weighted
route, a toll, a blockade) this becomes Dijkstra over the same graph and the signature does
not move. **Ties break by system id**, which is not an implementation detail: two equally
short routes have to be *the same* route on the server and the client, or the line drawn on
the map is not the line the fleet flies. Search is substring and case-folded — the names are
`ROOT-N` and a player who remembers the number types the number — ordered by id and capped, so
a one-letter query cannot ask the screen for 2,500 rows.

Seven tests, including "every system reaches every other", which is U1's connectivity
invariant asked from the planner's side rather than the generator's, and "every step of a
route is a gate that exists", because a plan the fleet cannot fly is worse than no plan.

**Still owed by U5, and it is most of it:** the screen. Region/constellation/system pinch
levels, gate links, labels, the security overlay, the selected-system panel, fleet markers,
the route line, TACTICAL ⇄ MAP — all engine surface work, and its acceptance is a *visual*
checkpoint against the print plus a frame-budget measurement, neither of which can be done
without a GPU. The neutral topology that crosses the seam at boot (D14) is not built either:
it is an engine type, and it should land with the surface that consumes it.

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
  U4 until this lands. **Half delivered 2026-08-20: the mesh, as `Stargate.obj`** — it arrived
  with U4 rather than after it, so the Structure stand-in was never needed. Ring/portal as
  specified, 1,888 vertices and 1,144 triangles, on the corpus palette (one sixth material in
  the export was authored onto `accent`, whose colour it already was — see
  [ADR-016 §10](ADR/ADR-016-procedural-universe-and-warp.md)). **The icons are still owed**
  and land with U4's client half, beside the route progress they sit next to.
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
- ~~**D6 — The interest/delta ADR** *(ADR-018 A14 — drafted during the station phase; its
  implementation slice follows U3c and gates shared grids)*.~~ **Delivered 2026-08-19:
  [ADR-022](ADR/ADR-022-interest-and-delta.md).** Culling and delta belong to the **session
  role** and nowhere else (ADR-019 §5d), because relevance is a property of a viewer and the
  sim tier has none. `SnapshotAck` on datagrams; the baseline is the **view as sent**, not the
  world as it was — the subtlety that makes interest and delta safe together. Keyframes take a
  new reliable **`Bulk`** channel (a view switch *is* a mid-session join, and 21 KB is not a
  datagram-shaped object) so they never queue behind the player's orders. The relevance hook
  **ranks in the game and truncates in the engine**. Owned and selected ships are never culled
  — that is pre-check parity, not politeness — and `culledCount` says how many the player is
  not being shown. Whole-snapshot refusal becomes priority truncation. `lastOrderSeqProcessed`
  leaves the world hash (one replay re-baseline). And ownership costs **no byte**: two spare
  `statusBits` carry the viewer-relative relationship the icon sheet reads.

  **What this changes for U3b/T2, which come first:** the per-client sender (A13) and the
  per-viewer roster are not optimisations to add later — they are the shapes this design
  assumes already exist.
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
