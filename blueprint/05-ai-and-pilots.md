# 05 — AI and pilots

Status: draft
Depends on: 00 (glossary), 01 (zones and their property sets), 02 (flight model, force controllers), 03 (weapons, damage), 04 (docking, lanes), 06 (who spawns NPCs), 11 (authority, tick)

## 1. Purpose and player experience

Every ship the player meets is flown by a pilot: an authored personality
attached to an NPC ship. Pilots make the universe look inhabited
(convoys in formation, patrols that turn to fight, haulers that run) and
they are the opposition in every fight.

What the player sees:

- **Fighters fight in passes.** An enemy fighter runs in firing, flashes
  past, extends, turns and comes back. Shoot it and it jinks; get on its
  tail and it weaves, breaks or cuts its engine and spins round to fire.
- **Role decides behaviour.** Fighters dogfight, gunboats make strafing
  runs on slow targets, cruisers lumber towards the enemy and let their
  turrets work, haulers never attack and flee when outmatched.
- **Groups behave as groups.** Wingmen hold stations on a leader, take
  the leader's target, break apart when hurt and re-form afterwards.
- **Skill is visible.** Weak pilots fire short bursts with long pauses
  and spray; aces fire nearly continuously into a tight cone and dodge
  more often. A player who sits still is hit by everyone.
- **The player's own ship uses the same machinery.** "Go to", "dock" and
  "join formation" are the manoeuvres NPCs use, run on the player's
  ship; manual flight is one more manoeuvre.

The AI is deliberately shallow per ship: it never plans, never
remembers, and coordinates only through leader and follower. Its variety
comes from weighted random choice among a dozen manoeuvres, each with a
few authored styles.

## 2. Concepts

