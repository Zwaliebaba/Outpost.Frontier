# Station Build Order — the Docking Phase

**Status:** Session output 2026-08-19 · **T1 built in full, and T2's identity cluster is on
the wire** (2026-08-20). Docking, the transfer bus, undocking and its fifteen seconds, the
parking ring and the event record are in the sim; `PlayerId` and the reserved resume token are
on `Hello`/`Welcome`, and the schema text carries the verdict-affecting constants and the
check-order sequence (D9/A21). **What T2 still owes is its client half and its per-client
`SnapshotSender`** — the first is screen work, the second is what U3c waits on. T3 is still
gated on P1. The design it delivers is
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
and its roster goes through the per-client sender (A13); T3 is gated on the
UI-architecture ADR (A19, deliverable D7 in the universe order).

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
grace window.
Client: DOCK as a **context action on the station structure** (the command row stays as
printed), the client-feeds approach chain (Move to perimeter → Dock when every member is
in radius, surfaced as a DOCKING chip), the ~1 s dock/undock fades in the existing overlay
vocabulary, protection shimmer from the status bit, **DOCKED roster blocks** (station name,
count, STATION button stub), toasts on dock and undock complete. Presence gains the docked
clause (ADR-017 §7) — the view may stay on a grid where only docked ships remain.
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
promise is a silent leak nothing catches, because nothing before U3c runs two clients; a dock validated at fleet scale (the 41-ship
starting fleet, footprint-derived radius) round-trips with parity.

### T3 — The hangar screen 🏁 H1
**Prerequisite: P1, the station-screen print — designed and agreed before this slice
builds. Gate (ADR-018) cleared: the UI-architecture ADR is delivered —
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

---

## Content & design deliverables (not slices — tracked so they cannot be quietly dropped)

- **P1 — The station-screen print.** The hangar is a new full-screen surface and must not
  be invented ad hoc at T3. Owner-reviewed like every print. Required elements listed in
  ADR-017 §6.
- **P2 — Dock and undock audio cues.** Bay ambience, the dock thunk, the undock release.
  Lands only after S15 gives audio its bank format. Deliberately last, like D4.

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
