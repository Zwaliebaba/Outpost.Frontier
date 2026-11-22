# 06 — Population and encounters

Status: draft
Depends on: 01, 02, 04, 05, 07, 11, 12

## 1. Purpose and player experience

Space would be empty without this part. No NPC ship is placed by hand in
a system file. Zones only state who lives there and how densely; a
controller on the authority creates ships around each player, gives each
group a job, and removes it when nobody is near.

The player sees a world that seems to exist without them. Freighters with
escorts leave lanes and dock at stations. Police fly patrol routes, scan
the player's hold and demand contraband. Outlaws wait along the paths to
their bases and sometimes break a lane. Space is busy at a gate and quiet
in the open. Every ship has a faction, a name, a face and a voice, and
they talk: to dock control, to each other, to the player.

Three properties shape play:

- **Place sets the danger.** Each zone names a level from 1 to 19. The
  same faction fields a weak ship at level 3 and a strong one at level
  15. Travelling outward is the difficulty curve.
- **Reputation sets the tone.** Who appears depends on the zone; whether
  they attack depends on the player's standing with their faction
  (chapter 07).
- **Combat is paced.** A fight is capped in size, and after a fight the
  population waits before sending anyone new.

The population is disposable. No ambient NPC is saved; a ship that
leaves the player's neighbourhood is deleted within seconds.

## 2. Concepts

Zone roles (path segment, lane corridor, population area) are defined in
chapter 01; NPC, pilot, formation, encounter, population, faction and
attitude in the glossary. This chapter adds:

| Concept | Definition |
| --- | --- |
| Population entry | The population fields of one zone: density, respawn time, battle size, relief time, encounter lines. |
| Encounter line | In a population entry: an encounter definition, a level and a relative chance, followed by the factions that may fly it, with weights. |
| Encounter definition | A file of group templates and a selection rule that picks one. Shared by many zones. |
| Group template | One group that spawns together: ship lines, a formation class, a job, allowed arrival methods, a lifetime. |
| Ship line | A count range, a class group, an optional level offset, a pilot job and optional extra tags. Yields identical ships. |
| Class tag | A label on an NPC ship definition: lawful or unlawful, its role (fighter, freighter, transport, gunboat …) and each level it serves. |
| Class group | A named set of class tags. Ship lines and caps name groups, never ship archetypes. |
| NPC ship definition | A record binding a ship archetype, a loadout, a pilot, a combat state graph, a level and class tags. |
| Faction properties | Per faction: its NPC ship definitions, name pools, voices, pilot appearances, formation per class, cargo-scanning habits. |
| Arrival method | How a new group enters the scene: out of a lane, from a dock, cruising in, or passing the player. |
| Itinerary | The standing task of a group's leader, which hands the pilot one directive after another: trade run, path patrol, wander. |
| Lifetime budget | Seconds a ship may exist unobserved; refilled near players, drained faster with distance. |
| Lane attack | A spawn that breaks a lane ring ahead of a travelling player and puts hostile ships there. |
| Piloted solar | A station, gate or weapon platform with a faction, a loadout and a pilot. Permanent; outside the population loop. |
| Line segment | One recorded piece of speech, with several variants per voice profile. |

```text
zone (system file)
 └─ population entry: density, respawn time, battle size, relief time
     └─ encounter line ×n: level, chance
         ├─ faction, weight ×n ───────────────► faction properties
         └─ encounter definition (shared file)     ├─ NPC ship definitions ×n
             ├─ selection rule: weighted          │    ├─ ship archetype (02)
             └─ group template ×n                 │    ├─ loadout (02)
                 ├─ ship line ×n: count, class    │    ├─ pilot, state graph (05)
                 │    group, level offset, job ───┘    └─ level, class tags
                 ├─ formation class ──► faction's formation for that class (05)
                 ├─ job: trade | path patrol | wander
                 └─ arrival methods, lifetime
```

A ship line never names a ship. It names a class group and inherits a
level from the zone; the faction drawn for the group supplies the
matching NPC ship definition. One encounter definition thus serves every
faction and level.

## 3. How it works

### 3.1 Where it runs

The population is one content controller on the authority (chapter 11)
with a heartbeat of 3 s. It works per player; a system without players
is not simulated and has no NPCs. A second controller, which supports
everything in space, runs the lifetime rule (3.8) once per second, the
dock queues and the radio chatter.

