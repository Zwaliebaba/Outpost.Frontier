# 02 — Ships, equipment and flight

Status: reviewed
Depends on: 00 (glossary), 01 (systems, zones, axes and scales), 11 (authority and the tick)

## 1. Purpose and player experience

This chapter covers what a ship is made of and how it moves. Everything the
player does in space happens through one ship: a hull bought at a ship
dealer, fitted with equipment from equipment dealers, flown with a mouse
and a handful of keys.

- **A ship is a kit.** A hull offers a fixed set of mounting positions. The
  player fills them with guns, a shield, a thruster and dispensers, and
  carries cargo in a hold of fixed volume. A better hull means more and
  higher-grade positions, more hull points, a bigger hold and a bigger
  power plant. It does not mean more speed.
- **Flight is immediate and forgiving.** The ship goes where its nose
  points, reaches top speed in well under a second and stops almost as
  fast. The player steers by moving the cursor away from the screen
  centre; the further out, the faster the turn.
- **Four speed regimes, each with a price.** Normal flight (80 m/s) is
  free and allows fighting. The thruster (200 m/s) burns a reserve of
  about fifteen seconds. Cruise (300 m/s) takes five seconds to
  engage, forbids weapons and can be knocked out by an enemy. Engine kill
  removes drag, so the ship coasts on its velocity while the nose turns
  freely.

The same object model and flight code drive every NPC ship and station,
so what the player learns about their own ship holds for all they meet.

## 2. Concepts

Archetype, instance, loadout and the other glossary terms are used as
defined in chapter 00. This chapter adds:

| Concept | Definition |
| --- | --- |
| World object | Anything simulated in a system. It has an identifier, a pose, an archetype and, if it can collide, a body. |
| Simple object | A world object with hit points, an owning player (or none) and a current zone. |
| Equipped object | A simple object that carries equipment, a power pool, parts, docking points and a pilot: a ship or a solar. |
| Body | The rigid body of an instance: mass, inertia about the three ship axes, linear and angular velocity, one convex collision shape per part. |
| Part | A separately damageable piece of the model with its own hit points and collision shape. |
| Hardpoint | A named position and orientation in the model, fixed or revolving, to which external equipment attaches. |
| Hardpoint class | A label such as "gun grade 4". A hull declares which classes each hardpoint accepts; an item declares its class. |
| Sub-object identifier | A small number, unique within one ship, naming the hull, a part or one equipment item. Damage, activation and stored loadouts address items by it. |
| Power pool | Energy store for weapons, shield rebuilding and cruise. |
| Thrust pool | A second, separate store for the thruster and for strafing. |
| Force controller | A contributor of force or torque to a body, run once per tick: engine, thruster, steering, strafe, nudge, and the scripted movers of chapter 04. |
| Axis throttle | The steering demand: a vector whose direction is the wanted rotation axis and whose length (0 to 1) is the fraction of full torque. |
| Nudge | A free thrust in any direction that pilots use for fine positioning. |

```text
ship archetype ──────────────┐             equipment archetype
  model: parts, hardpoints   │ instantiate          │ instantiate
  flight coefficients        ▼                      ▼
  classes per hardpoint   SHIP INSTANCE ──has──▶ equipment instances
loadout ────────────────▶  body, parts            internal: engine, power plant,
  item @ hardpoint         power and thrust pool    scanner, tractor, armour
  cargo × count            cargo stacks, pilot    external @ hardpoint: guns, shield,
                              │                     thruster, dispensers, lights
                              ▼
  force controllers push the body ─▶ damping ─▶ integration ─▶ pose
```

## 3. How it works

### 3.1 The kinds of world object

| Kind | Hit points | Equipment | Notes |
| --- | --- | --- | --- |
| Ship | yes | yes | The only kind that flies under force controllers. |
| Solar | yes | yes | Fixed; may carry guns, shields, lights (chapter 01). |
| Loot | yes | no | A drifting container of cargo (chapter 03). |
| Asteroid, dynamic asteroid | — | no | Field members (chapter 01). |
| Munition, guided munition, beam, mine, countermeasure | some | no | Chapter 03. Beams are rays, not bodies. |
| Detached equipment | no | no | External equipment with a body of its own, for example after being shot off. |

