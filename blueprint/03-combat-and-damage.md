# 03 — Combat and damage

Status: draft
Depends on: 01, 02, 11, 12

## 1. Purpose and player experience

Combat is what the flight model, the equipment system and the economy are
for. Almost every purchase changes one number in this chapter: damage per
second, energy per second, shield capacity, hull.

The player aims with the cursor. Guns converge on the point under it and
fire while the button is held and the power pool lasts. Bolts are slow
enough (500 to 800 m/s against ships at 80 to 300 m/s) that leading the
target matters, and short-ranged enough (550 to 800 m) that fights are
close. Guided munitions, mines and countermeasures use ammunition
instead of energy.

A target is worn down in layers. Its shield absorbs everything until it
collapses, then stays down for several seconds. In that window the hull,
individual parts and mounted equipment take damage: parts break off, guns
are shot away, and at authored thresholds the ship starts to smoke and
burn. At zero hull it explodes, hurts whatever is close and leaves some
cargo and equipment behind as loot.

The player's survival has the same layers plus two consumables, one for
hull and one for shield. Running away is a real option: cruise outruns
everything but disables weapons, and one munition exists to stop it.

## 2. Concepts

Glossary terms (munition, countermeasure, fuse, shield, hull, part, power
pool, repair consumable, tractor, loot, sub-object identifier) are used as
defined in chapter 00. This chapter adds:

| Concept | Definition |
| --- | --- |
| Launcher | External equipment that creates munitions: gun, turret, guided launcher, mine dispenser, countermeasure dispenser. All share one firing cycle. A launcher has one to eight barrels; all fire on each shot. |
| Bolt | An unguided munition. It has no body: it moves in a straight line and is traced against collision shapes. |
| Guided munition | A munition with a body, a motor (delay, acceleration, burn time) and usually a seeker (range, cone, turn-rate limit). It damages by exploding, not by touching. |
| Explosion | A record of area damage: radius, hull damage, energy damage, impulse. |
| Weapon type, shield type | Labels on a munition and on a shield generator. A table gives a damage multiplier per pair. |
| Shield bubble, shield generator | The bubble is the shield's collision shape around a ship; it is a sub-object, so a hit can name it. Generators give it capacity and regenerate it. |
| Hull-linked part | A part flagged so that a hit on it also damages the hull in full. |
| Damage list | The result of one damage event: new hit points per sub-object, what became of each, the cause and the inflictor. The unit the authority applies and replicates. |
| Damage cause | Hostile causes respect invulnerability; system causes (repair, recharge, fuse) do not. |
| Armour | Internal equipment with one number that divides damage to hull and parts. |
| Death fuse | A fuse whose last action destroys the object. While it burns, hostile damage cannot destroy the object. |
| Loot rule | Per item, how much of a carried stack survives its carrier's death as loot. |

```text
launcher ─▶ munition ─┬─ bolt ─────────────▶ hit report ─┐
                      ├─ guided, mine ─────▶ explosion ──┤
                      └─ countermeasure (diverts guided) │
collision, hazard zone, field mine, fuse action ────────┤
                                                         ▼
                                 DAMAGE LIST for one object
  bubble hit ─▶ shield only (weapon × shield modifier)
  otherwise ──▶ hull │ part │ equipment, and the power pool
  thresholds ─▶ fuses ─▶ effects, parts shed, death ─▶ explosion, loot
```

## 3. How it works

### 3.1 Who decides what

| Matter | Authority | Presentation |
| --- | --- | --- |
| A player's firing | Trusts the fire command; in multiplayer checks ammunition only | Runs the firing cycle for its own ship, sends the launcher list and aim point |
| NPC firing | Runs the cycle; announces launchers and aim point | Replays the shots |
| Bolt hits | Applies reported hits; in multiplayer traces NPC bolts itself | Traces bolts; in multiplayer reports only its own player's hits |
| Guided munitions, mines, countermeasures | Owns objects, steering, triggers and explosions | Mirrors them |
| Collisions | Computes damage to objects no player owns | Computes and reports damage to its own player's objects |
| Shields, hull, parts, fuse damage, death, loot | All of it; sends damage lists and fuse events | Applies the lists; plays fuse effects (inferred) |

