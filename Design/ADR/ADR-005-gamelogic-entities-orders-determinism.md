# ADR-005 — GameLogic: Fixed-Schema Tables, Group Orders, Decision Determinism

**Status:** Accepted · 2026-08-17 · §5's hash domain is amended by
**[ADR-022](ADR-022-interest-and-delta.md)** §7 (2026-08-19): `lastOrderSeqProcessed` is
per-session state and leaves the world hash, because with two commanders it is wrong twice
over — one player's sequence perturbs the other's feedback loop, and a replay's hash depends
on who submitted. **Landed 2026-08-22 with U3d-a**: the field left `World` entirely, the
session (`SnapshotSender`) keeps it per viewer, and the game's writer takes it as an
argument (`Game::WriteSnapshot` then; `Game::WriteTickTail` since U3d-b split the payload). Every replay hash moved once, here; no golden is stored in this tree, so the
re-baseline is the two-run comparisons re-agreeing on a new number rather than an edit ·
§2's "no inter-ship avoidance in MVP" superseded by
[ADR-015](ADR-015-ship-collision.md) (2026-08-18), which lands the avoidance this ADR's
consequences promised — inside Steering, plus a `Separate` step after Integrate · amended
by [ADR-018](ADR-018-scaling-baseline.md) (2026-08-19): §4a's check order joins the
compatibility gate (D9); `ShipId` widens to u32, staged (D6); `World::Spawn` takes an
injected id from the universe registry (D6a)
**Depends on:** ADR-001 (plane), ADR-002 (tick), ADR-004 (wire, quantisation)
**Feeds:** ADR-006 (extract), ADR-007 (ownership), Build Order (test harness)

## Context

GameLogic is the deterministic authoritative sim: entities, movement, orders, formations.
It links only against NeuronCore, is linked by the server as authority and by the client for
pre-check/preview parity (and prediction later). Two corpus requirements bind it: **F10** —
client presentation must be a pure function of replicated fields (`tactical-icon-system.png`),
and **BounceParity** — client pre-check and server validation must produce identical verdicts
and reason strings (`puck-and-wheel.png`). The open question is structure (ECS or not) and how
much determinism to buy, at what float-handling cost.

## Decision

### Entity & state structure
1. **No general-purpose ECS.** GameLogic uses **fixed-schema structure-of-arrays tables** with
   dense storage and stable ids:
   - `ShipId : u16` — stable per-session network id (also the wire id, ADR-04); id↔slot
     indirection table; dense slot arrays for state.
   - MVP ship state (parallel arrays): `classId u8`, `wingId u8`, `pos XMFLOAT2`,
     `heading float`, `vel XMFLOAT2`, `guidance {mode, groupRef, stationIndex}`,
     `hull/shield u8` (static in MVP). Storage is `XMFLOAT2`; math loads into `XMVECTOR`
     and stores back — never an `XMVECTOR` member or array (ADR-010 §3).
   - `ShipClassTable` — compiled-in registry of the **11-class closed set** from the icon sheet
     (Interceptor, Fighter, Bomber, Corvette, Frigate, Cruiser, Battleship, Carrier, Hauler,
     Miner, Structure): maxSpeed, accel, turnRate, pickRadius, formationSpacing. (Meshes exist
     for 9; Fighter and Cruiser are content gaps, noted in README.)
   - `OrderGroup` table — one per accepted order: `serverOrderId u32`, member ship ids,
     `formationId`, up to **4 legs** (pos, facing — the wire cap is the sim cap), per-leg
     solved stations, `state {Underway, Arriving, Done}`, `legIndex`, `doneTick`.
     A group that reaches `Done` **lingers `ORDER_DONE_LINGER_TICKS` (30) before it is
     retired**, rather than leaving the table on the tick it finishes. The reason is on the
     client: a ghost retires on *seeing* `Done` in a snapshot (ADR-014 §2c — absence is the
     signal), so a row that vanished immediately would race the read. `doneTick` is therefore
     simulation state and folds into the hash: it decides when the row leaves, and two worlds
     equal in everything else but differing here diverge exactly there. A finished group also
     never crowds a live one out of the snapshot's sixteen order slots.