| Concept | Definition |
| --- | --- |
| Pilot | A named personality: sixteen parameter blocks, each referenced by nickname. |
| Personality block | One group of parameters that tunes one manoeuvre or reaction (section 3.11). Blocks are shared between pilots. |
| Job | The personality block for strategy: whom to attack, when to flee, whether to stay in formation. Content may replace it per spawn. |
| Manoeuvre layer | Per ship, the component that owns the current behaviour, the directive, the target list, fire control and the steering arbiter. Every ship has one, the player's included. |
| Behaviour | One of 21 manoeuvres. It can say whether it applies now, and it can run. |
| State graph | A 21 × 21 table of weights: row = current behaviour, column = candidate next behaviour. One per ship role, in a leader and an escort variant. |
| Directive | An order from outside (mission script, population code, the player's interface) to run one behaviour with parameters, with a priority and a completion callback. |
| Steering primitive | A small reusable controller a behaviour drives: move to a point or along a direction, face a point or orientation, dodge, break away, dodge a missile, match a leader's throttle, follow a spline, hold the thruster on. |
| Steering arbiter | The single place that turns primitives' wishes into the ship's throttle, axis throttle, nudge, strafe, thruster, engine and cruise settings (chapter 02). |
| Damage level | A decaying sum of recent damage taken, compared with eleven thresholds to trigger reactions. |
| Scene threat ratio | Hostile strength near the ship divided by all strength near it; drives fleeing. |
| Attack preference | An ordered list of (target class, range, weapon kinds) in the job. |
| Formation | A named list of slot offsets; slot 0 is the leader. |

```text
content (06, 08) ──directive, job, forced targets──▶ MANOEUVRE LAYER (one per ship)
pilot ──16 blocks──▶   ├─ target selection (1 s)  ── scanner contacts, reputation, zone flags
state graph ───────▶   ├─ behaviour selection (3 s) ─▶ current behaviour (0.2 s step)
events: damage,        │                                   └─ steering primitives
 missile launched,     ├─ fire control (every tick) ─▶ guns, launchers, mines
 leader docked, death  ├─ consumables, countermeasures
                       └─ steering arbiter + obstacle probe ─▶ force controllers (02)
```

## 3. How it works

### 3.1 The layers

Who decides what:

1. **Content** (chapters 06 and 08) decides purpose: it spawns the ship
   with a pilot and a state graph, makes it leader or follower of a
   formation, and gives directives ("go there", "dock here", "follow
   that"). Content never steers.
2. **The manoeuvre layer** decides the manoeuvre: with no binding
   directive it picks behaviours through the state graph; it also picks
   the target and fires the weapons, independently of the behaviour.
3. **The behaviour** decides the geometry: where to point and how fast
   to go right now, by configuring steering primitives.
4. **The steering arbiter** resolves conflicts (reverse beats throttle,
   a killed engine beats cruise, cruise is refused while the way ahead
   is obstructed) and writes the control values of chapter 02.

Aiming and flying are separate. Fire control shoots whenever a weapon
bears on the target, whatever the behaviour is doing, so a fleeing ship
with a turret still fires backwards and a formation fires as one.

### 3.2 Timing

The layer runs once per tick for each ship, before physics. Inside it,
four clocks start at random phases so that ships created together do not
act in step:

| Activity | Period |
| --- | --- |
| Fire control, damage level, notifications | every tick |
| Behaviour step (steering decisions) | 0.2 s, or at once after a switch |
| Target selection | 1 s |
| Behaviour re-evaluation | 3 s; every tick while idle |

A behaviour's step returns a status: *committed* (do not interrupt me),
*interruptible*, *finished* or *cannot continue*. The last two force a
re-evaluation at once; *committed* suppresses the 3-second one. An
incoming guided munition or a tripped "turn and face" damage trigger
also forces one.

### 3.3 Choosing the next behaviour

At a re-evaluation the layer scores all 21 behaviours:

```text
for each candidate c:
    w = graph[current][c]            # 0 means "never from here"
    if a directive names c: w = 0.001
    if w == 0: skip
    verdict = c.evaluate()
        "must run now"   -> score 1
        "available"      -> score w
        "not applicable" or "disabled" -> skip
if any score is exactly 1: take it (the last such candidate wins)
else: draw one candidate at random, proportional to score
if nothing scored: fall back to the default behaviour
```

The default is idle for an NPC and free flight for a player. A graph
weight of 1 thus means "whenever available, always"; small weights such
as 0.05 mark transitions that are legal but happen mostly when the
behaviour itself insists.

**Worked example.** A fighter leader has just finished an attack pass.
Its graph row holds: evade 1.0, trail 0.4, engine-kill turn 0.3, strafe
run 0.15, attack pass 0.05, flee 0.05, follow 0.05. If an enemy that
targets it sits behind it within evade range, evade is available, scores
1 and is taken. Otherwise, against a moving fighter: strafe is not
applicable (the target is small and fast), flee is not triggered, trail
and attack pass are available, and the engine-kill turn is available if
the ship is faster than 50 m/s with a hostile within range. The draw is
then 53 % trail, 40 % engine-kill turn, 7 % another pass.

The shipped graphs (10 in all):

| Role | Leader variant reaches | Escort variant reaches |
| --- | --- | --- |
| Fighter | attack pass, trail, strafe, evade, engine-kill turn, flee, loot, go-to, dock, launch, lane | the same combat set plus formation; no go-to |
| Transport | go-to, dock, launch, lane, flee, loot | formation, dock, launch, flee, loot |
| Gunboat | strafe (dominant), attack pass, trail, formation, travel, flee | formation, dock, launch only |
| Cruiser | capital manoeuvre (weight 1), travel | formation, dock, launch only |
| Empty (two) | nothing: used for the player and for scripted objects | — |

A follower's graph gives formation weight 1 from idle, so escorts return
to station whenever nothing else applies. The player's ship always gets
the empty graph and has automatic targeting and firing switched off:
only directives move it.

**Directives.** A directive forces its behaviour immediately and stays
"armed" until the behaviour finishes, fails or is cancelled; the layer
then returns to the default and calls back whoever gave the order with
the outcome. While armed, the 3-second re-evaluation is suspended, but
"must run now" verdicts (missile evasion, flee) can still take over.
Content can also restrict the ship to a sphere or box (targets outside
are ignored), set flags and lock or disable individual manoeuvres for a
player.

### 3.4 The behaviours

"Small ship" below means fighter or freighter class.

| # | Behaviour | Applies when | What it does |
| --- | --- | --- | --- |
| 0 | None | never chosen | Placeholder for "no behaviour". |
| 1 | Attack pass | target is a small ship | Head-on firing pass in three phases; see below. |
| 2 | Go-to | by directive | Fly to a point, an object or along a spline, to an arrival range, optionally in cruise, with avoidance (chapter 04). |
| 3 | Trail | target is not a large ship, moves at 20 m/s or more, and the pilot is in its rear hemisphere | Tail chase: hold a distance of 150 to 300 m behind the target and match its throttle, turning at limited authority (0.3 to 0.6); when the target has slipped out of a 30° cone for too long, roll out, optionally on thruster, and try again (partly inferred). |
| 4 | Flee | must run when a flee rule fires (3.9) | Asks content where to go: a base to dock at, an object or a point. Runs go-to or dock as a sub-behaviour; if the leader fled, keeps formation on it. |
| 5 | Evade | an enemy within 20 to 300 m is targeting this ship or a follower and is not in front of it; must run when a guided munition is about to hit | Dodge, then break; see below. |
| 6 | Idle | default for NPCs | Hold still; re-evaluate every tick. |
| 7, 8, 9 | Dock, launch, lane travel | by directive, or following a leader | Chapter 04. |
| 10 | Formation | a leader is set | Hold a slot; see 3.10. |
| 11 | Capital manoeuvre | the ship is a cruiser with a target | Every 5 s re-plan a point near the centre of all hostiles, nudged away from other large ships within 1,500 m, at most 50 m above or below; creep there. Close to within 500 m of a large or fixed target. Turrets do the fighting. |
| 12 | Cruise | by directive | Engage cruise and hold it. |
| 13 | Strafe run | the ship is small or a gunboat, the target is neither, and the target moves at 20 m/s or less | Attack runs on a big, slow target; see below. |
| 14 | Guide | by directive | Lead another ship (usually the player) along a course, waiting when it falls behind; reports progress to content (by header only). |
| 15 | Engine-kill turn | nearest hostile within 500 m is a small ship and own speed exceeds 50 m/s; must run when the matching damage trigger trips | Cut the engine, coast, turn to face the pursuer and fire, optionally with a thruster burst, for a few seconds; then restart. Also serves the "face this" directive. |
| 16 | Loot | the ship has a tractor, no enemies, and loot of a wanted kind is in scanner range | Fly to the loot and tractor it. |
| 17 | Follow | by directive | Stay within a distance of another ship without a fixed slot. |
| 18 | Forced evade | by directive | For a set time (4 s by default) nudge sideways by a given vector, or brake at full reverse. |
| 19 | Free flight | default for players | Manual control; a specialisation of go-to that takes direction, throttle, cruise, engine, reverse, strafe and thruster wishes from the interface (chapter 02, 3.9). |
| 20 | Delay | by directive | Do nothing for a time, then report completion. |

**Attack pass.** State machine, driven by distance to the target:

| State | Event | Next | Effect |
| --- | --- | --- | --- |
| Extend | farther than the turn-in distance (500 to 1,500 m ± 10 to 25 %) | Run in | — |
| Extend | still close | Extend | Fly away in the authored style: straight, sliding sideways, or waggling. Status interruptible; gives up after 4 to 10 s. |
| Run in | farther than the turn-in distance | Run in | Face the lead point, authored throttle (0.8 to 1). Committed. |
| Run in | inside the turn-in distance | Run in | Same, with the style's jinks while the nose is nearly on target. |
| Run in | within the pass distance (150 to 350 m) | Pass | Optionally drop mines; tell the formation (3.10). |
| Pass | pass time (1 to 3 s) not elapsed | Pass | Fly straight by, break away to a side, or cut the engine and keep the nose on the target. |
| Pass | time elapsed | Extend | Finished: re-evaluate. |

If the pilot finds itself directly behind a target flying the same way,
it skips the cycle and simply holds the nose on it and fires.

**Evade.** A dodge phase (2 to 8 s) followed by a break phase (5 s),
then finished. The dodge repeats a move every 2 to 6 s in one authored
style: waggle (turn to a random direction inside a cone of 10° to 90°
off the nose), random waggle, slide (strafe left, right, up or down by
weights) or corkscrew (roll while turning). While the threat is in
front, the pilot stops dodging and flies straight at full throttle. If
the "dodge more" damage trigger is on, cone angles triple, the interval
halves and throttles go to 1. Within 50 m of the threat the thruster is
held on. The break is a sustained hard turn to a weighted side, or an
outrun, or a brake to full reverse so that the pursuer overshoots. A
guided munition overrides both phases: break, slide or thruster burst,
chosen by which reaction time its time to impact has fallen below.

**Strafe run.** Three states. *Attack:* fly at the target, or at the
selected component of it when that faces the attacker, at the attack
throttle. *Turn away:* entered when the distance to the target (or to
anything a forward ray hits) falls below a trigger range, defined as the
distance flown in the time the ship needs to turn 90°, plus one second,
at full speed. Pick a point 300 to 600 m off to the side, in the
horizontal plane, and fly to it. *Come back:* once the nose points away
and the target is farther than its radius plus that distance, attack
again. The trigger range comes from the closed forms of 3.6, so a
sluggish gunboat breaks off far earlier than a fighter without any
authored number.

### 3.5 Events a pilot receives

The authority tells the manoeuvre layer when: the ship took damage (the
amount feeds the damage level); a guided munition was launched at it;
an object it refers to left the system or died (targets, leaders and
watched objects are dropped; a follower whose leader died is promoted
or released by content); its leader started docking or entered a lane
(the order is copied to the followers); and it died itself (the layer
stops).

In the other direction the layer reports to content: combat state
changes (engaged, overwhelmed, no enemies), "target in my sights",
follow and guide progress, a request for a flee destination, and
"combat has drifted too far" (3.7).

### 3.6 Steering, prediction and obstacles

**Predictive turning.** To face a direction the pilot computes the
rotation from its present to its wanted attitude and demands full torque
about that axis. It then asks: "if I let go now, how far will I still
rotate?" When the remaining angle is within 98 % of that coast angle the
demand is cut to what holds the residual rate, and close to the goal the
demand is proportional to the remaining angle. Ships therefore stop on
their heading without overshoot at any turn rate. Roll is blended in to
bank the ship into turns.

**Closed forms.** All such questions are answered from the flight model
of chapter 02, not by trial simulation. With thrust `F`, drag `D` and
mass `m` (or torque, angular drag and inertia):

```text
terminal speed        v_inf = F / D
time from v0 to v1    t = -(m/D) * ln((v1 - v_inf) / (v0 - v_inf))    (v1 capped at 0.99 v_inf)
distance in time t    s = v_inf*t + (v0 - v_inf) * (m/D) * (1 - exp(-D*t/m))
throttle for speed v  = |v| * D / F, at most 1
```

They are used for when to start braking before a point, when to stop
turning, the strafe trigger range, how much throttle matches a leader's
speed, and whether a follower can catch up without cruise. Because the
flight model is a first-order lag these are exact for constant input.

**Obstacles.** Pilots do not plan paths. Each moving ship casts a ray
from its nose along a slowly spiralling direction ahead (look-ahead
proportional to hull length), every 0.5 s, or 0.1 s while something is
in the way. On a close hit it probes a cone of random rays, keeps up to
25 recently blocked directions, and steers along their average escape
direction with reduced throttle. Separately, the nearest ship or solar
on a collision course from the scanner list produces a sideways nudge.
The probes see whatever has a collision body; nothing knows the layout
of an asteroid field in advance.

**Zone flags.** What the AI does read about fields is the property set
of the zones around a point (chapter 01): object density (three
levels), danger density (three levels) and the cloud flag. They are
reduced to a tier:

| Tier | Object density | Danger density | Cloud |
| --- | --- | --- | --- |
| 0 | none | none | no |
| 1 | at most low | none | no |
| 2 | at most medium | at most low | allowed |
| 3 | anything else | | |

The job's field-targeting setting (never, low density, high density,
always = tier 0 to 3) is the highest tier the pilot will chase a target
into from outside. A pilot already inside a worse tier than its setting
fights whoever is there. This keeps patrols from following prey into
dense rock or minefields, and it is the main way fields work as hiding
places.

### 3.7 Target selection

Once a second, unless a directive forbids it:

1. **Leader rules.** A follower whose leader is idle in formation drops
   its target. With "wait for leader's target" set, it does nothing
   until the leader has one.
2. **Combat drift.** If the fight has moved more than 10,000 m from
   where it began, the pilot disengages and tells content, which sends
   ambient NPCs back to their business.
3. **Candidates** come from the scanner contact list: alive; reputation
   towards them at or below −0.6 (hostile, chapter 07); within 5,000 m;
   for a follower, within the job's leash from the leader (1,500 to
   10,000 m); passing the zone tier test of 3.6; inside the directive's
   area if one is set. Players can be excluded per job. When an outright
   enemy (reputation −1) is close, only outright enemies count. Up to
   eight targets forced by content are always candidates. At most 16.
