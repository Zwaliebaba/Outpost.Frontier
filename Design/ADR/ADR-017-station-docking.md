# ADR-017 — Station Docking: the Roster, the Hangar, and the Parking Ring

**Status:** Accepted · 2026-08-19 (owner design session) · amended by
[ADR-018](ADR-018-scaling-baseline.md) (2026-08-19): §2's dock radius is
**footprint-derived** — `max(DOCK_RADIUS_METRES, FormationExtentMetres + margin)`, same
pure function on both halves (D7); §5's protection arithmetic corrected (fifteen seconds
covers ~1.2–1.6 km for a Battleship, not ~3 km — the window is re-checked against the
class table); §9's transfer-bus ordering reads as `(applyTick, transferId)` (D17); rosters,
logs and `StationCommand` carry u32 ship ids and key on `PlayerId` (D5/D6) · **extended
2026-08-20 by §6a** (owner rulings on the four questions P1 §3 left open for review: the
wave-2 trigger, the composer's lifetime, wing colour, and the sort inside a wing) ·
**further amended 2026-08-20 by [ADR-024](ADR-024-mining-economy.md)**: §1's roster record
gains a per-ship cargo manifest (cargo is not damage — the repair rule stands); §6's tab
family activates **CARGO** and **REFINERY**; and the "no persistence" note ends — the
universe layer's durable state gains a journal (ADR-024 §7a) · **the first two of those
cashed in 2026-08-21 by E3**: the roster row carries `oreUnits[3]` and the station gains a
second universe-layer resident, the per-`(owner, station)` **Bay** — the roster's "three
fields, deliberately" argument was about *gauges*, and cargo is not one, so repair-by-absence
stands unchanged
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
**§6's wing assignment reached the screen on 2026-08-22** — see the note in §6 for what had
been holding it and for the two rulings the build made.
**And left the hangar on 2026-08-23 (I2)** — the docked scope is lifted and a wing can be
formed in space; see the amendment in §6, which also strikes it from the not-covered list.
**Amended 2026-08-23 by §6b (owner ruling, the fleet design review): membership is in-space
membership.** A wing's members are its ships on grids and on the bus; a roster holds members
of nothing. The wing forms at the **undock**, a dock writes **memory** rather than
membership, and a wing whose last member leaves space ends — its row with it. §3's
2026-08-22 dock-groups note and §6's docked-wing clauses are superseded where marked;
delivery is **T4** ([Station-Build-Order.md](../Station-Build-Order.md)), scheduled by
[Plan-of-Record.md](../Plan-of-Record.md).
**Built so far (2026-08-19, T1's sim half):** §1's roster, §2's `Dock` order and its
footprint-derived radius, §3's `Undock` and §6's `AssignWing` as shared-validated station
commands, §5's protection window and its player-command break, and §9's transfer bus with
its `(applyTick, transferId)` order, and §4's parking ring with its deterministic berth
scan. ~~**Not yet built:** §6's hangar screen, which is T3's, and the wire half of §8, which is
T2's.~~ **Both built:** §8's wire half with T2 (2026-08-19), and §6's hangar screen across
T3a/T3b (2026-08-22) and T3's remainder (2026-08-23) — which closed the last of it, the wing
rename control and assigning to wing 0 to disband. What is left of §6 is a visual checkpoint,
not code.

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
*([ADR-024](ADR-024-mining-economy.md) amends the record: it also carries the ship's cargo
manifest — cargo is not damage, so the repair rule below stands untouched.)*
*(**U3c-a amends it again, 2026-08-21: the row carries its ship's `PlayerId`.** §1's privacy is
bought by sending the roster **per viewer**, and a filter needs something to filter on. A docked
ship is the one place ownership could quietly go missing — it is on no grid to be asked about —
so if the roster did not remember, a commander's own hangar would stop being theirs the moment
they arrived. `WorldRegistry::Roster` still answers whole for the two callers that want the
station, grid teardown and the save file; `DockedFor(owner, station)` is what reaches a player,
**including the validator's own view** — an unfiltered roster there is not a display bug but an
authority one, since a commander could name another's hull in an `Undock` and be believed.)*
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

**→ Built 2026-08-21, and the readiness test is the pre-check itself.** The chain holds no
geometry, never learns what a dock radius is and measures no distance. Each frame it composes
the chained order exactly as it will send it and asks `WorldView::PreCheck`; the frame the
answer stops being a refusal, the order goes. That is ADR-014 §3's parity rule doing a
second job — the condition the client waits on *is* the function the authority judges with,
so there is no second definition of "close enough" on this side to drift from the first. The
approach **leg** is likewise aimed at the station itself rather than at a computed perimeter
point: [ADR-026](ADR-026-obstructed-footprints.md) already slides a formation clear of a hull
it cannot occupy, so aiming at the station is aiming at its perimeter, and a perimeter the
client computed would be a second piece of geometry to keep in step with the first. The chain
copies the fleet rather than following the selection, which the player keeps editing, and
cancels on a refused leg, a member leaving the world, an order the player gives themselves, or
the link dropping — a chip promising an arrival that stopped being coming is worse than no
chip.

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

> **Superseded, 2026-08-23 — by §6b, which moves the regrouping to the other edge of the
> threshold.** A dock now writes *memory* rather than membership and mints nothing; the
> undock composes the wing, which is what §3's first sentence said all along. The note
> stands below as the record of what was believed and why it was reasonable — the defect it
> fixed was real, and its motive (a routine round trip must not spend a call sign) survives
> as §6b's restore rule. What did not survive is the fix's address: minting at the dock put
> a fresh call sign on every partial errand, which is the exact cost the note's own
> exception was written to avoid.
>
> **Note, 2026-08-22 — a Dock now decides what wing its ships are in, and this is the
> amendment §3's own sentence was missing.**
> §3 says the undock selection *is* the composition and that "nothing else ships". That is
> true of a **fleet**, which is emergent from location — and it was quietly false of a
> **wing**, which is a number riding on the ship and is the thing the roster actually groups
> by. So four ships composed out of two wings undocked together, flew together, and still
> read on the HUD as the two wings they came from. The player who reported it was right, and
> the ADR had described their expectation in as many words two sections earlier.
>
> ~~**The rule: the ships one Dock names become one wing, unless that Dock names a whole wing
> and nothing else.**~~ *(Struck 2026-08-23 — §6b: a dock forms nothing.)* Docking part of a
> wing, or parts of several, forms a group; docking all
> of TALON leaves TALON alone. The exception is not tidiness — without it a refuel round trip
> renames the fleet that took it, and a commander is out of call signs in eight docks.
>
> **At the dock rather than at the undock**, so the regrouping is true the moment the ships
> are inside: the hangar opens on one column rather than on the columns they arrived from, and
> the undock needs no rule of its own because the ships already carry the right number.
>
> **The number is the lowest the commander is not using anywhere** — grid, roster and the
> transfer bus, which is the one that is easy to forget, since a ship mid-crossing is in
> neither of the first two. Lowest-unused rather than next-highest is what keeps this bounded:
> a wing that empties is a number that comes back, so the roster does not grow a row per dock
> and the call signs are not spent one per errand. The visible cost is that a released name
> reappears on different ships later, which is what a name that is presentation rather than
> identity is allowed to do (§6a.4).
>
> `AssignWing` is unchanged and is now the *recovery*: the dock groups what arrived together,
> and the hangar's own verb is how the player changes their mind — including putting a split
> back where it came from.

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
clears it — on a margin of two rather than four. *(**The cap itself went with U3d-b**,
2026-08-22: a tick's records are bounded by a bandwidth budget and packed into as many datagrams
as they take, so there is no per-snapshot ship count left to shrink. The arithmetic below is
kept because it is the record of what the shimmer cost when the cap was real, and because the
*rule* it justifies did not go away — the per-tick record is still multiplied by the population
twenty times a second.)* That is the honest cost of the shimmer, and
it is recorded here for the same reason `ORDER_STATE_RECORD_BYTES` records its own ("a field
added here costs ships"): the next person to want a status bit should find the price already
on the page. Two consumers of that shrinking margin are already designed — ADR-016 §6's
per-grid snapshot header, and any future gauge — so **the delta encoding ADR-004 reserved is
the growth path**, not another byte. *(That growth path is
[ADR-022](ADR-022-interest-and-delta.md) as of 2026-08-19, and it spends the margin question
differently than expected: ownership costs **no byte at all** — two spare bits of this very
`statusBits` carry the viewer-relative relationship the icon sheet actually reads, rather than
an owner id nobody looks at every tick. §1's roster privacy becomes a testable property in the
same slice, because the per-viewer sender it needs is the sender interest culling requires.
**It became one at U3c, 2026-08-21** — but not in that slice and not for that reason: the
per-viewer sender (A13) was necessary and nowhere near sufficient, because until ownership
existed the filter behind it was the identity function. What made the property testable was a
second commander to be private *from*.)* If T2 measures the margin as too thin to land on,
packing the bit into a spare high bit of `groupId` (wings are 1..255 but a session fields
eight) is the named fallback, rejected as the default only because a bitfield hidden in an
id field is exactly the mistake `groupId`'s own comment was written to prevent.

**→ Drawn 2026-08-21, and the engine still does not know what the bit means.**
`OverlayTuning::statusMarkBits` is a *mask*, zero by default; NeuronClient draws a mark for
any bit the mask names and has no opinion about any of them. The one line in the build that
says bit zero is undock protection lives in `Outpost.exe`'s config assembly, which is the
same arrangement `renderClassByHull` and the wing names already have. The shimmer is alpha
over a floor rather than to zero: a mark that reached invisible for part of each cycle would
read a protected ship as unprotected at the wrong glance, which is worse than no mark because
it is a wrong answer rather than no answer.

**And the dock/undock fades generalised, which is the honest version of them.** A dock is an
entity disappearing and an undock is one appearing, and the client can see both *without
being told*: an id in last frame's scene and not in this one has left the world. So the fade
is built by diffing the scene rather than by reading a docking event, and the same ~1 s ring
covers a dock, an undock, a warp-out, a kill and a ship falling out of the interest set —
every one of which is honestly "something that was there is not". Positions are remembered
from the last frame that had them, because by the time a frame notices a ship is gone there is
nothing left to ask. Both directions grow outward and are told apart by which way the alpha
runs; a ring collapsing inward on a docking ship would read as the ship being crushed rather
than as it going somewhere.

### 6. The hangar screen

A **full-screen surface** in the TACTICAL ⇄ MAP family — TACTICAL ⇄ STATION — reached from
the station context and from the roster's DOCKED blocks. **Its print is a named
deliverable designed and agreed before its slice builds** (the D1 pattern; P1 in the build
order). Required contents: the docked roster grouped by ~~wing~~ **hull class — §6b,
2026-08-23: docked ships are in no wing, so the pool groups by the one thing a docked row
still is** (class icons, counts, the
roster vocabulary), multi-selection, the formation dropdown, UNDOCK, wing assignment
*(since §6b: the undock's own wing chip rather than a second verb)*, and
visible stubs for the station's future (repair pricing, refit, market) exactly as the
strategic map stubs its unbuilt overlays. Fleet composition is a real screen's worth of
work; a 260-px roster column was rejected as its home.

**The print's "tabs, not tiles" call was cashed in on 2026-08-21, and it held.** P1 decided
that the station's future arrives as *sibling tabs in a fixed row* rather than as panels
inside the hangar, so that the hangar's layout never reshuffles when a service lands and each
service gets a full screen. The economy phase tested that on 2026-08-21 and it held:
[cargo-tab.png](../ScreenPrints/cargo-tab.png) (D-P2) and
[refinery-tab.png](../ScreenPrints/refinery-tab.png) (D-P3) added **CARGO** and **REFINERY**
to the row between HANGAR and REPAIR, and P1's own plate was re-captured the same day to carry
the seven-tab row rather than the five it was drawn with.

**What changed was the row; what did not change was the hangar.** No panel moved, no column
narrowed, no gesture renegotiated — two whole services arrived and the screen this ADR designs
is the same screen. That is the difference between a layout decision that anticipated growth
and one that merely left room, and it is worth having on the page for whoever proposes the
next tab: a service costs a tab and nothing else, and a service that would cost more than a
tab is a service that has misread this decision.

> **Note, 2026-08-22 — §6's wing assignment is built, and what delayed it was a wrong
> dependency rather than a missing one.**
> The hangar's two reorganisation controls were drawn dead with the reason "both need the user
> settings layer, because a wing's *name* is client-side". **That is true of renaming and false
> of creating.** A wing exists iff a ship carries its number — the paragraph below says so — so
> making one is a command the authority already takes, and the call sign only has to outlive the
> session that spent it. Three features were held behind a dependency one of them had.
>
> The cost was a player report, not a theory: dock two ships out of one wing and two out of
> another, and there is no way to make the four of them a wing. **Undocking them together is not
> the same answer** — §3 is right that the undock selection composes a *fleet*, but a fleet is
> emergent from location and a wing is a number that rides on the ship, so the four flew
> together and still read as two wings on the roster.
>
> Two rulings the build made, recorded so the print and the code do not disagree silently.
> **The print's `+ NEW WING` button is now a value of the wing chip**, because "new wing is
> picking an unused number" makes it a value of the same parameter rather than a second verb;
> the rect it had holds the button that sends the command. And **new wings stop being offered at
> `MAX_ROSTER_ROWS`**, because a wing past the roster's row cap would be created, carry ships and
> never be drawn — the chip going quiet is the honest end of an emergent thing, where offering
> the number would not be.
>
> **Assigning to wing 0 is still not offered.** It is how a wing disbands and the machinery takes
> it, but `BuildRoster` draws no row for wing 0, so the control would make ships vanish off the
> HUD. It arrives with the print's stray column, not before.

~~**Wings while docked are emergent, like fleets.**~~ **Wings are emergent, and since §6b a
docked ship is in none** *(2026-08-23)*. `AssignWing` writes a ship's `WingId` —
any value 1..255; a wing *exists* iff a ship **in space** carries its id, "new wing" is
picking an unclaimed number, disbanding is reassigning the last member. No wing table,
nothing new to desync. Wing **names** stay what they are today — content injected by the composition
root — with renames and names-for-new-wings living in the **user settings layer**
(ADR-012): presentation, client-side, never on the wire. The server knows wings as numbers
and nothing else. ~~`AssignWing` is docked-scope only for now (`NotDocked` otherwise): the
hangar is the reorganisation room, and in-space reassignment can arrive later without new
machinery if play demands it.~~

> **Amendment, 2026-08-23 — the docked scope is lifted, and play demanded it.**
>
> [Plan-of-Record §1](../Plan-of-Record.md)'s rule 3 is what asked: **wings are the control
> groups**, which is what lets ADR-020's input model select a fleet with no keys, no
> user-layer storage and no new entity. A control group a player can only *form* by first
> flying home is not one — so the clause above described a scope this design had outgrown.
>
> **It arrived without new machinery, as promised.** A `WingId` already rides on a ship in
> space (`World::Wings`), `WingPopulation` already walks grids as well as rosters and the
> bus — so "the lowest unused number" was already counting the fleet that is flying — and
> the wire, the verb and the shared validator are untouched. What changed is what the
> validator is judged against: `RosterView` carries a **grid** beside its station, and
> `RequiresDock` is now a second question `NamesShips` used to answer alone.
>
> **A station's anchor is also a grid's**, so the two lists usually fill side by side: what
> is docked here, and what is flying here. `AssignWing` may name either; `Undock` and the
> two transfer verbs may name only the roster, because moving a hull or a hold across a
> station's threshold is the whole of what they do. One command may name ships from both
> lists, which no surface can compose today and which costs a rule to forbid.
>
> **The refusal changed with it.** `NotDocked` reads *"not docked here"*, which is the wrong
> sentence for a player who selected ships in space, so an `AssignWing` that names a ship
> the view does not carry is refused `UnknownShip` — *"no such ship"* — true in a hangar and
> true on a grid. It is keyed on the **verb** and never on which list came up empty: the
> verb is a byte both machines have, while the view is built from the registry's owner index
> on one side and ADR-022 §8b's relationship bits on the other. A reason derived from the
> view would have differed between the halves in exactly the case this lift adds.
>
> **What it does not do is give it a surface.** Nothing in the client can compose an in-space
> assignment yet; that is I3's, alongside the order surfaces. This is the authority half,
> landed first so a screen slice is a screen slice rather than a screen and an authority
> change in one sitting.

> **Amendment, 2026-08-23 — the rename control and the disband, which close T3's list.**
>
> The sentence above says renames live in the user settings layer. N2 built that layer and
> T3's remainder builds the **control**: a tap on a wing's column header opens a field on the
> header itself, seeded with the current word and with the whole of it selected. Escape
> cancels, Enter commits, and a tap outside commits — the last of which is the one that
> matters, because touch has no Enter. The engine collects the characters and the game
> decides whether the word may be stored, which is `WorldView::RenameGroup`: an opaque group
> id and a string across the seam, and the engine never learns it renamed a wing.
>
> **Disbanding was the clause this section stated and the client refused to offer.** *"Disbanding
> is reassigning the last member"* — and the hangar's wing chip did not offer wing 0, because
> the tactical roster drew no row for it, so the button would have made ships vanish off the
> HUD. The strays have a row now, last on the panel and only when there are any, so a disband
> moves a fleet from one row to another where it can still be seen and re-grouped.
>
> **And building that row found a defect this section is the right place to record**, because
> it is about what a wing number *is*. A `WingId` is a byte **every commander numbers from
> one** — this design says so, in "any value 1..255" — so a roster that grouped by the number
> alone was grouping two commanders' fleets together. A hostile wing 1 on your grid was
> counted into your wing 1's row and would have been selected by a press on it. The fix is
> ADR-022 §8b's relationship bits, which arrived after the code that needed them; the reason
> it was never seen is that it takes two commanders on one grid, which is U3c's arrangement
> and which no gate had built.

**Remote hangars work.** Focus never gates command (ADR-016 §7): the station screen opens
for any station holding your ships, viewed or not, because the roster it reads is
replicated regardless (§8). Undocking remotely spawns the fleet under its summary; the
roster block offers VIEW as usual.

> **Note, 2026-08-22 — the MARKET stub has a forward-design print, and it stays FUTURE.**
> §6 above requires "visible stubs for the station's future (repair pricing, refit, market)",
> and the seventh tab has carried that stub since P1's row was re-captured.
> [market-tab.png](../ScreenPrints/market-tab.png) (D-P4) draws what opening it looks like —
> **the tab's status does not change**: the plate wears its posture in an amber banner, because
> **the market phase has no ADR in this corpus** and this print is the UI half of one that has
> not been written. It is recorded here so the stub is not mistaken for unplanned, and so the
> future ADR starts from a screen rather than from a blank page.
>
> Two things it settles are the tab row's, not the market's, and belong on this page. **The
> forward print did not cost a tab or a pixel of the hangar** — the second test of §6's
> "a service costs a tab and nothing else" claim, now run against a service two phases out.
> And the tab is a **station** service in the strict sense the row implies: an order lives where
> its escrow lives, so books are per-station and listing requires your Bay at *that* station,
> while browsing is free everywhere (the refinery's remote rule). A region-wide book would
> teleport goods past the transfer bus §4 exists to be.
>
> **What it does not settle is four rulings the market ADR must make**, listed on the plate and
> tracked in [README.md](../README.md)'s open-rulings register: the **currency** — name,
> denomination and faucet (the print says `{{ curName }}` through a tweak, because the word is
> not designable on a screen); the **fee point and rate**, the phase's currency sink, proposed
> on fills against proceeds so a listing that never fills costs nothing; **order lifetime**,
> until-cancelled as drawn versus an N-day expiry with escrow return; and **who seeds the first
> book**, player-to-player from zero or an NPC bootstrap vendor. Until those are answered the
> tab stays exactly what §6 made it: a stub with a drawing behind it.

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

### 6b. The wing lifecycle *(owner ruling, 2026-08-23 — membership is in-space membership)*

Recorded from the fleet design review of 2026-08-23. This section supersedes **§3's
2026-08-22 dock-groups note** and **§6's docked-wing clauses**, both left standing above and
struck where they state the old rule, so the record of what was believed survives beside
what replaced it.

**The owner's model, six clauses:** a docked ship belongs to no fleet; undocking a selection
makes it one, automatically; a docked ship can be sent out into a fleet already flying; a
ship that docks leaves its fleet; a fleet whose ships have all docked is over, and its slot
comes back; a fleet whose ships have all died is over the same way. The review compared
those clauses with the tree and found the tree answering nearly every one the other way:
wings attached at *dock* (a partial dock minted a fresh number — §3's note), persisted
*through* docking (the whole-wing exception kept the number occupied), and were never
visibly freed (`BuildRoster` drew every named wing forever, at zero). Three defects fell out
of that arrangement, each confirmed in code before this ruling reversed it: **a lone errand
minted a one-ship wing** and spent a call sign on it; **a parked wing and a dead one drew
the same zero row**; and **a number freed by destruction could resurrect its old call sign**
on an unrelated dock group, because the server reuses numbers and the client binds names to
them forever.

**The ruling in one sentence: a wing's members are its ships in space** — on a grid, or
mid-crossing on the bus — **and a roster holds members of nothing.** Everything below is
that sentence applied to one edge of the station threshold or the other.

**6b.1 — Docked ships are in no wing.** The hangar shows **one pool, grouped by hull
class**, with §6a.4's sort unchanged inside it — class descending, then ship id, which was
never about wings; it is what a composing player scans for. The roster row keeps its `wing`
byte and the byte changes meaning rather than shape: it is **memory** — the number the ship
last flew under — written by the dock, read by exactly one rule (6b.2's restore), and
grouped by nothing. Durable state is untouched: the byte's meaning moved and its bytes did
not, so a reloaded hangar remembers exactly what it remembered.

**6b.2 — The undock composes the wing — §3's own sentence, true of the number at last.**
`Undock` reads the wing byte `StationCommand` has carried since T2 and the verb has ignored
since it was written. **Zero — the field's default — means "the registry decides"; a number
means "this wing"**, which is the owner's third clause in one press: pick the docked ships,
set the chip to a flying wing's call sign, UNDOCK. The registry decides with one function,
at filing, asked once for the record and before the first row leaves — the dock note's own
discipline, run at the other edge:

- **Back where they came from**, when the selection is uniform memory of one number and is
  *every* row of this station's memory of it: the ships fly out as that wing — rejoining it
  if it still flies, restoring it if it does not. A whole wing docked and undocked whole is
  the same wing with the same call sign, which keeps the §3 note's motive — the round trip
  spends nothing — without keeping the wing alive while it is parked.
- **The lowest unclaimed number otherwise.** A number is **claimed** while any ship in
  space carries it *or any roster row remembers it* — which is exactly the three-place walk
  `UnusedWingFor` already does; what this ruling splits off is a second, narrower count
  (members: space only) for everything that is about liveness. Memory blocking the mint is
  what ends the resurrection defect: a number cannot be handed to strangers while any
  hangar still remembers whose it was.
- **Strays on exhaustion** — all 255 numbers claimed — and the composition flies as wing
  zero rather than not flying: undocking is never refused, §4's rule one register up.

No verdict changes and no byte moves. Both parameters have ridden the command since T2, the
validator refuses nothing new (any wing byte is legal, which is what emergent means), the
check order is untouched and the parity matrix with it.

**6b.3 — Docking leaves the wing** — definitionally rather than operationally: membership
is in-space, so the docked subset is out the moment its rows are filed, and the flying
remainder *is* the wing — number, name and row unchanged. That is the owner's fourth clause
generalised from one ship to any subset, and it deletes `WingForDockedGroup` whole: a dock
writes memory and nothing else.

**6b.4 — A wing ends when its last member leaves space** — docked or destroyed, the same
sentence: the owner's fifth and sixth clauses. Its roster row goes with it: `BuildRoster`
draws rows for wings with members on the watched grid, and **the permanent zero row is
struck**. A parked fleet is the DOCKED block's to report and a fleet elsewhere is its
location block's — the emergent layer doing the job it was built for. What the zero row was
for — *"which wing did I just lose"* — is one-shot news, and one-shot news is the toast
family's; that toast lands with the combat phase, because until it exists nothing can die,
and every emptying until then is a docking the DOCKED block already narrates.

**6b.5 — Numbers and names free when the last claim does.** A number with no member and no
memory is back in the pool. Names stay what §6 made them — client-side presentation, bound
to numbers — and the **name cap moves from names-ever-minted to rows-now-drawn**, which
retires `EnsureWingName`'s refusal spiral: under the old cap the seventeenth wing a session
named — the ninth composed, after the starting fleet's eight — lost its roster presence
silently, the exact failure that function exists to prevent. A binding outliving its wing may reappear when the number is minted again;
§6a.4 already ruled a name is presentation rather than identity and is allowed to do that,
and 6b.2's claim rule makes it rarer than it was.

**6b.6 — `AssignWing` returns to one scope: space.** A docked ship has no wing to be
assigned to, so the verb refuses the roster and keeps the grid — the fence has now moved
twice and for the same reason both times: the wing is the control group, and the control
group lives where control happens. The hangar's ASSIGN pair retires, and **the wing chip
moves to the undock composer as UNDOCK's second parameter**, beside the formation it
already has. The chip cycles "the registry decides" and the flying wings this client can
name — and it keeps T3's ruling that a chip cycles even when its button is dead, because
reading which wings exist is worth doing with nothing selected. **The rename control
follows the chip**: one field, one home, the same `TextEditState` machinery T3 built, so
naming a wing and making one are the same gesture in the same place. In-space assignment
is untouched and remains I3's surface.

**Deliberately absent, so nobody mistakes it for covered:**

- **No rally.** Undocking into a flying wing labels the ships; it does not move them — they
  park on §4's ring like any launch, and joining the wing's formation is an order the
  player gives. A system-issued rendezvous is named here as future work if play demands it.
- **No slot refusals.** More live wings on one grid than the roster has rows is possible
  and truncates the *panel*, never the fleet — the ships stay selectable in the world. The
  old chip rule ("new wings stop being offered at the row cap") retires with the chip that
  enforced it.
- **No change to the emergent fleet.** `FleetSummary`, the location blocks and the map's
  markers group by *place* and keep doing so; this ruling is about the wing — the thing the
  roster groups by. D-P5's doctrine layer above both is unaffected in substance: a deploy
  still composes at one station and still lands as roster rows — rows that now only ever
  show squadrons that are flying.

**Print deltas owed, per the manifest's rules:** P1 where the hangar regroups by class and
the composer gains the wing chip and loses the ASSIGN pair — a **major** bump, because
"wings as columns" was one of that print's own design calls and this section is the ADR
note the manifest requires beside a reversal; 07a where the roster stops drawing zero rows
— sized by the manifest at capture, major if its empty-row state is read as a call rather
than a state. Both are captured with T4b, and the plates and this section must not disagree
for longer than that slice.

**Delivery: T4** ([Station-Build-Order.md](../Station-Build-Order.md)) — T4a the registry
half, T4b the client half — scheduled by [Plan-of-Record.md](../Plan-of-Record.md).

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
  refit, trade, and priced repair are stubs on the print. *(The first economy arrived as
  [ADR-024](ADR-024-mining-economy.md): CARGO and REFINERY activate beside HANGAR; refit,
  trade and priced repair stay stubs.)*
- **No combat interaction beyond the reservations.** `CombatEngaged` is numbered and inert;
  protection is damage-immunity with nothing yet dealing damage; interdiction of the
  dock approach waits for combat. The early-break rule was shaped so combat can arrive
  without reshaping protection.
- **No visibility of others' docked ships, and no station raids.** Docked is absolutely
  safe and absolutely private. Both are stated costs of §1, accepted.
- **No persistence.** The roster is the obvious save anchor — the RESUME card's "Docked at
  Vesta-3" becomes literally true when a save file exists — but no save file exists, and
  this ADR does not create one. *([ADR-024 §7a](ADR-024-mining-economy.md) creates it and
  [ADR-025](ADR-025-persistence.md) designs it: an engine-owned journal plus snapshot at the
  universe layer, with the roster among the first things it writes down.)*
- **No AI commander.** A disconnect mid-approach halts outside the station (§2), the same
  gap ADR-016 §8 accepted, closed by the same future feature.
- ~~**No in-space wing reassignment** (§6)~~ — **built 2026-08-23**, see the amendment in §6;
  what stands is **no per-class dock ceremony** — a Carrier and an Interceptor dock alike,
  instantly; pageantry is presentation's if it ever wants it.

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
  **§6b adds T4** (2026-08-23): T4a the registry half of the wing lifecycle, T4b the client
  half, milestone **H2** — the round-trip loop under the new rules.
- **Schema bumps, enumerated once**: `OrderKind{+Warp reserved, +Dock}`;
  `OrderReason{9–13}`; `EntityRecord.statusBits`; `StationCommand`; `StationRoster` — one
  cluster in T2, riding the fail-closed hash.
- **The snapshot ship cap falls 45 → 43** (§5), still above `Snapshot.h`'s asserted floor of
  41 but on half the margin. T2 updates the constant's comment with the new arithmetic, and
  the static asserts catch it if the number is ever wrong. *(Superseded 2026-08-22: U3d-b
  removed the cap along with the full-snapshot format — see §5's note.)*
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