Kinds form a hierarchy, and rules are written against the most general
kind that fits, so docking, shields, power and targeting work the same on
a station as on a fighter.

An identifier is a nickname hash for authored objects and, for the rest,
a counter that only the authority advances; the two ranges never overlap.

Objects are destroyed at the end of a tick, never inside one, and every
reference another object holds (target, formation leader, gun lock, dock
queue) then reads as "gone". A builder should make this the only way
references behave; the original also has plain ones (per design notes).

### 3.2 Archetype against instance

An archetype is loaded once and never changes. An instance stores what
varies: pose and velocities, hull and part hit points, each equipment
item's health and on/off state, cargo counts, the two pools, the throttle,
the steering demand and the pilot.

Creating a ship takes an archetype, a loadout, a pose and a velocity. The
body is created with the archetype's mass and inertia; parts are created;
each equipment item is created with health as a fraction of its archetype's
hit points and attached to its hardpoint; the capacities of all power
plants are summed into the maxima of the two pools, which start full; the
force controllers are enabled.

The player's ship is built the same way from the character's stored
equipment list (chapter 11). Each entry has a sub-object identifier, an
archetype, a hardpoint name (empty for internal items, a reserved name for
the hold), a health fraction, a count and a mounted flag.

### 3.3 Hardpoints and mounting

Hardpoints live in the model file, named by kind plus number (weapon,
turret, torpedo, shield, thruster, mine, countermeasure, engine nozzle,
lights, trail, bay door, tractor source, cargo pod). A gun on a revolving
hardpoint can swing within the joint's limits.

1. The game has a fixed table of about sixty hardpoint classes: gun grades
   1 to 10, turret grades 1 to 10, torpedo grades 1 and 2, shield grades 1
   to 10 for each of three hull families (light fighter, heavy fighter,
   freighter), and ungraded classes for thruster, mine dispenser,
   countermeasure dispenser and cargo pod. Ten of them are the gun grades.
2. A ship archetype lists, for each class it supports, the hardpoints that
   accept it. A grade-3 gun position appears in the lists for grades 3, 2
   and 1: downward compatibility is authored, not implied by code.
3. An equipment archetype states its class (a gun may state several).
4. An item may be mounted only on a hardpoint that is in the hull's list
   for the item's class.

The authority also verifies that no gun or shield mounted on a
character's ship has a grade above the best grade the hull declares in
that family: a cheap guard against edited characters.

Internal equipment has no hardpoint and cannot be hit. Lights and effects
have no class; only authored loadouts mount them.

### 3.4 Equipment categories

Every item has an archetype with mass, volume, hit points and a "may drop
as loot" flag, and an instance that is active or not, destroyed or not. An
item *functions* when it is active and not destroyed.

| Category | Mounting | What it contributes |
| --- | --- | --- |
| Engine | internal | Forward and reverse thrust, most of the ship's drag, cruise. |
| Power plant | internal | Capacity and recharge rate of both pools. |
| Scanner | internal | Contact range (2,500 or 5,000 m) and cargo-scan range (2,000 m). |
| Tractor | internal | Pulls loot in from up to 1,500 m (chapter 03). |
| Armour | internal | Divides hull damage by a scale; grades NPC toughness. Values run from 0.25 to 30, most on a ladder of ratio 1.17; one record of 1e30 is how the data marks an indestructible ship. |
| Shield generator, gun, turret, launcher, mine and countermeasure dispenser | external | Chapter 03. |
| Thruster | external | Extra forward thrust paid from the thrust pool. |
| Cargo pod | external | Visible containers on large NPC haulers. |
| Cloak, scripted-effect and lane equipment | mixed | Used by solars and scripted ships. |
| Light, attached effect | external | Presentation only. |
| Cargo | hold | A stack of identical items with a count; no behaviour. |

The engine depends on the model. At creation it finds the hull's
exhaust-nozzle hardpoints (one to four). When a part carrying a nozzle is
destroyed the nozzle is removed, and an engine with no nozzle left stops
functioning unless flagged indestructible. No shipped engine has the flag,
so a ship can be crippled by shooting its engine section off.

