# ADR-026 — Obstructed Footprints: Solve, Then Slide

**Status:** Accepted · 2026-08-20 (owner rulings, four questions answered in one session)
**Depends on:** ADR-005 (tables, systems, order groups, determinism), ADR-015 (contact radii,
clearance factor, `Separate`), ADR-002 (tick), ADR-010 (math)
**Closes:** the corpus's **"obstructed footprint" open item**, named in ADR-005 §3a's
consequences and left explicitly open by ADR-015 §5 and `puck-and-wheel.png` §6. That item has
been open since before the MVP and was the last design question in
[MVP-Build-Order.md](../Archive/MVP-Build-Order.md), which closed and moved to the archive when
this ADR answered it.
**Extends:** ADR-005 §4 — an accepted order's leg anchor is no longer necessarily the point the
player named. ADR-015 §5's occupied-destination outcome is **kept**, as the fallback, and
ADR-021 §5's exemption of it is undisturbed.

## Context

A player points the puck somewhere and the formation solve turns that point into one station
per ship. Nothing has ever checked whether those stations are *in* anything. Formation stations
never overlap **each other** — that is guaranteed by construction and is why ADR-005 could skip
inter-ship avoidance for the whole MVP — but they can and do overlap the world: a station's
hull, a gate, a fleet already parked there.

What happens today is the honest consequence of every decision around it, and it is still a bad
outcome. ADR-015 gave the mover braking and deflection, ADR-021 gave idle hulls a way to step
out of the lane, and between them a ship no longer flies *through* what it cannot pass. So the
fleet arrives, brakes at contact range, sits outside the station it was assigned, and the leg
expires by ADR-005 §2's straggler deadline. **The order never wedges — it quietly fails to
mean what it said.** The formation the player composed never forms, no refusal is shown, and
the only signal is a footprint that does not fill.

Three constraints shape the answer, and two of them are the reason this stayed open so long:

- **The corpus has twice refused to refuse.** ADR-015 §5: "an order never wedges, it expires."
  ADR-017 §4's parking ring: "undocking is never refused for clutter — a design position rather
  than an edge case." A Move bounced for clutter would make the corpus say two different things
  about one situation.
- **The client cannot check this.** `OrderStateRecord` is fourteen bytes and carries **no leg
  anchors** (`Snapshot.h`), so a client cannot see any group's final-leg intention, and pending
  orders are sub-tick state that is never replicated at all. Adding an anchor to the record
  costs about two ships off a 43-ship cap whose margin is already one record (ADR-022's
  arithmetic). So whatever this decides, it cannot be decided identically on both sides — which
  rules out the pre-check-and-bounce shape every other geometric rule in the game uses.
- **The predicate already exists, and it is already deterministic.** ADR-017 §4's `FindBerth`
  answers "is this placement free" for undock, and it has been tested since T1.

## Decision

### 1. Solve, then slide

An order's leg is solved at the point the player named. If **any** station in that solved
formation is obstructed, the **whole formation** moves to the nearest free placement and is
solved again there. The formation is never deformed: shape and facing are preserved, and what
moves is the anchor.

The whole formation rather than the blocked stations alone, and this is the ruling rather than
an implementation detail. Displacing individual stations keeps the destination exact at the
cost of the thing the player actually composed — a Claw arriving as a dented Claw — and it
needs a per-station search with its own determinism story. Sliding needs one placement decision
and reuses a search that is already written.

### 2. Free means exactly what `FindBerth` means

A placement is free when the solved formation there:

- clears **every hull that will still be there** by ADR-015's own `AVOID_CLEARANCE_FACTOR` —
  the avoidance model's number, not a second one beside it that could drift; and
- lands inside **no other group's placed final-leg anchor**.

That second clause is what makes two fleets ordered to the same spot pick different placements
**with nothing reserved and nothing stored**, and it is why the predicate reads the group table
rather than a reservation list. It is ADR-017 §4's rule applied to a second caller — but the
two callers ask it about different worlds, and building it surfaced two qualifications that the
ruling did not anticipate and that the suite refused to let through without.

