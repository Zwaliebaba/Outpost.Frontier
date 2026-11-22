# 05 — AI and pilots

Status: draft
Depends on: 00, 01 (zones), 02 (flight model), 03, 04, 06, 07, 11

## 1. Purpose and player experience

Every NPC ship is flown by a pilot, an authored personality. Pilots
make the universe look inhabited and are the opposition in every fight.

- **Fighters fight in passes.** An enemy runs in firing, flashes past,
  extends and returns. On its tail, the player sees it weave, break, or
  cut its engine and spin round to fire.
- **Role decides behaviour.** Fighters dogfight, gunboats strafe slow
  targets, cruisers creep forward and let their turrets work, haulers
  never attack and flee when outmatched.
- **Groups behave as groups.** Wingmen hold stations on a leader, take
  its target, scatter when hurt and re-form afterwards.
- **Skill is visible.** Weak pilots fire short bursts with long pauses
  and spray; aces fire almost continuously into a tight cone. A player
  who sits still is hit by everyone.
- **The player's ship uses the same machinery.** "Go to", "dock" and
  "join formation" are NPC manoeuvres run on the player's ship; manual
  flight is one more manoeuvre.

The AI is deliberately shallow: no planning, no memory, coordination
only between leader and follower. Variety comes from weighted random
choice among a dozen manoeuvres with authored styles.

## 2. Concepts