2. **Systems are free functions run in a fixed, named order** each tick, single-threaded:
   `IngestOrders → GroupAdvance → Steering → Integrate → EmitSnapshot`.
   - *GroupAdvance*: a group's leg completes when every member is within station tolerance or
     a timeout of ticks expires (stragglers never wedge a fleet); then stations re-solve for
     the next leg.
   - *Steering*: per ship, seek assigned station (or hold): desired velocity toward target with
     arrival slowdown, clamped by class accel and turn rate (heading rate limit; ships move
     along heading — no strafing). Formation keeping falls out of station-seeking; no
     inter-ship avoidance in MVP (stations don't overlap by construction).
   - *Integrate*: semi-implicit Euler at fixed `dt = 0.05`.
3. **The formation solve is a pure function** in GameLogic:
   `SolveFormation(formationId, shipIds[], anchorPos, anchorFacing) → stations[]` — spacing
   from the largest member class; station *assignment* is by ascending ShipId (stable and
   cheap; a smarter cost-min assignment is a later drop-in inside the same signature). The
   client calls **the same function** for the order-puck footprint preview — the corpus demands
   the footprint be "the real formation solve", one tick per ship, never decorative.
   MVP formation set: `Line`, `Wedge`, `Claw` (crescent — the one on the prints).

3a. **What the three shapes are, and the two properties they share** (S10). Each is written in
   the anchor's own axes — how far right of it, how far ahead of it — so the trigonometry
   appears once and a shape is arithmetic on two numbers.

   - **Line:** abreast across the facing, centred on the anchor.
   - **Wedge:** an arrowhead. The first ship at the tip on the anchor, the rest falling back in
     two arms that alternate, at 45° so a step back and a step out are the same number.
   - **Claw:** a crescent whose arms reach *forward*, cupping the way the fleet will face, with
     the middle of the arc on the anchor and a fixed 120° sweep.

   **Every formation puts something on the anchor.** That is what keeps the puck honest across
   a formation change: the player pointed at a place, and whichever shape is selected something
   is there — so a single-ship order lands exactly where they pointed in all three.

   **Adjacent stations are exactly one spacing apart.** §2 above spends this: there is no
   inter-ship avoidance in the MVP *because* stations do not overlap by construction. It was
   prose from S6 until S10 made it a test over every count and class mix. Two of the three
   shapes needed care to hold it — a Wedge stepping a whole spacing on each axis puts its
   ships 1.41× apart (wasteful rather than wrong), and a Claw whose radius is derived from arc
   length puts them 0.83× apart at low counts, which is the failure this property exists to
   forbid. The Claw's radius therefore comes from the **chord**: `R = spacing / 2·sin(Δθ/2)`.

   *Not covered, and named so it is not mistaken for covered:* stations do not overlap **each
   other**. They can still land inside a gate, a station, or another fleet.
   `puck-and-wheel.png` §6 lists that under OPEN and it remains open.
4. **Order validation is a pure function** in GameLogic:
   `ValidateOrder(ValidationView, OrderSubmit) → Accepted | reason` with the reason enum
   (`EmptySelection, NotOwned, UnknownShip, QueueFull, OutOfBounds, InvalidFormation,
   TooManyShips`) defined beside it. **Parity rule: validation consumes wire-quantised values
   only** — the server quantises its own state to cm/turns16 before validating, exactly as the
   client's replicated view already is. Verdicts therefore cannot diverge on float noise; they
   can diverge on *staleness* (client view is ≤ a few ticks old), which is the designed and
   accepted reason a locally-passed order can still bounce from the server.

4a. **What it actually takes, and the one thing the client cannot compute** (S9). The first
   parameter was written `WorldView` above and is renamed here, because `Neuron::WorldView` is
   now a real and entirely different type — the client seam (ADR-014 §2). The signature is
   `ValidateOrder(const ValidationView&, const OrderSubmit&)`, and `ValidationView` is
   deliberately the *intersection* of what the two sides have: a span of ship ids and the leg
   count already queued. It is not a `World`, because the client has none — it has ids off a
   snapshot — and a function the client could not call would make the parity claim a claim
   about two different functions. The enum gained `UnknownKind` (and `Accepted` at zero) since
   this section was written.

   **The order of the checks is part of the contract.** An order that fails two rules must fail
   the same one on both machines, or the player reads a different explanation depending on
   which answered first. The sequence is: EmptySelection → TooManyShips → UnknownKind →
   InvalidFormation → QueueFull → OutOfBounds → UnknownShip.

   **`queuedLegs` is the asymmetry, and the client reports zero.** The server resolves it from
   the group the first named ship belongs to; the client cannot, because a snapshot order record
   carries a member *count* and not the members, so there is no way to ask which group a
   selection is in. Zero means an append that would overflow the queue passes locally and is
   refused a round trip later — the same designed-and-accepted case as staleness, and the
   direction that costs least. **The other direction is worse:** a client guessing high would
   locally refuse an order the server would have taken, and no amount of waiting gets the
   player past a refusal that is wrong. Closing it properly means the snapshot carrying group
   membership — two bytes per member per order against a 1,150-byte datagram — to make instant
   a refusal that is already correct. Not paid.

   *A defect this rule flushed out, worth recording because it was not a rounding curiosity:*
   `MetresToCentimetres` cast a float straight to `int32`. Beyond ±21,474 km that is undefined
   behaviour, and where it did not trap it wrapped — a target 10,000,000 km east arriving as
   one somewhere west, small enough for the bounds check to wave through. Reachable from any
   client that sends a large coordinate, so a validation hole rather than a quantisation
   detail. It saturates now, and NaN saturates to the low end where validation refuses it.

### Determinism
5. **Required: same-binary replay determinism.** Same build + same seed + same tick-stamped
   order log ⇒ bit-identical world state, forever. This is what makes desyncs debuggable,
   the replay harness possible, and F10 testable. Enforced by construction:
   - GameLogic reads **no wall clock, no OS entropy, no pointers-as-keys**; iteration order is
     dense-array order only; the *only* RNG is a seeded PCG32 (hand-rolled, ~20 lines, in
     NeuronCore) stored in world state.
   - `float32` arithmetic throughout; **/fp:precise** (default) on GameLogic, `/fp:fast`
     forbidden there; no per-file fp pragmas.
   - **DirectXMath rules (ADR-010 §6):** its implementation is chosen at compile time, so one
     binary has one path — matching this ADR's same-binary scope. `/arch` must not differ
     between GameLogic and the rest of the solution, and **`XM*Est` estimate functions are
     forbidden in GameLogic** (instruction-set-dependent accuracy). Swapping an exact call for
     an `Est` one breaks the replay suite, which is the intended alarm.
   - Single-threaded tick (ADR-007); any future parallel-for must use deterministic
     partitioning + ordered writes, gated by the replay test.
6. **Not required: cross-build / cross-platform bit determinism.** No custom transcendentals,
   no fixed-point, no /fp:strict tax. Costs accepted: replays and world-hash goldens are
   per-build artifacts; lockstep networking is permanently off the table — which the
   architecture never wanted: the server is the sole authority at every scale (ADR-002/004),
   and client prediction later reconciles against snapshots rather than requiring exactness.
7. **The replay harness is a deliverable, not a hope:** `GameLogicTests` (VS
   CppUnitTestFramework, added on main) runs scripted order logs twice and asserts equal
   FNV-1a world hashes per 20 ticks; the same harness runs in-exe via `selfTest`
   (ADR-008). Any PR that breaks replay equality is broken by definition.

## Alternatives rejected

- **Archetype/general ECS** — buys dynamic composition the game doesn't need (11 fixed
  classes, one movement model), costs indirection, iteration-order subtleties (determinism
  risk), and weeks. The SoA tables *are* the data layout an ECS would converge to; systems
  are already free functions, so a migration later is contained. Rejected for MVP.