A hit report names the munition archetype, the shooter, the object and
sub-object hit and the hit point. The authority does not re-check the
geometry. It drops reports from a player with no living ship and, when
player-against-player damage is off (chapter 11), any hit, collision or
blast that one player inflicts on another.

### 3.2 The firing cycle

Every launcher keeps a clock that counts up from its last firing attempt.
A fire command names launchers and one aim point. Each launcher refuses
with the first reason that applies:

1. it is destroyed, or disabled at zero health;
2. the clock is below the refire delay;
3. the power pool holds less than `power per shot × barrels`;
4. it needs ammunition and the hold has fewer units than barrels;
5. the owner is cloaked;
6. the owner is travelling in a lane;
7. guns only: the aim point lies more than 40° off the barrel axis;
8. guns and mine dispensers: cruise is engaged.

An accepted shot subtracts the power, removes one unit of ammunition per
barrel and creates one munition per barrel. The clock restarts on every
attempt that passed test 2, so trying to fire on an empty pool still costs
a refire cycle.

- Countermeasure dispensers skip test 8: decoys can be dropped in cruise.
- Energy weapons need no ammunition and guided weapons no power. The two
  families draw on different budgets: the pool, and cargo plus money.
- A launcher on a revolving hardpoint slews towards the aim point at its
  turn rate (90°/s on every record). With no fresh aim point for 2 s, a
  gun with a target tracks the target's lead point.

A bolt leaves with `aim direction × muzzle speed + shooter velocity`, so
its range relative to the shooter is `muzzle speed × lifetime`. For a
target at relative position `p` with relative velocity `v`, the lead point
is `target + v × t`, where `t` is the smallest positive root of
`|p + v·t| = muzzle speed × t`. Turrets, lock checks and the lead marker
use it.

### 3.3 Munitions in flight

**Bolts** carry hull damage, energy damage and a weapon type. Each tick a
bolt advances by `velocity × dt` and the segment is traced. The first
shape hit ends it; otherwise it vanishes when its lifetime (0.8 to 1.2 s
on most guns) runs out.

**Guided munitions** are bodies with mass 1, no damping and 2 hit points.

| Phase | Rule |
| --- | --- |
| Launch | Starts at muzzle speed (17 to 360 m/s) plus the launcher's velocity. It inherits the launcher's target and chosen sub-object if the lock time (0 s on every record) had elapsed. |
| Burn | From `delay` to `delay + burn time` the motor adds `acceleration × dt` along the nose. Final speed is `muzzle speed + acceleration × burn time`. |
| Homing | Each tick, if the target is inside seeker range and cone: turn at `3 × line-of-sight rotation rate`, capped at the maximum turn rate, only while the range is closing; then realign the velocity with the nose at unchanged speed. |
| Lost | Target outside range or cone once: the munition forgets it and flies straight. |
| Trigger | A shape other than the launcher's within the detonation distance (0.5 to 12.5 m): explode. |
| End | Lifetime over, or destroyed: explode in place. Destroyed by a countermeasure: vanish without a blast. |

A guided munition's own damage figure is zero; all its damage is the
explosion (3.6). A bolt, a blast or another guided munition destroys it.
Three things defeat it: a decoy, leaving its cone or range, and
out-turning it (the cap runs from 0.6 rad/s on the heaviest to 8.7 on the
lightest). Without a seeker it is a rocket; capital-ship flak uses that.

**Countermeasures.** A new decoy looks once at every guided munition
within its range (1,000 m) that its own launcher does not own. Each is
diverted to the decoy with the decoy's probability (70, 80 or 90 % by
grade), homes on it and is destroyed without a blast on reaching it. The
decoy lives 3 s; a munition still chasing it then has no target. A
dispenser notices a guided munition aimed at its owner within 999 m; an
NPC then drops a decoy automatically.

**Mines** are bodies with drag, 2 hit points, a 10 s lifetime and a 4 s
owner-safe time, during which the ship that dropped them is ignored.

| State | Event | Next | Effect |
| --- | --- | --- | --- |
| Drifting | an object within seek distance (400 m) | Seeking | — |
| Seeking | each tick | Seeking | Accelerate towards the nearest intruder at up to 200 m/s² until closing speed reaches top speed (60 to 80 m/s). |
| Seeking | intruder within detonation distance (4 m) | Gone | Explode. |
| any | destroyed by a hit | Gone | Explode. |
| any | lifetime over | Gone | Vanish without a blast. |

