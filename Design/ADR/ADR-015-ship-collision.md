# ADR-015 — Ship Collision: Contact Radii, Avoidance, Separation

**Status:** Accepted · 2026-08-18 (first post-MVP feature)
**Depends on:** ADR-005 (tables, systems, determinism), ADR-010 (math), ADR-002 (tick)
**Supersedes:** ADR-005 §2's "no inter-ship avoidance in MVP" — the scope that clause bought
is spent. It does **not** close the corpus's "obstructed footprint" open item (ADR-005 §3a);
§5 below says exactly how much of it this touches.

## Context

Ships fly through each other. The MVP bought that deliberately: ADR-005 §2 skipped inter-ship
avoidance because formation stations never overlap by construction, and its consequences
section promised the rest — "obstacle avoidance lands later inside Steering without structural
change." With the MVP closing, later is now.

What "collision" has to mean here is constrained by decisions already made. The plane is 2D
and authoritative (ADR-001); the tick is fixed 20 Hz (ADR-002); GameLogic must stay
same-binary replay-deterministic — dense-array iteration only, float32, no clock, no entropy,
no `XM*Est` (ADR-005 §5, ADR-10 §6); and ships move along their heading with no strafing
(ADR-005 §2), so "just sidestep" is not a move the model has. There is also no combat yet:
nothing needs momentum transfer or ramming damage, so contact is a *spatial exclusion*
problem, not a physics one.

## Decision

1. **Every hull class carries a contact radius** (`ShipClassInfo::collisionRadiusMetres`).
   Two ships are in contact when their centres are closer than the sum of their radii. The
   values are the hull radius the spacing table always implied — a quarter of
   `formationSpacingMetres`, rounded *down* to whole metres — so hulls parked on adjacent
   stations (one spacing apart) always have clear water between them. `Structure`, which has
   no spacing to derive from, gets 200 m against its 260 m pick radius, the same
   "pick is wider than the hull" proportion the capitals carry. Two bounds are held by test:
   contact < pick (picking is forgiving on purpose; contact must not be), and
   4·contact ≤ spacing.

2. **Avoidance lives in Steering, as promised, as two limits on what a seeking ship already
   computes.** Only seeking ships scan traffic; holding ships have no course to bend.
   - **Braking:** along the heading actually flown, the distance to first contact with each
     ship ahead is exact circle arithmetic (`ahead − sqrt(contact² − side²)`), and desired
     speed is capped by the same discrete-step `ArrivalSpeed` profile that stops a ship on its
     station, aimed at that point. The profile reaches zero a tolerance short, so a fully
     blocked ship parks with a 2 m gap rather than grinding in; and because allowed speed
     grows with distance, far traffic caps nothing — no horizon parameter exists to tune.
   - **Deflection:** in the frame of the course the ship *wants*, the nearest blocker sitting
     in its corridor bends the desired heading onto the tangent that clears it by
     `AVOID_CLEARANCE_FACTOR` (1.2) of the combined radius, considered out to the ship's own
     braking distance plus `AVOID_LOOKAHEAD_RADII` (4) contacts. The tangent formula is
     continuous at the corridor's edge — deflection is exactly zero when the blocker sits at
     the clearance line — so there is no boundary flicker to damp. Dead-ahead symmetry is
     broken by convention, not noise: dead-centre traffic is passed to port, so two ships
     meeting nose to nose each deflect to their own left and part. A blocker parked on the
     target itself is exempt from deflection — there is nothing sensible to steer around, so
     the ship brakes and parks adjacent (see §5).
   - The clearance factor must stay below √2: a Wedge's arms put formation neighbours 2√2
     radii off each other's course, and a wider corridor would have a formation in cruise
     avoiding itself. A test pins the bound.

3. **A fifth system, `Separate`, joins the tick after `Integrate`:**
   `IngestOrders → GroupAdvance → Steering → Integrate → Separate`. It resolves whatever
   overlap steering could not avoid — momentum a hull cannot shed, two ships converging on
   one point, authored overlap in spawn layouts. Pairwise circle projection, Gauss-Seidel in
   slot order, `SEPARATION_PASSES` (4) per tick:
   - **Positions move, velocities do not.** This is projection out of an invalid state, not
     physics: contact transfers no momentum, and the movement envelope (speed, turn,
     acceleration per tick) remains exactly what Steering granted — the envelope suite now
     asserts that through a multi-ship scramble.
   - **The split is by hull area** (radius²), so a Battleship shrugs through a crowd of
     Interceptors, and an *anchored* hull — `maxSpeed` zero, i.e. a station — is terrain: it
     takes none of the correction, ever. A fleet cannot relocate a station by parking in it.
   - **Each pass moves a pair at most `SEPARATION_STEP_FACTOR` (0.25) of their combined
     radius**, so avoidance-sized penetrations vanish the tick they appear while a deep
     authored overlap eases apart over a few ticks instead of teleporting on the client.
   - Coincident centres part eastward — an arbitrary axis that only has to be the same one
     every run.