4. **Overwhelmed?** Scene threat ratio = sum of hostile ranks ÷ (own
   rank + friendly ranks + hostile ranks), over ships in scanner range.
   Above the job's threshold (0.15, 0.35, 0.65, 0.85 or 1.0 for the five
   settings "easiest" to "hardest") the pilot enters the overwhelmed
   state and takes no target.
5. **Preference.** Walk the attack preferences in order; the first
   entry whose class has any candidate within its range decides the
   pool, and its weapon kinds (guns, guided, unguided, torpedo) become
   the weapons allowed against that target.
6. **Pick.** A player in the pool who has fewer than two attackers is
   taken first; a player who already has more than two is removed if
   anyone else is available. Otherwise keep the current target if it is
   in the pool, else pick at random. The limit of two is a global that
   missions can change.
7. With "attack as a formation" set, a leader gives its target to all
   NPC followers and a follower takes the leader's.

The attacker limit is the game's main fairness device: however many
enemies are present, about two shoot at each player while the rest find
other work.

### 3.8 Fire control and accuracy

**Rhythm.** Each gun cycles through a burst (1 to 5 s) and a pause (0.5
to 2.5 s), each jittered by up to ±50 %; within a burst it may fire
every 0 to 0.5 s if the weapon's own refire allows. A gun fires when it
is in its burst, the target is within gun range, the line of fire is
not blocked (a ray test, cached for a second) and the barrel points
within 3° of the aim point. Gun range is the longest range among the
ship's guns × 1.1 × a per-pilot random factor of 0.7 to 1.3, so pilots
open fire at different distances. Turret-mounted guns choose their own
target twice a second: the nearest hostile of an attackable class
within 1.2 × gun range that the turret can reach.