### 3.4 From hit to damage

Every source ends in a damage list for one object. For a munition hit on
a ship or solar:

1. **Shield.** If the hit names the shield bubble, only the shield is
   damaged:

   `shield damage = (hull damage × 0.5 + energy damage) × modifier`

   The modifier is looked up by weapon type and the generator's shield
   type; it is 1.0 when either is missing. Nothing passes through: a hit
   that empties the shield is absorbed whole. The bubble encloses the
   ship and collides while the shield is up, so it is hit first.
2. **Hull, part or equipment.** With the shield down the hit names what
   the bolt touched. The hull takes the hull damage. A part takes it, and
   if the part is hull-linked the hull takes the same amount as well. An
   equipment item takes it alone.
3. **Power.** In the cases of step 2 the energy damage is subtracted from
   the target's power pool.

| Target | Scaling |
| --- | --- |
| Hull | ÷ armour; × difficulty |
| Part | ÷ armour; ÷ 3 if a player owns the object; × difficulty |
| External equipment | ÷ 5 if a player owns the object; × difficulty |
| Shield, power pool | × difficulty |

"Difficulty" is the player's difficulty scale, 0 to 2, default 1. It
applies to the ship a player is flying, in single player only. The two
divisors mean a player loses wings and guns three and five times more
slowly than an NPC in the same hull.

What reaching zero does:

- *Shield*, at or below its offline threshold: it collapses (3.5).
- *Equipment*: destroyed and separated from the ship.
- *Part*: destroyed and shed as debris with a small explosion and a kick
  to both pieces. Equipment mounted on it and parts attached to it go
  with it. Losing every part that carries an engine nozzle stops the
  engine (chapter 02).
- *Hull*: the object is destroyed (3.9), unless it is invulnerable or
  already burning a death fuse.

Applying the list sets the hit points, tells content controllers and the
target's pilot who did how much (chapters 05 and 07), checks fuse
thresholds (3.8) and sends the list to the presentations.

Solars flagged indestructible ignore hull damage but still report the
attack, and their parts and equipment stay vulnerable: a station can be
provoked and lose its turrets without being destroyed.

**Worked example.** A mid-grade gun: 131.8 hull damage, no energy damage,
refire 0.25 s, 26.35 power per shot, 600 m/s, lifetime 0.916 s. That is
527 damage and 105 power per second out to 550 m. Against a shield of
2,884 with the unfavourable modifier 1.2, a shot removes
`131.8 × 0.5 × 1.2 = 79`. The shield collapses at 15 %, after 2,451
points: 31 shots, 7.8 s from one gun. A 1,600-point hull without armour
then lasts 13 shots, 3.3 s. A power plant of capacity 1,800 charging 183
per second feeds two such guns for about a minute and four for 7.5 s.
Energy, not refire, limits sustained fire.

### 3.5 Shields and power

A ship has one bubble and one or more generators. The bubble's capacity
is the sum of the generators' capacities, its offline threshold their
average (15 % on every record), its rebuild time their maximum (12 s).

Each generator, each update, while the shield is below full: if the power
pool holds `rebuild draw × dt` (10 per second on every record), subtract
it and add `regeneration rate × dt` to the shield; otherwise do nothing.
A drained pool stops shield recovery, which is the point of energy
damage.

| State | Event | Next | Effect |
| --- | --- | --- | --- |
| Up | health ≤ offline threshold | Down | Bubble stops colliding; "collapsed" announced; earliest return is now + rebuild time. |
| Down | rebuild time over, health above threshold, nothing inside the bubble's volume | Up | Bubble collides again; "rebuilt" announced. |
| Down | shield consumable used | Up, if health is now above threshold | The wait is cancelled. |

Regeneration continues while the shield is down. The "nothing inside"
test keeps the bubble from switching on around an intruder (inferred).

The power plant refills the pool at `charge rate × dt` (chapter 02); in
the data that is about a tenth of capacity per second.

### 3.6 Area damage

An explosion finds every collision shape within its radius `R` (at most
60), reduces them to their owning objects and treats each object once.
The exploding object is skipped.