| Concept | Definition |
| --- | --- |
| Pilot | A named personality: sixteen parameter blocks, each referenced by nickname. |
| Personality block | The parameters of one manoeuvre or reaction (3.11), shared between pilots. |
| Job | The block for strategy: whom to attack, when to flee, whether to fight in formation. Content may replace it per spawn. |
| Manoeuvre layer | Per ship, the owner of the current behaviour, the directive, target selection, fire control and the steering arbiter. |
| Behaviour | One of 21 manoeuvres. It can say whether it applies now, and it can run. |
| State graph | A 21 × 21 table of weights: row = current behaviour, column = candidate. One per ship role, in a leader and an escort variant. |
| Directive | An order from outside (mission, population code, the player's interface) to run one behaviour with parameters. |
| Steering primitive | A reusable controller that behaviours drive: move, face, dodge, break away, dodge a missile, match throttle, follow a spline. |
| Steering arbiter | The one place that turns those wishes into the control values of chapter 02. |
| Damage level | A decaying sum of recent damage, compared with eleven triggers. |
| Scene threat ratio | Hostile strength near the ship divided by all strength near it. |
| Attack preference | An ordered list of (target class, range, weapon kinds) in the job. |
| Formation | A named list of slot offsets; slot 0 is the leader's. |

```text
content ─ directive, job ─▶ MANOEUVRE LAYER ─▶ behaviour ─▶ primitives ─▶ arbiter ─▶ force controllers (02)
pilot, state graph ───────▶   target selection, fire control ─▶ weapons (03)
```

## 3. How it works

### 3.1 The layers

1. **Content** (chapters 06, 08) sets purpose. It spawns the ship with a
   pilot and a state graph, makes it leader or follower, and gives
   directives. It never steers.
2. **The manoeuvre layer** picks the manoeuvre through the state graph
   when no directive binds it. Independently, it picks the target and
   fires the weapons.
3. **The behaviour** sets the geometry: where to point and how fast to
   go now, by configuring steering primitives.
4. **The steering arbiter** resolves conflicts (reverse beats throttle,
   a killed engine beats cruise, cruise is refused while the way ahead
   is obstructed) and writes the control values of chapter 02.

Aiming and flying are separate: fire control shoots whenever a weapon
bears, whatever the behaviour is doing.

### 3.2 Timing

The layer runs once per tick per ship, before physics. Its clocks start
at random phases so that ships created together do not act in step.
Fire control and the damage level run every tick; the behaviour's step
every 0.2 s (at once after a switch); target selection every 1 s;
behaviour re-evaluation every 3 s, and every tick while idle.

A behaviour's step returns a status: *committed* (do not interrupt),
*interruptible*, *finished* or *cannot continue*. The last two force a
re-evaluation; *committed* suppresses the periodic one. An incoming
guided munition or the "turn and face" damage trigger also forces one.

### 3.3 Choosing the next behaviour

```text
for each of the 21 candidates c:
    w = graph[current][c]                 # 0: never from here
    if a directive names c: w = 0.001
    if w == 0: skip
    c.evaluate() -> "must run now": score 1 | "available": score w
                    "not applicable" or "disabled": skip
if any score is exactly 1: take it (the last such wins)
else draw one at random, proportional to score
if nothing scored: fall back to the default behaviour
```

The default is idle for an NPC and free flight for a player. A weight of
1 means "whenever available, always"; weights of 0.05 mark transitions
that are legal but (inferred) meant for when the behaviour insists.

**Worked example.** A fighter leader has finished an attack pass. Its
row holds evade 1.0, trail 0.4, engine-kill turn 0.3, strafe run 0.15,
attack pass 0.05, flee 0.05, follow 0.05. If an enemy that targets it is
behind it within evade range, evade scores 1 and is taken. Otherwise,
against a moving fighter, strafe does not apply and flee is not
triggered; trail, another pass and (above 50 m/s with a hostile close)
the engine-kill turn are available. The draw is 53 % trail, 40 %
engine-kill turn, 7 % another pass.

Ten graphs ship. The **fighter** leader reaches attack pass, trail,
strafe, evade, engine-kill turn, flee, loot and travel; its escort
variant adds formation and loses go-to. **Transports** reach only
travel, flee and loot. The **gunboat** leader prefers strafe (0.7 to
0.9); **cruiser** leaders run the capital manoeuvre (weight 1). Gunboat
and cruiser escorts reach only formation, dock and launch: they fight
with turrets from their slot. Two graphs are empty. With no target,
formation is the only candidate that applies to an escort, so escorts
return to station after a fight. The player's ship gets an empty graph
and no automatic targeting or firing: only directives move it.

**Directives.** A directive forces its behaviour at once and stays
armed until the behaviour finishes, fails or is cancelled; the layer
then returns to the default and reports the outcome. While it is armed
the periodic re-evaluation is suspended, but "must run now" verdicts
(missile evasion, flee) still take over. Content can also confine a
ship to a sphere or box and lock individual manoeuvres of a player.

### 3.4 The behaviours

"Small ship" means fighter or freighter class. Travel behaviours belong
to chapter 04 and are listed for completeness.

| Behaviour | Applies when | What it does |
| --- | --- | --- |
| Attack pass | target is a small ship | Head-on firing pass; see below. |
| Trail | target is no large ship, moves at 20 m/s or more, and the pilot is in its rear hemisphere | Tail chase: hold 150 to 300 m behind and match throttle at limited turn authority (0.3 to 0.6). After losing a 30° cone, roll out and retry (partly inferred). |
| Strafe run | own ship is small or a gunboat, the target is neither and moves at 20 m/s or less | Repeated runs on a big, slow target; see below. |
| Evade | an enemy within 20 to 300 m targets this ship or a follower and is not in front; must run when a guided munition is about to hit | Dodge, then break; see below. |
| Engine-kill turn | nearest hostile within 500 m is a small ship and own speed exceeds 50 m/s; must run on its damage trigger | Cut the engine, coast, turn to face the pursuer and fire for a few seconds, then restart. Also serves the "face this" directive. |
| Capital manoeuvre | own ship is a cruiser with a target | Every 5 s re-plan a point at the centre of all hostiles, away from other large ships within 1,500 m and at most 50 m above or below; creep there. |
| Flee | must run when a flee rule fires (3.9) | Ask content for a destination, then run go-to or dock as a sub-behaviour; keep formation instead if the leader fled. |
| Loot | a tractor is fitted, no enemies, wanted loot in scanner range | Fly to the loot and tractor it. |
| Formation | a leader is set | Hold a slot (3.10). |
| Idle | NPC default | Hold still. |
| Free flight | player default | Manual control: a go-to variant fed with direction, throttle, cruise, engine, reverse, strafe and thruster wishes (chapter 02, 3.9). |
| Go-to, dock, launch, lane travel, cruise | by directive, or copied from a leader | Chapter 04. |
| Follow, guide | by directive | Stay near a ship without a slot; lead a ship along a course, waiting for it. |
| Forced evade, delay | by directive | For a set time (default 4 s) nudge sideways or brake; wait, then report. |

A placeholder "none" completes the 21.

**Attack pass.** A state machine driven by distance to the target:

| State | Event | Next | Effect |
| --- | --- | --- | --- |
| Extend | still inside the turn-in distance (500 to 1,500 m ± 10 to 25 %) | Extend | Fly away in the authored style: straight, sliding or waggling. Interruptible; gives up after 4 to 10 s. |
| Extend | beyond it | Run in | — |
| Run in | beyond the turn-in distance | Run in | Face the target at full throttle. Committed. |
| Run in | inside it | Run in | Authored throttle (0.8 to 1), with the style's jinks while the nose is nearly on target. |
| Run in | within the pass distance (150 to 350 m) | Pass | Optionally drop mines; tell the formation (3.10). |
| Pass | pass time (1 to 3 s) running | Pass | Fly straight by, break to a side, or cut the engine and keep the nose on the target. |
| Pass | time elapsed | Extend | Finished: re-evaluate. |

A pilot that finds itself close behind a target flying the same way
skips the cycle, holds its nose on the target and fires.

**Evade.** A dodge phase (2 to 8 s), then a break phase (5 s), then
finished. The dodge repeats a move every 2 to 6 s in one authored style:
waggle (turn to a random direction within a cone of 10° to 90°), slide
(strafe to a weighted side) or corkscrew (roll while turning). While the
threat is in front the pilot stops dodging and flies straight at full
throttle. With the "dodge more" trigger on, cone angles triple, the
interval halves and throttles go to 1. Within about 50 m of the threat
the thruster is held on. The break is a sustained turn to a weighted
side, an outrun, or a brake to full reverse so that the pursuer
overshoots. A guided munition overrides both phases with a break, slide
or thruster burst, chosen by which reaction time its time to impact has
fallen below.

**Strafe run.** *Attack:* fly at the target, or at its selected
component when that faces the attacker. *Turn away:* entered when the
distance to the target, or to whatever a forward ray hits, falls below
a trigger range: the distance flown at full speed in the time the ship
needs to turn 90°, plus one second. Pick a point 300 to 600 m to the
side in the horizontal plane and fly to it. *Come back:* once the nose
points away and the target is farther than its radius plus that
distance. The trigger range comes from the closed forms of 3.6, so a
sluggish gunboat breaks off much earlier than a fighter with no authored
number.

### 3.5 Events

The authority tells the layer when the ship took damage, when a guided
munition was launched at it, when an object it refers to left or died,
when its leader began to dock or entered a lane (followers copy the
order), and when it died itself.

It reports to content: combat state (engaged, overwhelmed, no
enemies), "target in sights", follow and guide progress, a request for
a flee destination, and combat drift (3.7).

### 3.6 Steering, prediction and obstacles

**Predictive turning.** To face a direction the pilot demands full
torque about the axis of the needed rotation, then asks how far it would
still rotate if it let go now. When the remaining angle is within 98 %
of that coast angle, the demand drops to what holds the residual rate;
close to the goal it is proportional to the remaining angle. Ships stop
on their heading without overshoot at any turn rate. Roll is blended in
to bank into turns.

**Closed forms.** Such questions are answered from the flight model of
chapter 02, not by trial simulation. With thrust `F`, drag `D`, mass `m`
(or torque, angular drag, inertia):

```text
terminal speed        v_inf = F / D
time from v0 to v1    t = -(m/D) * ln((v1 - v_inf) / (v0 - v_inf))     (v1 capped at 0.99 v_inf)
distance in time t    s = v_inf*t + (v0 - v_inf) * (m/D) * (1 - exp(-D*t/m))
throttle for speed v  |v| * D / F, at most 1
```

They decide when to brake before a point, when to stop turning, the
strafe trigger range and the throttle that matches a leader's speed.
The flight model is a first-order lag, so they are exact for constant
input.

**Obstacles.** Pilots do not plan paths. A moving ship casts a ray from
its nose along a slowly spiralling direction ahead, with a look-ahead
proportional to hull length, every 0.5 s, or every 0.1 s while something
is in the way. On a close hit it probes a cone with random rays, keeps
up to 25 recently blocked directions and steers along their average
escape direction at reduced throttle. The probes see whatever has a
collision body; nothing knows the layout of a field in advance.

**Zone flags.** What the AI reads about fields is the property set
around a point (chapter 01), reduced to a tier:

| Tier | Object density | Danger density | Cloud |
| --- | --- | --- | --- |
| 0 | none | none | no |
| 1 | at most low | none | no |
| 2 | at most medium | at most low | allowed |
| 3 | anything else | | |

The job's field-targeting setting (never, low density, high density,
always: tier 0 to 3) is the worst tier into which the pilot chases a
target from outside. A pilot already inside a worse tier fights whoever
is there. Patrols thus do not follow prey into dense rock or
minefields: fields work as hiding places.