**Aim error.** The aim point is the lead point (where the target will
be when the shot arrives, chapter 03) plus an error fixed in the
target's frame and re-rolled every 0.25 to 3 s:

```text
angle  = cone × u^p          u uniform in [0, 1)
offset = distance × tan(angle), in a random direction across the line of sight
```

`cone` is 1° to 8° by pilot. The exponent `p` shapes the distribution:
above 1 most shots fall near the centre, below 1 near the edge. It is
authored separately for player targets (0.7 to 3) and NPC targets
(usually 6), so NPCs kill each other efficiently while the player gets
near misses. Against a player moving slower than about 3 m/s there is no
error. The "fire guns" damage trigger suspends the burst rhythm.

**Launchers.** Guided and unguided launchers fire at intervals of 10 to
60 s (±25 to 50 %), within 1,000 m, when the target is inside a 20° to
22.5° cone. A cruise-disrupting launcher fires only when the target has
been in cruise for 2 s and is 150 to 2,500 m away, then waits 5 s.
Mines are dropped at most every 10 s when a hostile is following within
250 m in a 30° cone behind and own speed exceeds 20 m/s, on an attack
pass if the pilot is so authored, or at once on the mine damage trigger.

**Countermeasures** are not reactive: the dispenser is switched on for
an authored part of a repeating period (3 s on, 0 to 3 s off in the
data).

