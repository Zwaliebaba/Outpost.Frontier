# ADR-021 — Ship Make-Way: Idle Hulls Clear the Lane and Go Home

**Status:** Accepted · 2026-08-19 (owner-reported defect)
**Depends on:** ADR-015 (contact radii, avoidance, separation), ADR-005 (tables, systems,
determinism), ADR-002 (tick)
**Extends:** ADR-015 §2 — avoidance gains a third limb, and it is the one the *other* ship
applies. It does not disturb §3's `Separate`, §4's determinism argument, or §5's
occupied-destination outcome, which is kept deliberately and re-pinned by a test.

## Context

ADR-015 gave the mover two ways to cope with traffic and gave traffic no way to cope with the
mover. Both of the mover's are one-sided, and both fail closed:

- **Braking** stops a ship short of what it cannot pass. That is the guarantee, and it is also
  the failure: the ship parks against the obstruction and stays there.
- **Deflection** bends the course around a blocker, but only where there is a corridor to bend
  into. A hull sitting on the line, an approach angle that leaves no tangent inside the
  remaining distance, a queue of hulls in a row — the ship brakes instead.

The owner reported it as behaviour rather than as a bug in either mechanism, and the report
contains the fix: *"With a ship in front, there is a collision and the source ship stops. In
this situation, the other ship can also move out of the way to let the source ship pass, and
after that the ship will move back to its old place."* The ship best placed to clear a lane is
the one that is not using it.

The constraints are ADR-015's, unchanged. The plane is 2D and authoritative; the tick is fixed
20 Hz; GameLogic stays same-binary replay-deterministic (dense-array iteration, float32, no
clock, no entropy, no `XM*Est`); and ships move along their heading and cannot strafe, so
"step aside" is a course and a journey rather than a nudge.

## Decision

1. **A ship that is not under way steps out of the lane of one that is, and flies back when the
   lane is clear.** It lives in `Steering`, beside the two limits it completes, and it is
   expressed as a **displaced target** rather than a push: the sidestep is sought through the
   same arrival profile, turn limit, alignment factor and traffic avoidance as any ordered move,
   so a hull making way cannot break its own speed, turn or acceleration envelope. The envelope
   suite asserts that through the scenario.

2. **The displacement is computed fresh each tick and never stored.** `m_guidances` keeps the
   ship's **berth** — where the player left it — and `Steering` takes its copy by value. Nothing
   about making way enters world state, so nothing about it enters `WorldHash`, and there is no
   new field to replicate, quantise or version. **Returning is not a mechanism**: when the last
   mover is past, no blocker is found, the displacement is zero, the effective target is the
   berth again, and the ship flies home under the seek it stepped aside with.

3. **Three rules make it stable, and each answers a specific way this goes wrong:**
   - **The corridor is tested against the berth, not the hull.** A ship that has stepped aside
     is by construction no longer in the way, so a test on its live position would read "clear"
     the instant it started moving and it would oscillate in and out of the lane. A berth does
     not move, so the answer holds still until the mover has actually gone past it.
   - **The trigger and the destination are one number** (`MAKE_WAY_CLEARANCE_FACTOR`, 1.35 of
     the combined contact radius). A berth further off the course than that is left alone; a
     berth inside it is stood aside to exactly that line. The displacement therefore falls
     continuously to zero at the boundary and there is no edge to flicker across. The value sits
     above `AVOID_CLEARANCE_FACTOR` (1.2) so that a mover flies *straight* past a hull that has
     made room rather than still bending around it, and below √2 for the reason that bound
     exists at all (ADR-015 §2) — formation neighbours sit 2√2 radii off each other's course,
     and a wider corridor would have a parked wing scattering itself the moment one of its own
     members was ordered out of it.
   - **A journey shorter than the room being made for it is not a journey.** This is what stops
     the feature feeding itself: a ship on its way *back* from a sidestep looks, to everyone
     else, exactly like a short-haul mover, and without this line a cluster of idle hulls could
     take turns clearing lanes for each other's returns. A real order is orders of magnitude
     longer than a sidestep, so nothing a player asks for is refused by it.

4. **The horizon is the mover's travel time at its class's *top* speed**
   (`MAKE_WAY_LOOKAHEAD_SECONDS`, 6 s), not at the speed it is doing. That is the whole point:
   the defect is a mover brought to a **stop** by traffic, and a horizon measured from current
   speed would be zero at exactly the moment the lane most needs clearing — the two ships would
   sit nose to nose forever, which is the bug with extra arithmetic. Class top speed is table
   data, so the horizon is a distance the jam cannot close.