### 3.7 Target selection

Once a second, unless a directive forbids it:

1. **Leader rule.** With "wait for leader's target" set, a follower
   selects nothing until its leader has a target.
2. **Combat drift.** If the fight has moved more than 10,000 m from
   where it began, the pilot disengages and tells content.
3. **Overwhelmed?** Scene threat ratio = sum of hostile ranks ÷ (own
   rank + friendly ranks + hostile ranks) over scanner contacts. Above
   the job's threshold (0.15, 0.35, 0.65, 0.85 or 1.0 for the settings
   "easiest" to "hardest") the pilot takes no target and reports itself
   overwhelmed.
4. **Candidates** are scanner contacts that are alive; regarded at −0.6
   or below (hostile, chapter 07); within 5,000 m; for a follower,
   within the job's leash from the leader (1,500 to 10,000 m); passing
   the tier test of 3.6; inside the directive's area if any. A job can
   exclude players. When a contact at −1 is near, only contacts at −1
   count. Up to eight targets forced by content are always candidates.
   At most 16.
5. **Preference.** Walk the attack preferences in order. The first
   entry whose class has a candidate within its range defines the pool,
   and its weapon kinds (guns, guided, unguided, torpedo) are the
   weapons allowed against that target.
6. **Pick.** A player in the pool with fewer than two attackers is
   taken first; a player with more than two is removed if anyone else
   is in the pool. Otherwise keep the current target if it is in the
   pool, else pick at random. Missions can change the limit of two.