### 3.9 Reactions: damage, missiles, consumables, fleeing, cargo

**Damage level.** Every hit adds its damage to a level that decays by
half of (maximum shield + maximum hull) per second. Eleven triggers
compare the level with a fraction of (maximum shield + maximum hull);
once tripped a trigger stays on for its hold time. So a trigger at 0.25
with these numbers fires when a quarter of the ship's total protection
is lost within about half a second: burst damage, not accumulated
damage.

| Trigger | Typical fraction | Effect |
| --- | --- | --- |
| Break | 0.2 to 1 | Evade goes to its break phase. |
| Dodge more | 0.25 to 0.3 | Evade dodges harder (3.4). |
| Brake | 1 | Evade breaks by reversing. |
| Engine-kill turn | 1 | Forces behaviour 15. |
| Thruster | 0.1 to 0.2 | Burst of thruster. |
| Roll | 0.1 to 0.3 | Roll while manoeuvring. |
| Fire guns, fire missiles | 1 | Fire regardless of rhythm and cone. |
| Drop mines | 0.25 | Drop all mines now. |
| Break formation, break apart | 0.2 to 0.5 | See 3.10. |

A fraction of 1 can hardly be reached and means "off".

**Missiles.** For the nearest closing guided munition within 1,000 m
the pilot estimates time to impact as distance ÷ (missile speed − own
speed × cosine of the angle between the two velocities), and reacts
when it falls below a reaction time (0 to 1 s).

