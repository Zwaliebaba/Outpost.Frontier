# ADR-017 — Station Docking: the Roster, the Hangar, and the Parking Ring

**Status:** Accepted · 2026-08-19 (owner design session) · amended by
[ADR-018](ADR-018-scaling-baseline.md) (2026-08-19): §2's dock radius is
**footprint-derived** — `max(DOCK_RADIUS_METRES, FormationExtentMetres + margin)`, same
pure function on both halves (D7); §5's protection arithmetic corrected (fifteen seconds
covers ~1.2–1.6 km for a Battleship, not ~3 km — the window is re-checked against the
class table); §9's transfer-bus ordering reads as `(applyTick, transferId)` (D17); rosters,
logs and `StationCommand` carry u32 ship ids and key on `PlayerId` (D5/D6) · **extended
2026-08-20 by §6a** (owner rulings on the four questions P1 §3 left open for review: the
wave-2 trigger, the composer's lifetime, wing colour, and the sort inside a wing)
**Depends on:** ADR-002 (tick), ADR-004 (wire), ADR-005 (orders, validation, determinism),
ADR-009 (universe model, stations), ADR-012 (JSON, user settings layer), ADR-014 (seam),
ADR-015 (contact, stations as terrain), ADR-016 (anchors, universe runtime, summaries,
presence)
**Supersedes / amends:** the docking half of ADR-009 §9's MVP fence (ADR-016 expired the
gates half; this expires the rest); ADR-016 §3's anchor record (Station anchors gain an
**undock point and facing**); ADR-016 §7's presence rule (presence now **includes docked
ships**); ADR-016 §4's transfer bus (it arrives with **this** phase — dock and undock are
its first records, and U3a inherits it for warp).
**Delivery plan:** [Station-Build-Order.md](../Station-Build-Order.md) (T1–T3, milestones
H0/H1), interleaved into the universe phase after U2.
**Built so far (2026-08-19, T1's sim half):** §1's roster, §2's `Dock` order and its
footprint-derived radius, §3's `Undock` and §6's `AssignWing` as shared-validated station
commands, §5's protection window and its player-command break, and §9's transfer bus with
its `(applyTick, transferId)` order, and §4's parking ring with its deterministic berth
scan. **Not yet built:** §6's hangar screen, which is T3's, and the wire half of §8, which is
T2's.

## Context

Owner brief: in the MVP the fleets live outside the station. Fleets should **dock** and
**undock** when they are close by; ships are **invulnerable for a fixed number of seconds**
when they undock and **automatically move to a free space** around the station so fleets do
not pile up on each other; and while docked, the player can **compose new fleets from the
docked ships available**.

The ground is better prepared than it looks. The corpus already assumes docking exists:
`session-surfaces.png`'s RESUME card reads *"Docked at Vesta-3 · 4 days 6 hours since last
session"* — docked is the safe end-of-session state — and its RECONNECT UNDER FIRE screen
says *"Combat flag clears in 06s — you cannot dock or log off safely until then"*, so combat
is already expected to gate docking one day. ADR-016 made a **fleet emergent** — your ships
sharing a location, derived and never stored — which turns the recombination requirement
from a feature into a consequence: if docked ships are a location, then *the set of ships
you choose to undock together is the new fleet*, and there is no fleet entity to create,
name, or desync. Stations are already on-grid hulls that never move and never yield
(ADR-015: terrain). Anchors already carry an authored warp-in point and facing (ADR-016 §3),
and arrival by formation-solve-plus-separation is already the house pattern for ships
appearing in space. One anticipation did *not* survive contact with the design:
`GuidanceMode`'s comment reserved a future `dock` mode, and docking turns out not to be a
movement mode at all — it is a despawn. The comment was right that richer verbs would arrive
and wrong about the shape of this one, which is worth a line here so nobody goes looking for
the mode.

Decisions taken in the session, recorded here with their reasons: **docked ships live
off-grid** in a station roster; **a fleet docks together and instantly** once every member
is close; **protection is a flat 15 seconds, broken early by the player's own command**;
**undocking ships park themselves** at a scanned free berth on a ring; the hangar is a
**full screen** with its own print; wings while docked are **emergent** like fleets; the
phase lands **after U2**, pausing the universe track.

## Decision

### 1. Docked ships are a roster, not hulls

Docking removes ships from the world. A **station roster** at the universe layer — beside
ADR-016's transit records, as a third place a ship can be: *on a grid, in transit, or
docked* — holds, per station, the docked ships as `(ShipId, class, wing)` and nothing else.
Ids persist through docking: the roster keeps them, undock respawns them, and every log,
order, and roster row means the same ship before and after.

What this buys, all at once: clutter prevention **by construction** (a docked ship has no
position to clutter with); the grid teardown rule is untouched (docked ships are not world
state, so a station grid with everything docked can still tear down when nobody watches);
docked ships cost no snapshot bytes and no tick time; and they are untouchable by future
combat, which is exactly the safety the RESUME card promises. The accepted consequences are
stated rather than implied: other commanders at the station cannot see what is docked
there, and docked ships are perfectly safe — station raids, if they ever exist, will need
their own design.

**The roster deliberately keeps no damage state, and that *is* the repair rule.** "Repair
on dock" is not a system that runs; it is the absence of a field: a docked ship has no
gauges, so undocking spawns it full. When combat later wants repair to cost time or money,
the roster grows gauges and a repair verb, and this clause is the named hook.

### 2. Dock — together, instant, close by

**`OrderKind::Dock`** joins the order vocabulary. It rides `OrderSubmit` like any order —
same acked stream, same shared validation on both halves, same bounce parity (ADR-005 §4).
The enum grows by append, and because this phase now lands before U3a, **`Warp = 4` enters
the enum as a reserved value** (nameable, refused `UnknownKind`, exactly the
Attack/Stance/Abilities pattern) **so that ADR-016's stated number stays true, and `Dock`
takes 5.**

The rule is the gate rule, reused: **every member of the order inside
`DOCK_RADIUS_METRES` (5,000 m) of the station structure**, refusal `NotAtStation`
otherwise, `UnknownStation` when the named station is not this grid's. On accept the order
never enters the group table: ingest consumes it into a tick-stamped **dock record** on the
transfer bus, and the ships despawn into the roster at the next between-ticks apply point —
the whole fleet, one moment, matching "a fleet warps together and arrives together." A
fleet may dock at cruise; there is no speed to shed into a roster. Dock is **Replace-only**
(`InvalidQueueMode` otherwise): the queue holds legs of one movement plan, not a program of
verbs, and "fly these waypoints then dock" is the client-feeds pattern below, not a server
program (ADR-016 §8's decision, applied again).

**The approach is the client's** (the route-feeding pattern): the DOCK action works from
anywhere on the grid — the client feeds a Move to the dock perimeter, then submits Dock
once every member is inside the radius, surfacing the chained intent as a DOCKING chip. The
server only ever accepts in-radius docks; a disconnect mid-approach halts the fleet outside
the station, the same accepted cost as a route halting at a gate.

On the tactical surface, DOCK is a **context action on the station structure** — select
ships, act on the station — not a sixth button: the printed command row stays exactly as
printed, and the station is the natural home for every future station verb (trade, repair,
missions). The dock radius comfortably contains the station's warp-in standoff, made a bake
invariant (§8), so warp-arrive → dock chains without a crawl — the gate-radius trick, run
at stations.

### 3. Undock — the selection is the fleet

Undocking is not an order on world ships — there are none to name — so it is the first
**station command**: a small message family beside `OrderSubmit` on the same reliable acked
stream, sharing its sequence counter, its `OrderAck`, and its reason enum. Verbs: `Undock`
and `AssignWing` (§6). Validation is a pure shared function over a **RosterView** — the
replicated docked roster both halves hold — so the client pre-checks and the server decides
with the same code, and the parity contract extends rather than forks.

`Undock` names up to `MAX_SHIPS_PER_ORDER` (64) docked ships and a formation (the standard
dropdown, Line default) — refusals `NotDocked` for a ship absent from *this* station's
roster, plus the existing selection reasons. A fuller hangar undocks in waves. On accept:
an **undock record** on the transfer bus; at the apply point the ships spawn by formation
solve at the anchor's authored **undock point and facing** (Station anchors grow these two
fields — ADR-016 §3 amended; the bake places the point ~800 m off the structure, facing
outward, and Vesta-3's is hand-authored), with ADR-015 separation as the floor — exactly
the warp-arrival pattern, including its known edge: a maximal formation of capitals can
solve wider than the grid, is clamped, and heals over a few ticks. Spawning into a world
with no live grid spins one up; an undock *is* ships arriving (ADR-016 §4's rule, exercised
from a new door).

Because a fleet is emergent, this section is also the whole answer to "create new
combinations of ships": the undock selection *is* the composition. Nothing else ships.

### 4. The parking ring — free space, deterministically

The moment undocked ships exist, the world files a **system-issued move order** for them —
a real `OrderGroup` (it gains one flag: `systemIssued`), so the ETA, the drawn lane, the
straggler deadline, and player override all come free — to a **berth**: a candidate anchor
on a parking ring around the station.

Candidates are **bearings, not arcs** — a fixed arc lies about a capital line whose
footprint is kilometres wide. Two rings (`PARKING_RING_METRES` 2,500 and 4,000 — both
inside the dock radius, so a parked fleet re-docks without moving), **12 bearings each**,
scanned in a fixed order: start at the bearing of the undock point, alternate outward
left/right, inner ring then outer. A candidate is **free** when the *solved formation* at
that anchor (facing the outward radial, so parked fleets face away from the station)
passes two tests: **no hull on the grid** inside `AVOID_CLEARANCE_FACTOR` × combined
contact radius of any solved station — the avoidance vocabulary, reused rather than
duplicated — and **no other group's final-leg anchor** inside the candidate's bounding
circle padded by one largest-class spacing. The second clause is what makes two same-tick
undocks pick different berths with **no reserved-berth state to store or hash**: a berth is
"taken" exactly when live positions or live intentions say so.

First free candidate wins. All 24 refused → the fleet **holds at the undock point**,
protection still ticking, separation keeping it honest — **undocking is never refused for
clutter**, the straggler philosophy applied to real estate. The player can replace the
parking order at any time; it is just an order.

### 5. Undock protection — 15 seconds, solid, broken by command

Every undocked ship is stamped `protectedUntilTick = spawn + UNDOCK_PROTECTION_SECONDS
(15) × TICK_RATE`. Protection means **immunity to damage** — a forward design, since
combat does not exist; what it decides *today* is collision: **protected ships stay
solid**. ADR-015 applies throughout — they exclude space, brake, deflect, separate — so
"protected" never means "overlapping," and `Separate` needs no phase flag.

**The player's own command ends it early.** Ingesting a *player-submitted* order clears
protection on every ship it names; the system parking order does not (that is what
`systemIssued` is for). Fifteen seconds at a Battleship's cruise covers a ~3 km parking
flight; everything smaller is parked with time to spare; and the early break is the
standard anti-abuse shape waiting for combat: you cannot shoot from under the station's
skirts.

Replication is one bit: `EntityRecord` gains a **`statusBits`** byte (engine-neutral, the
game defines the bits — the `typeId`/`groupId` pattern; and unlike the old always-zero
`flags` byte this one is a bitfield *on purpose*), bit 0 = protected. The other seven are
where in-warp and combat-flagged will live, which is why a byte and not a widened `typeId`.

**Priced, because in this tree a replicated field costs ships.** `ENTITY_RECORD_BYTES`
goes 20 → 21, and `MAX_SHIPS_PER_SNAPSHOT` is derived from it: with the 1,150-byte budget,
the 16-byte header and the reserved 224-byte order area, the 910 bytes left hold **43 ships
instead of 45**. The MVP fleet is 41 and `Snapshot.h`'s floor asserts ≥ 41, so the cap still
clears it — on a margin of two rather than four. That is the honest cost of the shimmer, and
it is recorded here for the same reason `ORDER_STATE_RECORD_BYTES` records its own ("a field
added here costs ships"): the next person to want a status bit should find the price already
on the page. Two consumers of that shrinking margin are already designed — ADR-016 §6's
per-grid snapshot header, and any future gauge — so **the delta encoding ADR-004 reserved is
the growth path**, not another byte. *(That growth path is
[ADR-022](ADR-022-interest-and-delta.md) as of 2026-08-19, and it spends the margin question
differently than expected: ownership costs **no byte at all** — two spare bits of this very
`statusBits` carry the viewer-relative relationship the icon sheet actually reads, rather than
an owner id nobody looks at every tick. §1's roster privacy becomes a testable property in the
same slice, because the per-viewer sender it needs is the sender interest culling requires.)* If T2 measures the margin as too thin to land on,
packing the bit into a spare high bit of `groupId` (wings are 1..255 but a session fields
eight) is the named fallback, rejected as the default only because a bitfield hidden in an
id field is exactly the mistake `groupId`'s own comment was written to prevent.

### 6. The hangar screen

A **full-screen surface** in the TACTICAL ⇄ MAP family — TACTICAL ⇄ STATION — reached from
the station context and from the roster's DOCKED blocks. **Its print is a named
deliverable designed and agreed before its slice builds** (the D1 pattern; P1 in the build
order). Required contents: the docked roster grouped by wing (class icons, counts, the
roster vocabulary), multi-selection, the formation dropdown, UNDOCK, wing assignment, and
visible stubs for the station's future (repair pricing, refit, market) exactly as the
strategic map stubs its unbuilt overlays. Fleet composition is a real screen's worth of
work; a 260-px roster column was rejected as its home.

**Wings while docked are emergent, like fleets.** `AssignWing` writes a ship's `WingId` —
any value 1..255; a wing *exists* iff a ship carries its id, "new wing" is picking an
unused number, disbanding is reassigning the last member. No wing table, nothing new to
desync. Wing **names** stay what they are today — content injected by the composition
root — with renames and names-for-new-wings living in the **user settings layer**
(ADR-012): presentation, client-side, never on the wire. The server knows wings as numbers
and nothing else. `AssignWing` is docked-scope only for now (`NotDocked` otherwise): the
hangar is the reorganisation room, and in-space reassignment can arrive later without new
machinery if play demands it.

**Remote hangars work.** Focus never gates command (ADR-016 §7): the station screen opens
for any station holding your ships, viewed or not, because the roster it reads is
replicated regardless (§8). Undocking remotely spawns the fleet under its summary; the
roster block offers VIEW as usual.

### 6a. The four the print left open *(owner rulings, 2026-08-20)*

P1 landed on 2026-08-19 and its §3 marked four questions **OPEN — FOR THE PRINT REVIEW**:
questions the print was right not to answer alone, because each is a rule about behaviour
rather than a matter of layout. They are answered here, where §6 is, so T3 builds against a
decision rather than against a proposal. The print's drawing stands unchanged on all four —
what was missing was the sentence behind it.

**6a.1 — A wave launches when the undock point clears, with a timeout.** A hangar is
uncapped (see "what this deliberately does not do") and an undock order caps at 64
(`MAX_SHIPS_PER_ORDER`), so undocking a full hangar is more than one order and something has
to say when the next one goes. The rule is **§4's own clearance predicate, applied to the
undock point instead of to a berth**: the next wave launches when its solved formation at the
point clears every hull there. One function, two call sites, nothing new to keep in step —
and it self-scales, because an Interceptor wave clears in a second and a Battleship wave takes
exactly as long as a Battleship wave takes. A flat delay was rejected for being wrong at both
ends: short enough for Interceptors spawns capitals inside each other, long enough for
capitals makes a light wave wait for nothing.

**The timeout is the part that matters, and it exists because §4 has a full-ring case.** When
all 24 berths are taken the fleet *holds at the undock point* — that is §4's designed outcome,
not an error — and a wave gated purely on the point clearing would then wait forever, on a
screen with nothing to say why. So the gate is bounded: if the point has not cleared within
`UNDOCK_WAVE_TIMEOUT_SECONDS`, the wave launches anyway and `Separate` resolves the overlap,
which ADR-015 §3 already names as one of its three jobs ("authored overlap in spawn layouts").
That keeps **"undocking is never refused for clutter"** true, which is the position §4 took
and the one the print draws a diagram to promise.

**The gate is the client's, and nothing bounces on it.** Waves are client-fed, the way the
approach chain and ADR-016 §8's routes are: each wave is an ordinary `StationCommand{Undock}`
that `ValidateStationCommand` judges on its own terms, and clearance is not one of them. So
this is pacing, not validation — a client that timed a wave badly spawns ships that `Separate`
eases apart, and no verdict changes. That is why the predicate does **not** need to become a
shared pure function the way `DockRadiusMetres` and `ValidateStationCommand` did: `FindBerth`
stays `World`'s, and the client answers "is the point clear" from what it already holds — the
replicated hull positions and classes, plus `SolveFormation`, both of which are pure and on
both machines already. T3 should not go looking for a server call that is deliberately absent.

**6a.2 — The composer persists within a session, and reconciles against the roster.** A
half-built selection survives navigation and clears on undock, as the print proposed — plus
the clause that makes it safe: **on every open, rows the roster no longer holds are dropped.**
The hazard the print named is real (a selection naming ships that undocked from another
surface meanwhile, so UNDOCK bounces on ships still listed), and the fix costs nothing,
because the composer is a set of ship ids and §8's ~1 Hz roster is already the authority on
which ids are docked. Clearing on every navigation was rejected: re-picking a thirty-ship
mixed selection after one glance at the map is a real cost on the one screen whose whole
purpose is composing selections. ADR-020 asked for the composer's retained-state lifetime as
a rule rather than a per-print proposal; "persists, reconciled" is that rule.

**6a.3 — No per-wing colour. Position is identity.** Chips are not tinted by wing, and the
reason is that **colour in this tree already means relationship** — own-fleet phosphor, allied
cyan reserved — and it has already cost once: the selection ring shipped as allied cyan and
was changed to phosphor because a player reading colour fast parses their own selection as
someone else's ship. Wing tint would put a second meaning on that channel on the one screen
where a player is picking ships to send somewhere. It also does not scale: wings run 1..255
(§6) and no legible, accessible palette covers that. Identity is carried by the column a chip
sits in and the wing's own tag beside its name. This is the same rule ADR-020's seam already
states from the other direction — a badge **class index** crosses to the engine, never a
colour.

**6a.4 — Sort inside a wing: class descending, then ship id.** Heaviest first, which is what
P1 draws (BBS, DST, FRG, INT reading down a column) and what a player composing by class
scans for. The tiebreak is the **ship id and deliberately not the name**: names live in the
user settings layer (§6), client-side and per-machine, so sorting by them would make two
clients of the same commander show the same wing in different orders, and would reshuffle the
list the moment somebody renamed a ship. The id is the one key both machines share, and
sorting by it matches `Formation.h` already assigning stations by ascending `ShipId` — the
list a player reads and the formation it flies in are ordered by the same number.

### 7. Presence and the view

ADR-016 §7's rule — the view may point at any grid where the player has ships — is amended
one word's worth: **docked ships count as presence.** You may watch the space outside a
station you are fully docked at; docking your last fleet does not black-screen you or
force-jump the view; a viewer holds the grid alive exactly as before. Watching without any
presence at all remains the intel overlay's future. On-screen, docking is a ~1 s fade into
the structure and undocking a fade out of it — the existing overlay vocabulary, no new
pass — plus a toast on completion; the fleet roster grows **DOCKED blocks** (station name,
count, STATION button) beside its location blocks.

### 8. The wire

One clustered schema bump (fail-closed, as always):

- `OrderKind`: **`Warp = 4` reserved, `Dock = 5`** (§2).
- `OrderReason` appends: **`UnknownStation = 9`, `NotAtStation = 10`, `NotDocked = 11`,
  `InvalidQueueMode = 12`, and `CombatEngaged = 13` — reserved**: numbered and named now,
  returned by nothing until combat exists, because the reconnect print already promises a
  combat flag that refuses docking. U3a's `UnknownAnchor`/`NotAtGate` append *after* these —
  the cost of landing first, recorded so the numbering surprises nobody.
- The validation check order (ADR-005 §4a's contract) extends: … → `InvalidQueueMode` →
  `InvalidFormation` → `QueueFull` → `UnknownStation` → `OutOfBounds` → `UnknownShip` →
  `NotAtStation` — target checks before ship resolution, position checks after it, and the
  parity matrix pins the final sequence in T1.
- **`StationCommand`** (client → server, reliable, shared seq/ack/reason with orders): verb,
  station, ship ids, formation or wing id (§3, §6).
- **`StationRoster`** (server → client, ~1 Hz): per station holding the player's ships —
  station id, then `(shipId u16, classId u8, wingId u8)` per ship; 64 ships ≈ 260 bytes.
  This is **the first resident of ADR-016 §6's summary family**, landing before U3b builds
  the rest of it; fleet summaries arrive beside it, not instead of it. No gauges in the
  record — §1 means docked ships have none.
- `EntityRecord.statusBits` (§5).

`NotAtStation`'s pre-check runs against replicated quantised positions like every other
check; a 5,000 m radius against centimetre quantisation leaves parity nothing to trip on,
and staleness bounces stay the designed exception.

### 9. Determinism, the transfer bus, replay

**The transfer bus arrives with this phase**, not with warp: tick-stamped records, applied
between ticks in (apply tick, record order) — dock and undock records first, U3a's transits
inheriting the mechanism. `World` gains the transfer seam the bus needs either way:
transfer-out (remove preserving id) and transfer-in (spawn **with** a given id), shared by
undock now and warp later. Station commands are inputs on the acked stream, so the replay
contract holds by construction: a session replay is the per-grid order logs **plus the
transfer log** (ADR-016 §4's sentence, now with its first entries), and the double-run
suite extends over dock/undock/assign scenarios with the **roster folded into the
registry-level hash**. The berth scan reads live positions and live final-leg anchors in
dense order with no RNG draw; parking is as replayable as steering.

### What this deliberately does not do, so nobody mistakes it for covered

- **No hangar capacity.** Stations dock unlimited ships; a cap is a strategic knob with no
  economy behind it yet. Named so a future scarcity design is a decision, not a discovery.
- **No economy at the station.** Repair is free because the roster cannot hold damage (§1);
  refit, trade, and priced repair are stubs on the print.
- **No combat interaction beyond the reservations.** `CombatEngaged` is numbered and inert;
  protection is damage-immunity with nothing yet dealing damage; interdiction of the
  dock approach waits for combat. The early-break rule was shaped so combat can arrive
  without reshaping protection.
- **No visibility of others' docked ships, and no station raids.** Docked is absolutely
  safe and absolutely private. Both are stated costs of §1, accepted.
- **No persistence.** The roster is the obvious save anchor — the RESUME card's "Docked at
  Vesta-3" becomes literally true when a save file exists — but no save file exists, and
  this ADR does not create one.
- **No AI commander.** A disconnect mid-approach halts outside the station (§2), the same
  gap ADR-016 §8 accepted, closed by the same future feature.
- **No in-space wing reassignment** (§6) and **no per-class dock ceremony** — a Carrier
  and an Interceptor dock alike, instantly; pageantry is presentation's if it ever wants it.

## Alternatives rejected

- **Docked ships stay on-grid, hidden inside the station.** Blocks grid teardown forever,
  spends snapshot budget and tick time on ships doing nothing, and "present but invisible
  and unhittable" is an edge-case factory for collision and future combat. Rejected.
- **Per-ship trickle docking** (each ship vanishes as it reaches the bay). Reads physical,
  but the fleet docks piecemeal — "is my fleet docked?" has no answer mid-approach, and
  partial fleets are exactly what the together-instant rule exists to prevent. Rejected.
- **A timed dock hold** (spool-style seconds inside the radius). Buys an interdiction
  window before anything can interdict; unearned state. The combat flag is the future
  gate, and it gates the *verdict*, not a timer. Rejected for now.
- **Unconditional protection timer.** Simplest, but once combat exists it hands attackers
  a 15-second immunity they can shoot from. The early-break costs one flag and closes it
  in advance. Rejected.
- **Protection as ghost-through-traffic.** Guarantees a clean launch into any crowd, at
  the price of hulls overlapping on screen and a solidity toggle inside `Separate`. The
  undock point plus the berth scan make crowding rare; separation already heals the rest.
  Rejected.
- **Fixed berth arcs / a reserved-berth table.** Arcs lie about formation footprints;
  a reservation table is new hashed state doing a job that live positions plus live
  final-leg anchors already do deterministically. Rejected.
- **Server-side approach** (Dock from anywhere as one server-executed program). The same
  trade ADR-016 §8 already decided: it widens the order schema and puts a planner on the
  server that must agree with the client's presentation. The client feeds. Rejected.
- **DOCK on the command row.** Amends a printed surface and grows the row for every future
  station verb; the station context scales instead. Revisitable as an *addition* if
  discoverability suffers. Rejected as the primary home.
- **Hangar as a side panel or roster expansion.** Fleet composition, wing surgery, and the
  station's future screens in a squeeze; the print corpus is full-screen surfaces with
  clear handoffs. Rejected.
- **A fixed wing table.** A wing registry with create/disband verbs and a name field on
  the wire, to do what a `u8` on each ship already does. Emergent wings match emergent
  fleets. Rejected.

## Consequences

- The delivery plan is **[Station-Build-Order.md](../Station-Build-Order.md)**: T1 (sim +
  transfer bus), T2 (wire + tactical surfaces, milestone **H0** — the headless dock/undock
  loop), T3 (the hangar screen, milestone **H1** — dock, recombine, undock, watch it park).
  The universe phase pauses after U2 and resumes at U3a, inheriting the transfer bus.
- **Schema bumps, enumerated once**: `OrderKind{+Warp reserved, +Dock}`;
  `OrderReason{9–13}`; `EntityRecord.statusBits`; `StationCommand`; `StationRoster` — one
  cluster in T2, riding the fail-closed hash.
- **The snapshot ship cap falls 45 → 43** (§5), still above `Snapshot.h`'s asserted floor of
  41 but on half the margin. T2 updates the constant's comment with the new arithmetic, and
  the static asserts catch it if the number is ever wrong.
- **U1's spec is amended before it builds** (Universe-Build-Order): Station anchors carry
  the undock point and facing; new bake invariants — warp-in point inside the dock radius,
  undock point clear of the structure's contact radius, parking rings inside the grid.
- The replay contract gains its transfer log **early**, with the registry hash covering
  rosters; U3a's suites extend a mechanism instead of introducing one.
- **Named deliverables**: 
- **P1 delivered 2026-08-19:** `ScreenPrints/station-screen.png` — tabbed station
  surface (HANGAR active; REPAIR/REFIT/CONSTRUCT/MARKET stubbed as disabled tabs).
  Print proposals awaiting review: wave-2 trigger, composer persistence,
  no per-wing colour, class-then-name sort.
 **P2 — dock and undock audio cues** (after S15 gives audio its home, deliberately last).
- New constants join the envelope suite's guardianship: `DOCK_RADIUS_METRES` (5,000),
  `PARKING_RING_METRES` (2,500 / 4,000), 12 bearings per ring,
  `UNDOCK_PROTECTION_SECONDS` (15) — table data, retunable as table edits.
- ADR-009 §9, ADR-016 §3/§4/§6/§7 and the README's supersession list carry amendment notes
  pointing here.