7. With "attack as a formation" set, a leader gives its target to all
   NPC followers and a follower takes its leader's.

The attacker limit is the main fairness device: however many enemies
are present, about two shoot at each player.

### 3.8 Fire control and accuracy

**Rhythm.** Each gun alternates a burst (1 to 5 s) and a pause (0.5 to
2.5 s), each jittered by up to ±50 %. It fires when it is in a burst,
the target is within gun range, the line of fire is clear (a ray test,
cached for a second) and the barrel points within 3° of the aim point.
Gun range is the longest range among the ship's guns × 1.1 × a per-pilot
random factor of 0.7 to 1.3, so pilots open fire at different distances.
Turret-mounted guns pick their own target once a second: the nearest
hostile of an attackable class within 1.2 × gun range that the turret
can reach. Armed solars fight this way (inferred).

**Aim error.** The aim point is the lead point (chapter 03) plus an
error held in the target's frame and re-rolled every 0.25 to 3 s:

```text
angle  = cone * u^p            u uniform in [0, 1)
offset = distance * tan(angle), in a random direction across the line of sight
```

`cone` is 1° to 8° by pilot. The exponent `p` shapes the spread: above
1 most shots fall near the centre, below 1 near the edge. It is authored
separately for player targets (0.7 to 3) and NPC targets (usually 6), so
NPCs kill each other efficiently while the player gets near misses. A
player slower than about 3 m/s is aimed at without error.