### 3.5 Two energy pools

Each tick every functioning power plant adds `charge rate × dt` to the
power pool and `thrust charge rate × dt` to the thrust pool, capped at the
maxima. The power pool pays for weapon fire and shield rebuilding
(chapter 03) and for cruise (20/s). The thrust pool pays for the thruster
(125 to 165/s) and for strafing (2/s on player hulls).

Player power plants have power capacities of 1,000 to 11,900, recharging
about a tenth of capacity per second; all have a thrust capacity of 1,000
recharging at 100/s. Because the pools are separate, running away never
costs firepower and firing never costs the escape.

### 3.6 Cargo and the hold

Used volume is the sum over cargo stacks of `count × unit volume` plus the
volume of every internal item. Commodities have unit volume 1 (a few 0);
player equipment has 0. Free space for an item type is
`floor(remaining / unit volume)`, further capped for repair and shield
consumables by two per-hull limits (10 to 71 on player hulls) and for each
ammunition type by a global limit of 50.

Stacks merge by archetype and carry one health value; some commodities
decay at an authored rate while carried. Jettisoning opens the bay doors
and spawns loot; the doors stay open for the animation plus four seconds.
Cargo does not change the ship's mass (inferred: mass is read from the
body, which is built from the archetype).

### 3.7 The flight model

Ships fly in zero gravity as rigid bodies. A ship faces its −Z axis, +X is
right and +Y is up; distances are metres (per design notes; chapter 01).

**Linear motion**, each tick of length `dt`:

```text
D      = (hull drag + engine drag) × zone drag factor
k      = D / m                                   [1/s]
v_next = v × (1 − k·dt) + (ΣF / m) × dt
```

`ΣF` sums the force controllers below. Engine drag counts only
while the engine is active. When `k·dt` reaches 0.9 the factor becomes
`exp(−k·dt)`, which keeps light ships stable on long steps.

Two consequences shape the game:

- **Top speed is `F / D`.** Mass cancels; it only sets the time constant
  `m / D` with which the ship approaches that speed.
- **Almost all drag belongs to the engine.** Player hulls have drag 1,
  their engines 599. Switch the engine off and `k` falls 600-fold.

**Angular motion** works the same way per ship axis (X pitch, Y yaw, Z
roll), in the ship's own frame:

```text
k_i      = angular drag_i / inertia_i
ω_i_next = ω_i × (1 − k_i·dt) + (torque_i / inertia_i) × dt
torque_i = steering torque_i × axis throttle_i
```

The exponential switch differs here: all three axes change together once
the sum of their squared `k_i·dt` reaches 0.5. The maximum turn rate about
an axis is `steering torque / angular drag`,
the response time `inertia / angular drag`. All three are authored per
axis, so a hull can roll quickly and pitch slowly.

Two global limits guard the integration: 1,000 m/s and 0.94 rad of
rotation per tick.

**Worked example**, a first-tier light fighter as authored: mass 100, hull
drag 1, engine thrust 48,000, engine drag 599, reverse fraction 0.2,
steering torque (24,000, 24,000, 58,000), angular drag (15,000, 15,000,
35,000), inertia (2,800, 2,800, 1,000), strafe force 20,000, thruster force
72,000.

| Quantity | Calculation | Result |
| --- | --- | --- |
| Top speed | 48,000 / 600 | 80 m/s |
| Time constant | 100 / 600 | 0.17 s (95 % of top speed in 0.5 s) |
| Stopping distance from 80 m/s | v × m / D | 13 m |
| Reverse speed | 0.2 × 48,000 / 600 | 16 m/s |
| Thruster speed | (48,000 + 72,000) / 600 | 200 m/s |
| Strafe speed | 20,000 / 600 | 33 m/s |
| Pitch and yaw rate | 24,000 / 15,000 | 1.6 rad/s (92°/s), response 0.19 s |
| Roll rate | 58,000 / 35,000 | 1.66 rad/s, response 0.03 s |
| Coasting, engine off | k = 1 / 100 | speed halves in 69 s |