**Repair consumables.** When hull (or shield) falls to the authored
fraction and a consumable is carried: wait a pre-delay, use one, wait a
post-delay. Almost all shipped pilots have the fraction at 0: NPCs do
not heal. Only a few campaign pilots do.

**Fleeing.** Flee becomes "must run" when any holds: (hull + shield) ÷
(maximum hull + maximum shield) is below the job's fraction (0 to 0.75);
the scene threat ratio exceeds the job's flee threshold (same five
steps as in 3.7); no gun or launcher works any more and the job says to
flee unarmed; or the leader flees and the job says to follow suit.
Content supplies the destination and removes or docks the ship on
arrival.

**Cargo.** A pilot does not decide to drop cargo in combat. When a
patrol scans it and demands a good (chapter 06), the pilot answers from
the scene threat ratio: at or above the job's loot threshold it
jettisons the cargo, below it refuses and is attacked. Looting is
governed by a loot preference (commodities, equipment, consumables),
which as shipped is never read successfully, so NPCs do not loot.

### 3.10 Formations

Content creates a formation by naming a leader and giving each follower
an offset from a formation definition; followers switch to the escort
variant of their state graph. Offsets are in the leader's frame (+Z is
behind).

Each step a follower computes its slot as the leader's position plus
its own offset rotated by the leader's orientation, faces a point ahead
of the slot (by its own radius plus a standoff of 200 m, or 500 m in
cruise) and drives its throttle to match the leader's speed while
closing the position error, engaging cruise or the thruster when the
closed forms say it cannot catch up otherwise. When the leader docks or
enters a lane, each follower receives the same order.

In combat the formation block and the job decide cohesion:

- With "attack as a formation" off, followers fight individually and
  return to their slots when idle.
- With it on, attack pass, trail, strafe and the engine-kill turn
  report "not applicable" for a follower during the active part of a
  cycle (0 to 30 s on, 0 to 30 s off), so the group flies and fires as
  one, then loosens.
- A follower breaks formation on the break-formation damage trigger or
  when a missile's time to impact is short, for at most an authored
  time (0 to 15 s), leaving by an authored exit (break away from the
  centre, with thruster, brake, outrun).
- Starting an attack pass, a pass-by, a dodge or a break can tell the
  leader "break apart" for a time and "regroup" afterwards, so a whole
  flight scatters and re-forms on one pilot's manoeuvre.
- A "leader makes me tougher" flag lets a follower use the leader's gun
  and repair parameters.

A player joins an NPC's formation through the same behaviour; the
target must be a ship (not a capital ship), within 5,000 m, not
hostile, not launching or in a lane, and not itself following the
player. Formation definitions reserve a slot for a joining player.

### 3.11 What a personality is made of

| Block | Modulates |
| --- | --- |
| Evade: dodge | Activation range, style weights, cone, interval, duration, distance, throttles, direction weights. |
| Evade: break | Duration, style weights (sideways, outrun, reverse), direction weights, thruster delay. |
| Attack pass: run in | Turn-in distance, extend time, extend style, throttles, jink parameters. |
| Attack pass: pass by | Pass distance and time, style, break direction, mines on pass. |
| Trail | Distance, lock cone, break time, turn authority, thruster. |
| Strafe | Run-away distance, attack and turn throttle. |
| Engine kill | Search and face times, thruster use, maximum target distance. |
| Repair | Thresholds and delays for hull and shield consumables. |
| Gun | Burst, pause, interval, accuracy cone, the two exponents, range factor, error refresh; turret variants. |
| Mine, missile | Intervals, cones, ranges; cruise-disruptor rules. |
| Damage reaction | The nine combat triggers of 3.9. |
| Missile reaction | Detection distance and three reaction times. |
| Countermeasure | On and off times. |
| Formation | Cohesion cycle, break triggers, exit style, toughness sharing. |
| Job | Attack preferences, component order, threat thresholds, flee rules, field targeting, loot rules, leader rules, scan chance. |

### 3.12 Difficulty

There is no global difficulty setting. Three things scale opposition:

1. **Pilot tier.** Most factions' pilots come in four tiers that differ
   only in the gun block and the dodge block. From lowest to highest
   the accuracy cone falls from 5° to 2°, the pause between bursts from
   2 s to 0.5 s, the in-burst interval from 0.2 s to 0, the error
   refresh from 0.5 s to 0.25 s; dodges last a shorter time each and
   repeat sooner. Manoeuvre choice does not change.
2. **Ship and loadout level** (chapter 06). The NPC record that names
   the pilot also names hull, loadout and a level; level feeds the
   scene threat ratio as rank.
3. **The attacker limit** of 3.7.

Pilot tier is chosen per NPC record, not derived from level: high-level
records lean towards the upper tiers but all tiers occur at most levels.

### 3.13 Authority and presentation

NPC manoeuvre layers run on the authority and steer authority-owned
bodies. The player's manoeuvre layer runs on the presentation side with
the player's ship (per design notes); the authority forwards directives
for a player's ship to that side. Target selection for a player is the
player's own. Random draws use the shared generator, so AI is not
reproducible between runs.

## 4. Data that defines it

| Record | Principal fields | Volume and ranges |
| --- | --- | --- |
| State graph file | graph count, behaviour count (must equal 21), then per graph: role name, variant, 441 weights | 10 graphs; weights 0, 0.05 steps to 1 |
| Personality block (15 kinds besides job) | nickname plus the parameters of 3.11 | population file: 4 to 29 per kind, 77 gun blocks |
| Job block | attack preferences (class, range, weapon kinds) ×1 to 12; five-step thresholds; flags | 45 |
| Pilot | nickname, optional parent pilot, up to 16 block references; unset references inherit | 64 ambient, 255 campaign |
| Formation | nickname, slot offsets, optional player slot | 93; 1 to 12 slots, most have 7; spacing 30 to 250 m |
| NPC record (chapter 06) | hull, loadout, level, pilot, state graph role | 404 |

Example pilots, anonymised:

| Pilot | Parent | Overrides |
| --- | --- | --- |
| `example_patrol_low` | — | all 16 blocks; gun cone 5°, pause 2 s; formation "stay"; job "attack as a formation" |
| `example_patrol_high` | `example_patrol_mid` | gun block (cone 2°, pause 0.5 s), dodge block |
| `example_hauler` | — | gun block only; everything else default |

## 5. Connections

- **Needs 01:** property sets of zones (density, danger, cloud).
- **Needs 02:** the control values (throttle, axis throttle, nudge,
  strafe, thruster, engine, cruise) and the drag-based flight model that
  the closed forms invert.
- **Needs 03:** weapon ranges, refire, lead point, guided munitions,
  mines, countermeasures, damage events.
- **Needs 04:** dock, launch, lane and go-to behaviours in detail.
- **Needs 07:** reputation between any two objects; rank.
- **Gives 06:** the pilot and state graph an NPC record names; combat
  state, drift and flee requests; the scan answer.
- **Gives 08:** directives with completion callbacks; forced targets;
  the attacker limit; manoeuvre locks for the player.
- **Gives 09:** which manoeuvres are currently available to the player.
- **Gives 11:** NPC control input is produced on the authority.

## 6. Design notes for the builder

**Essential.**

- The split into purpose (content), manoeuvre choice (weighted graph),
  geometry (behaviour) and control (arbiter), with fire control running
  beside the manoeuvre rather than inside it.
- Behaviours that both *veto themselves* and *report commitment*. The
  graph only supplies taste; correctness lives in the verdicts.
- Running the player's autopilot through the same layer.
- The attacker limit and the separate accuracy against players. Without
  them group fights are either lethal or toothless.
- Burst-based damage reactions with decay: they make NPCs respond to
  being hit hard, not to being low.

**Feel.** The pass rhythm of extend, run in, pass; authored styles and
direction weights; staggered clocks; the per-pilot gun range factor; the
escape through engine kill.

**Era.** A fixed table of 21 behaviours with a file that must match its
size; reactions and countermeasures on timers rather than on
perception; no memory, no cover, no focus fire beyond the leader's
target; thresholds in five named steps; parameters that the shipped
loaders ignore.

**Simplifications.**

- A builder with a higher-order flight model loses the closed forms and
  needs numeric prediction or tuned controllers. Keeping the flight
  model first-order is the cheaper choice.