**2a. A ship that is going somewhere is traffic, not an obstruction.** Only hulls that will
still be there can block a destination. A ship under orders is ADR-015's problem and ADR-015
solves it: the fleet flies through space the other has vacated by the time it arrives. Without
this, two fleets ordered to **swap places** both slide — each because the other is standing on
its destination *now* — which is the one case the avoidance model was written to make work, and
the case ADR-015's own suite pins.

"Under orders" is membership of a live group, **not** `GuidanceMode`. Two fleets are accepted in
one batch, so at the moment the first is placed the second has not been given its guidance yet
and still reads as idle; orders are the thing that is already true by then. The question is
asked only when a hull is actually within clearance, which is why it can afford to be a scan:
a placement in open space never reaches that line.

**2b. An intention is a placement, not a request.** A group whose leg has never been applied
still carries the raw point somebody asked for — it has not been through this function yet and
may not survive it. Counting that as an intention makes two fleets sent to one point *both*
slide off it, each deferring to a claim the other had not actually staked. Ingest places groups
in sequence, so by the time the second is placed the first is real, and the asymmetry falls out
with nothing stored.

**`FindBerth` takes neither qualification, and must not.** A berth is chosen for a fleet that is
*arriving*, and one already occupied is occupied whether or not its tenant has plans; and its
parking orders are still **pending** when the next fleet scans, which is exactly what stops two
same-tick undocks picking one berth. So the shared predicate carries one flag naming which
question is being asked, and ADR-017 §4's behaviour is bit-identical to what it was.

**The predicate is therefore extracted, not copied.** `FindBerth` keeps its station-shaped
candidate pattern and this gets its own (§3); what they share is one function that answers
"is this solved placement free", called by both. A second copy of a clearance loop is a second
thing to keep in step with ADR-015's factor, and one of the two would eventually lose.

### 3. The candidate pattern: two rings, fanned from the approach bearing

Twenty-four candidates, in the shape ADR-017 §4 already uses and for the same reasons: two
rings, twelve bearings each, deterministic scan order, first free one wins. Two differences,
both following from a player-chosen point rather than a station doorway:

- **The rings are sized from the formation's own extent**, not from a station's parking radius.
  What "near enough to where I pointed" means plainly scales with the fleet: the same allowance
  cannot serve a three-Interceptor wing and a sixty-ship line.
- **Bearings fan outward from the fleet's approach bearing**, so a fleet blocked by something in
  its path stops **short** of it rather than overshooting past it. Arriving beyond the
  obstruction reads as the fleet ignoring the order; stopping short of it reads as the fleet
  obeying it as well as it could.

**Bounded on purpose.** An unbounded spiral always finds a placement, which sounds like the
better answer and is not: it can put a fleet a long way from the point the player chose, and a
distant formation is a worse surprise than a near one that parked short.

### 4. All twenty-four taken: fly to the asked point anyway

The fallback is today's behaviour, deliberately and unchanged: the fleet is sent to the point
the player named, brakes at contact range under ADR-015, and the leg expires by its deadline.

This is the clause that keeps the corpus consistent. An order is never refused for clutter and
never wedges; when the world genuinely has no room, the fleet gets as close as the avoidance
model allows and the deadline ends the leg. ADR-015 §5's occupied-destination outcome is not
superseded here — it is **demoted to the fallback**, which is a better place for it than the
default.

### 5. The slide happens when the leg becomes active

Not at submission. Each leg is placed at the moment the fleet commits to it: at ingest for a
`Replace`, and as each queued leg becomes the active one.

A queued third leg may fly minutes after it was accepted, and a placement computed at
submission time would be a precise answer to a question about a world that has since moved.
One rule, one code path, both queue modes — and every placement judged against the world as it
is when it matters.

The consequence is that a slid anchor is **world state written during the tick**, so it enters
the world hash like any other. That is required rather than tolerated: two runs of the same
build must slide identically, which they do, because every input to the decision is already in
the hash and the scan order is fixed.

### 6. The puck previews, and the preview is advisory

The client draws the slid footprint using the half of the predicate it can evaluate — hull
clearance against the ships in its snapshot — and the ghost reconciles to the authority's real
anchor when the snapshot arrives.