**Launchers.** Guided and unguided launchers fire every 10 to 60 s
within 1,000 m when the target is inside a cone of about 20°. A cruise-disrupting launcher fires only at a target that has
cruised for 2 s and is 150 to 2,500 m away, then waits 5 s. Mines are
dropped at most every 10 s when a hostile follows within 250 m inside a
30° cone astern and own speed exceeds 20 m/s; on an attack pass if so
authored; or all at once on the mine trigger.

**Countermeasures** are not reactive: the dispenser is on for part of
a repeating period (3 s on, 0 to 3 s off).

### 3.9 Reactions

**Damage level.** Every hit adds its damage to a level that decays by
half of (maximum shield + maximum hull) per second. Eleven triggers
compare it with a fraction of that same sum; a tripped trigger stays on
for its hold time (0.1 to 1 s typically). A trigger at 0.25 fires when
one burst removes a quarter of the ship's total protection; steady fire
registers only while it removes more than half of the total per second.
The triggers measure how hard the ship is being hit now, not how
damaged it is.

| Trigger | Typical fraction | Effect |
| --- | --- | --- |
| Break; dodge more; brake | 0.2 to 1; 0.25 to 0.3; 1 | Evade breaks; dodges harder; breaks by reversing. |
| Engine-kill turn | 1 | Forces that behaviour. |
| Thruster; roll | 0.1 to 0.2; 0.1 to 0.3 | Thruster burst; roll while manoeuvring. |
| Fire guns; fire missiles | 1 | Fire regardless of rhythm and cone. |
| Drop mines | 0.25 | Drop all mines now. |
| Break formation; break apart | 0.2 to 0.5 | See 3.10. |

A fraction of 1 can hardly be reached and means "off".

**Missiles.** For the nearest closing guided munition within 1,000 m,
time to impact is estimated as distance ÷ (missile speed − own speed ×
cosine of the angle between the two velocities). The pilot reacts when
it falls below a reaction time (0 to 1 s).

**Repair consumables.** When hull or shield falls to an authored
fraction and a consumable is carried: wait a pre-delay, use one, wait a
post-delay. The repair block every shipped pilot references has the
fraction at 0, so NPCs do not heal, though the mechanism is complete.

**Fleeing.** Flee becomes "must run" when (hull + shield) ÷ their maxima
is below the job's fraction (0 to 0.75); or the scene threat ratio
exceeds the job's flee threshold (the five steps of 3.7); or no gun or
launcher works and the job says to flee unarmed; or the leader flees
and the job says to follow suit. Content supplies the destination: a
base to dock at, an object or a point.

**Cargo.** A pilot does not shed cargo in combat. When a patrol scans
it and demands a good (chapter 06), it jettisons if the scene threat
ratio is at or above the job's loot threshold; otherwise it refuses and
is attacked. The job's loot preference is never set successfully by
the shipped data, so NPCs do not loot.

### 3.10 Formations

Content names a leader and gives each follower an offset from a
formation definition, in the leader's frame (+Z is astern). Followers
switch to the escort variant of their graph.