The player buys a ship as a package of hull plus default equipment. All
31 packages carry engines of this thrust and drag, so
they share the 80, 200 and 300 m/s regimes. They differ in mass (50 to
300, hence response of 0.08 to 0.5 s), turn rate (23 to 96°/s) and
everything that is not flight. Large NPC ships use other engines: haulers
80 m/s, gunboats 120 m/s, cruisers and capital ships 30 m/s, turning at 4
to 29°/s.

**Force controllers.**

| Force controller | Force or torque | Cost |
| --- | --- | --- |
| Engine | `throttle × thrust` along the nose, times the reverse fraction (0.1 or 0.2) when throttle is negative | none (authored 0) |
| Engine in cruise | see 3.8 | 20/s from the power pool; no thrust in a step the pool cannot pay |
| Thruster | fixed force along the nose (72,000 on all six models) | 125 to 165/s from the thrust pool; switched off when the pool cannot pay |
| Strafe | fixed force left, right, up or down in the ship frame | per-hull rate from the thrust pool; stops when it cannot pay |
| Nudge | `nudge vector (length ≤ 1) × nudge force`, in the ship frame | none; pilots only |
| Steering | axis throttle taken into the ship frame, scaled per axis by steering torque | none |

Better thrusters are not faster: they drain less and survive more damage.
The basic model's net drain is 165 − 100 = 65/s, so a full pool lasts
about 15 s and refills in 10 s.

### 3.8 Throttle, reverse, strafe, thruster, engine kill and cruise

- **Throttle** runs from −1 to 1. Setting it switches a killed engine back
  on; setting it to exactly 0 cancels cruise.
- **Reverse** is a switch that commands throttle −1; with drag this high
  it is mainly a brake.
- **Strafe** is a switch with four directions.
- **Thruster** is a switch. It is refused in cruise, and engaging cruise
  switches it off.
- **Engine kill** deactivates the engine item. Thrust ends, engine drag is
  no longer counted, cruise is dropped, and the ship keeps its velocity
  while steering works at full authority, because steering has its own
  force controller and its own drag. This is the game's one doorway into
  inertial flight: fly past a target, kill the engine, turn and fire
  backwards while drifting. Throttle, strafe, thruster or cruise ends it,
  and the returning drag pulls the velocity back onto the nose within half
  a second.
- **Cruise** is a mode of the engine:

| State | Event | Next state | Effect |
| --- | --- | --- | --- |
| Off | engage | Charging | Throttle forced to 1; thrusters off; guns refuse to fire from now on; "charging" announced. |
| Charging | charge time elapsed (5 s on every engine) | Cruising | "Cruising" announced. Until then thrust is normal engine thrust, but the 20/s drain already applies. |
| Cruising | each tick | Cruising | Thrust = `D × cruise speed × throttle × r`; `r` grows by `dt / 5 s` up to 1. |
| Charging, Cruising | disengage (toggle, zero throttle, engine off, pilot request) | Off | Throttle forced to 1; "off" announced. |
| Charging, Cruising | hit by a disrupting weapon (chapter 03) | Cooldown | Throttle forced to 1; a timer is set to −5 s; "disrupted" announced. |
| Cooldown | timer reaches 0 | Off | — |
| Cooldown | engage | Charging | The timer runs on from its negative value: the wait is the remaining cooldown plus the charge time. |

The cruise speed is one global constant, 300 m/s. Because cruise thrust is
defined as drag times that speed, every ship cruises at the same speed
whatever its mass, engine or zone. `r` starts at the ratio of normal top
speed to cruise speed (0.27 for player ships), so the hand-over is seamless
and full speed arrives about 3.7 s after charging ends. Mission scripts can
engage cruise instantly, skipping the charge.

A zone may scale drag (chapter 01). That lowers normal, thruster and
strafe speeds in proportion but leaves cruise speed alone.

### 3.9 From the player's input to motion

The player's ship is driven through the same manoeuvre layer as an NPC
(chapter 05). Under manual control that layer runs a "free flight"
manoeuvre fed with the player's direction, throttle, cruise wish, engine
wish and reverse flag. Automatic manoeuvres (go to a point, dock, join a
formation) replace it on the same ship with nothing changed below.