For a target with bounding radius `r` at centre distance `D`, let
`e² = max(0, D² − r²)`. The band factor `f` is 1 if `e < R/3`, 2/3 if
`e < 2R/3`, 1/3 if `e < R`, else 0.

- A target with a working shield: each shield takes `(hull damage × 0.5 +
  energy damage) × f × the shield's blast resistance`, and nothing else
  is touched, whatever `f` is. No weapon-type modifier applies.
- Otherwise the hull takes `hull damage × f × the archetype's blast
  resistance`. Each external equipment item and each part takes the same
  formula with its own centre, radius and resistance. The power pool
  takes `energy damage × f × resistance`.
- Loot, guided munitions and mines take the hull formula.
- If the explosion has an impulse, each object no player owns is pushed
  away from the centre.

Because parts and equipment are tested one by one, a blast beside an
unshielded ship can remove several guns at once.

A cruise disruptor is a fast guided munition with a wide seeker cone
(90° to 120°), a large radius (50 m) and little damage. Its blast has its
own damage cause. An NPC ship that receives a damage list with that
cause loses cruise with the cooldown of chapter 02, even if only its
shield was hit. The same should hold for a player's ship (inferred).

### 3.7 Other sources of damage

- **Collisions** (chapter 02): damage by A to B is `0.5 × relative speed
  × min(1, mass_A / mass_B)`. It goes to the sub-object touched, the
  bubble included, and is scaled like any hit.
- **Hazard zones** (chapter 01): a ship accumulates `rate × immersion ×
  dt`. Once more than 1 s has passed and the sum exceeds 1 % of the ship's
  total hit points, it is applied as a fraction `q`: the hull loses `q`
  of its maximum, then every external equipment item and every part
  likewise. If `q > 1` the ship is destroyed. Lumps keep message traffic
  low.
- **Field mines** (chapter 01) are not munitions. Each has a detection
  radius (35 to 150 m) and a recharge time (1.5 to 3 s). While a ship is
  inside the radius and the mine is charged, it sets off its explosion,
  is not consumed, and recharges.
- **Lethal rocks** in a field destroy any object reported as touching
  them, unless it is invulnerable.

### 3.8 Fuses

A fuse has a name, a lifetime and actions placed at fractions 0 to 1 of
that lifetime. Action kinds: start an effect at a hardpoint; destroy or
damage a named or random part; destroy or damage the equipment on a
named or random hardpoint; damage the hull; apply a blast impulse; set a
pilotless ship tumbling; switch invulnerability; dump cargo; ignite
another fuse; destroy the object. An action's time may be a range,
rolled when the fuse is lit.

Archetypes list fuses for the hull, and each part lists its own, as
(fuse, starting fraction, hit-point threshold). When a damage list takes
hit points from above a threshold to it or below, the fuse is lit. When a
repair takes them back above, the fuse is unlit and its effects are
withdrawn. Thresholds are absolute: a small ship of 1,600 hull typically
has three stages at 400, 200 and 133.

Each update the lit window grows by `dt / lifetime` in both directions
from the starting fraction, and every action inside it runs once. Lit at
0 this is a plain timeline.

Fuses give large objects a staged death. A capital ship's part lists a
death fuse with threshold 1. Reaching it starts a sequence that destroys
sections and attachments in order and ends with "destroy the object".
While a death fuse burns, hostile damage neither destroys the object nor
sheds further parts, so the sequence always completes.

NPC ships get a generic version. When an NPC takes fatal damage, the
authority asks the content controllers for a delay. Given a positive
time, it lights a death fuse of that length: the ship leaves its
formation, its wingmen are told, and it dies when the fuse ends. The
delay is for the pilot's last words (chapter 05).

### 3.9 Death and loot

On violent destruction, in order: hull to zero; the content controllers
receive "destroyed" with killer, position and the droppable items; the
object's explosion plays, which may itself do area damage (7 of 70 do:
radius 100 to 200 m, damage 300 to 40,000); loot is created.

Droppable items are everything on board that is intact, flagged lootable
and not a cargo pod. The content side rewrites the list first:

1. Merge stacks of the same item.
2. Sum the worth `W` of all players within 5,000 m (chapter 07).
3. For a stack of `c` units with rule (chance `p`, minimum worth `w0`,
   worth per unit `w1`, base `b`, ceiling `m`, cap `k`): nothing if
   `W < w0`. Otherwise `s = min(c, (W − w0) / w1)`, `n = min(b + s, m, c)`
   and the drop is `n × p`, its fraction rounded up with that
   probability, at most `k`.
4. Give each item 50 to 80 % health.
5. Add bonus loot: for each bonus record whose rank range holds the dead
   ship's rank, roll its chance and add a random count of its item.

Items without a rule fall back to a default with chance zero, so an NPC's
guns do not drop unless the item has a rule. The bonus table is how any
fight can return repair consumables.

At most six containers are created, each with the item's container
model, up to its units-per-container, the ship's velocity plus a random
kick, and a spin. Loot that no player has come within 2,500 m of for 60 s
is removed. A container has hit points and can be shot. The tractor
(range 1,500 m) is told which loot to pull; the pull was not traced.
This path makes loot for NPCs, and for a player's ship only in single
player.

### 3.10 Repair consumables

One item restores hull (600 per unit), one restores shield (300 per
unit). A use asks the authority for a count. It computes `missing / per
unit`, rounds up when the remainder exceeds 5 % of a unit, caps the
result by the count and the stack, applies the new value as a damage list
with a system cause and removes the units. The hull item repairs the hull
only: parts and equipment stay damaged until a base repairs them
(chapter 08). The shield item goes to the weakest shield and cancels the
rebuild wait, so it brings a collapsed shield back at once.

NPCs carry the same items: when the ratio falls to a threshold (default
35 %), wait (2 s), use one unit, wait again (5 s). The pilot record
overrides the three numbers (chapter 05). A ship archetype caps how many
of each a ship may carry (chapter 02).

### 3.11 Weapon groups and firing controls

Each launcher has an active flag. "Fire" fires every active gun at the
cursor. Separate actions launch each kind of guided munition, mines and
countermeasures, and fire launcher 1 to 10 alone.

There are six weapon groups, each a list of hardpoint names. "Assign"
stores the hardpoints of the active launchers, "select" makes exactly
that set active, and "fire group" fires the set once without changing
the selection. Naming hardpoints, not items, lets a group survive a
change of gun. The authority stores the groups with the character.

### 3.12 Invulnerability

An object holds invulnerability commands, one per issuer kind (launch
protection and fuses were seen; mission scripts inferred). Each says:
protect from everything, protect from players, and the largest fraction
of health the object may lose. Flags combine by "or", fractions by
"minimum". While protected, no sub-object falls below `1 − fraction` of
its maximum. System causes bypass the rule.

## 4. Data that defines it

Counts are from chapter 12.

| Record | Principal fields | Values |
| --- | --- | --- |
| Gun (513) | hit points, hardpoint class, power per shot, refire delay, muzzle speed, turn rate, munition, lootable | refire 0.12 to 4 s; power 0 to 293; hit points 400 to 81,000 |
| Bolt munition | hull damage, energy damage, weapon type, lifetime | hull damage 4.6 to 1,956 |
| Guided munition (76; 71 seeking) | explosion, seeker kind, range and cone, maximum turn rate, detonation distance, lifetime, units per container, cruise-disruptor flag; motor delay, acceleration, burn time | range 1,000 to 3,000 m; cone 35° to 120°; lifetime 3 to 56 s; 3.6 to 1,152 m/s² for 0.6 to 11 s |
| Explosion (86 for munitions, 70 for objects) | radius, hull damage, energy damage, impulse; effect, debris | munitions: radius 2 to 50 m, hull 16 to 11,736, energy 0 to 7,824 |
| Mine and dispenser (10) | seek distance, top speed, acceleration, drag, detonation distance, owner-safe time, lifetime, explosion | blast 326 to 3,912 at radius 16 m; refire 1 s |
| Countermeasure and dispenser (3) | range, diversion percentage, lifetime; refire, automatic-release range | 70, 80, 90 %; refire 0.25 s |
| Shield generator (123) | capacity, regeneration rate, shield type, offline threshold, rebuild time, rebuild draw, hardpoint class | capacity 450 to 12,900; two families refill in about 45 s; 30 records regenerate 1 per second |
| Weapon type (21) | one multiplier per shield type (9) | 0.8, 1.0 or 1.2 |
| Armour (24) | damage divisor | 0.25 to 30, and one effectively infinite |
| Part (246 on ships, 238 on solars) | model node, hit points, hull-linked flag, debris kind, separation explosion, impulses, cap model, fuses | hit points 1 % to 100 % of hull; 224 of the ships' parts are hull-linked |
| Fuse (209; about 1,960 actions) | lifetime, death flag; per action its time or time range and parameters | lifetime 0.01 to 15 s |
| Loot rule (434), bonus loot (124) | the six numbers of 3.9; item, rank range, chance, count range | chance 0 to 100 %, median 8 %; bonus chance 0.5 to 44 % |