Each step a follower computes its slot (leader position plus its offset
rotated by the leader's orientation), faces a point ahead of the slot
by its own radius plus 200 m (500 m in cruise), and drives its throttle
to match the leader's speed while closing the position error. It
cruises when the leader does. When the leader docks or enters a lane,
each follower gets the same order.

In combat the formation block and the job decide cohesion:

- With "attack as a formation" off, followers fight alone.
- With it on, the combat behaviours report "not applicable" for a
  follower during the active part of a cycle (0 to 30 s on, 0 to 30 s
  off): the group flies and fires as one, then loosens.
- A follower breaks formation on its damage trigger or when a missile
  is close, for at most an authored time (0 to 15 s), leaving by an
  authored exit: away from the centre, on thruster, braking or
  outrunning.
- Starting a run-in, a pass, a dodge or a break can tell the leader
  "break apart" for a time and "regroup" afterwards, so a flight
  scatters and re-forms on one pilot's manoeuvre.

A player joins an NPC's formation through the same behaviour. The
target must be a ship below capital class, within 5,000 m, not hostile,
not launching or in a lane, and not itself following the player.

### 3.11 What a personality is made of

| Block | Modulates |
| --- | --- |
| Evade: dodge; break | Activation range, style and direction weights, cone, interval, duration; break duration and style. |
| Attack pass: run in; pass by | Turn-in distance, extend time and style, throttles; pass distance, time, style, mines. |
| Trail; strafe; engine kill | Distance, lock cone, turn authority; run-away distance; times, target distance. |
| Gun | Burst, pause, accuracy cone, the two exponents, range factor, error refresh; turret variants. |
| Mine; missile | Intervals, cones, ranges; cruise-disruptor rules. |
| Damage reaction; missile reaction | The nine combat triggers; detection distance and three reaction times. |
| Repair; countermeasure | Thresholds and delays; on and off times. |
| Formation | Cohesion cycle, break triggers, exit style; a flag that lends a follower its leader's gun and repair blocks. |
| Job | Attack preferences, threat and flee thresholds, field targeting, loot rules, leader rules, scan chance. |

### 3.12 Difficulty

There is no global difficulty setting. Three things scale opposition:

1. **Pilot tier.** Most factions' pilots come in four tiers that differ
   only in gun block and dodge block. From lowest to highest the
   accuracy cone falls from 5° to 2°, the pause between bursts from 2 s
   to 0.5 s and the error refresh from 0.5 s to 0.25 s; the dodge cone
   widens from 60° to 90°. Manoeuvre choice does not change.
2. **Ship and loadout level** (chapter 06). The NPC record that names
   the pilot also names hull, loadout and level; level is the rank in
   the scene threat ratio.
3. **The attacker limit** of 3.7.

Tier is set per NPC record, not derived from level; every tier occurs
at most levels.

### 3.13 Authority and presentation

NPC manoeuvre layers run on the authority. The player's layer runs with
the player's ship on the presentation side (per design notes), and the
authority forwards directives for that ship there.

## 4. Data that defines it

| Record | Principal fields | Volume |
| --- | --- | --- |
| State graph file | graph count, behaviour count (must be 21); per graph a role name, a variant and 441 weights | 10 graphs; weights 0 and 0.05 to 1 |
| Personality block | nickname and the parameters of 3.11 | ambient file: 3 to 29 per kind, 77 gun blocks, 45 jobs |
| Pilot | nickname, optional parent pilot, up to 16 block references; unset ones inherit | 64 ambient, 255 campaign |
| Formation | nickname, slot offsets, optional slot for a player | 93; 1 to 12 slots, most have 7; tens to hundreds of metres apart |
| NPC record (06) | hull, loadout, level, pilot, graph role | 404 |

Examples: `example_patrol_low` sets all 16 blocks (gun cone 5°, pause
2 s); `example_patrol_high` inherits from the tier below and replaces
gun block (cone 2°, pause 0.5 s) and dodge block; `example_hauler` sets
only a gun block.

## 5. Connections

- **Needs:** zone property sets (01); control values and the flight
  model (02); weapon ranges, lead point, guided munitions, damage
  events (03); the travel behaviours (04); reputation and rank (07).
- **Gives 06:** the pilot and graph an NPC record names; combat state,
  drift and flee requests; the scan answer.
- **Gives 08:** directives with completion reports, forced targets, the
  attacker limit, manoeuvre locks for the player.
- **Gives 09:** which manoeuvres are available to the player now.
- **Gives 11:** NPC control input originates on the authority.

## 6. Design notes for the builder

**Essential.**

- The split into purpose, manoeuvre choice, geometry and control, with
  fire control beside the manoeuvre, not inside it.
- Behaviours that veto themselves and report commitment. The graph
  supplies taste; correctness lives in the verdicts.
- The player's autopilot running through the same layer.
- The attacker limit and the separate accuracy against players. Without
  them group fights are either lethal or toothless.
- Burst-based damage reactions: NPCs respond to being hit hard, not to
  being low.

**Feel.** The rhythm of extend, run in, pass; authored styles;
staggered clocks; the per-pilot gun range factor; the engine-kill turn.

**Era.** A fixed table of 21 behaviours; timers in place of
perception; no memory, cover or focus fire beyond the leader's target;
parameters the shipped loaders ignore.

**Simplifications.**

- With a higher-order flight model the closed forms are lost, and the
  builder needs numeric prediction or tuned controllers. Keeping the
  flight model first-order is cheaper.
- Attack pass, evade, trail, formation and flee, plus travel, give most
  of the experience. Strafe and the capital manoeuvre matter only once
  large ships exist.
- A behaviour tree or scoring system can replace the graph if the
  weighted randomness between equally valid manoeuvres survives.
- Four tiers of one pilot per role would be hard to tell from the
  original's 64, which differ mostly in dodge style.

## 7. Open questions

1. Why reputation −1 is a class of its own in target selection
   (chapter 07).
2. Whether asteroids have collision bodies on the authority away from
   players. If not, NPCs fly through fields unhindered and only the
   zone tier keeps them out.
3. Trail, engine-kill turn, guide, follow and idle were read in part;
   their state tables are not established.
4. Job fields with no visible effect: target toughness preference, the
   component attack order outside strafing, and the combat drift
   distance, which the loader discards (10,000 m is built in).
5. How a joining player's formation offset is assigned (presumably the
   reserved slot).

