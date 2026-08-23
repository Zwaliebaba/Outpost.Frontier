# Universe Build Order — Post-MVP Phase One

**This document does not sequence** *(2026-08-22)*. It says what each U-slice contains, what its
accept is, and — in the **Built** lines, which are most of its length — what landed and what that
cost. **When a slice is built is [Plan-of-Record.md](Plan-of-Record.md)'s**, which sequences
across all three phases and the work that belongs to none of them. The paragraph below is the
state of this phase, not a claim about what happens next.

**Status:** Session output 2026-08-19 · **U1, U2, U3a, U3b's sim and wire halves, U4's sim
half, U5's pure half and U5a, U3c and U3d-a/U3d-b built** (U3c 2026-08-21; U3d-a and U3d-b 2026-08-22).
**U3d — interest and delta — was added 2026-08-22**: ADR-018 A14 scheduled it for "after U3c"
and no build order had absorbed it, so it is specified below. **Its first two sub-slices are
built and R19 is closed**; ~~what is left of it is U3d-c's counted chip, which is screen work and
sits behind the input model with the rest~~ **and U3d-c landed 2026-08-23, so U3d is built** —
the counted chip's *visual checkpoint* is what is left of it, and that is an R1 item rather than
a slice. What is left after it is **screen work**:
~~U3b's client half, U4's route feeder and icons, U5's map itself and U6~~ **U5a landed
2026-08-23** — the strategic map's seam, camera, cull, layout and hit tests, all device-free and
verified over the real 2,500-system bake, with the surface drawn and navigable. What is left of
U5 is **U5b**: the visual checkpoint against the print and the frame-budget measurement, which
need a GPU. U3b's client half, U4's route feeder and icons, and U6 need a GPU and a person —
~~with no exceptions left~~ **and, as of 2026-08-22, behind the input model the plan of record
establishes**, since a screen built against the mouse adaptation would be retrofitted for touch
afterwards. **U5 also grew a fifth overlay it does not yet mention** — the RESOURCES site layer,
drawn 2026-08-22, tracked as N7 in the plan. **D1, U6's design gate, was drawn on 2026-08-21** — source in,
plate owed upstream and ~~four rulings owed~~ **all four answered 2026-08-23 (ADR-016 §9b), so
U6 has no design gate left**. U3c was the exception and it is done — it split into **U3c-a**
(ownership in the simulation) and **U3c-b** (the second commander on the wire), because a ship
had no owner and minting a second id against a registry where both commanders owned everything
would have passed the privacy accept for the wrong reason. **🏁 Its accept is met (run 188):
two commanders, distinct ids, disjoint grids, private rosters and summaries, view rights
enforced, orders refused `NotOwned`, and a reconnect inside the grace window that comes back as
the same commander with its fleet intact.** The rationale for the split is with the slice.
The design this plan delivers is
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
anchors** in ~14.2 MB, `universeHash db10606904062335`. *(**Superseded by E1b, 2026-08-20**: the economy phase re-baked this file to add 6,223 `Site` anchors, so the committed universe is now **24,841 anchors in 18.93 MB at `universeHash ad9555dd776008a6`**. Nothing U1 produced changed — the re-bake is purely additive and every station, planet and gate anchor kept its id, and the bigger file still parses in **183 ms in Release** against the 167 ms below — but the numbers quoted here are U1's, not the tree's. See [Economy-Build-Order.md](Economy-Build-Order.md)'s E1b.)* Parse + hash of that file measured in
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
~~**Still owed:** `Ids.h`'s scale comment is corrected, but the deterministic per-order arrival
offset (D18) has only the anchor fields reserved for it — the rule itself is U3a's.~~ **The rule
was not U3a's, because U3a's own note then said it owed nothing** — see U3a below. The fields
this slice reserved were read for the first time on 2026-08-22 by **N4**.

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
~~**Still owed:** the viewer hold exists and is exercised by the tests, but **nothing calls
`AddViewer`/`RemoveViewer` for a player's view**~~ — **closed 2026-08-22 by N5**, two slices
after the "until U3b" this line used to carry expired. The seam is
`Simulation::ViewerOpened`/`ViewerClosed`, called when a session opens on a grid, when a view
switch is accepted, and when the socket goes; the composition root keeps the viewer-to-grid map
and this registry keeps the count, which is ADR-022 §1's split applied to a hold. Presence
gating had hidden it, because a grid a player may watch is one their ships are standing on and
therefore one somebody already holds — and what it did not hide is the grid with no ships on it,
where the sweep tore the world down and `RankRelevance` rebuilt it on the next tick. See
[ADR-016 §7](ADR/ADR-016-procedural-universe-and-warp.md)'s note for what that cost a scout.
`HostForAnchor` returning 0 and `TransferId` having no bus behind it were both closed by T1.

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

~~**Still owed by U3a:** nothing.~~ `etaSeconds` during transit landed with U3b's summaries
below, which is where it belongs: a fleet mid-crossing is in no world, so no grid's order
records can carry it.

**One thing was owed and this line is why nobody found it: D18's arrival offset.** U1 wrote
`arrivalSpreadRadiusCm` into every anchor and said *"the offset rule is U3a's"*; this line then
said U3a owed nothing. Two slices each believed the other had it, and the field was baked,
parsed, hashed and read by nothing for three days while `ApplyTransit` placed every crossing on
the raw `warpInPoint` — so two fleets warping to one hub on one tick landed on top of each other
and were pushed apart by ADR-015 separation afterwards. **Closed 2026-08-22 as N4**; the rule and
its numbers are with [ADR-018 D18](ADR/ADR-018-scaling-baseline.md). Worth reading as a pattern
rather than as one bug: *"still owed: nothing"* is a claim about a slice's own list, and the item
that gets lost is always the one another slice put on it.

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

The header carries `gridAnchor` -- the game's own `SnapshotHeader` at the time, and `DeltaHeader`/
`KeyframeHeader` since U3d-b moved the envelope to the engine -- and `ReplicatedView` drops its
history when that number changes. That is **the smear guard**, and the failure it prevents needs only two
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

~~**Still owed by U3b:** A15's RTT-parameterised acceptance and A16's presence edges.~~
**A16 built 2026-08-23; A15 stands.**

**Built (U3b's client half, 2026-08-23).** The map shows where the player's ships are, and a
system with ships in it can be watched from it.

Four things are worth reading rather than inferring from the diff.

**Most of this slice turned out to be already built, which is the third time in a row.** The
build order says U3b's client half *"needs a GPU and a person"*; auto-follow, the settle, the
refusal path and the location blocks' IN WARP state were all in the tree and device-free, and
what was actually missing was the markers, VIEW and one presence rule. That line has now been
wrong for U5a, for U4's client half and for this — the device-free share of a screen slice is
consistently larger than the plan assumes, and it is worth stopping saying otherwise.

**Markers are counts at places, and the fold is the game's.** A summary row is an *anchor* and
the map draws *systems*, so `BuildMapMarkers` is where one becomes the other — a client that
could do it would need the anchor table, and a client with the anchor table has the universe.
Two counts rather than one: ships that are *there* (standing or docked, which are one number
because both mean "there") and ships *crossing to* it, which draw as an arrival with the
game's own ETA word. Adding them together would put a fleet in a system it has not reached, on
the one screen a player uses to decide where things are.

**VIEW is gated on presence and not on routing**, which is a distinction the panel has to make
because routing a fleet to a system you have nothing in is the *ordinary* use of this screen.
A system the player is only arriving at carries no anchor at all — `MayView` gates on presence
and the far end of a warp that has not landed is a request the authority is right to refuse,
which is the same reason auto-follow follows on arrival rather than on departure.

**A16's second edge was a gap in a function that already existed, and that is why it was
invisible.** `FollowTarget` answers `NO_FOLLOW_TARGET` for two situations that look identical
from outside and are opposite to a player: having ships where you stand (a reason to stay) and
having nothing here and nothing to go to because everything you own is mid-crossing (D16's
*"every fleet in transit → the map is the view"*). `EveryFleetIsCrossing` is that second
question asked separately. Its own edge case is the one a mutation test found: a player
watching the grid a fleet is *arriving at* has every block crossing and is doing the most
reasonable thing on the screen, so the here-guard is load-bearing and now has a test that
proves it. **D16's first edge — presence lost under a pinned camera — is not built**, and it
is not forgotten: camera pinning is U6's focus polish and does not exist, so there is no
pinned state to test.

~~**Still owed by U3b:** A15's RTT-parameterised acceptance, which needs a transport shim and a
stopwatch on a real client~~ **the shim landed 2026-08-23**, and D16's pinned-camera edge behind U6.

**Built (A15's buildable half, 2026-08-23).** `NeuronCore/DelayedTransport.h/.cpp` is the
latency hook `Transport.h` had none of, and `ClientApp::ViewSwitchBudgetSeconds` is the
acceptance stated as a function instead of as a sentence.

**The old acceptance was not conservative, it was conditional.** *"Roster click switches view
to smooth motion in under half a second"* is true at loopback RTT and is an unstated assumption
about the network everywhere else: a switch is a request, an answer and the settle over the
interpolation refill, so its floor is the round trip. At 200 ms of settle a 40 ms link has
240 ms to beat and a 300 ms link has 500 — the flat number was a target *plus* a guess, and
`ViewSwitchBudgetSeconds(roundTripMs)` is the two separated so a measurement and a document
cannot quote different numbers.

**A decorator, so the shipping transport never learns latency injection exists.** `QuicTransport`
is wrapped rather than modified: no branch in the real send path, and no configuration key that
could ship enabled — the hook is `ClientConnection::SetInjectedOneWayMs`, set in code by a
harness and never read from a file. At zero no shim is constructed at all, so the ordinary path
is not *equivalent to* the unshimmed one, it **is** it.

**The delay lands on delivery rather than on send**, which is truer as well as simpler: `Send`
returning "accepted" is a statement about the local socket, and deferring it would make that
statement a guess about the future. Wrap both ends of a loopback pair and the two directions
are the two halves of a real round trip, which is why the shim reports `2 x oneWayMs`.

**Constant delay and never jitter**, stated because it is a limit rather than an oversight:
held events are released in arrival order, which is only FIFO-safe while every event waits the
same time. A jittered shim would reorder a channel the transport guarantees is ordered.
Reordering and loss are a different experiment with a different acceptance.

**Still owed by A15:** the timed observation itself — a stopwatch on a real client at a chosen
RTT — which needs a person and a running game, and is an R1 item rather than a slice.

> **Both examined 2026-08-23, and neither is buildable here — for different reasons, which is
> why they should stop being one line.**
>
> **Superseded the same day: both were built.** A16 landed with U3b's client half and A15's
> shim with A15 itself, which is worth noting rather than quietly deleting — the note below
> concluded *"neither is buildable here"* about two things that were buildable within hours.
> What it got right is the split: they were one line and are two problems. What it got wrong is
> the same misjudgement the last three slices made, that a screen-adjacent item needs a GPU and
> a person. It is kept for the reasoning; read its conclusions as retired.
>
> ~~**A16's presence edges have no destination.**~~ **Unblocked 2026-08-23 by U5a.** Both rules
> route to the map — *"presence lost under a pinned camera → the map"*, *"every fleet in
> transit → the map"* — and `SurfaceId::Map` was an enumerator nothing pushed and nothing drew.
> It is a screen now: the tactical bar's location breadcrumb pushes it, `BuildMapSurface` draws
> it, and the shared `◀ TACTICAL` chip pops it. So A16 is back to being blocked on *effort* --
> and on the half of U3b that owns the view-switch path -- rather than on a surface. What it
> still needs before it can be accepted is a fleet marker to lose presence *of*, which is U3b's
> client half and not U5's. ~~*(Retired: U3b's client half built the marker and the rule the
> same day.)*~~ **A16's second edge is built; its first waits on U6's camera pinning.**
>
> **A15 is an acceptance procedure, not a feature**, and it is half-answerable. The *settle* is
> built and named: `ClientApp::VIEW_SETTLE_SECONDS` is 200 ms, which is ADR-002's
> interpolation-buffer refill made a designed pause rather than a pretended instant. What is not
> ~~built is the **injected-delay shim** — `Transport.h` has no latency hook of any kind~~
> **(retired: `NeuronCore/DelayedTransport.h/.cpp` is that hook, built 2026-08-23)** — and
> even with one the accept is a *timed observation of a real client*, so it needs a GPU and a
> person like the rest of the R1 queue. **That half stands.** The target is therefore stated rather than measured:
> **RTT + 200 ms**, which is the form A15 asks for, replacing W0's flat "under half a second".

**The location blocks landed 2026-08-21**, and they landed as a *generalisation* rather than as
a new panel. T2 built `DockedBlock` for the hangar's roster; U3b's second and third cases were
waiting behind it, because a fleet on a grid the player is not watching is exactly as invisible
to the scene as a docked one, and a fleet mid-warp is in no world at all. All three are one
sentence with a different word in it — *this many of yours, over there* — so `LocationBlock`
carries a `stateLabel` and an `etaSeconds`, and the panel reads `ELSEWHERE` rather than
`DOCKED`.

**The word is the game's and the engine never switches on it.** `DOCKED`, `IN WARP` and
`ON GRID` cross the seam as a string, not an enum, so three facts about this game stay out of a
library that serves two (ADR-014 §2b). What the client decides is the *colour*: a crossing is
the one state that stops being true on its own, so it reads in the caution amber that means
"not settled" everywhere else on this HUD.

**The assertion that earned the gate is about the case that gets no block.** A fleet standing on
the grid the player is looking at is already on screen as hulls, with rings, bars and a roster
row each; listing it again as "6 ships over there" is one fleet counted twice on one HUD with no
way to tell which count is the lie. `RunLocationBlockGate` drives a four-place summary through
the real decoder and checks the crossing carries its ETA with no button, the docked block offers
the hangar with no ETA, the off-grid fleet offers the view switch, and every block was given its
state in the game's own words.

**The ~200 ms settle landed the same day, and what it protects is not what the name suggests.**
The obvious reading is that the interpolation buffer needs covering while it refills — and it
does not: `ReplicatedView::SampleAt` already shows the oldest frame it holds rather than
nothing, so a switch produces a correct-but-unblended frame and never a smear or a snap. That
was checked before anything was written.

What actually needed protecting is **the client's own reactions to the scene changing under
it**, and one of them was already wrong. A grid switch changes *every id on screen at once*,
and T2's transit fades exist precisely to notice ids appearing and disappearing — so a switch
read as a whole fleet leaving and another arriving, and would have drawn a ring for every one
of them. Nobody went anywhere; the camera moved. Two features, each correct alone, wrong where
they met.

**The window exists because the two signals race.** `ViewChanged` is reliable and ordered;
snapshots are datagrams, so the new grid's first frame can land on either side of the notice
that the grid changed. The settle re-baselines the transit list **every frame** it runs rather
than once at the notice, which is what makes it robust in both directions — a single clear at
the notice still leaves one frame exposed if a snapshot arrives first. The ghosts and the
approach chain are cleared with it, for their own reasons: a promise about ships on a grid
nobody is watching is one that hangs, and a two-step order's second step names ships that are
about to leave the screen.

**A18's first toast row came with it.** `ViewChanged` had been sitting in `ClientConnection`
with no reader — the same shape the summary family was in before T2's client half — so a
refused view switch was silent. It now raises a toast carrying the game's own reason text, on
the path a refused order already takes, because a player who cannot go somewhere is owed the
same sentence whichever surface they asked from.

**Auto-follow landed 2026-08-21, and its policy is one sentence:** *a player watching a place
they have nothing at is watching the wrong place.* Everything else in it is that sentence being
careful.

**The trigger is arrival, not departure, and that is forced rather than chosen.** `MayView`
gates a view request on presence, so the grid a fleet is warping *to* refuses the client until
the fleet is standing on it. Following at departure would mean asking for a grid the authority
is right to refuse; the honest window between the two is the crossing, which is U6's transit
view and not this.

**The guard is "nothing of mine is here", not "my fleet moved".** A station where ships are
docked, or a grid where half the fleet stayed behind, is a place the player has a reason to be,
and yanking the camera because the other half arrived somewhere would be the HUD overruling a
decision they made. Only an empty place follows. Ties break by anchor rather than by arrival
order, because two grids holding the same count is a real state and the camera has to land the
same way twice.

**One factoring change came out of it, and it improved the seam.** `BuildLocationBlocks` had
been dropping the row for the grid being watched — correct for the panel, wrong as a rule,
because auto-follow's whole question is "does where I am looking still hold anything of mine"
and the game had already deleted the answer. The blocks now carry every place plus an
`inScene` flag the game sets, the panel skips on that flag, and the client decides what it may
notice rather than being told. The decision itself moved to `FollowTarget` in `AutoFollow.h`,
a pure function over the blocks: the rule is the whole feature, and a rule that lives inside a
class holding a swap chain is a rule nobody can assert against. Seven tests cover it.

**A scenario lever landed with it (2026-08-21), because half of what U3b builds cannot be
reached from the shipped world.** Auto-follow, the settle, the view-refused toast and a location
block for a place you are not standing all need a world the default start does not build — so
they could be written, unit-tested, and never once *looked at*, which is exactly the gap R1
records three defects escaping through.

`AppConfig` grew a `scenario` block of **knobs rather than named presets**: a preset list is a
vocabulary that drifts from what it sets up, while each knob is one fact about the world and
they compose. The first is `secondFleetWings` — a second fleet for the same commander, on the
next anchor in bake order nobody is standing on. The anchor is
deliberately *not* configurable: `HomeAnchorFor` already answers that question deterministically
for a second commander, so the config says *how much, elsewhere* and never carries an id a
re-bake could move.

**Every knob is off by default and that is load-bearing rather than polite.** The committed
`Outpost.json` must keep producing the world it produced yesterday, because the replay hash is a
property of the shipped scenario (ADR-005) — a knob that changed it by existing would make every
future determinism failure ambiguous. Verified: the shipped config still reports replay hash
`69c58e2751c0df22`, and the variant runs from a scratch directory holding only a modified
`Outpost.json`, leaving the repository untouched.

**It found two defects within a minute of the first frame**, which is the whole argument for
building it. The `ELSEWHERE` heading was drawing over nothing — the block count it tested was
the game's raw count, not the number that survives the panel's own skip, so in the ordinary case
(one fleet, standing where you are looking) the heading sat above empty space. And the first
block that ever named a grid which was not a station's drew **`?`**: the name table held stations
only, a hangar-shaped assumption from T2 that a location block quietly outgrew. Neither is a
number any test could have been wrong about; both are a frame.

**Warp ghosts landed 2026-08-21, and what they needed was a way to say "not here".**

Every other order draws its promise where it will happen: a footprint ring, one tick per ship,
a lane from the fleet to it. A warp's destination is an **anchor, not a point** (ADR-016 §3),
so there is nothing on this plane to ring — and the nearest candidate is wherever the gesture
landed. Left alone the ghost solved a real formation at that point and promised the fleet would
assemble there while it was in fact leaving the system, and *the more carefully the footprint
was solved, the more convincing the wrong answer looked*.

So `OrderPreview` gained **`onThisGrid`**, which the game answers and the client obeys: the
overlay and the lane skip a ghost that has no place, and the promise moves into the chrome as a
warp chip beside the other things the world cannot show — amber while the authority has not
answered, own-fleet phosphor once it has, which is the same two-colour promise the plane draws
for a Move, said in text because text is what fits. A refused warp needs nothing there: the
bounce toast already carries the reason, and a chip that lingered to say "that did not work"
would be the same sentence twice.

**And the verb was found to be unreachable.** Nothing in the tactical HUD sets
`OrderIntent::anchor` — only the approach chain does, for Dock — so every warp the command row
could compose carried no destination and came back `UnknownAnchor`. WARP was a live-looking
button that could only ever bounce, which is the defect MINE had before its gate. It is now
greyed carrying that same reason, so the row says immediately what the authority would have said
a round trip later. **The gate is a statement about the surfaces this build has, not about the
game**, and it lifts by deletion the day the strategic map or the system view can name an
anchor — which is U5's and U6's work, exactly where ADR-016 §9 puts it.

**What is still not proved is an accepted switch end to end.** Auto-follow is what will make
one happen for real, and the harness still cannot set the scenario up: it needs a single
commander with presence on **two** grids at once, and nothing in `selfTest` puts one there.
The rule is a tested unit and an untested integration, which is the same sentence as before and
now has a named cause rather than a missing feature. The wire underneath
all of it is in the tree, and `selfTest` drives the view gate end to end in the shipping
binary.

**What it can now prove is half a switch, and the half it cannot is worth naming.** U3c landed
on 2026-08-21 with two commanders on disjoint grids, so the *refused* path is driven end to
end over a real socket: a commander asks to watch a grid they have no presence on, the request
is answered, the refusal carries `NoPresence`, and the feed is still pointed where it was. The
**accepted** path is still unproven, and it is the one carrying the smear guard — that needs a
single commander with presence on **two** grids at once, which no scenario in the tree sets up
yet. So the guard is a tested unit and an untested integration, and the honest place to close
it is U3b's client half, where auto-follow is what will make a switch happen for real.

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

**U3c splits, 2026-08-21, and the reason is that "no new mechanism" turned out to be half
true.** The *wire* needs none: `Simulation`'s seam is already player-keyed on every method
that matters — `WriteSnapshot(PlayerId)`, `MayView(PlayerId)`, `WriteSummaries(PlayerId)`,
`ApplyOrderBytes(PlayerId)` — and A13's per-client `SnapshotSender` already serves one
commander per connection. But the *simulation* needs one it has never had: **a ship has no
owner.** `WorldRegistry` keeps no `ShipId → PlayerId` anywhere, `RosterEntry` has no owner
field, and `DurableShip.owner` is a field the format reserved which capture fills in with
`SOLE_PLAYER_ID` unconditionally. Every player-keyed accessor already *takes* the id and
then ignores it, each with a comment saying that is where the filter goes — `CargoFor`'s is
the clearest ("one player today, so every ship on a grid is theirs"). Minting a second
`PlayerId` against that registry would prove nothing at all: both commanders would own
everything, and every privacy assertion in the accept would pass for the wrong reason.

So it splits the way E1 and E4 did, on the same two tests — a hard dependency direction and
very different blast radii:

  U3c-a  **ownership in the simulation.** `ShipId → PlayerId` at the universe layer, the
         accessors' identity functions become real filters, the roster remembers whose ship
         it holds, and the durable format carries an owner that was actually asked for.
         GameLogic and the format; no wire change and no `ServerHost` change.
  U3c-b  **the second commander on the wire.** `ServerHost` stops minting `SOLE_PLAYER_ID`
         for every session, sessions survive a disconnect for D5's grace window, and the
         twin-client `selfTest` is written against a registry that can already tell the two
         apart. NeuronServer and Outpost.

**U3c's accept is unchanged and belongs to U3c-b** — the slice is not done until two clients
have run the loop over a real socket. What U3c-a buys is that when they do, a passing privacy
assertion means what it says.

**Where the owner lives is the one design decision here, and ADR-018 D2 already made it.**
Not a column in `World`: worlds forget, durable state lives at the universe layer, and a
player identity inside the deterministic SoA would put accounts in the replay domain and the
physics in the way of every future change to them. So it is an index on `WorldRegistry`,
beside the rosters and the bus — which also means it folds into `Hash()` for D8's reason
rather than a new one: a replay that reproduced every ship and forgot who owned them would
agree about a universe where nothing belonged to anybody.

**Accept (U3c-a): met, run 180 (2026-08-21)** — green on both configurations with `Outpost/`
compiling for the first time. Ships have owners through every path that moves one — spawn,
transfer, dock, undock, load — the accessors filter on the viewer with a test per accessor that a
second commander sees none of the first's, the roster survives a restart with its owners, and
the whole thing round-trips the durable format. Headless: no socket, no second connection.

**Built 2026-08-21.** `m_ownerByShip` sits beside `m_locationByShip` on the registry, indexed
the same way, and the two are written by one call and cleared by one call — so the invariant is
one sentence a reader can hold: *a ship the registry cannot locate is a ship it has no owner
for.* `RecordLocation` no longer exists as a thing a caller can reach; there is no way to spell
recording a location without saying whose it is, which is the mistake this would otherwise have
been one careless call site away from.

**The owner rides the crossing, per member.** In transit a ship has left one world and not
reached the next, so nothing can be asked about it — whatever the far side needs has to travel.
That is E2's defect exactly, one level up: E2 shipped without the cargo riding and a Miner
arrived empty, and an owner arriving empty is quieter and worse, because a fleet that stops
being anybody's produces no error at all. Per **member** rather than per request, matching
`oreUnits` and for the same reason — it is a property of the ship, not of the order. A crossing
carrying two commanders' ships is not something today's validator can produce, but "cannot
happen" on a request-wide owner means that if it ever does, every hull in the fleet silently
changes hands. The registry stamps it as it collects, never the world (ADR-018 D2).

**Two of the six were holes rather than gaps**, and both are worth naming because neither would
have shown up as a bug report. `MayView` walked `m_patrolShips` — the composition root's
scripted patrol list — so it returned the same answer for every viewer: the second commander to
connect could have watched the first one's grid. And the validator's `RosterView` was handed the
**station's** roster rather than the asker's half, which is not a display bug but an authority
one: a commander could name somebody else's hull in an `Undock` and the validator would find it
on the roster and agree. `DockedFor` is what anything reaching a player uses now; `Roster` still
answers whole for the two callers that genuinely want the station — grid teardown, and the save
file.

**A special case went away rather than being kept.** `Summaries` counted
`ShipCount() - authoredCount` with a paragraph explaining that a station would otherwise read as
a one-ship fleet parked at every station in the universe. That is still true and is now merely
*implied*: authored occupants belong to `INVALID_PLAYER_ID`, so counting the ships a viewer owns
cannot count them. `AnAuthoredOccupantIsNobodysFleet` is the test that the consequence holds.
The same constant is why an anonymous handshake is not a skeleton key — without the guard,
`INVALID_PLAYER_ID` would match every piece of furniture in the shard, and the one identity
nobody has to authenticate as would be the one that can watch any grid with a station on it.

**The format goes to version 3, and this is the case the version number was written for.**
`DurableShip.owner` has existed since E4a; capture filled it with `SOLE_PLAYER_ID`
unconditionally. So a version 2 file does not lack ownership — it **asserts** that every hull in
the shard belongs to player one, and reading one under the new rules would hand the first
commander to connect the entire universe. ADR-025 §2 records it.

**What was verified, and how.** `GameLogicTests` **380 methods across twelve files, 0
failures** under clang 18 on Linux, of which 16 are new in `OwnershipTests.cpp`; the store's
14/14 and `NeuronCoreTests`' tasking 18/18 beside them; clang-tidy clean on every changed file.
The new tests were then **mutation-tested**, because a privacy suite that has never failed is
indistinguishable from one that cannot: reverting `HasPresence` to ignore the viewer fails 3,
un-filtering `Summaries` fails 1, and un-filtering `DockedFor` fails 3. One existing test had to
change and it is the slice's best evidence — `ABayIsPerOwnerInTheHashToo` docked its second ship
under the one commander there was and then had commander TWO transfer ore off it, which the
validator now refuses. It was only ever passing because a commander could reach into another's
hold.

`Outpost/` is Windows-only and has not been compiled here — `MayView`, the summary sender and
the roster it sends are CI's first build of this slice.

**U3c-b built 2026-08-21 — and it needed no schema bump, which is T2 collecting a debt.**
`Hello` and `Welcome` have carried a `PlayerId` and a `resumeToken` since A12, shipped as zero
with a note saying the alternative was "a schema bump on the day sessions first survive a
disconnect". This is that day, and the two fields simply started carrying values: no layout
change, no hash move, no client refused at the door.

**`ServerHost` mints per PLAYER, starting at `SOLE_PLAYER_ID`** so the first client to connect
is still player one and every single-commander scenario means what it meant. A `Hello` that can
prove it is coming *back* is resolved **before** anything mints, because the other order would
hand a reconnecting commander a fresh id and a fresh fleet while their ships were still
standing on a grid.

**The grace window is `ResumeTable`, in a file of its own, and that is the slice's one
structural decision.** All of it — the deadline in ticks, the token check, expiry — is
decidable without a socket, so it is driven by eleven tests instead of by a four-minute live
run. That is the argument that put `DurableStore` where it is, applied again. Two of those
tests exist because a live run would never reach them: the boundary tick (inclusive, and
"expired" versus "expiring" differ by one), and the u32 tick rollover, where a wrapping
deadline would end *every session on the shard* at the instant the counter turned over.

**The token is a resume handle and not a credential**, and `SessionResume.h` says so at
length rather than leaving the word "token" to imply something is verified. Nobody is
authenticated; whoever holds one is treated as the player. It is seeded from the OS so it
cannot be counted to, and rotated on every use so one seen on the wire is worth one reconnect.
ADR-018 D5 declined to mint one at T2 on the ground that inventing a token would be inventing a
security model with it — that reasoning is untouched, and what changed is only that resume
became a requirement.

**`Simulation::PlayerJoined` is a new seam, and it returns nothing on purpose.** The engine
hands over an id; what a commander is *given* is a game question (ADR-014 §3). The composition
root's answer is deliberately a placeholder and says so in the code: a commander who already has
ships is not new (a reloaded shard knows them, and spawning would hand them a second fleet on
every restart), and one who is gets **one wing on a grid of their own**. One wing rather than
the boot fleet's eight is arithmetic, not generosity — a full snapshot carries 43 ships, the
boot fleet is forty plus a station, and a second full fleet would sit on the cap that the
interest/delta slice (ADR-022, D6) exists to lift.

**The old `selfTest` was quietly assuming one commander, and this found it.** Its
approach-disconnect section connects, flies a fleet, drops mid-leg, and reconnects as an
"observer" to check the ships are still outside. Every connection used to be `SOLE_PLAYER_ID`,
so it got the right answer without asking the question; with ids minted per player the observer
would have been a *different* commander on a *different* grid, finding no approaching ships
because there were none there to find. It resumes the commander that left now — which is a
better test than it was, since the grace window is exercised by a section that is not about it.

**What was verified, and how.** `ResumeTable` 11/11 and `GameLogicTests` 380/380 under clang 18
on Linux, with the store's 14/14 and the tasking suite's 18/18 beside them; clang-tidy clean.
`ServerHost`, `ClientConnection` and the composition root are Windows-only and have **not** been
compiled here, and neither has the twin-client `selfTest` — **CI is the first run of U3c's
accept**, and the numbers land in a `Record run` commit rather than being predicted.

**🏁 U3c's accept is met — run 188, 2026-08-21.** Debug|x64, Release|x64 and Spike 2 all green:
**822 tests on MSVC with none failing**, every source guard green, no clang-tidy finding, and
one warning in the whole build (the pre-existing `NeuronClient\Picking.cpp(51)` C4723).
`selfTest`: **PASSED** on both configurations, and the accept is its own log:

```
self test: two commanders hold sessions at once -- ok
self test: the two commanders have different ids -- ok
self test: the second commander is put on a grid of their own -- ok
self test: a request to watch another commander's grid is answered -- ok
self test: and refused -- ok
self test: and the authority refuses it NotOwned -- ok
self test: the second commander is told where their own fleet is -- ok
self test: and never where the first one's is -- ok
self test: a dropped commander can reconnect -- ok
self test: and comes back as the same commander -- ok
self test: on the grid they were watching -- ok
self test: with the fleet still theirs -- ok
```

**It took five red runs to get there, and every failure was the same thing: an assumption that
was true while there was one commander.** Worth listing, because the list is the slice's real
finding and none of these arrived as a bug report.

1. **`EverySessionIsServedItsOwnSerialisation` asserted the viewer was `SOLE_PLAYER_ID`** —
   which a broadcast sender ignoring the viewer entirely would also have satisfied, since there
   was one value the field could hold. It reads both `Welcome`s now and asserts a snapshot was
   serialised for each commander by name.
2. **The start grid was everyone's grid.** `ServerHost` opened every session on
   `Simulation::World()`, so a second commander was shown a grid they had no presence on and
   refused a view of their own fleet.
3. **The shard's `WorldMeta` was everyone's description.** Fixing (2) by handing over an anchor
   id alone left the `Welcome` advertising the shard's grid while the feed sent another's — and
   the client keeps no other record of where it is. Hence `WorldFor(PlayerId)` returns a whole
   `WorldMeta`: the grid's number and its origin cannot be allowed to disagree.
4. **`ServedWorld()` was everyone's world, and nothing checked whose ships an order named.**
   Every order went to the start anchor and could name any hull standing there. `Validate.cpp`
   had carried the reason since the MVP — *"NotOwned is unreachable in the MVP: there is one
   player and every ship is theirs. The code exists because ownership is a field, not a
   redesign"* — and this is the slice where the field exists and `NotOwned` is returned.
5. **The self test's oldest order fixture named `ships.front()`**, which on a station grid is
   the station. So "the authority accepts it and the ack returns" had been proving the ack path
   works by telling a space station to move a hundred metres to the right.

**The replay hash did not move:** `69c58e2751c0df22`, byte for byte E2's through E4b's, with
Spike 2 confirming Debug and Release agree. Ownership folds into `WorldRegistry::Hash()` and the
durable hash, not into `ComputeWorldHash`, and the replay scenario is six ships in a bare World.

**The shard snapshot grew to 1,908 bytes** (durable hash `d589ed5beb6b3324`) against E4b's 1,394
at tick 173, and unlike E4b's twelve bytes this is not arithmetic worth predicting: the self
test now leaves three extra commanders on the shard with a wing each, so most of the growth is
fleets that did not exist before. The Release soak reads 8.745 ms mean / 16.024 ms worst at the
capped grid, inside the tripwire, against E4b's 9.330 / 14.542. Content is untouched and the
universe parses in **203 ms** on Release.

**Unblocked 2026-08-20, and worth naming what that leaves.** Both things this slice was
waiting on are in the tree — the per-client `SnapshotSender` (A13) and U3b's view request with
its `MayView` gate — so the machinery to serve *a* commander per connection exists. What does
not is the second commander's identity: `ServerHost` mints `SOLE_PLAYER_ID` for every session,
and `WorldRegistry::Summaries()` and `Roster()` answer for everyone because there has only
ever been one of them. That is the work, and the shape ADR-018 D5 gives it — key on
`PlayerId`, filter rather than restructure — has not moved.

### U3d — Interest and delta *(ADR-022; ADR-018 A14 — the slice A14 scheduled and nobody wrote down)*

**Its home was the gap.** ADR-018 A14 delivered [ADR-022](ADR/ADR-022-interest-and-delta.md) and
scheduled its **implementation slice for "after U3c"**. U3c landed 2026-08-21; no build order
absorbed the slice, and five places across this corpus went on referring to "the interest/delta
slice" as the thing that lifts the shared-grid gate. It is written here because ADR-022 is this
phase's deliverable **D6** and the gate it lifts is U3c's — the numbering follows A14's own
words rather than inventing a phase.

**It retires [R19](Risk-Register.md), the register's only High/High row.** The full-snapshot cap
is 43 records against 42 of authored content: margin one, for the entire roadmap. Two commanders
meeting at the starter station is 83 records, 62 % over, and today's designed behaviour is that
`WriteSnapshot` refuses and *the whole grid's snapshot is dropped for every viewer*. That is a
session-killing outage by construction, and it is why this is the next slice rather than a later
one.

**It splits three ways, along the seam this repo already respects** — sim truth, then its
replication, then its presentation. The split is not tidiness: U3d-a is arithmetic over structs
and a hash edit, provable in `GameLogicTests` on a machine with no GPU and no socket; U3d-b is
wire, provable in `NeuronCoreTests`/`NeuronServerTests` and `selfTest`; U3d-c is a screen. Landing
them together would put a replay-contract edit behind a datagram format behind a person at a
display.

---

#### U3d-a — The ranking, and one subtraction from the world hash

The seam gains **`RankRelevance`** (ADR-022 §4): `InterestQuery{ viewer, grid, selection,
focusXMetres, focusYMetres, viewHalfExtentMetres }` in, a **priority-ordered** list of `ShipId`
out. **It never truncates** — the caller knows the budget, the callee knows the game — which is
ADR-014's pattern applied literally, and it is what lets a hostility tier change be a GameLogic
edit that touches no engine code.

GameLogic implements §4's three tiers: **tier 0** the viewer's owned ships on this grid, anything
selected, and the grid's structures (never truncated, §5a); **tier 1** ships with a visible
relationship inside the camera's extent, nearest to focus first; **tier 2** everything else,
nearest to focus first and round-robin across ticks so a distant ship updates at a lower cadence
rather than never. Structures are tier 0 for a stated reason — one or two per grid, and a station
that flickered out of interest would take the player's sense of place with it.

**`lastOrderSeqProcessed` leaves the world hash** (§7). It is per-session state living in shared
state — world-global, written as a max across all submitters, folded into `WorldHash` — which
with one commander is invisible and with two is wrong twice: one player's order sequence perturbs
the other's feedback loop, and a replay's hash depends on which client happened to submit. It
moves to the session. **The wire field stays exactly where it is** and becomes per-viewer, which
is what it always read as.

**This is a replay-contract edit** (ADR-005 §5), and the only determinism cost in the whole of
ADR-022. Every recorded replay golden re-baselines with it, and the new number is stated in the
slice's own note rather than discovered by a red test.

**Accept:** `GameLogicTests` — ranking is a **total order with no duplicates and no omissions**
over the grid's ships (a ranked list that lost a ship is a ship that can never be sent); tier 0
appears before any tier 1 and tier 1 before any tier 2; an owned ship far from focus still
outranks a neutral one under the cursor; a selected foreign ship is tier 0; round-robin over tier
2 visits every ship within a bounded number of ticks; **double-run bit-identity is unaffected**,
because ranking is a read. The replay hash moves once, to a stated number, and Spike 2 confirms
Debug and Release agree on it.

**Built (2026-08-22).** The ranking is `GameLogic/Relevance.h` — `RankRelevance` over the grid's
dense arrays, `Relationship`/`RelationshipOf` beside it — and the seam is `Simulation::RankRelevance`
taking a neutral `InterestQuery`. `UniverseSimulation` forwards; the composition root is a line of
wiring, as ADR-014 §2a asks. **The hook never truncates**, which is the clause the whole split
rests on, and the suite asserts the ranking is a permutation of the grid before it asserts
anything about tiers.

**Two readings had to be taken rather than found, and both are recorded in `Relevance.h`
itself rather than only here.**

- **`Allied` and `Hostile` have no producer.** The relationship enum is four-valued from its
  first line, because it is `tactical-icon-system.png` §3's colour channel and U3d-b's two status
  bits; but nothing in this corpus decides whether a foreign commander is one or the other. That
  is the combat phase's, and the Plan of Record names that phase as having no ADR and no build
  order. Today every ship that is not the viewer's own is `Neutral`, which is the honest answer
  and not a placeholder: with no diplomacy and no PVP flag, nobody is allied and nobody is at war.
- **Tier 1 reads as "inside the camera's extent"** rather than ADR-022 §4's literal *"a visible
  relationship **and** inside the extent"*. Taken literally the first conjunct is empty by
  construction while the point above holds, so the tier would be dead code and untestable. What
  the tier is *for* is stated in the same table — the ships the player is looking at get a steady
  cadence and the rest get a rotated one — and the extent is the whole of what expresses that
  today. When a hostility model exists the conjunct is added; the tier does not have to be
  invented then. **This is a reading of the ADR, not an amendment to it**, and it is flagged here
  so the next person finds it as a decision rather than as a discrepancy.

The extent is a **box** rather than a radius, because a half-extent describes what a screen shows
and a radius would rank the corners of the player's own screen below empty space beside it.

**Tier 2's round-robin is a rotation by `tick % count`**, not a stride. A stride only visits
everything when it is coprime with the count, and the count changes whenever a ship spawns — a
bound that depends on an arithmetic coincidence is not a bound. The rotation gives the accept's
clause directly: every tier 2 ship leads its tier once per `count` ticks. It is scoped to tier 2
and the suite asserts an owned ship holds the lead across forty ticks, because the guarantee
(§5a) is not a cadence.

**`lastOrderSeqProcessed` left the world hash, and left `World` altogether.** `World::m_lastOrderSeqProcessed`
and its accessor are gone; `SnapshotSender` keeps it per viewer, advanced from `ServerHost` **only on
an accepted verdict** — a refusal carries a sequence too, and advancing on one would promote a ghost the
authority turned down. `Game::WriteTickTail` takes the number as an argument (it was
`Game::WriteSnapshot`'s defaulted one until U3d-b split the payload), so the wire field
did not move, did not change width, and the callers asking about a *world* rather than serving a
commander (the self test's determinism harness, most of the suite) read an honest zero rather than a stub.

**The replay re-baseline was a non-event, and that is worth writing down.** No golden hash is stored
anywhere in this tree — the suites and the self test run a scenario twice and compare — so "every
recorded golden re-baselines" cost nothing to collect. What *did* need a test is that the number is
gone: `ARetiredOrdersSequenceLeavesNoTraceInTheReplayDomain` flies two worlds through the same order
under sequences 1 and 900,000 and waits for the group to finish its linger and be erased, because a
*live* group carries `clientOrderSeq` in the group table and always did. Retired is the only state in
which the high-water mark was the sole carrier, and it is the state in which the two worlds used to
diverge for the rest of the session.

**Verified:** Debug x64 builds clean. `GameLogicTests` 401/401 (13 new in `RelevanceTests.cpp`),
`NeuronServerTests` 43/43 (3 new: the high-water mark, two viewers keeping their own, and a refused
order over the real loopback failing to advance it), `NeuronCoreTests` and `NeuronClientTests`
unchanged and green. The headless self test passes end to end, including the two-commander privacy
run and the tick-cadence check.

#### U3d-b — The wire cluster, and the baseline that is a view rather than a world

**One fail-closed schema bump** carrying all of it, as ADR-018's Consequences require:

- **`SnapshotAck`** (§2a), C→S on the **unreliable** channel: `{ u16 gridId, u32 tick }`. Highest
  acked tick per (client, grid) wins and anything older is ignored, so reordering is a non-event
  and a lost ack costs one larger delta rather than a stall.
- **`DeltaHeader`** (§3b): `{ u32 tick, u32 baselineTick, u16 gridId, u16 culledCount,
  u8 partIndex, u8 partCount, u16 recordCount }`. **Every part is independently applicable** — it
  names its own tick, grid and baseline — so there is no reassembly buffer and no fragmentation
  timeout. `partCount` is what makes §2d's whole-tick rule checkable.
- **The keyframe, on a channel of its own.** `TransportChannel` gains **`Bulk`**, a second
  reliable ordered stream — an **ADR-003 §1 amendment**, already recorded there. The reason is
  head-of-line blocking in both directions: a keyframe is not fresh state but the *baseline* for
  all of it, it must arrive intact, at the cap it is ~21 KB, and on `Control` it would park in
  front of the player's orders.
- **`EntityRecord::id` widens to u32** (§8a, ADR-018 D6) — the constraint that held it at u16 was
  that one datagram had to hold everything, and §5b removes it.
- **Relationship, not ownership** (§8b): **two bits of `statusBits`**, viewer-relative, giving the
  icon sheet's OWN/ALLIED/NEUTRAL/HOSTILE channel for **zero extra bytes**. An owner id per record
  would cost four bytes on every entity to answer a question the client asks once.

**The baseline is what was *sent*, not what the world was** (§2b) — the subtlety the whole slice
turns on. Under culling the client's picture is a *subset*, so delta-encoding against the grid's
true state would describe changes the client never had a baseline for. The session host keeps,
per client and per viewed grid, a ring of the **views it transmitted** for `BASELINE_RING_TICKS`
(32, 1.6 s). **No ack in the ring ⇒ keyframe** (§2c), unconditionally: there is no partial-resync
mode to get subtly wrong.

**Truncate by priority, never refuse** (§6). Fill the tick's budget from U3d-a's ranked list;
what does not fit keeps its place for the next tick. **`TICK_BUDGET_BYTES` is a bandwidth figure,
not the datagram size** — the 1,152-byte datagram cap never moves; what moves is how many of them
a tick may use (§5b). When tier 0 alone exceeds the budget **the budget loses** and the overrun is
counted as `interestOverrun` beside `tickOverrun` (§5c): culling a player's own fleet to hit a
bandwidth number is the one outcome this ADR exists to prevent.

**`leftInterest` is an explicit id list** (§5e). A record absent from a delta means *unchanged*; a
record that has left the interest set has to be *named*, or the client leaves a ghost hull frozen
on the plane forever.

**Accept:** `NeuronCoreTests` — every new message round-trips, and a part that names a baseline
the peer does not hold is refused rather than misapplied. `NeuronServerTests` — the ring evicts at
32 ticks and the eviction produces a keyframe; an ack older than the highest is ignored; a delta
against a *sent view* differs from one against the world where culling made them diverge, which is
§2b as a test rather than as a paragraph. `selfTest` — **a culled grid over the real loopback in
which every owned and selected ship is present in every tick's union of parts** (the guarantee is
a test, not a promise) and `culledCount` is non-zero and correct. **`WriteSnapshot`'s refusal is
replaced here and not before** — until this slice it stays the loud failure T2's accept tests.

#### U3d-c — The client half, and the honest sentence

The client acks (§2d — **apply each part on arrival for freshness, ack tick *T* only when every
part of *T* has arrived**), applies deltas against its own retained baseline, takes a keyframe as
a mid-session join on the `Bulk` stream, and retires `leftInterest` ids rather than leaving them
frozen.

**`culledCount` renders through the icon ladder's existing counted-chip rung**
(`tactical-icon-system.png` §5, §5d) — the same affordance that already answers *"there are more
ships here than there are pixels"*. The player is never told a grid is empty when it is not; they
are told **how many** they are not being shown.

**Accept:** `NeuronClientTests` — parts applied out of order leave the same view as in order; a
tick with a missing part is applied but **not acked**; a keyframe replaces rather than merges;
`leftInterest` retires a hull and no ghost survives. Visual checkpoint: the counted chip reads a
real `culledCount` on a grid over budget.

> **The counted chip landed 2026-08-23, and the rung it was to render through did not exist.**
>
> Both this slice and [ADR-022 §5d](ADR/ADR-022-interest-and-delta.md) say `culledCount`
> *"renders through the icon ladder's **existing** counted-chip rung"*. It does not exist: the
> density ladder (`tactical-icon-system.png` §6) is not built, so there was no rung to reuse and
> `NeuronClient/CountedChip.h` is it, built here and waiting for its second caller.
>
> **And it could not have been that rung anyway**, which is the finding rather than the
> inconvenience. A density merge knows where its group is and draws *"an extent outline plus a
> count"*; a **culled** entity is one the server did not send, so the client holds a number and
> nothing else — no position, no extent, not even a bearing. Drawing it on the plane would
> invent exactly the *"position the client cannot justify"* that the print forbids in the same
> sentence. So the chip is a **screen-space statement about the feed**, sited with the readouts
> that are about the connection rather than about the world.
>
> Zero draws nothing. §5d's rule is that a player is never told a grid is empty when it is not;
> it does not ask for a chip reading "none hidden" on every fully-sent frame. And the player
> never has to wonder whose hulls are behind it: §5a guarantees owned and selected ships are
> never culled, so the answer is always *somebody else's*.
>
> `RenderScene::culledCount` is the seam, filled in `BuildScene` from the newest frame's header
> rather than from the interpolated sample — halfway between "9 hidden" and "11 hidden" is a
> number the authority never stated.
>
> **What is still owed is the visual checkpoint**, which needs a grid over budget on a real
> client and therefore a GPU and a person. It joins the R1 queue rather than being counted here.

---

**Built (2026-08-22).** One fail-closed schema bump, as ADR-018's Consequences require, and
`PROTOCOL_VERSION` went to 4 beside it — `WireType::Snapshot`'s payload stopped being opaque game
bytes, so a build that predates this would read a `DeltaHeader` as the game's old snapshot header
and find a plausible tick with everything after it shifted. That is exactly what the version
exists to refuse, and it is belt and braces beside the two hashes.

**The envelope changed owner, and that is the shape of the whole slice.** GameLogic used to own
the snapshot payload from its first byte. It now owns two things: the **entity record's meaning**
(`MakeShipRecord`, the `statusBits` bits) and the **tick tail** (`WriteTickTail` — the order
records and the session's high-water mark, opaque to the engine exactly as a summary frame is).
Everything else — the delta header, the records, the baseline ring, the parts, the keyframe — is
the engine's, because interest and delta are the session host's job (§1) and `EntityRecord` was
always NeuronCore's type. `Simulation::WriteSnapshot` is gone; the seam is `RankRelevance` +
`WriteEntities` + `WriteTickTail`.

**`RankRelevance` grew a return value the ADR does not name**: how many of the front are
guaranteed. The engine cannot work that out for itself — which ships are tier 0 is game
semantics — and §5c's "when the guaranteed prefix alone exceeds the budget, the budget loses"
is unimplementable without it.

**One message the ADR does not name had to be invented, and it amends ADR-016 §7.** §4's
`InterestQuery` is a focus, an extent and a selection; §7 says the server has no business
holding any of them. Both cannot stand, and §5a's guarantee — owned **and selected** ships are
never culled — is the one that decides it: a server that is never told the selection cannot keep
it. So `ViewFocus` (C→S, unreliable, on change) carries the camera and up to
`MAX_VIEW_SELECTION` ids, and `SnapshotSender` holds the latest. **The simulation still learns
none of it**, which is what §7's sentence was protecting: the focus lives on the session and
`World` gains nothing.

**`ShipId` and `EntityRecord::id` widened together**, because Ids.h's reason for the first being
u16 was that it matched the second. Two things fell out. The registry's durable-load duplicate
check was a bitset indexed by `ShipId` — free at 64k bits, half a gigabyte at u32 — and is now a
sort. And `Neuron::EntityId` was introduced as a named alias, because the client's seams passed
entity ids and grid ids side by side as bare `std::uint16_t` and the compiler had nothing to say
about the dozens of sites that had to widen together.

**Two defects the widening exposed, both real and both older than this slice.**
`OrderSubmitBytes` still charged two bytes per ship id after the writer had widened to four, so
the size helper and the wire disagreed. And `ClientApp::BeginContextAction` passed
`INVALID_ENTITY_ID` as `PickPoint`'s **radius in metres** — a 65 km pick floor, so everything on
the grid was "under the cursor", and the comment promising "the same pick the selection uses" was
not true. The widening turned 65 km into 4.29 million, which is how it was found; it now passes
the camera's screen floor like the selection does.

**One transport surprise, and it is the kind that only shows up over a real socket.** QUIC does
not put a stream on the wire until something is written to it, so the client's bulk stream —
opened, started and then silent, because on that channel the **server** speaks first — was a
stream the server had never heard of. Every keyframe was refused and a joined client watched
nothing at all. `QUIC_STREAM_START_FLAG_IMMEDIATE` is the fix and it is load-bearing rather than
an optimisation.

**Verified:** Debug and Release x64 build clean and warning-free. 1,054 tests pass on both —
`NeuronCoreTests` 76, `GameLogicTests` 401, `NeuronClientTests` 531 (10 new in `DeltaTests.cpp`
for the receiver: baseline refusal, parts out of order, whole-tick acking, keyframe replacement,
`leftInterest` retirement), `NeuronServerTests` 46 (new: truncation with an honest
`culledCount`, the guarantee overriding the budget, ring eviction producing a keyframe, and a
stale ack failing to walk the baseline backwards). The headless self test passes end to end, and
its counters show the steady state the design intends: **3 keyframes over 124 ticks** — one per
join, deltas after.

**What this slice did *not* do**, so U3d-c's scope is not overstated: `culledCount` reaches
`ReplicatedView::CulledCount()` and stops there. ~~Nothing draws it yet. The counted chip is the
whole of what U3d-c has left~~ **— and the chip landed 2026-08-23, so U3d-c is done and U3d with
it; what is left is its visual checkpoint (R1's queue).** The client's ack, keyframe and
delta-apply paths — which the build order lists under U3d-c — landed here because the wire
cannot be tested without a reader.

**What this slice unblocks, stated so it is not rediscovered:** **shared grids** (U3c ran on
disjoint ones and ADR-018 D3 gates the rest behind exactly this); **A11's remainder**, the
`EntityRecord` widening D6 staged to wait for it; and **NET-5's open half** — fan-out, datagram
scheduling and client apply at 1,024 become testable for the first time, because a world past the
cap can finally be serialised. **R10's wire half should be scheduled with this slice, not after
it.**

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

~~**Still owed by U4, and it is the client half:** the route feeder (Dijkstra over the gate
graph, one order per completed hop — the pure half of it, search and route-solve, is
`UniverseRoute` and already built), route progress on the HUD, the STATIC-family tactical icon
and map glyph, and the halt in the event record, which is a *client* fact today — the server
sees a fleet arrive at a gate and cannot know a route existed. The client's pre-check cannot
judge a jump either — though **half of that sentence stopped being true on 2026-08-21**: the
client's view was built from ids alone, so no `Dock` or `Warp` had ever been pre-checkable, and
`MakeValidationView` now fills the marks, the station, the site and the hold room. Dock
pre-checks properly (which is what made T2's approach chain fire at all); `Warp` still cannot,
because `reachableAnchors` and `jumpAnchor` are the two fields nothing on this side fills, and the fields
those need arrive together with the surfaces that raise them.~~

**Built (U4's client half, 2026-08-23).** A fleet crosses a route the player drew on the map.
`NeuronClient/RoutePlan.h/.cpp` is the feeder — a fixed sequence of legs, one sendable at a
time, advancing on the authority's own `OrderProgress::finished`; `WorldView::BuildRoutePlan`
is the seam that composes them; `ClientApp::SetRouteDestination`/`FeedRoutePlan` press it and
pump it, and the map's SET DESTINATION is wired to the first. `Warp` pre-checks now: the two
fields nothing filled are filled, which took **one function asked twice** rather than two that
agree — `ReachableAnchors` came out of `WorldRegistry::ReachableFrom` into `UniverseRoute` so
both halves call it.

Five things are worth reading rather than inferring from the diff.

**The client may not spell `Warp`, so the game composes the orders.** A `RouteLeg` is a kind
and an anchor, both opaque and both echoed. That is not fastidiousness: a hop across a gate is
*two* orders — warp to the gate on this side, then warp through it — minus the one a fleet
already standing on the gate does not need, and which of those a route needs is a fact about
gates. `RoutePlan` would feed a chain of anything; what makes it a route is entirely on the
other side of the seam.

**A leg carries the hop it belongs to, and that field is the HUD not contradicting the map.**
The panel lists a route in *jumps* and the plan holds it in *orders*, so a chip counting legs
would read `7/10` beside a panel that said five. Only the half that pairs the orders knows
which two are one jump, so it stamps the number and the client reads it.

**The hold is the honest half of the feeder, and it is a named limitation rather than a bug.**
A client can only pre-check an order for ships in its own scene, and *every leg takes the fleet
off the grid this client is watching*. So from the second leg the local pre-check answers
`UnknownShip` — which here means "not on the grid I am watching" rather than "no such ship".
The plan therefore **holds and retries** on that one reason and halts on every other, and the
chip says `WAITING` rather than `STOPPED`. What lifts it is the view following the fleet
(U3b's client half / U6's auto-follow), not more work here.

**SET DESTINATION greys on a fleet as well as on a system**, which U5a's draw did not: the
button was lit whenever a system was selected, while the handler refuses without ships. The
map is a screen a player can reach with nothing selected at all — which is not true of the
command row — so the two now read one flag.

**ADD WAYPOINT stays drawn and dead, and the reason narrowed.** U5a refused both buttons
because the feeder did not exist. A waypoint's legs start from the *previous waypoint* rather
than from the fleet, so serving it means handing the game a list of systems to string together
— a change to both route seam calls. That is U5's remaining route work, not U4's feeder.

**Two of the four owed items are reported rather than built, and both are blocked on
something real** (see the two notes below): the halt in the event record, and the
STATIC-family icon.

**The halt cannot go into D19's event record, and ADR-016 §9a.1's instruction to put it there
cannot be carried out as written.** The record is per-commander at the *universe* layer —
server-side, beside the transfer bus and the rosters — and a route exists only in one client's
memory, by §8's own choice of "no schema change, no server work". So emitting into it means
either a client→server message (the server work §8 refused) or a client-side second record.
The second is worse than it looks: **the halt worth logging is the one the client is not there
for.** §8's priced cost is *"a disconnected player's fleet halts at the next gate"*, and the
client that would write that entry is the one that went away. Every other halt happens with
the player watching, where the toast and the chip already say so, and an away-log line about
an event they witnessed is not an away-log line. What the tree *does* record is the arrival:
`EventKind::Arrived` fires wherever the fleet stops, so the away-log can already say "your
fleet reached KIL-7". What it cannot carry is the *intent* — "of fourteen" — and intent is
client-side until something server-side holds the route. That is exactly §8's named future AI
commander, and this is the second thing that waits on it.

**The STATIC-family tactical icon is blocked twice over, and the second one is the surprise.**
The icon *system* (`tactical-icon-system.png`) is unbuilt — U3d-c established this when the
counted chip turned out to have no ladder rung to render through — so there is no family for a
gate to join. But the client also does not know an entity's hull class at all: `SceneEntity`
carries an id, a plane position, a pick radius, two gauges and a status byte, and nothing on
the wire tells it what shape a thing is. So the icon needs a replicated field as well as a
system, which makes it a slice rather than a line, and it is recorded on U6 rather than
pretended at here.

**Not driven over the wire, on purpose.** A jump is 400 ticks by design, so a loopback
scenario would add twenty seconds of wall clock to a gate that runs on every push in two
configurations, to exercise an order path the dock already covers. The crossing runs instead
in `selfTest`'s device-free half, ticked as fast as the CPU allows in the shipping binary:
the gate stands on its grid, a fleet at it is let through, a fleet across the grid is refused
`NotAtGate`, and the crossing lands in the system on the far side.

### U5 — Strategic map v1 *(depends only on U1 — runs in parallel with U2–U4)*
**Gate (ADR-018): D7 is delivered — [ADR-020](ADR/ADR-020-ui-architecture.md); the upload ring
is re-sized; A20's instruments still owe their run.** ~~the upload ring and fixed GPU budgets
re-sized from the corpus caps (1,024 entities / 2,500 nodes)~~ **Done 2026-08-23** —
`NeuronClient/UploadBudget.h` derives the per-frame segment from the ceilings the renderer is
built for, and **this slice is why it could not wait**: the 256 KiB constant it replaced was
sized for the tactical view, the map's instances alone are four times that, and a short ring
makes a pass drop its stream *entirely* — so U5's own acceptance ("the full 2,500 render inside
the frame budget with the `Ui` span proving it") would have been measured against a blank screen.
`MAX_MAP_NODES` and its label allowance are the client's statement of what it is built to draw;
a map that outgrows them is a config change rather than a rebuild. **Still owed: spike 3 and the
S5 frame check**, both of which need a GPU and a person, so this slice measures the map rather
than the MVP's constants only once somebody has run them. The screen is built as an **engine surface fed neutral
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

~~**Still owed by U5, and it is most of it:** the screen.~~

**Built 2026-08-23 as U5a — the seam and the device-free half.** `NeuronClient/MapView.h` is
D14's neutral graph and the row types the panels print; `NeuronClient/MapScreen.h/.cpp` is the
screen — zones, camera, cull, layout and six hit tests; `Outpost/ReplicatedWorldView` fills all
five seam calls from the committed bake; `ClientApp` draws it and the TACTICAL ⇄ MAP handoff
runs. Thirty-eight tests over the layout, the camera, the graph and the hit tests, plus a
device-free run of the whole seam over a **real 2,500-system bake**.

**Five calls in three shapes**, which is ADR-020 §6's contract spent the way the station
surface already spends it: two asked-once builders (the graph, the overlay list), one at
summary rate (the legend), two pure query functions (a system's facts, a route). The route
call takes only a *destination* — the origin is the game's, because the client holds a grid id
and "which system is that grid in" is a fact about the universe.

**The map is the first surface with a camera rather than a zone table**, which is ADR-020 §7's
declared overflow rule for it (*"scroll the panels; the graph is a viewport"*) taken literally:
pan and pinch are arithmetic on three floats, the pinch holds the point under the fingers, and
a node off screen emits nothing. It is also the **first and only consumer of
`GestureState::pinchScale`** — I1 built the pinch and nothing had a use for a zoom until now.
The gesture's ratio is measured from where the pinch *began*, so the camera takes the change
since last frame rather than the state itself; feeding the state in directly compounds it into
an exponential zoom, which is the kind of defect a screenshot does not catch.

**Three findings came out of building it.**

*The print predates the input reversal.* `strategic-map.png` was authored 2026-08-08 with ~24 px
overlay rows and ~20 px checkboxes — mouse sizes — and ADR-020's 2026-08-22 amendment made touch
primary while keeping the 48 px floor for *every* interactive widget. So every pressable thing
here is sized through the floor and the print's own heights are a floor away from what ships.
At 1.6× on a 900-pixel window the rail then wants more height than there is, and the ruling is
that the **legend** gives way — it is a readout *of* the overlay, where an overlay list missing
SECURITY BAND is a control the player cannot reach. The floor also meets a 6 px system pip in
the graph, which is why `HitMapNode` resolves to the **nearest** target rather than the first:
two adjacent systems can both be under one finger, and first-found would let bake order decide.

*Colour had to become a class.* The first draft carried a packed `tintRgba` across the seam,
on D14's *"colours arrive as data"*. That is a colour which ignores the player's colour-vision
palette — on the one screen whose whole subject is a coloured overlay. It is a `StandingColour`
now, resolved by the client through its own palette, and the set is closed at four because
those are the four `ContrastAudit` proves clear the floor in every palette. The security
gradient becomes three bands and the exact number still reaches the player in the badge, which
is `strategic-map.png` §2's own argument about categorical and continuous fills.

*The gate-link budget has no headroom, exactly.* `MAX_MAP_LINKS` is 3,000 and the committed
2,500-system bake produces **exactly** 3,000 undirected links — measured, not estimated. It is
now declared once, in `UploadBudget.h` beside `MAX_MAP_NODES`, so the number the seam accepts
and the quads the renderer has room for cannot drift; and the builder logs when it drops one,
because a map quietly missing a gate is a map a player plans a route around.

**What U5a deliberately did not build, named rather than left to be discovered.** The search
box is drawn dead: `TextEditState` exists and is wired to no surface, and a box that took focus
and then swallowed keys would be worse than one that visibly cannot. ~~SET DESTINATION and ADD
WAYPOINT are drawn and refused~~ — the map plans and the client feeds the queue one jump at a
time (§3, ADR-016 §9a.1), and that feeder is U4's; sending the first hop as a bare warp would be
a different promise from the one the button makes. ~~Fleet markers and VIEW-on-presence need
U3b's client half.~~

**Three of those four have since landed, and the list was one entry short.** SET DESTINATION
went with U4's client half and the markers and VIEW with U3b's, both on 2026-08-23. **ADD
WAYPOINT was not named and should have been**: a waypoint's legs are planned from the previous
waypoint rather than from the fleet, so serving it means handing the game a list of systems to
string together — a change to both `SolveMapRoute` and `BuildRoutePlan`, neither of which takes
an origin. It is U5b's, not U4's. And **search stopped being blocked the day T3 landed**: the
wing-rename control built the text-entry surface whose absence is the reason above, so what
search needs now is a field on this screen rather than a mechanism anywhere. Label de-confliction — the print's four-candidate placement — is a budget
spent in visible-node order instead, because de-confliction needs a glyph metric and a look.

**Still owed: U5b — two accepts that need a GPU, and two features that do not.** The features
are **ADD WAYPOINT** and **search**, both described above and both buildable now. The accepts
are the visual checkpoint against
the print at region level over the real bake, and the frame-budget measurement with the `Ui`
span proving it.

**Both are further along than "owed" suggests.** The clustering half of the checkpoint is
already mechanical: the device-free run asserts that **no two constellation discs overlap on
screen** at the fit over all 250 of the corpus's constellations, which is U1's invariant
asserted in pixels rather than in metres. And the CPU half of the frame budget is measured:
projecting and culling the whole 2,500-system graph — 3,000 links, 250 hulls, 200 labels —
takes **0.03–0.04 ms per frame** at 1440×900, which is about half a percent of a 16.6 ms frame.
What U5b's measurement still owes is the *GPU* half: whether 17,548 `UiInstance` quads through
one upload and one draw hold the same budget. Two things in the projection had to be rewritten
to get there and both are recorded in the file: the hulls accumulate in one pass over the nodes
rather than one pass per constellation (600,000 comparisons a frame, at the cap), and the route
line finds its endpoints by binary search over a run the builder already leaves sorted.

### U6 — System view and focus polish
**Prerequisite: the system-view print (D1) — drawn 2026-08-21.** The source is in the corpus and
its design calls are in ADR-016 §9; what stands between it and "agreed" is the plate export and
four owner rulings, listed with the deliverable below.
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

- ~~**D1 — System-view print.**~~ **Drawn 2026-08-21**, in the station family's format and
  against ADR-016 §9. **Neither the source nor the plate is in `ScreenPrints/` any more**
  *(2026-08-22)*: D1 is tracked upstream and the plate is owed there, which is what
  [ScreenPrints/MANIFEST-1.0.md](ScreenPrints/MANIFEST-1.0.md) records when it lists what is
  correctly not in this corpus. **U6 is not blocked by that** — the print's decisions were
  lifted into [ADR-016 §9](ADR/ADR-016-procedural-universe-and-warp.md) the day it was drawn and
  are normative there; what an artefact would still buy is a plate to check the built screen
  against, and it buys it upstream.

  **What the print decided**, with the three that reach past the screen recorded in
  [ADR-016 §9](ADR/ADR-016-procedural-universe-and-warp.md): anchors are targets and
  everything else is backdrop, so the screen says what is clickable by how it draws it; **sites
  are a fourth anchor kind** drawn from the bake and the epoch index rather than from a message,
  which this deliverable predated; and the layout **cannot state distance, so it states time** —
  evenly spaced rings, no scale bar ever, and an ETA from the same arithmetic the tactical ghost
  prints. The other three: warp is the tactical grammar unchanged, fleet markers are counts at
  places from the summary family, and a warp issued here is a single hop with routing left to
  the map.

  ~~**Open — four rulings owed before U6 builds:**~~ **All four answered 2026-08-23**, recorded
  at [ADR-016 §9b](ADR/ADR-016-procedural-universe-and-warp.md) with the measurements that
  produced them. **U6 has no design gate left.**
  1. **Ring spacing past eight anchors** → **rings have a capacity and overflow outward**, in
     bake order so a layout is stable between sessions. §7's *"the committed universe has
     systems with more"* understated it: the bake says **70.6 %** of systems exceed eight, so
     this is the ordinary case rather than an edge.
  2. **Where sites sit** → **their own outer ring**, and this is *half of ruling 1's answer*:
     taking sites off the main rings drops the over-eight share from 70.6 % to **34.9 %**, so
     the two questions were never independent. The reason not previously written down is that
     sites **move** — `SiteEpochPlacement` is per epoch, so placing them among the planets makes
     the planets' ring re-lay itself overnight.
  3. **One marker or two at a mixed anchor** → **one marker with a split count**, which keeps
     one mechanism across two resolutions: the strategic map merges docked and on-grid because
     at that resolution both mean "there", and the system view splits the same single mark
     because there the difference takes different verbs.
  4. **Does a gate name the far side?** → **yes, `GATE → KIL-7`.** It costs nothing — the client
     holds the topology from boot, so it is an index lookup and no message — and four gates a
     player cannot tell apart are four anchors they must guess between.
- **D2 — `Gate.obj` + icons.** Ring/portal silhouette, radially symmetric, the shared
  five-material palette; STATIC-family tactical icon and map glyph. Structure stands in from
  U4 until this lands. **Half delivered 2026-08-20: the mesh, as `Stargate.obj`** — it arrived
  with U4 rather than after it, so the Structure stand-in was never needed. Ring/portal as
  specified, 1,888 vertices and 1,144 triangles, on the corpus palette (one sixth material in
  the export was authored onto `accent`, whose colour it already was — see
  [ADR-016 §10](ADR/ADR-016-procedural-universe-and-warp.md)). ~~**The icons are still owed**
  and land with U4's client half, beside the route progress they sit next to.~~ **Corrected
  2026-08-23: they did not, and could not.** U4's client half landed without them because the
  icon *system* is unbuilt (`tactical-icon-system.png`'s density ladder — the same thing U3d-c
  found had no rung) **and** the client does not know an entity's hull class at all:
  `SceneEntity` carries an id, a plane position, a pick radius, two gauges and a status byte,
  and nothing on the wire says what shape a thing is. So the icons need a replicated field as
  well as a system, which makes them a slice rather than a line. Recorded on U6 and at
  [ADR-016 §10](ADR/ADR-016-procedural-universe-and-warp.md).
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

  **Its implementation slice is [U3d](#u3d--interest-and-delta-adr-022-adr-018-a14--the-slice-a14-scheduled-and-nobody-wrote-down)**,
  written 2026-08-22. A14 scheduled it for "after U3c" and no build order absorbed it, so for a
  day the corpus referred five times to a slice that existed nowhere. That is the gap this
  deliverable's own wording created — *the ADR was the deliverable*, so delivering it struck the
  row through while the work it schedules had no home.

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
