# ADR-005 — GameLogic: Fixed-Schema Tables, Group Orders, Decision Determinism

**Status:** Accepted · 2026-08-17
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
     solved stations, `state {Underway, Arriving, Done}`, `legIndex`.
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
4. **Order validation is a pure function** in GameLogic:
   `ValidateOrder(WorldView, OrderSubmit) → Accepted | ReasonCode` with the reason enum
   (`EmptySelection, NotOwned, UnknownShip, QueueFull, OutOfBounds, InvalidFormation,
   TooManyShips`) defined beside it. **Parity rule: validation consumes wire-quantised values
   only** — the server quantises its own state to cm/turns16 before validating, exactly as the
   client's replicated view already is. Verdicts therefore cannot diverge on float noise; they
   can diverge on *staleness* (client view is ≤ a few ticks old), which is the designed and
   accepted reason a locally-passed order can still bounce from the server.

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
  structural change.