**Shape of the gun list.** 266 guns fit player ships: 166 fixed energy
guns, 81 turrets, 15 guided launchers and 4 heavy launchers, in ten
hardpoint classes. The other 247 have no class and cannot be mounted by a
player; 162 of them appear in ship and solar loadouts. From class to
class the median price doubles while median damage per second grows about
1.35 times (136 to 2,150), and power per second with it (42 to 489).
Within a class, guns trade refire (0.12 to 0.5 s) against damage per
shot and differ in weapon type, speed and range. Seven energy guns
invert the split, with hull damage near zero and energy damage 30 times
higher: they strip shields and drain pools.

| Example | Class | Damage | Refire | Power per shot | Flight |
| --- | --- | --- | --- | --- | --- |
| `example_gun_mid` | 6 | 131.8 hull | 0.25 s | 26.4 | 600 m/s, 550 m |
| `example_turret_heavy` | 8 | 424 hull | 0.33 s | 106 | 702 m/s, 800 m |
| `example_missile_light` | 3 | blast 373.5 at 16 m | 1 s | ammunition | 33 rising to 100 m/s; turn 4.4 rad/s |

**Weapon against shield.** Seven weapon families and three shield
families are declared in three tiers each; the tiers carry identical
numbers and the data uses only the first. The seven families reduce to
three patterns, each a rotation of (0.8, 1.0, 1.2) over the shield
families. 229 munitions name a weapon type; the rest get 1.0.

**Guided weapons by role.** Light missiles: blast 245 to 4,890 by class
at 16 m, 90 to 100 m/s. Shield-breaking missiles: the same flight with
energy damage 652 to 7,824. Heavy guided munitions: blast 2,400 to
11,700 at 25 m, 50 to 60 m/s, turn 0.6 rad/s; for stations and capital
ships. Cruise disruptors: 700 to 800 m/s, blast 37 to 159 at 50 m.

## 5. Connections

Needs:

- 01: physical zone, immersion, damage rate; field mines, lethal rocks.
- 02: hardpoints and their classes; power pool; cruise and its
  disruption cooldown; collisions; parts and engine nozzles.
- 11: fire command, hit and collision reports, damage-list and fuse
  messages; the player-against-player switch.
- 12: record counts.

Gives:

- 04: launch protection is an invulnerability command.
- 05: launchers and the lead point; damage notifications with inflictor
  and amount; NPC use of consumables and decoys.
- 06: armour as the toughness scale of spawned ships; rank for bonus
  loot.
- 07: "damaged by" and "destroyed by" events for reputation and kill
  credit; loot as income.
- 08: invulnerability and fuses as mission tools; damage left for base
  repair.
- 09: shield, hull and power read-outs; lead marker; launcher status;
  weapon groups.

## 6. Design notes for the builder

**Essential.**

- *Layers with different rules.* Shield regenerates and takes half of
  hull damage; hull does not regenerate; parts and equipment are lost.
- *No bleed-through, a long collapse.* The shield is perfect until 15 %,
  then gone for 12 s. That gives a fight its rhythm: break the shield,
  then make the window count.
- *Energy as the firing budget.* Refire sets burst damage, the pool sets
  sustained damage. Without it the best loadout is "all the biggest
  guns".
- *Two ammunition economies.* Free, energy-limited guns and paid,
  count-limited guided weapons.
- *Cruise disables guns, and one weapon disables cruise.* Flight becomes
  a decision for both sides.