5. **Which side to stand on is decided in seconds, not metres.** Ships cannot strafe, so a hull
   asked to step the way it is pointing away from must swing its whole length round first: a
   Frigate facing north, told to clear thirty metres south, spends six seconds turning to cover
   a distance it would fly in one — and finishes the turn about when the traffic has already
   gone past. Both sides are therefore costed as *turn time plus travel time* and the cheaper
   wins. The choice is stable under its own outcome (turning toward the chosen side only makes
   that side cheaper) and stable against the mover (deflection bends away from whichever side
   the hull went, which keeps the berth's own lean pointing the same way). A tie goes to
   starboard, the convention deflection already breaks its symmetry with.

6. **`GuidanceMode::Hold` is read as the berth it names.** Until now `Hold` meant "stay where
   you were put" and "stay where you are" interchangeably, because nothing could displace a held
   ship except `Separate` and a shove is not a journey to undo. A held ship that steps out of a
   lane and then holds *there* has not made way, it has moved house. So a held ship off its
   berth seeks it — **unless another hull is standing in it**, which is both the sensible rule
   (flying home into someone is a shove dressed as a homecoming) and the one that keeps an
   authored stack settled: two ships spawned on one point are parted by `Separate`, each then
   sees the other in the berth they share, and both stay put. `Hold` is only ever set at spawn,
   with the berth set to the spawn position (`World::Spawn`), so there is no case where this
   reads a stale target.

7. **Determinism is untouched by the same construction as ADR-015 §4.** Slot-order iteration
   over positions, classes, guidance and headings — none of which the scan writes before reading
   — pure class-table lookups, no RNG draw, no clock, no `XM*Est`; the CRT calls (`sqrt`,
   `fabs`, `atan2`) are ones `Steering` already leaned on. Cost is a second O(N²) scan per tick,
   run only for ships that are *not* under way, which at the snapshot's ~64-ship scale is noise
   at 20 Hz and folds into the same spatial grid ADR-015 §4 names as its successor.

## What this deliberately does not do

- **The occupied destination still stands.** A hull berthed on the mover's target is exempt:
  stepping off a spot someone is arriving at only means stepping back into them once they park.
  The mover brakes and parks adjacent and the leg ends by its own deadline — ADR-015 §5's
  designed outcome, kept, and now pinned by a test of its own so a later change cannot erode it
  by accident.
- **No pathfinding, still.** Making way clears a lane; it does not find one. A ship that cannot
  reach its station because the geometry forbids it still parks and lets the leg expire.
- **No group awareness.** `Steering` does not know that orders or groups exist (ADR-005 §1) and
  this does not teach it. The consequence is worth stating: a member that has arrived on its
  station and then steps aside for a *third party* is, for those seconds, not on its station, so
  its group's leg does not complete until it is back. That is arguably correct — a fleet with a
  member out of position has not arrived — and it is bounded by the leg deadline either way.
- **No wire change.** Positions were always replicated; some of them now move for a reason the
  client is not told and does not need. No schema field, no quantisation constant, no client
  change.

## Alternatives rejected

- **Store the sidestep in world state (a per-ship "making way" flag or offset).** The honest way
  to break the circularity — a ship returning from a sidestep looking like a mover — and it
  costs a field in `WorldHash` and a lifetime to maintain across spawn, despawn and swap-and-pop.
  The journey-length rule in §3 buys the same property for one comparison and no state, and
  states the reason out loud where the arithmetic is.
- **Trigger on the yielding ship's live position rather than its berth.** Simpler to read and
  self-defeating: the trigger goes away the moment the ship begins to obey it. Every variant of
  this needs hysteresis bolted on; testing the berth needs none, because the berth is the thing
  that is genuinely still in the way.
- **Let `Separate` do it — resolve the contact by pushing the idle hull aside.** It already
  pushes, and pushing is exactly what is wrong with it: positions move without velocity, so the
  blocker slides shoulder-first out of the lane and stays wherever it was left, with no reason
  to go back and no envelope governing how it got there. The owner's report asks for both halves
  and only a *course* has a return leg.
- **Reciprocal avoidance (ORCA and family), so both ships share the correction.** Rejected in
  ADR-015 for reasons that have not changed — ships cannot strafe and capitals turn at
  0.22 rad/s, so the reciprocity model has no feasible solution to share. Making way is the
  asymmetric case where one ship has no course to protect, which is precisely why the cheap
  answer works here.
- **A right-of-way convention (heavier hull wins, or first-come).** More rules to explain and
  the same outcome in every case that matters: the ship with somewhere to be keeps its lane and
  the ship without one moves. Class does not need to enter it.

## Consequences

- The reported defect is gone: a ship ordered past parked traffic gets past it, and the traffic
  is where the player left it afterwards. Four scenarios hold the property from both ends —
  a hull dead centre on the lane leaves and returns; a row of six clears for a Battleship and
  settles quiet, contact-free and bit-identical across two runs; a station never makes way; a
  hull on the destination keeps its berth.
- Idle ships now move on their own, which is new in this simulation and visible. It is bounded:
  a sidestep is at most `MAKE_WAY_CLEARANCE_FACTOR` of a combined contact radius from the berth,
  it only happens with a mover inside the horizon, and it always ends at the berth it started
  from.
- Balance gained a second knob with teeth beside ADR-015's contact radius: the clearance factor
  decides how much room traffic makes, and the lookahead decides how early. Both are constants
  beside the rest of the envelope and retuning them is a one-line edit.
