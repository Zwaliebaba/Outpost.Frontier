# Station Build Order — the Docking Phase

**This document does not sequence** *(2026-08-22)*. It says what each T-slice contains, what its
accept is, and — in the **Built** lines, which are most of its length — what landed and what that
cost. **When a slice is built is [Plan-of-Record.md](Plan-of-Record.md)'s**, which sequences
across all three phases and the work that belongs to none of them. T3's remaining half needed the user
layer; **N2 built it on 2026-08-22**, so what remains of T3 is the rename control and the visual
checkpoints rather than a dependency.

**Status:** Session output 2026-08-19 · **T1 and T2 built in full** (2026-08-21) —
**🏁 H0 is met: every named criterion is covered.** Docking, the transfer bus, undocking and its fifteen seconds,
the parking ring and the event record are in the sim; the identity cluster, the per-client
`SnapshotSender`, the summary family's frame and the station command's own path onto the
acked order stream are on the wire; and `selfTest` drives the whole headless loop over real
QUIC loopback — dock, roster, undock, respawn, shimmer bit, roster follows. **T2's client
half landed 2026-08-21**: the DOCK context action, the approach chain and its chip, the
DOCKED blocks, the dock/undock toasts, the protection shimmer and the ~1 s transit fades.
**One thing was owed and it is not code** — nobody has looked at the four new marks on a
screen (R1), and they are listed below as such. **P1 exists** — `ScreenPrints/
station-screen.png`, landed 2026-08-19 — and its four open review questions were answered
2026-08-20 ([ADR-017 §6a](ADR/ADR-017-station-docking.md)), so **T3 has no design gate left**:
what remains of this phase is screen work.

**Amended 2026-08-23 — §6's docked scope was lifted and it was not a station slice that did it.**
I2 needed wings as control groups, which a player can only *form* at a station is not, so
`AssignWing` now accepts ships on the grid as well as on the roster ([ADR-017 §6's
amendment](ADR/ADR-017-station-docking.md)). No wire change, no new verb, no wing table — ADR-017
§6's own "without new machinery" held. Two consequences for this phase: **the hangar is no longer
the reorganisation room by necessity**, only by convenience; and the refusal a station command
gives changed — an `AssignWing` naming a ship the view does not carry is refused `UnknownShip`
rather than `NotDocked`, because "not docked here" is the wrong sentence for a verb that no longer
needs a dock. What is *not* built is a surface for it: nothing in the client can compose an
in-space assignment, which is I3's.

**T3 has started, and it split (2026-08-22).** **T3a is built** — the client *navigates*:
ADR-020's surface stack, input router, focus, text editing and scrolling list, all
device-free and tested; the ELSEWHERE column laid out where it is pressed rather than inside
the draw; its block button live and opening a station surface; and a frame that skips the
world half beneath a full-screen screen. **T3b is built too** — the hangar's seam, its
composer, its geometry, its draw, and UNDOCK on the wire: `StationCommand` had a format, a
validator, a server path and a `selfTest` since T2 and no client line that could produce one,
and now a player can issue one. **What T3a and T3b both owe is R1's category** — neither the
station surface nor the screen that now fills it has been on a display — which makes **two**
visual checkpoints this phase is carrying rather than one. The design it delivers is
[ADR-017](ADR/ADR-017-station-docking.md); where this document and that one disagree, the
ADR wins on *what* and this one on *when*.

**Where it sits:** interleaved into the universe phase — **after U2, before U3a** (owner
sequencing). U1 and U2 land first so the bake carries the undock-point fields and the world
registry exists for the roster to live beside; the universe track then pauses while T1–T3
run, and resumes at U3a inheriting the transfer bus T1 introduces.

**Scaling baseline (owner decisions, 2026-08-19):**
[ADR-018](ADR/ADR-018-scaling-baseline.md) rides this plan too: T1 carries the
footprint-derived dock radius (D7), the `(applyTick, transferId)` bus order (D17), u32 ship
ids in every durable artifact (D6/A11), and the first event-record emissions (A17); T2's
schema cluster is the **identity cluster** — `PlayerId` plus the reserved token/resume
field (D5/A12), the schema text growing the verdict constants and check order (D9/A21) —
and its roster goes through the per-client sender (A13, delivered); T3 is gated on the
UI-architecture ADR (A19, deliverable D7 in the universe order — delivered).

The rules are the MVP build order's, unchanged: each slice independently testable, lands
green, sized at "a few days" or less, later slices assume earlier ones. Landed slices
carry a **Built** line. Test placement follows the Dependency Map: sim
truth in `GameLogicTests`, wire in `NeuronCoreTests`/`NeuronServerTests`, screens in
`NeuronClientTests`, the loopback loop in `selfTest`.

Milestones: **H0** — *the headless loop* (dock, roster, undock, park — over the real
loopback) · **H1** — *the hangar loop* (dock a fleet, recombine wings, undock a new
combination, watch it park itself clear of the old one — on screen) · **H2** — *the
lifecycle loop* (the six clauses of ADR-017 §6b walked in one sitting — added 2026-08-23).

**Amended 2026-08-23 — §6b adds T4.** The fleet design review reversed the wing lifecycle
([ADR-017 §6b](ADR/ADR-017-station-docking.md)): membership is in-space membership, the
wing forms at the undock, a dock writes memory, and a wing with nothing in space draws no
row. T4 below is its delivery — T4a the registry half, T4b the client half — and it
supersedes, in the tree, the dock-groups rule T3b's Built line records landing: that record
stands as history, and T4a is what removes the code it describes.

---