**This is deliberately not a parity guarantee, and it is the one place in the game where a
client-side geometric prediction is allowed to differ from the server's answer.** It is
allowed here because it cannot bounce: §4 means every Move is accepted, so there is no verdict
to disagree about, and ADR-014 §3's BounceParity is untouched. What can differ is *where the
ghost sits* for the fraction of a second before the first snapshot lands, by at most a ring —
and reconciling an optimistic ghost against the authority's answer is machinery that already
exists and already runs on every order.

The alternative was to put leg anchors on the wire so the client could evaluate the intention
clause too. It was declined on arithmetic: about two ships off a cap with margin one, still
blind to pending orders, and broken again by ADR-022's culling the day interest management
lands.

## Alternatives rejected

- **Refuse the order with a `FootprintObstructed` reason.** The cheapest by a distance, and it
  matches how `Dock` refuses an out-of-radius fleet — validation is shared, the client
  pre-checks, the puck draws refusing, and parity holds for free. Rejected because the corpus
  has already taken the other position twice (ADR-015 §5, ADR-017 §4), and because the two
  situations are not distinguishable to a player: being told "no" for clutter after composing a
  formation is a worse experience than being placed a hundred metres off.
- **Displace only the blocked stations.** Keeps the destination exact and breaks the formation's
  shape, which is the thing the player composed. Also the most code, for the least-wanted
  outcome.
- **Slide at submission time.** Settles the anchor before the ack returns, so the first ghost is
  exact. Rejected for queued legs, per §5.
- **Put leg anchors on the wire.** Per §6.
- **Leave it, and document the outcome as intended.** The status quo has been the de-facto
  answer for the whole life of the corpus, and stating it plainly was a real option. Rejected
  because it is a silent partial failure: nothing tells the player their formation will not
  form, and "the order expired" is not a thing the HUD ever says.

## Status of the build

**The sim half is built (2026-08-20).** `World::FindClearPlacement` is §3's search,
`World::IsPlacementFree` is §2's predicate — extracted from `FindBerth` rather than copied —
and `ApplyLeg` slides on first activation only, per §5. Three tests cover the rule, the
fallback and the two-fleets case; the self test's replay hash came through unchanged.

**§6's advisory preview is not built.** The puck draws the point the player asked for, so a
fleet is placed correctly and the player does not see it coming until the snapshot lands. This
is the one outstanding piece of this ADR, and it is recorded here rather than in a build order
because [MVP-Build-Order.md](../Archive/MVP-Build-Order.md) — whose S10 owned the question —
is closed and archived. It is client work: the hull half of §2's predicate evaluated over the
replicated view, drawn under the puck, with the ghost reconciling as it already does.

## Consequences

- **`Move` is the only order this touches.** `Dock` names a station and is judged by ADR-017's
  footprint-derived radius; `Warp` arrives at an authored warp-in point where ADR-016 §3 already
  promises clean water; `Undock` has had ADR-017 §4 since T1. Move is the only order whose
  destination is an arbitrary point a player chose, which is the only case that can be
  obstructed by accident.
- **One predicate, two callers.** The clearance-plus-intention test leaves `FindBerth` and
  becomes shared; `FindBerth` keeps its own candidate pattern and its own fallback (hold at the
  undock point) because a station doorway and a player's click are different questions.
- **The world hash covers slid anchors** (§5). No new determinism argument is needed — the
  inputs and the scan order were already deterministic — and the self test's replay hash came
  through the implementation **unchanged**, because a scenario with room in it never slides.
- **Two qualifications came from the build, not from the design session** (§2a, §2b). Both were
  found by existing tests going red rather than by review, and both are recorded above because
  a rule discovered by a suite is still a rule. The shape of each is the same: the first draft
  answered "is something there?" when the question is "will something be there?".
- **The ghost may move once, shortly after an order** (§6), where before it never did. That is
  visible and intended; it is the client learning what the authority decided.
- **`puck-and-wheel.png` §6's OPEN entry is answered**, by this ADR rather than by a reprint.
  The print's other content is unaffected.
- **The last design question in the MVP build order closes.** What remains there is a build item
  and two manual checkpoints that have since been signed off.