4. **Determinism is untouched by construction.** Slot-order iteration, pure functions of the
   class table, no RNG draw, no clock, no `XM*Est`; the CRT calls used (`sqrt`, `asin`,
   `atan2`) are the ones Steering already leaned on, under the same same-binary scope
   (ADR-005 §6). The replay suite gained a converging-crowd scenario — eight independent
   orders onto one point, the maximum-contention case — asserting bit-identical hashes.
   Cost is O(N²) per tick in both the scan and the resolver: at the snapshot's ~64-ship scale
   that is noise at 20 Hz. A spatial grid is the known next step *when a measured tick says
   so*, and it slots inside `Steering`/`Separate` without changing either contract.

5. **What this deliberately does not do, so nobody mistakes it for covered:**
   - **No pathfinding.** Deflection is local: a wall of parked hulls can still brake a ship
     to a stop short of its station. The leg then ends by its own deadline (ADR-005 §2's
     straggler rule, unchanged) — an order never wedges, it expires. The "obstructed
     footprint" corpus item — stations solved inside a station's or another fleet's space —
     remains open; what changed is the failure mode: the ship now parks at contact range
     instead of interpenetrating.
   - **No contact damage, no momentum.** Combat will decide what a collision *costs*; this
     decides only that it cannot be occupied space.
   - **No replication change.** Positions were always on the wire; they simply stopped
     overlapping. No schema field, no quantisation constant, no client change — the
     interpolated view renders separation as a short slide, which the step cap keeps under a
     quarter radius per tick.

Measured on the real table, same-binary: two Interceptors swapping places head-on graze at
exactly their 34 m contact and arrive 13 ticks (~4 %) behind an empty-lane run; a Battleship
ordered through a station rounds it holding the 1.2× tangent to the metre (384 m against a
320 m contact) and pays ~7 % over the straight line. The starting fleet's authored layout
turned out to carry one real overlap — the Carrier and Battleship wings' line ends sit 221 m
apart against a 227 m contact, invisible until something measured it — and `Separate` heals it
on tick 1, moving exactly those two hulls ≤ 3.4 m each, everything else bit-still; re-parking
the boot fleet is the scenario owner's call, not this ADR's.

## Alternatives rejected

- **Hard collision only (no avoidance).** Resolves overlap but ships ram at cruise and grind
  along each other shoulder-first — the defect becomes "ships bulldoze through other ships",
  which is the same complaint with friction. Steering was always the promised home
  (ADR-005's consequences), and braking + deflection is what makes the result read as piloting.
- **Velocity-obstacle / ORCA-family avoidance.** Strictly better paths in dense crowds, and a
  reciprocity model this game cannot honour: ships cannot strafe, capitals turn at 0.22 rad/s,
  and the solver's failure modes (feasibility gaps under turn constraints) are exactly the
  hard part. The tangent-plus-brake pair is degenerate-case-free and fits in eighty lines.
- **Impulse physics (velocity response, restitution).** Buys bounce nobody asked for, breaks
  the "velocity is always along heading" invariant that keeps Integrate one line, and couples
  the movement envelope to contact. Rejected with its motivation — there is no ramming
  gameplay to serve yet.
- **A spatial broadphase now.** Structure for a cost nobody has measured at MVP scale;
  premature until ship counts grow past the snapshot's own cap. Named as the successor in §4.

## Consequences

- The visible defect is gone: ships route around traffic, brake rather than ram what they
  cannot round, and nothing ends a tick inside anything else beyond the resolver's per-tick
  step. Eight `ShipContactTests` scenarios hold the property from both directions (never
  interpenetrate; still arrive).
- Arrival semantics kept one asterisk: a target with a hull parked on it is unreachable, and
  the ship parks adjacent while the leg expires by deadline — the designed straggler outcome,
  now reachable by geometry as well as by speed. The fix for *placing* stations somewhere
  reachable is the still-open obstructed-footprint item.
- `GroupAdvance`'s arrival check reads positions after the previous tick's separation, so a
  formation "arrived" is a formation genuinely parked clear — the spacing table's ≥ 4-radius
  rule is what guarantees those two claims agree, and the table test now enforces it.
- Balance gained a knob with teeth: a class's contact radius changes how fleets flow through
  each other. It is table data beside the rest of the envelope, envelope-suite-guarded, and
  retuning it is a table edit.