### T1 — Docking in the sim, and the transfer bus
The universe runtime gains the **station roster** (`(ShipId, class, wing)` per station —
no gauges, per ADR-017 §1) and the **transfer bus** (tick-stamped records, applied between
ticks in (apply tick, record order) — ADR-016 §4's mechanism, arriving early with dock and
undock as its first records). `World` gains the transfer seam: transfer-out preserving id,
transfer-in spawning **with** a given id — shared by undock now, warp later.
`OrderKind::Dock` (with `Warp = 4` entering reserved) validated by the shared function:
every member inside **the footprint-derived radius —
`max(DOCK_RADIUS_METRES, FormationExtentMetres(order) + margin)` (ADR-018 D7, same pure
function both halves)**, Replace-only — reasons `UnknownStation`, `NotAtStation`,
`InvalidQueueMode` appended, the check-order contract extended and pinned by the parity
matrix. The transfer bus applies in **`(applyTick, transferId)`** order with the monotonic
`transferId` stamped at filing (ADR-018 D17); in-flight records fold into the registry
hash. Rosters, logs, and transfer records carry **u32 ship ids** and key on **`PlayerId`**
(ADR-018 D5/D6 — the id enters the wire with T2's cluster). Dock, undock, wing-assign and
berth-hold events **emit into the per-commander event record** (ADR-018 A17) from this
slice. §5's protection window is implemented against the **corrected** arithmetic
(~1.2–1.6 km in fifteen Battleship seconds, not ~3 km — ADR-018 D7). Station commands as pure shared validation over a `RosterView`:
`Undock` (≤ 64 ships, formation) and `AssignWing` (emergent 1..255, ~~docked-scope,
`NotDocked`~~ — **lifted 2026-08-23 by I2**: it may name ships on the grid as well as on the
roster, and a ship it cannot find is `UnknownShip`; see ADR-017 §6's amendment). Undock applies as: formation solve at the anchor's authored undock point and
facing → per-ship `protectedUntilTick` (15 s; cleared on ingesting any player order naming
the ship; system orders exempt) → the **system-issued parking order** to the first free
berth (two rings × 12 bearings, deterministic scan, freedom = clear solved stations + no
foreign final-leg anchor in the padded footprint; all taken → hold at the undock point).
**Accept:** `GameLogicTests`: double-run bit-identity over a scenario mixing docks,
undocks, wing assigns and two same-tick undocks (which must pick different berths); the
roster in the registry-level hash, including across a teardown/recreate of the station
grid; protection stamped, expiring, and broken by a player order but not by the parking
order; a full-ring scenario holding at the undock point with no contact pair; the
validation-parity matrix over every new reason; repair-by-construction asserted (dock a
damaged ship — when gauges exist to damage — undock full).

**Built (T1, the docking half, 2026-08-19).** The vocabulary and the crossing, which are
what every remaining piece of this slice stands on.

`OrderKind` grew `Warp = 4` (reserved — ADR-016 published the number before this phase
landed, and renumbering it to close the gap would have made a written-down wire value a lie)
and `Dock = 5`. `OrderReason` grew 9–13, `CombatEngaged` inert as designed.
`OrderSubmit` carries an `AnchorId` — one field for both verbs, so the client's "act on that
structure" gesture fills the same slot whichever verb it resolves to — and it is on the wire,
with the schema text bumped for both.

`ValidateOrder` decides a Dock: Replace-only, the station has to be this grid's, and every
member has to be inside `DockRadiusMetres` — `max(DOCK_RADIUS_METRES, footprint + margin)`
over the order's **own solved formation** (ADR-018 D7), exposed as a function because the
client draws the circle the server judges against. The check order is ADR-017 §8's, pinned by
a test that breaks each rule *and every rule after it*. `ValidationView` grew optional
`shipMarks` (quantised position + class) and the grid's station: optional so a caller with
only ids can still validate the orders that only need ids, and refused rather than waved
through when a Dock is asked of a view that cannot answer where a ship is.

`GameLogic/Transfer.h` is the transfer bus, arriving with this phase as ADR-017 §9 says
rather than with warp. A world files requests during its tick (`World::TransferOut` removes
a ship preserving its id, class and wing — the seam undock and warp both inherit); the
registry stamps `(hostId, counter)` because the counter is the host's; records apply
**between** ticks in `(applyTick, transferId)` order (ADR-018 D17), which is what makes "no
world reads another mid-tick" true rather than hoped. Station rosters live at the universe
layer beside the bus, and both fold into `WorldRegistry::Hash` — a station grid tears down
with a full roster and the roster is untouched, which is the other half of "worlds forget".

Two gaps the tests found rather than review. Spawning into a *borrowed* world left the ship
out of the ship→location index, so "where are my ships" could not answer for the starting
fleet; `WorldRegistry::Spawn`/`Despawn` now exist beside `Borrow` and are the path. And a
despawn that bypassed them leaked a stale index entry past the grid's teardown, so teardown
sweeps the index by anchor — skipping the ships docked there, which do not leave with the
grid.

**Built (T1, the undock half, 2026-08-19).** `GameLogic/Station.h/.cpp` is the roster and
the commands over it. `RosterView` + `ValidateStationCommand` is the shared pure function
both halves call — no `World` in the header, which is what makes the client able to call it
— with its own check-order contract (`EmptySelection` → `TooManyShips` → `InvalidFormation`
→ `UnknownStation` → `NotDocked`) held by the same kind of test the order side has.

An accepted `Undock` files a transfer and the fleet **leaves the roster at filing**, which
is a dock run backwards: leave the source when the record is written, arrive at the
destination when it applies. That also makes a second undock naming the same ship in the
same tick a refusal rather than a race. At the apply point the fleet is solved together at
the anchor's **authored undock point and facing**, spawned with its own ids, classes and
wings, and stamped `protectedUntilTick` — fifteen seconds computed from `World::TICK_SECONDS`
rather than written as a tick count, so the window stays fifteen seconds if the tick rate
moves. A transfer record now carries a **fleet** rather than a ship, because "together, one
moment" should be a fact about the record and not about how the records happened to be
ordered — and because the arrival solve needs every member at once to place any of them.

`AssignWing` applies on the spot and files nothing: a wing is a number a ship carries, so
nothing crosses. `OrderGroup` gained `systemIssued`, and ingesting a *player's* order clears
protection on the ships it names while a system order does not — without that distinction
the parking order would disarm the fleet it parks.

**Built (T1, the parking ring and the event record, 2026-08-19).** `World::FindBerth`
scans §4's 24 candidates — two rings, twelve bearings, fanning out from the undock bearing,
inner ring before outer — and the first **free** one wins. Free means the fleet's *solved*
formation there clears every hull on the grid by ADR-015's own clearance factor, and lands
inside no other group's final-leg intention. That second clause is what makes two same-tick
undocks pick different berths with no reserved-berth state to store or hash.

The suite found the hole in that immediately: two fleets undocking on the same tick both
arrive **before any ingest runs**, so the first one's parking order was still *pending* and
therefore invisible to the second, and both were sent to the same berth. The scan reads the
pending queue as well now — a pending order is a live intention by every definition that
matters: it has been accepted, it is world state, and it becomes a group on the next tick.

All 24 taken means the fleet holds at the undock point, and that is a design position rather
than an edge case: undocking is never refused for clutter. A test fills the ring and asserts
exactly that — no parking order, the fleet still there, still protected.

`GameLogic/EventRecord.h/.cpp` is ADR-018 D19's producer, emitting on dock, undock, wing
assignment and berth hold. Three numbers and no text — a string here could not be
translated and the client already knows how to name a station — and `count` is what makes
"eight ships docked at Vesta-3" one line instead of eight. It is **outside the registry
hash**: an event describes something the simulation already did, and folding the description
in as well would make a replay depend on how talkative the build was.

**Still owed by T1:** repair-by-construction asserted against real gauges, which needs
gauges to exist first. Everything else in ADR-017's sim half is built.

### T2 — The wire and the tactical surfaces 🏁 H0
One clustered schema bump (ADR-017 §8, **widened by ADR-018 into the identity cluster**):
`OrderKind{+Warp reserved, +Dock}`, `OrderReason{9–13}` (including reserved
`CombatEngaged = 13`), `EntityRecord.statusBits`
(bit 0 = protected — `ENTITY_RECORD_BYTES` 20 → 21 and the snapshot ship cap 45 → 43, still
over the asserted floor of 41; the constant's comment is updated with the new arithmetic),
`StationCommand` on the acked order stream (shared seq/ack/reason, **u32 ship ids** — A11),
`StationRoster` at ~1 Hz (the first resident of the summary family — U3b builds beside it),
**plus ADR-018's identity fields: `PlayerId` and a reserved token/resume slot in
`Hello`/`Welcome` (D5/A12), and the schema text growing the verdict-affecting constants and
the check-order sequence (D9/A21)**. `EntityRecord.id` stays u16 in this cluster — the
allocator's u16 window holds until the delta cluster widens the record (D6). The roster is
sent **per viewer through the per-client `SnapshotSender`** (A13): ADR-017 §1's privacy
rule is a wire fact from the first message, and sessions survive disconnect for the D5
grace window. **Built 2026-08-20 (A13).** The sender is per client, the seam takes a
`PlayerId` viewer, and the roster travels in the summary family's own frame under one engine
wire type (`Summary`) with a game-side kind byte — so `FleetSummary` rides the same frame
rather than needing a second one. The composition root asks the registry where *this*
commander's ships are and sends a roster for each station a `Docked` row names, which is
what makes the frame self-consistent. **And its own path onto that stream, 2026-08-20.** ADR-017 §8 put `StationCommand` on the
acked *order* stream — one sequence, one `OrderAck`, one reason enum — and until this slice
nothing carried it: the format and its validator were written and tested, and no line in
`NeuronServer`, `NeuronClient` or `Outpost` mentioned either, so **UNDOCK could not be
commanded at all**. Both messages open `u32 orderSeq` and then a byte whose value spaces
overlap (`Move` and `Undock` are both zero), so `CommandKind` now leads the payload — the
same one-engine-type, game-owned-byte shape the summary family uses. Two prerequisites came
out of doing it: `Welcome` grew **`gridAnchor`** (PROTOCOL_VERSION 3), because a client that
could be *told* about a station still had no number to address one with —
`ValidationView::stationAnchor` was filled by `World` on the server and by nothing on the
client — and `ReplicatedShip` grew **`statusBits`**, because the protection bit was on the
wire and stopped at the view, so the shimmer had nothing to draw from. The shape itself is
held by a test rather than by a comment: `EverySessionIsServedItsOwnSerialisation` joins two
clients over real loopback and asserts the simulation was asked to write **at least as many**
snapshots as the two of them received — a count that sits at about half under a
broadcast-shaped host, so it fails on the shape rather than on a symptom.
Client: DOCK as a **context action on the station structure** (the command row stays as
printed), the client-feeds approach chain (Move to perimeter → Dock when every member is
in radius, surfaced as a DOCKING chip), the ~1 s dock/undock fades in the existing overlay
vocabulary, protection shimmer from the status bit, **DOCKED roster blocks** (station name,
count, STATION button stub), toasts on dock and undock complete. Presence gains the docked
clause (ADR-017 §7) — the view may stay on a grid where only docked ships remain.

**Built 2026-08-21.** Seven pieces, and four of them turned on the same discovery.

**The readiness test is the pre-check itself, and that is the whole of the approach chain.**
`ApproachChain` never learns what a dock radius is, holds no geometry and measures no
distance: each frame the client composes the chained order exactly as it will send it and
asks `WorldView::PreCheck`, and the frame the answer stops being a refusal, the order goes.
The condition the client waits on is therefore *the same function the authority judges with*
— ADR-014 §3's parity rule doing a second job, with no second definition of "close
enough" to drift from the first. The approach *leg* is aimed at the station itself rather
than at a computed perimeter point, for the matching reason: ADR-026 already slides a
formation clear of a hull it cannot occupy, so aiming at the station is aiming at its
perimeter, and a perimeter this side computed would be a second piece of geometry to keep in
step with the first. The chain holds its own copy of the fleet, because the selection is a
live thing the player keeps editing; it cancels on a refused leg, a member leaving the
world, an order the player gives themselves, and the link going away.

**The client had summaries arriving and nothing reading them.** `ClientConnection` has
framed, ordered and queued `Summary` payloads since T2's server half, and `PendingSummaries()`
had exactly one caller in the tree: `selfTest`. So the client knew where its ships were and
could not say so. `WorldView::ApplySummary` is the missing seam call — opaque bytes in,
the game decodes them — and with it the DOCKED blocks, the counts and the toasts all fall
out of one arrival. **A frame is a complete statement**, so the block list is replaced
wholesale rather than merged: the writer stops sending a roster for a place with none of your
ships in it, and silence is indistinguishable from "unchanged", so a merge is a hangar that
never empties. The blocks come from the **fleet summary rows** and the ship lists from the
rosters, which matters because a roster is what gets dropped first when the frame runs out of
room — a station whose roster did not fit still draws a correct block, and only the hangar
behind it is empty.

**Toasts are a delta, and the first summary of a session raises none.** A dock finishing
leaves no trace in the scene — the ship despawned — so the only evidence the client has
is that a count went up. Comparing two statements is not a stand-in for a wire message that
ought to exist: the roster *is* the authority's record of the fact, and an event message
beside it would be a second copy to keep in step. What the guard buys is the difference
between a state and an event: everything already docked when a client joins would otherwise
arrive as a stack of "docking complete" toasts about things that happened before the player
was watching. The **words** come across the seam (`WorldView::PollNotices`, drained per
frame) because "a fleet finished docking" is a sentence only the game can write; the level
and the dwell stay the engine's, because how loud a message is belongs to the surface.

**The shimmer is drawn without the engine learning what the bit means.**
`OverlayTuning::statusMarkBits` is a *mask*, zero by default, and the one line in the build
that says bit zero is undock protection lives in `Outpost.exe`'s config assembly. NeuronClient
draws a mark for any bit the mask names and knows nothing about any of them. The alpha pulses
on the CPU over a floor rather than to zero — a mark that reached invisible for part of
each cycle would read a protected ship as unprotected at the wrong glance, which is worse than
no mark because it is a wrong answer rather than no answer.

**The fades generalised, and the generalisation is the honest version.** A dock is an entity
disappearing and an undock is one appearing, and the *engine can see both without being told*:
an id in last frame's scene and not in this one has left the world. So `EntityTransitList`
diffs the scene, and the same ~1 s ring covers a dock, an undock, a warp-out, a kill and a
ship falling out of the interest set — every one of which is honestly "something that was
there is not". Departing positions are remembered rather than looked up, because by the time
a frame notices, there is nothing left to ask. Both directions grow outward and are told apart
by which way the alpha runs; a ring collapsing inward on a docking ship would read as the ship
being crushed.

**Owed: the visual checkpoint.** Four new marks — the DOCKING chip, the DOCKED blocks, the
shimmer and the transit rings — are tested for their arithmetic and their data path and have
never been on a screen. R1 records three defects that every device-free test passed through,
and this is exactly that category: a colour written the wrong way round or a ring drawn behind
the hull it is about is a frame to notice, not a number.
**Accept 🏁 H0:** `selfTest` drives the headless loop over the real loopback — dock a
fleet, observe the roster message and the ships leaving the snapshot, undock a subset,
observe spawn, shimmer bit, parking order and its lane in the order records, protection
expiry; a mid-approach disconnect halts the fleet outside; the snapshot budget asserts still
hold at the narrowed cap and a 43-ship snapshot round-trips inside one datagram; every
pre-existing suite green. **ADR-018 additions:** the **over-cap refusal is tested loudly**
(a grid pushed past the snapshot cap refuses with a counted, logged event — the designed
behaviour until the interest/delta slice, A13/SIM-4 — now designed as
[ADR-022](ADR/ADR-022-interest-and-delta.md) and **built 2026-08-22 as U3d-b**, whose §6
replaced this refusal with priority truncation, and whose §1 is why the sender T2 writes must be
**per client from its first
line**); the roster message is observed to reach **only** its owner's connection — which
ADR-022 §1 restates as a rule rather than a test: on a broadcast-shaped sender that privacy
promise is a silent leak nothing catches, because nothing before U3c runs two clients — **and
U3c ran them on 2026-08-21, which turned the rule back into a test: the `selfTest` now asserts
that the second commander never receives the first's roster or summaries, over a real socket**;
a dock validated at fleet scale (the 41-ship
starting fleet, footprint-derived radius) round-trips with parity.

**🏁 H0 met, 2026-08-20 — the last four criteria closed.** The loop had run end to end
since T2's server half landed; what was missing was four of the named checks around it, and
they were missing quietly, which is the failure mode a milestone with an unenumerated "three
outstanding" invites.

**The parking order, in its own lane of the order records.** The undocked fleet does not merely
appear at the undock point — the authority issues it a Move to the first free berth, and that
order takes a lane in the snapshot's order area like any other. `selfTest` now finds it there,
identified by the two things true of it and of nothing the client sent: `clientOrderSeq` zero,
because a system order has no sequence to echo, and a membership of exactly the fleet that
left. It is also the only observation in the gate that proves `systemIssued` did not disarm the
protection it was issued alongside — the ships are parking *and* still wearing bit 0.

**A fleet at the snapshot cap, round-tripped.** *(Both tests named here were retired by U3d-b
on 2026-08-22, along with the cap they were about: ADR-022 §5b replaced the per-snapshot ship
count with a per-tick byte budget spread over as many datagrams as it takes, and §6 replaced the
refusal with priority truncation and an honest `culledCount`. What follows is the record of what
T2 proved while the cap was real — `TheMvpFleetFitsOneDatagram` became
`TheTickTailFitsBesideAUsefulNumberOfRecords`, and `AFleetAtTheCapRoundTripsInsideOneDatagram`
plus `AFleetTooBigForOneDatagramIsRefusedRatherThanTruncated` became
`AGridPastTheOldCapIsReplicatedRatherThanRefused`.)*

`TheMvpFleetFitsOneDatagram` puts 41 ships on
the wire and the static asserts cover the arithmetic; **neither ever encoded the cap itself**.
That mattered more after T2 than before, because the cap fell 45 → 43 when `EntityRecord` grew
its status byte, so the margin over the MVP's own content is two records — and a budget with
two records of headroom wants measuring in bytes written, not in a `constexpr` that agrees with
itself. `AFleetAtTheCapRoundTripsInsideOneDatagram` writes 43, decodes them, checks every id
survived, and checks 44 is refused, so it is the boundary rather than a number that happens to
work.

**A fleet-scale dock, through the wire, with parity.** The two tests that existed covered the
halves separately: the radius scales (24 ships, no wire), and the verdicts agree (two ships,
marks written by hand). `AFleetScaleDockRoundTripsThroughTheWireWithParity` is the whole
41-ship fleet in the MVP's own class mix, serialised and decoded, with the client's view
assembled **from the decoded records** — ids in the order the wire put them, positions through
centimetre quantisation and back. Radius and verdict both have to match, accept and refuse, and
the refusal has to carry the same reason.

**A mid-approach disconnect leaves the fleet outside.** A second client flies the approach's
first leg and vanishes mid-flight; a third joins afterwards and reads what the authority kept,
which is a better witness than the connection that left. Nobody docks, and the fleet is still
on the grid — which on this wire *is* "outside", since a docked ship leaves the snapshot
entirely (ADR-017 §1).

That last one was drafted as a check that the fleet had come to a **stop**, and it failed for a
reason worth keeping in the record: this simulation's fleet was on a scripted patrol, so it was
never stationary and no disconnect would have made it so. "Halted" was not a property that world
had. (The patrol has since been removed — the fleet stands still until its commander says
otherwise — but the check stays roster-based, because a speed at an instant was the wrong
question either way.)
What the criterion is actually about — the client that would have sent the Dock is gone, so
nothing docks — survives that intact, and is now checked against the roster over a window
rather than a speed at an instant.

Two criteria are met by exemption rather than by a check, both recorded rather than assumed:
**protection expiry** is pinned tick-by-tick in `RegistryTests` and deliberately not re-asserted
over a socket, because fifteen seconds is far longer than this gate should sit; and the
**roster-privacy observation** is ADR-022 §1's rule rather than a test, because nothing before
U3c runs two clients to leak between.

> **Closed 2026-08-21 by U3c**, and the delay was right for a reason this note did not have:
> two clients were necessary and not sufficient. Until U3c-a gave ships owners, both commanders
> owned everything, so a twin-client test would have asserted privacy and passed on a registry
> that had none. The observation is a test now — `selfTest`'s twin-client section, with the
> second commander's assertions written *negatively* — and it can fail.

### T3 — The hangar screen 🏁 H1
**No design gate left. P1, the station-screen print, landed 2026-08-19 —
[station-screen.png](ScreenPrints/station-screen.png), the artefact this slice is built
against and checked against — and its four open review questions were answered 2026-08-20 as
[ADR-017 §6a](ADR/ADR-017-station-docking.md). Gate (ADR-018): the UI-architecture ADR is
delivered —
[ADR-020](ADR/ADR-020-ui-architecture.md) — so the hangar inherits the surface stack, the
input router, focus and text editing for wing renames (atlas-charset policy, D15.1),
the scrolling list, and the composer's retained-state lifetime as a rule rather than a
per-print proposal.** The TACTICAL ⇄ STATION surface: docked roster grouped by wing with the roster
vocabulary, multi-selection, formation dropdown, UNDOCK, wing assignment (existing wings
plus "new wing" picking an unused id; names for new wings and renames in the user settings
layer, client-side only), repair/refit/market as visible stubs, handoff and keybinding in
the settings screen. Remote hangars: the screen opens for any station holding the player's
ships, viewed or not.
**T3 splits in two, and the seam is the one this repo already respects.** **T3a** is the
client that *navigates* — the machinery ADR-020 §5a said would arrive with the first screen
that needs it, and the column that reaches it. **T3b** is the hangar itself: its tab row, its
roster, the composer and UNDOCK. The split is not tidiness. Everything in T3a is arithmetic
over structs and can be held by tests on a machine with no GPU; everything in T3b is a screen
whose acceptance is a person looking at it. Landing them together would have put the first
behind the second's checkpoint, which is how a device-free defect ends up waiting on a device.

**Built (T3a, 2026-08-22).** Four types, a column, and a frame that does less under a screen.

`SurfaceStack` is §1's, and the rule that carries it is not the depth cap: a surface already
in the stack is **returned to** rather than pushed again, so `◀ TACTICAL` from the hangar and
`◀ BACK` from settings are one call and a player circling between two screens twenty times is
still one step from home. `SurfaceChange` is two ids rather than a list because at most one
surface is live, so at most one can have an exit to run — popping back past two breadcrumbs
still exits one screen, since the others ran their exits when they were covered. At full depth
the oldest breadcrumb is dropped rather than the push refused: a control that does nothing is
worse than any depth policy.

`InputRouter` is §2's, and it replaces `m_uiConsumedPress` — which said the one thing a
client with a single surface needed to say and could say neither of the other two: that a
wheel notch was spoken for while the click was not, and that a key belongs to a field rather
than to the camera. Three channels, claimed independently. The keyboard rule is the one that
had to be written down, and `ActionSurvivesTextEditing` is a **table** rather than a habit, so
adding an action fails the build until somebody has answered for it. Reading it beside
`Window`'s binding table is what makes it sound per action although printability is per key:
`PanForward` is `W` *and* `↑`, and `ResetView` is `Home`, so a focused field takes all three
on both counts.

`UiFocus` names the surface as well as the widget, so an exit clears its own field rather than
one two screens down. `TextEditState` is §3's machinery — bytes outside, codepoints inside,
and the cap in codepoints because a cap in bytes fits sixteen Latin letters and five of
something else. `UiScrollState` is §4's, and it closes `HudRoster.h`'s deferred "scrolling is
a surface rather than a bigger number": it is neither. Two additions to the input frame came
with them — `Escape` as `InputAction::Back`, an *action* because it is the one input with a
routing order in the design, and `WM_CHAR` on `Window`, assembling UTF-16 surrogate pairs
where the messages are, which is the only place the pairing is unambiguous.

**The ELSEWHERE column stopped being drawn and started being laid out.** Its blocks were
positioned inside `BuildHud` by a running total, which was fine while the button on them was
paint: nothing pressed it, so nothing could disagree with it. It opens the hangar now, so the
column's arithmetic moved into `HudRoster` as functions of the row index, `UpdateHud` measures
the column and lays the blocks out, and the draw takes what it is handed (§5.1). A test writes
out the running total the draw used to keep and asserts the replacement lands in the same
place — the move did not also move the column.

**The station surface is thin, and one part of it is blocked rather than deferred.** The
ground, the way back, and which place this is — read by anchor rather than remembered, because
the roster arrives at ~1 Hz and a hangar whose ships all left should say so rather than hold
the count it opened with. Its **tab row cannot exist yet**: ADR-020 §6's leak test forbids
`NeuronClient` learning that a tab is called REFIT, so the words have to cross the seam as
data, and that call is T3b's.

Two defects found by writing it rather than by running it. A full-screen surface has to claim
the pointer **ahead of** the left-press gate: gating on a left press first leaves the *right*
button unclaimed, and the order puck lives there, so a right-drag across the hangar would have
sent a fleet somewhere on a grid the player cannot see. And a navigation unwinds the puck as
well as the selection box — a half-made gesture must not finish itself against the next
screen.

`RenderFrame` now chooses between the two calls it already made: a full-screen surface clears
the back buffer directly, skips Opaque, Nebula and OverlayWorld and the resolve between them,
and records `Ui`. No pass is added, removed, reordered or branched. Extract goes with it; the
*network* half still runs on every surface, so returning to tactical costs no refill and owes
no settle.

**Seventy-seven tests in `NeuronClientTests`, none of which needs a device** — the surface
stack, the router's three channels and its keyboard rule, the caret's arithmetic over UTF-8,
the scrolling list, and the column's layout against the running total it replaced.

**Owed by T3a, and it is the R1 category exactly:** nobody has looked at the station surface
or the live block button on a screen. ~~Also owed, and smaller: the tactical chrome is still
built underneath a surface that covers it — a `UiDrawList` fill nobody sees, whose fix is an
early return once T3b makes that half a function of its own.~~ **Closed 2026-08-22**, the way it
was described: `ClientApp::BuildTacticalHud` is that half as a function of its own, and `BuildHud`
now chooses between it and `BuildStationSurface` rather than building the first and covering it
with the second. The cost of a screen is the screen.

Two things the extraction had to keep, and both are in its comment: the toast layer stays
**outside** the branch, because toasts are a cross-surface layer drawn over whichever ran
(ADR-020 §1); and the clock is the one thing passed in rather than re-derived, because reading
`SecondsSinceStart` a second time would animate the lanes off a different instant than the toasts
advanced on. The zone metrics are re-derived, since every one is a function of `m_uiLayout` and
`m_uiTuning` that `UpdateHud` had already resolved. The moved block writes no member state — it
reads the layout the update pass resolved and draws — which is what makes skipping it safe rather
than merely cheaper.

**Built (T3b, 2026-08-22).** The hangar: a seam, a composer, a geometry, a draw, and a verb
that leaves the machine.

- **The seam.** `StationIntent` in NeuronCore beside `OrderIntent`, because both seams speak
  it; `EncodeStationCommand` as `EncodeOrder`'s twin; `PreCheckStation` running the
  authority's own `ValidateStationCommand` over the same `RosterView`. `StationView.h` holds
  the tab row, the group rows and the ship-chip rows — §6's shape exactly — and never spells
  one of the words in them. `StationAction` is the piece that was not obvious: everything on
  the screen was already data, but UNDOCK is a *verb*, and a client that cannot know a tab is
  called REFIT cannot know a verb is called Undock either. It crosses as
  `OrderKindOption`'s shape, with `StationActionOptions` over the same `FORMATION_IDS` the
  order side already offers.
- **The composer.** `RosterSelection` — `Selection`'s sibling rather than its rival, on
  durable roster ids because a docked ship is in no snapshot. Session lifetime and reconciled
  on every look (ADR-017 §6a.2), and `WaveCount` declares the game's cap as waves *before* the
  press rather than meeting the player as an error after it.
- **The geometry.** `StationScreen` — `CommandRow`'s "laid out and hit-tested in one file"
  applied to a whole screen, which is where that rule starts paying: sixty chips, a tab row
  and a primary action are sixty-plus rects a draw and an input handler could disagree about
  independently. Overflow follows §7's declared rules for this surface — the tab row drops,
  the wing columns scroll on one shared offset, the parking diagram letterboxes.
- **The draw and the gesture.** Tap toggles a chip, a press on a header takes the whole wing
  (a press rather than a hold: a header has no competing gesture, and a dwell timer earns its
  cost on a chip), the formation cycles in place, and UNDOCK sends one wave per press. The
  button is live or greyed on `PreCheckStation`'s verdict, so it greys for the reason the
  bounce would have carried, in the same words, because it is the same function.

**Fifty-one more tests, a hundred and twenty-eight device-free in all**, mutation-tested at
each step. Two defects the tests found rather than the compiler: `StationVisibleRows` and
`BuildStationColumns` disagreed by one row wherever the slack under the last chip was at
least a chip tall and short of a chip and a gap, and `UpdateCamera` — which runs *before*
the router exists and so cannot be reached by any claim — was zooming and panning a camera
under a screen that covers the world.

**And the owner found the one nothing device-free could (2026-08-22): an empty hangar over a
full station.** The screen drew `35 SHIPS DOCKED` in its status bar and `NOTHING DOCKED HERE`
in the roster beneath it — one surface contradicting itself, which is the shape of a bug worth
reading before its fix.

The cause was in the *decode*, not the screen. `WorldRegistry::Summaries` walks the grids and
then the rosters, so a commander flying at a station while some of their hulls sit inside it is
**two rows at one anchor** — `OnGrid` first, `Docked` second — against exactly **one**
`StationRoster` record, because a roster is a fact about a station rather than about a fleet.
`ApplySummary`'s merge matched the roster to a block on the anchor alone, so the grid row
claimed it, and because the list is *moved* rather than copied the docked row was left holding
an empty one. The hangar then reported the station honestly: there was nothing in the roster it
was given.

Two things made it invisible until somebody docked. It needs a commander in **two states at one
anchor**, which no test built and no gate exercised — the family's own gate wrote a single
docked row, which is the arrangement in which the bug cannot happen. And **U3b widened the block
list to every row** where T2 had kept only the docked ones; that was right for the ELSEWHERE
panel, and it silently introduced the duplicate anchor the merge was written before there could
be one.

The status bar was a second defect with the same root: it found its block by anchor alone, took
the first, and printed the word `DOCKED` under whatever count that block carried — so a fleet
in space read as docked. It takes `StationRosterCounts::docked` now, which is the roster's own
count *before* the client's caps, so the bar and the list are one statement about one place.
Not the columns' sum and not `chips`: a ship in a wing past the column cap is counted in
neither, and a bar about a station must not be a bar about what fitted on the screen.

Gated in `RunSummaryFamilyGate` — the frame with two rows at one anchor, asserted through
`BuildStationRoster` rather than through the count, because the count was right the whole time
and the bug only ever showed in the list. It lives with the self test rather than in a unit
suite for the reason the decoder fix already gave: `ReplicatedWorldView` is in the executable
and has no test project.

**The reorganisation room landed 2026-08-22, and the note above it was wrong about why it
had not.** It said wing assignment, wing creation and renames all needed the user settings
layer. **Renaming does. The other two do not** — a wing exists iff a ship carries its number
(ADR-017 §6), so creating one is a command the authority already takes, and all a new wing's
call sign has to survive is the session that made it. Three things were being held behind a
feature that only one of them depends on, and the cost of the mistake was the one a player
reported: dock two ships out of one wing and two out of another, and there was no way to make
the four of them a wing.

Built as a second `StationAction` beside UNDOCK — `BuildStationActions` returned one where
`MAX_STATION_ACTIONS` had been four since T3, and the screen read `[0]` in five places, so
the verb had a format, a validator, a server path and a registry test and no line anywhere
that could compose one. The screen now walks its actions, and each carries its own parameter
list, option index, pre-check verdict and wave count.

**Two decisions worth having on the page.** The print's `+ NEW WING` button is a *value* of
the wing chip rather than a second control: §6 defines a new wing as picking an unused
number, so it belongs in the same parameter list, and the rect the print gave it now holds
the button that sends the command. And `StationAction` gained `consumesSelection`, because
the two verbs disagree about what happens to the composer afterwards and the engine may not
tell them apart by reading the verb.

**Names come from the composition root**, which holds seven spare call signs beside the
starting fleet's eight — the roster's row cap is what makes seven the right number, and a
`static_assert` says so. A wing whose number outlived its name (a save reloaded without the
settings layer) is given a dull generated one when its ships arrive, because a fleet the
player cannot see is worse than a fleet called `WING 9`. Gated in `RunWingAssignmentGate`,
for the reason every other client-side gate lives there.

**And then the rule underneath it, the same day**, because the button was not what the report
was about. Wing assignment made regrouping *possible*; what the player expected was that
docking part of a fleet already *did* it. ADR-017 §3 had promised as much — "the undock
selection is the composition" — and that promise was only ever true of a fleet, which is
emergent from location, never of a wing, which is the number the roster groups by. So a Dock
now forms one wing out of the ships it names, unless it names a whole wing and nothing else;
the number is the lowest the commander is not using on any grid, roster or crossing. See the
note in ADR-017 §3.

Gated four ways in `RegistryTests` (part of a wing splits; two wings merge; a whole wing keeps
its number; the wing the dock formed is the wing they undock into) and end to end through a
real client and server in `RunDockingLoop`, which is where the last link lives — a split the
roster records and the respawn then loses looks exactly like a split that never happened.

~~**Still owed by T3:** wing *renames*, which do need the user settings layer;~~ **the layer
landed 2026-08-22 as N2**, and with it the half of that debt that was not a control: a call sign
the player composed now comes back next session, and a name they put on an authored wing outranks
the content's without overwriting it — so deleting `Settings.json` restores TALON and ANVIL
exactly. Gated in `RunWingNameLayerGate`, beside the assignment gate and for the same reason.

~~**Still owed by T3:** the rename **control**~~ **built 2026-08-23**, ~~assigning to wing 0 to
disband~~ **built the same day**. What is left of T3 is the visual checkpoints above.

**The rename control, and the two things it needed that were not the layer.** T3a built
`TextEditState` and `UiFocus` and neither had a consumer for two slices; this is the first
editable field anywhere in this client, and it is what ADR-018 D15.1 meant by "editable text".
A tap on a wing header opens it, seeded with the current word and with the whole of it selected,
so replacing a call sign is *typing* and keeping part of it is one arrow key away. It closes
three ways and they are three different answers: Escape cancels, Enter commits, and **a tap
outside commits** — which is the one that matters, because touch has no Enter and a field a
finger cannot finish is a field a finger cannot use. Committing on blur rather than cancelling
is a choice about whose work is cheaper to lose.

Two channels had to arrive with it. `InputAction::Confirm` is Enter, and it is the pair that
makes `ActionSurvivesTextEditing` a real question rather than a formality: Escape is *routed*
because it means something to a surface as well as to a field, and Enter is *suppressed* because
it means nothing to any surface here. And `TextEditKey` is a **third input channel** beside
actions and characters — an action asks "was this button pressed", text asks "what did the
player mean to write", and Backspace and the arrows ask "how did they mean to move or delete".
Six actions with the same table answer would have been six entries whose question was not
really the table's; it also keeps `Home` honest, which is `ResetView` to a camera and
start-of-line to a field.

**The charset filter is asked of the atlas**, which is where D15.1 says to ask it: the atlas is
the only thing that knows which codepoints this build baked, and a second copy of that list is
how a name renders at one size and boxes at another. It is the one line of this control no
device-free test covers.

**And the station surface moved onto the gesture seam to make room for it**, which closes half
of R30. `station-screen.png` §1 says *"holding a wing header takes"* and T3b built it as a
press, with the reason written down: *"a header has no competing gesture"*. It has one now, so
the departure is undone rather than defended and both gestures land where the print put them.

**Disband, and the thing that was really blocking it.** `StationActionOptions` refused to offer
wing 0 because `BuildRoster` drew no row for it, so the button would have made ships vanish off
the HUD. The strays have a row now — last on the panel, and only when there are any, because a
wing is a thing the player made and an empty one is news while "nothing is unassigned" is the
ordinary state.

**What the stray row found on the way there is the more serious half of this commit.** Wing 0
was skipped with *"the stations are in it"*, which was true and was not the reason. A wing
number is a byte **every commander numbers from one**, and `BuildRoster` was counting *every*
ship on the grid — so a hostile fleet flying their wing 1 inflated this player's TALON row and
dragged its gauges towards a fleet they do not command. `BuildGroupMembers` had it too: a press
on TALON would have selected the enemy wing 1 with it, and then offered an order over the pair.
It needs two commanders on one grid to show, which U3c made possible and no gate had built; the
bits that tell them apart are ADR-022 §8b's, which arrived *after* this code, so the function
that could not ask read like a function that had decided not to. Gated in
`RunRosterOwnershipGate`, which is the arrangement that was missing: a frame with two
relationships in it.

**One thing N2 moved rather than closed, recorded so it is not counted twice.** The names the
player owns are written at **shutdown**, not at the keystroke — a rename is a keypress and a save
is a file rename, and doing the second on every one of the first would put the settings file in
the path of a fast typist. The cost is that a session killed rather than closed loses the names it
minted, which is the right trade for call signs and is the first thing to revisit when the
settings screen (N3) puts a display mode in the same file.

**Accept 🏁 H1:** the owner's loop in one sitting — fly to the station, DOCK from the
context action, open the hangar, move three ships into a new wing, select a mixed
composition, UNDOCK, and watch the new fleet appear at the undock point, shimmer, and park
itself on a free berth clear of a fleet already parked; visual checkpoint against P1;
rename a wing and see it survive a client restart (user layer — **its layer is built as of
2026-08-22, the round trip is gated in two suites, and the rename *control* landed 2026-08-23;
what this clause still needs is a person to press it**); undock at an unviewed
station from the roster block and jump to it with VIEW.

### T4 — The wing lifecycle 🏁 H2 *(added 2026-08-23; delivers ADR-017 §6b)*

**The design it delivers is [ADR-017 §6b](ADR/ADR-017-station-docking.md)** — membership is
in-space membership — and the slice splits along the seam this phase always has: **T4a** is
registry truth, device-free; **T4b** is the seam and the two screens that read it. Where
this spec and §6b disagree, §6b wins on *what*. The wire is untouched by design:
`StationCommand` has carried both `formation` and `wing` since T2, `Undock` starts reading
the second, and zero in it — the field's default — means "the registry decides". No verdict
changes, no check-order change, no schema-text bump.

**T4a — the registry half.** The work, enumerated:

1. **Two counts where there was one.** `WingPopulation`'s three-place walk (grids, rosters,
   bus) keeps serving `UnusedWingFor` and is renamed for what it now answers — a number is
   **claimed** — and a second, narrower count answers **members**: grids and bus only,
   because a roster holds members of nothing. Liveness questions (is this wing flying, may
   it be restored) ask members; minting asks claims.
2. **A dock forms nothing.** `WingForDockedGroup` and the `formed` logic at the Dock apply
   point are deleted; a dock writes `row.wing = member.wing` — memory, §6b.1 — and nothing
   else.
3. **The undock decides.** At filing, once per command, before the first row leaves (the
   dock note's own discipline at the other edge): `command.wing != 0` → that number;
   otherwise **restore** when the selection is uniform memory of one number and is every
   row of this station's memory of it (rejoin if it flies, restore if it does not), else
   the **lowest unclaimed** number, else — all 255 claimed — wing zero. The decided number
   is written into every `TransferMember.wing`.
4. **`AssignWing` refuses the roster.** The verb validates against `onGrid` only — a docked
   ship named in one is `UnknownShip`, the reason the verb already keys — and the apply
   path drops its roster branch. I2's grid scope is unchanged.
5. **Events.** The undock's decision emits `WingAssigned` beside `Undocked` when it minted
   or restored, so the record says a wing began without a client having to infer it.

**T4a accept** (`GameLogicTests`, the house idioms): a partial dock leaves the flying
remainder's wing untouched and writes memory on the docked rows; a whole wing docked and
undocked exactly restores its number; a pit-stop subset docked and undocked exactly rejoins
the flying number; a hand-mixed selection mints the lowest unclaimed; a remembered number
is skipped by the mint (the resurrection defect's pin); `wing = N` joins a flying N; two
same-tick undocks mint different numbers (the second sees the first's members on the bus);
double-run bit-identity over a scenario mixing all of the above; parked memory survives the
durable round trip (`DurableStateTests` — same bytes, new meaning, no format change); the
validation-parity matrix unchanged, asserted.

**T4b — the client half.** The work, enumerated:

1. **The intent carries two parameters.** `StationIntent` grows a second parameter slot and
   `EncodeStationCommand` fills formation *and* wing for an undock — a seam change, not a
   wire one; the bytes were always there.
2. **One action, two chips.** `BuildStationActions` returns UNDOCK alone; the ASSIGN pair
   retires; `StationAction` grows a second option list, and the screen binds the existing
   `assignWing` chip rect to the undock's wing parameter. The chip still cycles when the
   button is dead (T3's ruling, kept on purpose).
3. **The chip's values.** "The registry decides" (sends zero) plus the flying wings this
   client can name — `StationActionOptions` rewritten; the next-new and wing-zero values
   retire with the verb that needed them.
4. **The hangar groups by hull class.** `BuildStationRoster` columns are classes, §6a.4's
   sort inside; wing memory is drawn by nothing.
5. **The roster draws live wings.** `BuildRoster` emits a row per wing with members on the
   watched grid — the permanent zero row is struck; the strays row is unchanged. The name
   cap counts rows drawn rather than names minted, and `m_playerWingNames`' pointer-stability
   reserve widens to the id space so the cap's retirement cannot move names on screen.
6. **The rename control moves to the chip** — same `TextEditState`, one home; the hangar's
   class headers are not renameable. Client-side `FreeWingId` retires with the chip value
   that read it.
7. **Print deltas:** P1 (class columns; the composer's second chip; no ASSIGN pair) — a
   **major** bump, since "wings as columns" was a §2 call of that print and §6b is the ADR
   note the manifest requires beside a reversal — and 07a (no zero rows), sized at capture.
   Plates re-captured in the same commit, per the manifest.

**T4b accept:** `NeuronClientTests` over the chip pair, the class grouping and the two-slot
intent encode; `RunWingLifecycleGate` in `selfTest` over real loopback — dock a whole wing,
see one class-grouped pool and no row for it, undock it exactly and see the same call sign
return; undock a hand-mixed selection and see exactly one new row; `AssignWing` naming a
docked ship bounces `UnknownShip` end to end. **Accept 🏁 H2**, the owner's six clauses in
one sitting on screen: dock everything and the HUD shows blocks and no wing rows; undock a
mixed selection and one fleet appears under one call sign; dock one of its ships and the
row's count drops while the hangar shows the ship in the pool; undock it back into the wing
via the chip; dock the whole wing and its row leaves the panel; undock it whole and TALON
is TALON; nothing anywhere draws a zero row. Visual checkpoint against the re-captured P1
and 07a — R1's category, a person looking at a screen.

---

## Content & design deliverables (not slices — tracked so they cannot be quietly dropped)

- ~~**P1 — The station-screen print.**~~ **Delivered 2026-08-19:**
  [`ScreenPrints/station-screen.png`](ScreenPrints/station-screen.png). Tabs rather than
  tiles so the hangar's layout never reshuffles as station services land; wings as columns
  because the wing is the unit a player undocks; the undock composer accumulating across
  wings; and the parking diagram on the screen because ADR-017 §4 made the player a promise
  about self-parking that they have to be able to see. No hull bars anywhere — the roster
  holds no damage state, which is the one rule that shapes the layout.
  **Its §3 review questions are answered** — owner rulings, 2026-08-20, recorded as
  [ADR-017 §6a](ADR/ADR-017-station-docking.md): a wave launches when the undock point clears
  by §4's own predicate, bounded by a timeout so §4's full-ring hold cannot stall it forever;
  the composer persists within a session and reconciles against the roster on every open; no
  per-wing colour, because colour already means relationship and has already cost once; and
  ships sort class-descending then by **ship id**, not by name, because names are client-side.
  **T3 has no design gate left.**
- **P2 — Dock and undock audio cues.** Bay ambience, the dock thunk, the undock release.
  ~~Lands only after S15 gives audio its bank format.~~ **Its gate cleared 2026-08-19 when S15
  landed** — `GameData/Audio/SoundBank.json` is that format, and its own header says a cue's
  falloff, instance cap and retrigger are "edits here, not rebuilds", so a dock thunk is a bank
  entry plus a WAV plus the one line that raises the event. Still deliberately last, like D4,
  but that is now a choice about ordering rather than a dependency.

## Sequencing rationale

- **After U2, not now**: the roster's structural home is the universe runtime and the
  registry's spin-up/teardown is what undock-into-a-cold-grid leans on; building against
  the single MVP grid was priced and declined as accepted rework with no accepted benefit.
- **Before U3a** (owner call): docking is the next thing played, and it *reduces* U3a —
  the transfer bus, the transfer seam on `World`, and the reserved `Warp` value are all in
  the tree before warp starts.
- **T1/T2 split** along the seam the repo already respects: sim truth first, then its
  replication and presentation — replication bugs must never masquerade as sim bugs.
- **The print before T3**: retrofitting a screen design after the screen exists is how the
  corpus stops being the governing artefact. Same clause as U6's, same reason.