## 8. Basis

Design: `design/assessment/02-simulation-core.md` sections 1 and 2;
`design/sim-records.md` (behaviour tick records, the loot preference
note); `design/adr/0003-simulation-oracle.md`.

Data: `AI/state_graph.db` and `MISSIONS/pilots_population.ini`
(tabulated), `pilots_story.ini` (counted), `formations.ini`,
`npcships.ini` (pilot, level, graph), zone `property_flags`. Scenarios
`dogfight.txt`, `skirmish.txt`, `formation.txt`.

Code read closely: `common_behaviormanager.cpp` (the tick, behaviour
switch, directives, state graph, events), `common_behaviortypes.cpp`,
`common_pubstategraph.cpp`, `include/common_pub.h` (`Personality`, the
directive ops), `common_evadebehavior.cpp`, `common_buzzbehavior.cpp`,
`common_strafebehavior.cpp`, `common_formationbehavior.cpp`,
`common_targetmanager.cpp`, `common_aifirecontrol.cpp`,
`common_sysmgr_gungroups.cpp`, `common_sysmgr_timertable.cpp`,
`common_gunconvergence.cpp`, `common_repairmanager.cpp`,
`common_fleebehavior.cpp`, `common_cship.cpp` (`get_time_to_accelerate`,
`get_distance_travelled`, `get_projected_throttle` and their angular
forms), `Content/PilotPersonalities.cpp`.

Read in part: `common_aisteer.cpp`, `common_obstacleavoidance.cpp`,
`common_pathfind.cpp`, `common_matchthrottleidea.cpp`, the large-ship,
trail, face, loot, base-follower, drastic-evade, free-flight, cruise
and idle behaviours, `Content/PatrolGuides.cpp`. By header only: go-to,
dock, launch, lane, guide, follow, delay.

Names here against the code: attack pass = Buzz; engine-kill turn =
Face; capital manoeuvre = Large Ship (move); forced evade =
DrasticEvade; lane travel = InstantTradeLane; steering primitive =
idea; steering arbiter = thrust manager; manoeuvre layer = behaviour
manager with its system manager.

Inference: trail; what each dodge and break style looks like (from
style names and parameters); that the value added to the damage level
is the damage amount; the reading of 0.05 weights; the purposes given
for the attacker limit, the zone tiers and the accuracy exponents.