1. **Cursor to direction.** The cursor offset from the screen centre is
   divided by half the screen width and height, giving `x` and `y` in −1
   to 1. Mouse flight is on once the steering button has been held for
   0.25 s (sooner if the cursor moves clear of where the press began), or
   after the player toggles a lock. Cutscenes and lane travel suppress it.
2. **Direction to demand.** Pitch demand is `−y`, yaw demand `−x`, each
   divided by a turn-sensitivity factor and clamped. Roll has no direct
   input.
3. **Banking and levelling.** In the default chase view the manoeuvre
   layer adds roll: the ship banks into a turn by up to the hull's maximum
   bank angle (15° on freighters, 20 to 35° on fighters), scaled by the
   turn demand and by throttle (never less than 0.4), and rolls back
   towards a reference "up" when the demand is zero. The bank is real ship
   orientation, produced through the roll axis of steering.
4. **Demand to torque.** The result is the axis throttle, which steering
   turns into torque as in 3.7. The steady turn rate is proportional to the
   cursor's distance from the centre: rate control, so a cursor held still
   off-centre keeps the ship turning.

Throttle, reverse and engine wishes pass through a small arbiter in the
manoeuvre layer that owns the precedence rules of 3.8: reverse beats
throttle, a killed engine beats cruise, and cruise is refused when the way
ahead is obstructed.

### 3.10 The tick

Within one tick, for one system:

1. Every object updates: scripted movers; zone membership; each equipment
   item (pools recharge, cruise timers advance); the pilot's decision step;
   then the ship recomputes its damping from its current drag and inertia.
2. The physics step runs once: force controllers accumulate pushes; velocities
   are damped and the pushes committed; limits are applied; position and
   orientation advance; contacts are detected.
3. Object poses are copied from the bodies.
4. Beams are traced and the tick's collision list is turned into damage.

The step length is not fixed. In single player it is the frame time,
capped at 0.1 s. A multiplayer authority accumulates time and steps in
slices of 0.1 s plus a remainder. Per design notes, no step is shorter
than 0.005 s, and the "simulation rate" constant of 16 Hz only tunes
collision look-ahead. The flight formulas are therefore mildly frame-rate
dependent, which the high damping hides.

### 3.11 Collisions in outline

- Each part of a model has a precomputed convex shape tagged with the
  part's identifier, so a contact reports which part was hit. Physically
  attached equipment is merged into the ship's body and split off when
  detached.
- All bodies share one material: friction 0.1, elasticity 0.9. Collisions
  are bouncy and nearly frictionless.
- A contact between the same pair is reported at most once per 0.5 s. A
  munition leaves collision detection after its first contact.
- Damage dealt *by* object A *to* object B is
  `0.5 × relative speed × min(1, mass_A / mass_B)`. Ramming something
  heavier hurts the rammer at the full rate and the victim hardly at all.
  At cruise speed that is 150 points against a starter hull of 1,100:
  collisions punish, they rarely kill.
- Bodies without force controllers (loot, debris) use a default damping
  of 0.5 linear and 0.2 angular per second.
- Docking, lane travel and jumps switch a ship's collisions off and move
  it with scripted force controllers (chapter 04).

### 3.12 Authority and presentation

Both sides run the same flight code. The side that owns a ship integrates
it: a player's presentation side flies the player's ship from input,
reports its pose and computes its collision damage; the authority flies
NPCs; everyone else extrapolates a ship's last reported pose and steers a
local body towards corrections (per design notes; chapter 11). Cruise,
thruster and engine switches are applied locally at once, then sent to the
authority and relayed. Mounting rules, hold capacity and the equipment
list are the authority's. In single player both sides share one world.

## 4. Data that defines it

**Ship archetype** (69 records: 36 fighters, 8 freighters, 8 haulers, 4
gunboats, 4 cruisers, 8 capital ships, 1 mining ship; 31 purchasable as packages, and one more hull good without a
package).

