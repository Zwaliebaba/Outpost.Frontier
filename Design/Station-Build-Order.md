# Station Build Order — the Docking Phase

**Status:** Session output 2026-08-19 · **T1 and T2 built in full** (2026-08-21) —
**🏁 H0 is met: every named criterion is covered.** Docking, the transfer bus, undocking and its fifteen seconds,
the parking ring and the event record are in the sim; the identity cluster, the per-client
`SnapshotSender`, the summary family's frame and the station command's own path onto the
acked order stream are on the wire; and `selfTest` drives the whole headless loop over real
QUIC loopback — dock, roster, undock, respawn, shimmer bit, roster follows. **T2's client
half landed 2026-08-21**: the DOCK context action, the approach chain and its chip, the
DOCKED blocks, the dock/undock toasts, the protection shimmer and the ~1 s transit fades.
**One thing is owed and it is not code** — nobody has looked at the four new marks on a
screen (R1), and they are listed below as such. **P1 exists** — `ScreenPrints/
station-screen.png`, landed 2026-08-19 — and its four open review questions were answered
2026-08-20 ([ADR-017 §6a](ADR/ADR-017-station-docking.md)), so **T3 has no design gate left**:
what remains of this phase is screen work. The design it delivers is
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
combination, watch it park itself clear of the old one — on screen).

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
`Undock` (≤ 64 ships, formation) and `AssignWing` (emergent 1..255, docked-scope,
`NotDocked`). Undock applies as: formation solve at the anchor's authored undock point and
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
[ADR-022](ADR/ADR-022-interest-and-delta.md), whose §6 replaces this refusal with priority
truncation, and whose §1 is why the sender T2 writes must be **per client from its first
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

**A fleet at the snapshot cap, round-tripped.** `TheMvpFleetFitsOneDatagram` puts 41 ships on
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
reason worth keeping in the record: this simulation's fleet is on a scripted patrol, so it is
never stationary and no disconnect would make it so. "Halted" is not a property this world has.
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
**Accept 🏁 H1:** the owner's loop in one sitting — fly to the station, DOCK from the
context action, open the hangar, move three ships into a new wing, select a mixed
composition, UNDOCK, and watch the new fleet appear at the undock point, shimmer, and park
itself on a free berth clear of a fleet already parked; visual checkpoint against P1;
rename a wing and see it survive a client restart (user layer); undock at an unviewed
station from the roster block and jump to it with VIEW.

**T3 now gates a slice in another phase, which it did not when it was written.** The economy's
**E5b** — the CARGO and REFINERY tabs — are *sibling tabs of the tab row this slice brings*
([Economy-Build-Order](Economy-Build-Order.md)), so building them first would mean inventing the
frame P1 already specifies. Two consequences worth carrying: T3's real cost is higher than its
own accept implies, because it is also the first exercise of ADR-020's surface stack — which is
**delivered as a design and not built**: `NeuronClient/` has no `Surface`, no input router and
no focus model today, and the HUD is a fixed set of panels. And E5b's own data half no longer
waits on any of it — **E5a landed 2026-08-21**, so the economy's four summary kinds are decoded
and held client-side before a screen exists to draw them, which is the arrangement U3c-a had and
for the same reason.

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