- Five behaviours (attack pass, evade, trail, formation, flee) plus
  travel give most of the experience. Strafe and the capital manoeuvre
  matter only once large ships exist.
- A behaviour tree or utility system can replace the graph, as long as
  the weighted randomness between equally valid manoeuvres survives.
- Per-faction pilots mostly differ in dodge style. Four tiers of one
  pilot per role would be hard to tell from the original's 64.

## 7. Open questions

1. How the reputation value between two particular objects is formed,
   and why −1 is treated as a separate "outright enemy" class
   (chapter 07).
2. Whether asteroids have collision bodies on the authority away from
   players. If not, NPCs fly through fields unhindered and only the
   zone tier keeps them out. The navigation layer also caches the
   zone's field-kind flags every period; no reader was found.
3. Trail, the engine-kill turn, guide, follow and idle were read in
   part. Their state tables are not established; the "search time" and
   "face time" of the engine-kill block are unexplained.
4. Several job fields have no visible effect: target toughness
   preference, component attack order beyond its use in strafing, and
   the combat drift distance, which the loader reads and discards.
5. How a joining player's formation offset is assigned (presumably the
   reserved player slot).
6. Whether the presentation side runs NPC behaviours at all, for
   example to predict remote ships.

## 8. Basis

Design documents: `design/assessment/02-simulation-core.md` sections 1
and 2; `design/sim-records.md` (behaviour tick records, the note on
loot preference); `design/adr/0003-simulation-oracle.md`.

Data: `outdir/data/AI/state_graph.db` (tabulated in full),
`MISSIONS/pilots_population.ini` (tabulated; `pilots_story.ini`
counted), `MISSIONS/formations.ini`, `MISSIONS/npcships.ini` (pilot,
level, state graph), `UNIVERSE` zone `property_flags` (distribution).
Scenarios `Oracle/scenarios/dogfight.txt`, `skirmish.txt`,
`formation.txt`.

Code read closely: `Common/common_behaviormanager.cpp`
(`UpdateBehaviorTick`, `SetCurrentBehavior`, `set_current_directive`,
`set_state_graph`, the event handler), `common_behaviortypes.cpp`,
`common_pubstategraph.cpp`, `include/common_pub.h` (`Personality` and
the directive ops), `common_evadebehavior.cpp`,
`common_buzzbehavior.cpp`, `common_strafebehavior.cpp`,
`common_formationbehavior.cpp`, `common_targetmanager.cpp`,
`common_aifirecontrol.cpp`, `common_sysmgr_gungroups.cpp`,
`common_sysmgr_timertable.cpp`, `common_gunconvergence.cpp`,
`common_repairmanager.cpp`, `common_cship.cpp` (the `get_time_to_…`,
`get_distance_travelled`, `get_projected_throttle` family),
`common_fleebehavior.cpp`, `Content/PilotPersonalities.cpp`.

Read in part: `common_aisteer.cpp` (`LimitAxisThrottleForTurn`,
`TurnHelper`), `common_obstacleavoidance.cpp`, `common_pathfind.cpp`,
`common_largeshipbehavior.cpp`, `common_trailbehavior.cpp`,
`common_facebehavior.cpp`, `common_lootbehavior.cpp`,
`common_basefollowerbehavior.cpp`, `common_drasticevadebehavior.cpp`,
`common_freeflightbehavior.cpp`, `common_cruisebehavior.cpp`,
`common_idlebehavior.cpp`, `Content/PatrolGuides.cpp` (scan answer).
By header only: go-to, dock, launch, lane, guide, follow, delay.

Names used here against the code: attack pass = Buzz, engine-kill turn
= Face, capital manoeuvre = Large Ship (move), forced evade =
DrasticEvade, lane travel = InstantTradeLane, steering primitive =
"idea", steering arbiter = thrust manager, manoeuvre layer = behaviour
manager with its system manager.

Inference: the description of trail; the reading of reputation −1 as
"outright enemy"; that state-graph weights of 0.05 are meant as "only
when the behaviour insists"; the purpose statements attached to the
attacker limit, the zone tiers and the accuracy exponents; the mine
"following" test read as a hostile behind (the code tests a hostile
ahead of the dropper's own nose that flies the same way, which is the
geometry of being chased only if read from the pursuer; treat as
uncertain).