| Field group | Fields | Player hulls |
| --- | --- | --- |
| Identity | nickname, text identifiers, role type, purchase class (light, heavy or very heavy fighter, freighter) | — |
| Model | model file, materials, level-of-detail distances, cockpit, chase-camera offsets and lag | — |
| Body | mass; inertia per axis | mass 50 to 300 (NPC to 100,000) |
| Hull | hit points | 1,100 to 12,600 (NPC to 4,000,000) |
| Flight | hull drag; steering torque and angular drag per axis; nudge force; strafe force and power rate; maximum bank angle | drag 1; strafe 20,000 at 2/s |
| Hold | volume; repair- and shield-consumable limits | 20 to 275; 10 to 71 |
| Hardpoints | one line per supported class listing the hardpoints that accept it; exhaust nozzle count; bay-door, tractor-source and shield-attachment hardpoints | 3 to 8 weapon positions, top grade 3 to 10 |
| Docking | berth size the hull may use (chapter 04) | — |
| Destruction | explosion, fuses with hull thresholds, hit-effect tiers (chapter 03) | — |

Each ship archetype is followed by its **parts** (246 in all; 1 to 5 on a
player hull, up to 18 on a capital ship): model node, hit points, whether damage to it also counts against
the hull, breakaway mass and impulses, debris and damaged model
(chapter 03).

| Nickname | Role | Mass | Hull | Hold | Torque (pitch, yaw, roll) | Angular drag | Inertia | Turn rate |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `example_light_fighter` | fighter | 100 | 1,300 | 25 | 24k, 24k, 58k | 15k, 15k, 35k | 2.8k, 2.8k, 1k | 92°/s |
| `example_freighter` | freighter | 300 | 3,600 | 275 | 54k each | 60k each | 16.8k each | 52°/s |
| `example_capital` | capital | 100,000 | 69,300 | n/a | 1.5e9 each | 1.32e10 each | 1.5e10 each | 6.5°/s |

**Engine** (60 records, 8 distinct flight profiles): thrust (48,000 to
3,000,000), drag (160 to 100,000), reverse fraction (0.1 or 0.2), cruise
charge time (5 s), cruise power rate (20/s), normal power rate (0),
indestructible flag, and presentation fields: flame and trail effects,
audio cues for each engine and cruise transition, and how pitch and
loudness follow throttle.

**Power plant**: capacity and charge rate; thrust capacity and thrust
charge rate. NPC plants are effectively bottomless (50,000 at 50,000/s).

**Thruster** (6 records): force, power rate, hit points (1,000 to 8,000),
hardpoint class, effects. **Scanner** (2), **tractor** (1), **armour**
(24): the numbers of 3.4.

**Loadout** (494 records, median 22 equipment lines): nickname, intended
archetype, equipment lines (item, optional hardpoint), cargo lines (item,
count, optional pod hardpoint). NPC loadouts come in graded series per
hull, so one hull serves many difficulty levels (chapter 06).

**Global constants**: cruise speed 300 m/s; cruise ramp 5 s; extra drag in
cruise 3; disruption cooldown 5 s; collision damage factor 0.5; and the
material, damping, limit and ammunition values quoted in section 3.

## 5. Connections

Needs: from 01 the zone an object is in, the zone drag factor, axes and
units; from 11 who owns which object, the tick driver and the stored
character.

Gives:

- 03: sub-object identifiers and parts as damage targets; the power pool;
  hardpoints and joints for guns; collision events; the cruise disruption
  entry; armour.
- 04: the body and force controller mechanism that docking, lane and jump movers
  take over; the hull's berth size.
- 05: the commands a pilot may issue (throttle, axis throttle, nudge,
  strafe, thruster, cruise, engine switch) and the predictions it derives
  from 3.7: speed for a throttle, time and distance to reach a speed.
- 06: loadouts and armour as the NPC difficulty dial.
- 07: hulls and equipment as goods; hold volume and carry limits.
- 09: throttle, pool levels, cruise and steering state for the HUD;
  per-hull camera parameters.

## 6. Design notes for the builder

**Essential.**

- *Drag-dominated flight with top speed `F / D`.* The ship obeys the
  nose, aiming with the mouse equals steering, and speed is tuned without
  touching mass. Without it the game is a Newtonian simulator, and its
  combat distances, AI and mission pacing stop working.
- *Drag owned by the engine.* Engine kill, drifting and "engine shot off"
  all fall out of one rule.