- *Damage lists.* One replicated record serves weapons, hazards, repair
  and scripts alike.

**Feel.**

- Hull-linked parts: shooting a wing off is a visible bonus, not a
  different tactic, because the hull takes the same damage.
- A player's parts and guns are three and five times tougher than an
  NPC's. Losing equipment is a threat, rarely an event.
- The 0.8/1.0/1.2 table rewards matching weapon to shield by a fifth: a
  reason to look at the target, not a hard counter.

**Accidents of the era.**

- Hits are detected on the shooter's presentation, and the authority
  checks neither geometry nor a player's refire and power. That gives
  lag-free hits and is easily abused. Validate range, rate and energy on
  the authority, or trace bolts there.
- A fire attempt on an empty pool restarts the refire clock.
- Lock time and shot dispersion exist in code and are zero or absent in
  the data.

**Simplifications and their cost.**

- No part or equipment damage: loses crippling, stripping the turrets
  of stations and most fuse content; saves per-part collision shapes.
- No energy damage: loses the shield-stripper and pool-drain roles.
- Fuses as plain timelines lit at zero: loses "unlight on repair".
- Loot with a flat chance per item: loses the scaling of drops with
  player wealth.

## 7. Open questions

1. How does a player's own ship learn that its cruise was disrupted? The
   authority's cancellation is written for NPC ships only.
2. Is the automatic countermeasure release active for the player's ship?
   The presentation has a handler for it; an update flag that may
   suppress it was not decoded.
3. What does a player's ship drop on death in multiplayer? The loot path
   here excludes it.
4. What do the "automatic turret" flag on guns and the "toughness" number
   on guns and shields control?
5. Where is the cap of 50 units per munition type on a player (a code
   default) enforced?
6. Do the 85 guns that are neither mountable nor in a loadout file serve
   NPC tables elsewhere, or are they dead records?
7. Not traced: the blast resistance of shield bubbles, a test that
   excludes some ships from bonus loot, and which data marks a part
   whose loss destroys the whole object (the code has such a flag).

## 8. Basis

Design documents, for orientation: `design/sim-records.md`,
`design/adr/0001-object-model.md`, `design/adr/0007-server-world.md`,
assessments 02 and 04. Every rule above was read in code.

Data, under `outdir/data/`: `EQUIPMENT/weapon_equip.ini`,
`weaponmoddb.ini`, `st_equip.ini`, `misc_equip.ini`, `select_equip.ini`;
`FX/explosions.ini`, `FX/fuse.ini`; `MISSIONS/lootprops.ini`;
`constants.ini`; `SHIPS/shiparch.ini` and `SOLAR/solararch.ini` (parts,
fuse lines); `SOLAR/asteroidarch.ini` (field mines);
`INTERFACE/keymap.ini`; the four loadout files. Counts are cited from
chapter 12, which also notes one fuse file outside the manifest.

Code, under `CppSource/`: `Common/common_celauncher.cpp`,
`common_cegun.cpp`, `common_cedroppers.cpp`, `common_cprojectile.cpp`,
`common_cmine.cpp`, `common_ceshield.cpp`, `common_cepower.cpp`,
`common_fuses.cpp`, `common_repairmanager.cpp`, `common_physys.cpp`
(`Collision::CalcCollisionDamage`), `common_misc.cpp`;
`Server/server_gameobject.cpp`, `server_guided.cpp`, `server_mine.cpp`,
`server_countermeasure.cpp`, `server_fuse.cpp`, `server_loot.cpp`,
`server_classes.cpp` (`StarSystem::explode`, `munition_collision`,
`collision_damage`, `MedMine`), `IServerImpl.cpp`;
`Content/SpaceSupport.cpp`, `LootProps.cpp`;
`OutpostFrontier/Client/client_gameobj.cpp`, `hud_weapon_groups.*`,
`hud_status_core.cpp`, `cli_projectile_fx.cpp`.

Inferred, as marked: the presentation's share of fuse playback; the
purpose of the bubble's "nothing inside" test; cruise disruption of the
player's ship; mission scripts as issuers of invulnerability. That a
munition never collides with its launcher rests on the launcher's
identifier serving as the munition's collision group; the filtering
itself was not read. Damage causes were read as numeric literals; their
names here are descriptive.