- **OOP entity hierarchy (Ship : Entity, virtuals)** — cache-hostile, serialization-hostile,
  couples state to behaviour. Rejected.
- **Per-ship independent orders (no OrderGroup)** — loses shared legs/ETAs/ghost identity the
  HUD replicates, and makes formation re-solves N separate decisions. Rejected.
- **Bit-exact cross-platform determinism (fixed-point or strict-fp + own math)** — weeks of
  cost servicing a lockstep model we structurally rejected. Rejected with its motivation.

## Consequences

- The sim is trivially testable headless: construct world, feed orders, tick, hash — the
  GameLogicTests project carries movement-envelope, formation-geometry, validation-parity,
  wire round-trip, and replay-equality suites from the first sim slice onward.
- Adding a component in MVP = adding an array + touching EmitSnapshot + schema string —
  deliberate, visible friction that keeps replication honest.
- The client reaches GameLogic through the engine's `WorldView` seam (validation, formation
  solve, order encoding), never by linking it — ADR-014 overturned the earlier ruling. The
  parity guarantee is unaffected: it is the same function, reached through an interface.
- No avoidance/pathfinding in MVP: open-space plane, station-seeking only. Obstacle avoidance
  (the corpus's "obstructed footprint" open item) lands later inside Steering without
  structural change. **Both halves of that are now settled:** avoidance landed in
  [ADR-015](ADR-015-ship-collision.md) and [ADR-021](ADR-021-ship-make-way.md), and the
  obstructed-footprint item itself is closed by
  [ADR-026](ADR-026-obstructed-footprints.md) — a solved formation that lands in something
  slides whole to the nearest free placement, and an order still never wedges.