- *Cruise defined as a target speed, not a force.* One constant governs
  travel time across every system, and no ship is the slow one in a convoy.
- *Cruise costs: charge time, no weapons, disruptable.* They make travel
  and combat distinct states. Drop any one and fights can be left at will.
- *Hardpoint classes with authored compatibility.* A hull is valuable for
  the grades it accepts.
- *One object model for player, NPC and station.*

**Feel, tunable.** The time constants (0.1 to 0.5 s linear, 0.2 s
angular); the 0.25 s hold before mouse flight; banking; bouncy collisions;
uniform player top speed. Varying top speed by hull is an easy
differentiator; the original declined it, which keeps formations, escorts
and pursuit simple.

**Accidents of the era.** The variable step with linearised damping (use
a fixed tick, or the exponential everywhere); kind tests by bit mask;
presentation fields inside simulation archetypes; owner-side flight
authority, a bandwidth compromise that trusts the player's machine.

**Simplifications and their cost.**

- No physics library: integrate the two formulas of 3.7 and sweep simple
  shapes for contacts. Loses tumbling debris; flight never needed more.
- One pool instead of two: escaping and fighting then compete.
- No per-part collision shapes: shooting specific pieces off becomes
  imprecise.

## 7. Open questions

1. Whether physically attached equipment adds its mass to the ship's body
   when merged. Equipment masses (10) are small against hull masses.
2. What sets the turn-sensitivity factor of 3.9 and a per-ship multiplier
   on angular drag. Both default to no effect; their writers were not
   traced.
3. The rule that decides whether an external item is attached with its own
   collision shape or without one.
4. Whether the player can strafe vertically, and how the controls step
   or set the throttle; the bindings were not read (chapter 09).
5. Whether a force controller's push takes effect in the same physics step or the
   next; steady-state behaviour is the same either way.

## 8. Basis

Design documents: `design/adr/0001-object-model.md`,
`design/adr/0003-simulation-oracle.md` (context section),
`design/sim-records.md` (force-controller inputs, units, axis convention),
`design/assessment/02-simulation-core.md` sections 1 and 4,
`design/facts.md` F16.

Data, tabulated by script: `outdir/data/SHIPS/shiparch.ini` and the three
loadout files; `outdir/data/EQUIPMENT/engine_equip.ini`, `misc_equip.ini`,
`select_equip.ini`, `st_equip.ini` (thrusters), `goods.ini` (which hulls
are purchasable, their default engines); `outdir/data/constants.ini`.
Derived speeds and turn rates were computed from these with the formulas
of 3.7.

Code under `CppSource/`: `Common/common_cship.cpp`, `common_ceengine.cpp`,
`common_cethruster.cpp`, `common_csteering.cpp`, `common_cepower.cpp`,
`common_ceqobj.cpp`, `common_cequip.cpp`, `common_cecargo.cpp`,
`common_phycontroller.cpp`, `common_physicalsystem.cpp`,
`common_physys.cpp` (`Collision::CalcCollisionDamage`),
`common_constants.cpp`, `common_misc.cpp`, `common_archetype_ships.cpp`,
`common_thrustmanager.cpp`, `common_behaviormanager.cpp`,
`common_gotobehavior.cpp`, `common_freeflightbehavior.cpp`,
`common_faceidea.cpp`, `common_aisteer.cpp`, `common_cegun.cpp`;
`include/common_archetype.h`, `common_cequip.h`, `common_cobject.h`;
`IVP/ivp_physics/ivp_core.cpp`, `ivp_environment.cpp`;
`OutpostFrontier/Client/player_behavior.cpp`, `client_gameobj.cpp`,
`client_equip_fx.cpp`, `space_state.cpp`; `Server/server_gameobject.cpp`,
`server_classes.cpp`, `server_pub.cpp`, `IServerImpl.cpp`;
`Oracle/scenarios/free_flight.txt`, `turn_to_heading.txt`.

Inference: that cargo does not change mass; that banking is real
orientation, not a visual offset (read from the code path, not seen
running); the 15 s thruster endurance and 3.7 s cruise ramp (computed);
the reasons in section 6. Owner-side authority, dead
reckoning, the minimum step and the role of the 16 Hz constant come from
the design notes.