| Occasion | Action |
| --- | --- |
| Heartbeat, for each player in space and not inside a lane | Possibly add one group (3.3) |
| A player launches from a base or enters a system through a jump point | Fill the neighbourhood at once (3.4) |
| A player enters a lane | Roll for a lane attack (3.9); fill the neighbourhood of the far end, 1,200 m beyond the last ring |

### 3.2 Which zones speak for a position

Zones overlap freely, so the controller first decides which population
entries apply:

- **At the ship:** every path segment containing the point, plus the
  smallest other populated zone containing it. A lane corridor counts as
  size zero and always wins. If any of these has density 0, nothing
  spawns here; this is how an author keeps a place quiet.
- **Near:** populated zones within 2,000 m not in the first list, unless
  flagged "not additive". They count at reduced weight (factor 0.6).
- **Ahead** (single player only): such zones in the view frustum up to
  4,000 m. They are populated at a point 5,000 m ahead with a third of
  their density, so the place the player flies towards is inhabited.

Lane corridors and mission sites are recognised by a fixed word in the
zone's nickname (a lane corridor must also be a box over ten times
longer than wide and high); the other roles by their fields.

### 3.3 The steady loop

Let `D` be the largest density among the zones at the ship (a near zone
replaces it when `D` is below 0.6 of that zone's density) and `n` the
number of NPC ships within the counting radius of the player: 3,750 m in
single player, 9,200 m in multiplayer. For each zone in turn, with
respawn time `T`:

```text
if n >= D: nothing
chance = clamp((D - n) / T, 0, 1)
if random() < chance: try to spawn one encounter from this zone
stop at the first zone that produced ships
```

At most one group appears per player per heartbeat; the rate falls as
the neighbourhood fills and stops at `D` ships. Groups have one to five
ships, so the count can overshoot.

Worked example. A path segment in open space has density 3 and respawn
time 90. With nobody around the chance is 3/90 per heartbeat: one group
about every 90 s. With two ships present it is 1/90: about every 270 s. A
gate zone has density 10 and respawn time 10: the chance is (10 − n)/10,
certain when empty and one in two with five ships near. A major lane has
6 and 15.

Density thus means "ships near a player who is here", not "ships in the
zone"; zones can be any size without changing the load.

### 3.4 Filling the neighbourhood on arrival

Waiting 90 s for company would show an empty world, so on arrival
groups are created at once:

1. The budget is `D`; in multiplayer minus the NPC ships already within
   9,200 m, since another player may have populated the place.
2. Each zone gets a share of the budget proportional to `1 / T`.
3. Encounters are drawn from each zone until its share is reached (at
   least one ship), without the chance roll.

These groups appear 100 to 1,875 m from the player, never by lane. Dock
controllers do not hail them for 30 s.

### 3.5 Choosing an encounter, and what can veto it

Within a zone an encounter line is drawn by relative chance; one that
cannot spawn leaves the draw and another is tried. The selection rule
picks a group template by weight. The faction is drawn by weight from
those under the encounter line that own an NPC ship definition for every
ship line at this level. A template may instead tie its faction to
another template's ("hostile to group 0"); the data holds only an
example.

A group is refused if any of these holds:

| Check | Rule |
| --- | --- |
| Battle size | The player is in combat (a hostile object within 2,500 m) and has at least the zone's battle size of hostile contacts (typically 4). |
| Relief | The player is not in combat, but the last fight ended less than the zone's relief time ago (typically 30 s). Friendly traffic waits too. |
| Mission switches | A mission may switch the population off, declare spheres free of it, or forbid per player the factions hostile to that player, or all others. |
| Uniqueness | The template forbids a second copy while one lives, or has reached its creation limit. |
| Class caps | The zone caps how many ships of a class group may be near (authored: one patrol of each kind near a path; four lawful, four unlawful in an area). See open question 1. |
| Placement | No arrival method finds a valid position (3.7). |

Battle size and relief pace the game. Without the first a busy zone
would feed a fight without end; without the second the next patrol would
arrive while the player still collects loot. A mission can also make
ambient and mission ships tolerate each other: their mutual attitudes
are raised to just above the hostile threshold.

### 3.6 Building the ships

For each ship line the count is uniform between minimum and maximum; a
minimum of 0 makes escorts optional. The line's class group and the level
tag (zone level plus the line's offset, at least 1) select the faction's
candidate NPC ship definitions; one is chosen uniformly. From it come
ship archetype, loadout, state graph and pilot. The line's pilot job
replaces the job part of the pilot's personality (chapter 05), so one
pilot record serves as patrol leader, escort or freighter captain.
Escort lines usually carry an offset of −1: leaders are a step better
than their wingmen.

Each ship also receives:

- a **name** and a private reputation record affiliated to its faction
  (3.11; chapter 07);
- a **pilot appearance** and a **voice** from the faction's lists, with
  gender weighted by the faction's share of male voices;
- the **formation** the faction uses for the template's formation class,
  so factions fly the same encounter in different shapes (chapter 05);
- for a trade job, **cargo**: a commodity chosen by weight from what the
  two endpoint bases trade and the faction's goods list, filling 80 % of
  the hold. Freighters and transports behind the leader carry the same
  good. A destroyed trader thus drops what its route plausibly carries.

Only the leader gets the itinerary; the others follow in formation.

### 3.7 Placing the group

One of the template's arrival methods is tried at random until one
succeeds.

| Method | Where the group appears | Conditions |
| --- | --- | --- |
| Out of a lane | At a ring of a lane within 5,000 m, as if it had just left the lane | Not if a ship travelling that way is within 15,000 m of the exit or left under 7 s ago |
| From a dock | Launching from a docking ring, jump point, station or capital ship the faction may use | Single-ship groups only; small ships from docks within 2,700 m, large ones from 3,500 to 4,000 m |
| Cruising in | Path patrols: on a segment of the zone's path, 2,500 m or more away. Others: on a sphere of radius 2,500 to 2,550 m around where the player will be in 5 s | Point clear of objects and outside every exclusion sphere |
| Passing close | Ahead of a moving player by 2,500 m plus 2.5 s of its speed, 150 m above its course | As above |

The last three never place a group within 2,500 m of another player.
That distance is also the radius within which the authority shows a
player objects that nobody owns (chapter 11): ships are created at the
edge of what can be seen (inferred).

### 3.8 What ambient NPCs do, and how they leave

The itinerary gives the leader its next directive whenever the last one
is done. Fighting and fleeing are the pilot's own business (chapter 05);
the itinerary resumes afterwards.

**Trade run.** Two dockable objects are chosen that the faction may use
and the ship fits: near the lane it arrived by, along the zone's path, or
anywhere in the system. A jump point stands for a base of the faction in
the next system.

| State | Event | Next |
| --- | --- | --- |
| Travel to target, lanes included | Arrived in range | Request docking |
| Docking | Docked | Wait 20 to 30 s, launch, swap the two targets, travel |
| Docking | Not docked | Swap the targets, travel |

**Path patrol.** The leader flies the zone's path point by point between
two objects on it, turning at the ends or looping. A patrol of a faction
that scans cargo meanwhile looks for ships within 2,500 m: it closes in,
announces the scan (if the faction does), scans, and on finding a good
from the faction's list demands that it be dropped, waits for the answer,
then collects the jettisoned cargo or turns hostile. Lawful and unlawful
factions share the mechanism; one confiscates, the other robs.

**Wander.** The leader hops 1,500 to 3,000 m to random points in its
zone that it can reach in a clear straight line, with shorter hops when
boxed in.

**Lifetime.** Each ship has a lifetime `L` from its template (default
24 s) and starts with a budget of `2 L`. Once per second:

| Condition | Effect on the budget |
| --- | --- |
| A player is within 2,500 m; or (single player) the ship is within 4,000 m inside a 70° cone of the player's view; or the ship is in a lane or near the lane stretch a player is travelling | Refill to at least `L` |
| Otherwise, nearest player at distance `d` | Subtract `min(15, 1 + (d − 2,500) / 100)` |
| No player in the system | Subtract 15 |
| Budget below 0 | Remove the ship, unless (single player) the player has it targeted. A formation goes only as a whole. |

A wingman only refills; the leader's clock decides for the group. Beyond
3,900 m the drain is fifteenfold, so a group is gone 2 to 3 s after the
player loses it. Removal is silent (inferred). NPCs never travel between
systems: one that enters a jump point leaves play, one arriving through
a jump point is created there (inferred).

### 3.9 Lane attacks

When a player enters a lane, the controller may stage an ambush:

1. At most one roll per lane per 180 s.
2. The probability is the largest "lane attack" value among the path
   segments within 2,000 m of the lane's rings whose factions include
   one hostile to the ring's owner (authored 5 to 80 %, median 15 %).
3. A ring ahead of the player, beyond the next one, with no player
   within 5,000 m is chosen by weight; then one such zone and one
   hostile faction.
4. The ring's shield is drained, which is what interrupts the lane
   (chapter 04). One group from the zone's encounter is created 1,400 m
   beyond the ring; its leader waits 25 s, then patrols the zone's path.

The player drops out of the lane among ships of a faction with a reason
to be there. A mission can switch lane attacks off per player.

### 3.10 Difficulty by place

Level is authored per encounter line, from 1 to 19. Each system occupies
a band two to four levels wide: in the data the starting systems span 1
to 4, most lie between 6 and 13, the remote ones reach 15 to 19. A
faction covers its levels with a ladder of NPC ship definitions. One
faction's fighter ladder, typical of the rest:

| Level | Hull | Loadout | Pilot grade |
| --- | --- | --- | --- |
| 1, 2, 3 | light | first | medium, hard, ace |
| 4, 5, 6 | light | second | medium, hard, ace |
| 7 … 12 | heavy | first, then second | medium, hard, ace, twice |
| 13 … 19 | very heavy | first, then second | medium to ace, then easy to ace |

A definition can serve a range of levels (a police fighter tagged for
levels 4 to 12), so a faction confined to one region needs only a few.
Capital ships sit at 22 and 25, above every zone level; only missions
place them (inferred).

The ambient population is not scaled to the player: nothing in the spawn
path reads the player's level, ship or wealth. Generated missions do
scale (chapter 08); the table that maps campaign progress to mission
difficulty is theirs.

### 3.11 Names

A name has two parts, each a text pattern filled with text identifiers:

- **Group part:** the faction's short name, a word for the kind of group
  (one for trade runs; for patrols and wanderers one of two, by the
  faction's legality), a designation word and a number from 0 to 19.
- **Individual part:** for small ships a rank title, first name and last
  name from the faction's pools; the title is one of three, chosen by
  the ship's level against two thresholds per faction. Wingmen are
  "escort ‹n›" with rank and last name. Large ships are "‹ship type›
  ‹faction's ship prefix› ‹ship name›".

Wingmen reuse the leader's designation. Names, designations and ship
names are reserved while their owner lives and released 30 s later, so
living NPCs do not share a name while the pool lasts.

### 3.12 Radio chatter

Speech is assembled from pieces. A **line** is a speaker,
a listener and a list of line segments: "this is", the faction's short
name, the designation word, the number, "to", the station's name,
"requesting", "permission to dock". Stations, systems and factions carry
a prefix from which their segment names are formed. For each segment the
voice profile states how many recorded variants it has (typically 1 to
8); one is drawn with a bias to the low numbers. A voice also declares
the roles it can speak (dock controller, docking ship, combat at three
intensities, scanning, idle chatter …); no line is built for a voice
without the role.

A **conversation** is an ordered list of lines. The authority knows each
segment's duration, plays a line, waits until it has finished, and plays
the next if its speaker still exists.

| Source | Trigger |
| --- | --- |
| Dock control | A ship enters a 2,500 m sensor volume around a dockable solar: hail and identification; later request, clearance, stand-by or refusal as the dock queue decides (chapter 04) |
| Combat | Damage taken or dealt, a death, engaging or declining, fleeing with a reason, calls for help, friendly fire, all clear |
| Cargo scan | Each step of the patrol's scan sequence |
| Idle | A formation leader within 2,500 m of a player may start small talk with its wingmen: status, rumours, trade |
| Player hail | A hailed ship identifies itself or brushes the player off |

Every exchange has a category with a minimum interval and a
probability. It is refused if the same category was spoken within
10,000 m less than its interval ago (default 15 s; combat 3 to 6 s; idle
talk 30 s); two combat categories pass only with 5 % and 40 %. Without
this a six-ship fight would talk without pause.

### 3.13 Authority and presentation

Everything above runs on the authority. The presentation receives ship
creations with name, faction, appearance, voice and rank, and "comm"
messages naming speaker, listener, voice and segments; it plays the
segments back to back and shows the speaker (chapter 09).

## 4. Data that defines it

**Zone population entry** (system file; about 3,800 zones in 53 systems
have encounters):

| Field | Meaning | Range; typical |
| --- | --- | --- |
| Density | Ships wanted near a player here | 0 to 30; 3 on paths, 5 to 6 in fields and lanes, 9 to 10 at jump points |
| Respawn time | Divisor of the spawn chance | 8 to 90; 90 on paths, 10 to 20 at hubs |
| Battle size | Hostile contacts beyond which nobody joins | 0 to 9; 4 |
| Relief time | Seconds of calm required after a fight | 0 to 50; 30 |
| Lane attack | Percentage for 3.9 | 5 to 80; on about 780 zones |
| Class cap | Count, class group | 1 per patrol kind; 4 lawful, 4 unlawful |
| Not additive | Does not act as a "near" zone | on 129 zones |
| Encounter line | Definition, level, relative chance | 1 to 10 per zone; four fifths have one |
| Faction line | Faction, weight; follows its encounter line | 1 to 11; usually one |

Path segments also carry path label, index, use (patrol or trade) and
mission eligibility (chapters 01, 08). Each system file declares the
encounter definitions its zones use, by local name and file (about 22
per system). Zone records also hold a zone-wide level, a population type
and raw faction weights that agree with the encounter lines but are not
read by the game: probably the input of an authoring tool (inferred).

```text
[zone]  example_sys_path_outlaw1_2          (a cylinder, chapter 01)
  density 5; respawn 30; battle size 4; relief 20
  path label outlaw1, index 2; use trade
  class cap 1 × each patrol kind
  encounter example_path_trader, level 7, chance 1
    faction example_outlaw_grp, weight 1
```

**Encounter definition** (42 files, one or two group templates each):

| Field | Meaning | Values in the data |
| --- | --- | --- |
| Ship line | minimum, maximum, class group, level offset | 1–1 leader; 0–1, 0–2 or 2–3 escorts; offset −1 on most escort lines |
| Pilot job | Job block for the line (chapter 05) | about 20: leaders and members of patrol, assault, defence and escort; scout, freighter, transport, trader, smuggler, scavenger, repair |
| Extra tags | Class tags added to the ships | mark patrol leaders for the class caps, and wanderers |
| Formation class | Looked up in the faction's formations | fighters, freighters, transports, gunboats |
| Job | Itinerary | path patrol (24 templates), trade (18), wander (15) |
| Arrival | Allowed methods | cruise only (26), all (13), all but lane and jump point (11), lane only (6) |
| Lifetime, uniqueness | Seconds; simultaneous copies; times to create | 24; allowed; unlimited |
| Selection rule | Template index, weight | weights 1 to 5 |

```text
example_lane_freighter
  group 0: freighter ×1 (job freighter)
           fighter ×0–1 (job escort leader), fighter ×0–2, offset −1 (job escort)
           formation class freighters; job trade; arrival lane
  group 1: freighter ×2, same escorts; formation class freighters, variant 2
  selection: group 0 weight 5, group 1 weight 5
```

The files form a grid: area, path and lane variants of trade
(freighter, transport, armoured, smuggler, prisoner transport), patrol
(lawful, unlawful, bounty hunters), assault, scout, defend, scavenger,
repair and lifting ships.

**NPC ship definition** (404 shared): nickname, ship archetype, loadout,
pilot, state graph (fighter for three quarters; transport, gunboat,
cruiser), level, class tags.

**Class group** (78): a nickname and member tags; one per level and per
ship role, plus broader sets.

**Faction properties** (55): lawful or unlawful; preferred kind of jump
point; 0 to 39 NPC ship definitions (median 8); 1 to 4 voices; up to 12
pilot appearances; name pools as ranges of text identifiers (typically
134 male and 83 female first names, 300 last names, 8 to 13 designation
words, 40 to 60 ship names); three rank titles with two level
thresholds; formation per class; and, for 25 factions, the goods they
scan for, whether they announce the scan, and a scan chance of 0.3.

**Voice properties**: per voice profile, the roles and the variant count
of each segment; the voice files give each segment's duration.
**Piloted solars**: on the object in the system file, a faction, a
loadout, a state graph and one of four pilot grades; about 1,500 objects
have one.

## 5. Connections

**Needs:** from 01, zones, spatial queries, paths, lanes and rings; from
02, ship archetypes, loadouts, hold sizes; from 04, docking, dock
queues, lane travel and interruption; from 05, pilots,
jobs, state graphs, formations, directives; from 07, attitudes (±0.2
between factions, −0.6 for hostility to the player), per-object
reputation, base markets; from 11, the content controller with heartbeat
and timers, the rule that only occupied systems run, and the visibility
radii (2,500 m unowned, 5,000 m other players, 10,000 m group members;
multiplayer only); from 12, record formats, text identifiers, voice
profiles.

**Gives:** to 03 and 05, opponents with level, loadout and pilot; to 04,
lane attacks and NPC dock traffic; to 07, the ships whose destruction
moves reputation, and trader cargo as loot; to 08, the same builders
(missions create ships through the faction ladder, names and formations
of this chapter) and the switches that silence or restrict the
population; to 09, names, ranks, faces and comm messages.

Base inhabitants (people in rooms) come from a separate controller and
belong to chapters 08 and 10.

## 6. Design notes for the builder

**Essential.**

- Population follows players, not zones. This makes the cost independent
  of world size and makes saving NPCs unnecessary.
- Indirection through class and level. Zone → encounter → class group →
  faction → ship definition lets about 40 encounter files and 400 ship
  definitions populate 3,800 zones for 55 factions. Encounters written
  per faction and level would be two orders of magnitude more data.
- Give every group a job with a destination. Traffic that docks,
  launches and rides lanes is what makes stations look like stations.
- Pace combat with a battle cap and a relief time.
- Fill on arrival; trickle afterwards.

**Feel.** Cargo scans, lane attacks, dock chatter, idle talk, named
pilots with ranks, weaker escorts, populating the place ahead. Each can
be dropped singly; together they make the world seem alive.

**Accidents of the era.** Zone roles recognised by nickname; unread
tool fields in zone records; chances that do not sum to one;
segment-spliced speech, which fits thousands of lines on a disc; the
single-player-only "ahead" list and view-cone test, which exist because
single player shows the whole system.

**Simplifications.** One density per system loses the contrast between
hubs and open space. Scaling to the player instead of levels loses the
geography of danger, on which economy and campaign lean (chapters 07,
08). Wandering instead of itineraries loses dock and lane traffic.
"Delete beyond a radius" is an adequate lifetime rule.

## 7. Open questions

1. **Class caps.** The caps in the data name class groups, while the
   reconstructed comparison matches exact tags on ships and ship lines.
   As read, the caps never bind. Whether the original resolved groups
   here, or the caps were dead data, is not established.
2. **Trade through jump points.** What happens to a trader whose target
   is a jump point (removed, or turned back) was inferred from the
   itinerary, not traced through the docking code.
3. **Unread fields.** Zone level, population type, raw faction weights,
   lane identifiers on corridors, "lane down" percentages and a
   difficulty level on solars are in the data and read by no code found.
4. Not mapped: which numbered chatter category is which exchange; a
   ship flag that excludes some NPCs from the density count; how
   factions are fixed for the 60 encounters declared with a faction at
   system level.

## 8. Basis

Design: `design/assessment/04-server-content.md`, sections 3 and 6.

Data, tabulated by script: `outdir/data/MISSIONS/ENCOUNTERS/*.ini`,
`MISSIONS/npcships.ini`, `shipclasses.ini`, `faction_prop.ini`,
`rankdiff.ini` (belongs to generated missions), `voice_properties.ini`,
`pilots_population.ini` (block kinds only), `AUDIO/voices_space_*.ini`,
and the `[zone]`, `[Object]` and `[EncounterParameters]` records of all
files under `UNIVERSE/SYSTEMS/`, two read in full.

Code, in `CppSource/Content/`: `Populators.cpp/.h` (controller, zone
lists, arrival fill), `Encounters.cpp` (population entry, spawn chance),
`EncounterParams.cpp/.h` (definitions, faction pick, vetoes, group
building), `Cargo.cpp/.h` (faction and ship matching, cargo),
`SpawnPlacement.cpp` (arrival methods, leader targets),
`SystemInfo.cpp/.h` (lifetime, lane attack), `Zones.cpp` (zone parsing;
the nickname words are `Tradelane` and `vignette`), `PatrolPaths.h`,
`Guides.h`, `GuidesInternal.h`, `Guides.cpp` (itineraries),
`NpcShip.cpp`, `NpcShips.h`, `NpcCreate.cpp/.h` (piloted solars),
`NpcNames.cpp/.h`, `FactionProps.h`, `Gcs.h`, `Gcs.cpp` (admission rule,
variant pick), `Voices.h`, `RandomEncounters.h`, `RandomMissionGen.h`
(voice roles), `SpaceSupport.cpp` (heartbeat, category intervals),
`MissionScript.cpp` (population switches). Also
`CppSource/Server/server_classes.cpp` (visibility radii) and
`CppSource/Common/common_universe.cpp` (zone keys the shared loader
reads).

Inference: the link between spawn distance and visibility radius; silent
removal; NPCs at jump points; capital ships outside the population; the
authoring-tool reading of unread fields. The scan sequence, the chatter
triggers and the lane-attack spawn rest on the reconstructed headers'
state lists and comments, not on a full reading of those functions.
