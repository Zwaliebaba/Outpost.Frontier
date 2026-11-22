# 06 — Population and encounters

Status: draft
Depends on: 01 (zones, lanes), 02 (ships, loadouts), 05 (pilots, formations, directives), 07 (factions, reputation), 11 (authority, content controllers, visibility), 12 (records)

## 1. Purpose and player experience

Space would be empty without this part. No NPC ship is placed by hand in
a system file. Zones only state who lives there and how densely; a
controller on the authority creates ships around each player, gives each
group a job, and removes it again when nobody is near.

The player sees a world that seems to exist without them. Freighters with
escorts leave lanes and dock at stations. Police fly patrol routes, stop,
scan the player's hold and demand contraband. Outlaws lie along the paths
that lead to their bases and sometimes break a lane to stop its traffic.
Near a gate space is busy, in the open it is quiet, in a remote field
only scavengers and their hunters appear. Every ship has a faction, a
name, a face and a voice, and they talk: to dock control, to each other,
to the player.

Three properties matter to the player without ever being explained:

- **Place sets the danger.** Each zone names a level from 1 to 19. The
  same faction fields a weak ship at level 3 and a strong one at level
  15. Travelling outward is the difficulty curve.
- **Reputation sets the tone.** Who appears depends on the zone, not on
  the player; whether they attack depends on the player's standing with
  their faction (chapter 07).
- **Combat is paced.** A fight is capped in size, and after a fight the
  population waits before sending anyone new.

The population is disposable. Nothing about an ambient NPC is saved; a
ship that leaves the player's neighbourhood is deleted within seconds.

## 2. Concepts

Zone, zone role, path segment, lane corridor and population area are
defined in chapter 01; NPC, pilot, formation, encounter, population,
faction and attitude in the glossary. This chapter adds:

| Concept | Definition |
| --- | --- |
| Population entry | The population fields of one zone: density, respawn time, battle size, relief time and a list of encounter lines. |
| Encounter line | One line of a population entry: an encounter definition, a level and a relative chance, followed by the factions that may fly it with their weights. |
| Encounter definition | A file of one or more group templates and a selection rule that picks one of them. Shared by many zones. |
| Group template | One group of ships that spawns together: ship lines, a formation class, a job, the allowed arrival methods and a lifetime. |
| Ship line | A count range, a ship class, an optional level offset, a pilot job and optional extra tags. One line yields one or more identical ships. |
| Class tag | A label on an NPC ship definition: lawful or unlawful, its role (fighter, freighter, transport, armoured transport, gunboat and so on) and every level it serves. |
| Class group | A named set of class tags. Ship lines and caps refer to groups, never to ship archetypes. |
| NPC ship definition | A record that binds a ship archetype, a loadout, a pilot, a combat state graph, a level and class tags (chapter 12 calls it the NPC ship record). |
| Faction properties | Per faction: the NPC ship definitions it can field, name pools, voices, pilot appearances, formation names per class, and its cargo-scanning habits. |
| Arrival method | How a new group enters the scene: out of a lane, launched from a dock, cruising in, or passing close to the player. |
| Itinerary | The standing task of a group's leader, which hands the pilot one directive after another: trade run, path patrol, wander, or none. |
| Lifetime budget | Seconds a ship may exist unobserved. It refills while a player is near and drains faster the farther away the nearest player is. |
| Lane attack | A special spawn that breaks a lane ring ahead of a travelling player and puts hostile ships there. |
| Piloted solar | A station, gate or weapon platform with a faction, a loadout and a pilot. Permanent; not managed by the population loop. |
| Line segment | One recorded piece of speech, identified by name, with several variants per voice profile. |
| Chatter category | A kind of radio exchange with its own minimum interval and probability. |

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
level from the zone; the faction drawn for the group supplies the NPC
ship definition that matches both. One encounter definition therefore
serves every faction and every level.

## 3. How it works

### 3.1 Where it runs

The population is one content controller on the authority (chapter 11)
with a heartbeat of 3 s. It works per player, in the system the player is
in; a system without players has no NPCs because it is not simulated at
all. A second controller, which supports everything in space, runs the
lifetime rule of section 3.8 once per second, the dock queues, and the
radio chatter.

The controller acts on four occasions:

| Occasion | Action |
| --- | --- |
| Heartbeat, for each player in space and not inside a lane | Possibly add one group (3.3) |
| A player launches from a base, or enters a system through a jump point | Fill the neighbourhood at once (3.4) |
| A player enters a lane | Roll for a lane attack (3.9), then fill the neighbourhood of the far end, 1,200 m beyond the last ring |

### 3.2 Which zones speak for a position

Zones overlap freely, so the controller first decides which population
entries apply to a point:

- **At the ship:** every path segment that contains the point, plus the
  smallest other populated zone that contains it. A lane corridor counts
  as size zero, so it always wins that comparison. If any of these has
  density 0, nothing spawns here at all; this is how an author keeps a
  place quiet.
- **Near:** populated zones within 2,000 m that are not in the first
  list, unless they opt out with a flag ("not additive"). They count
  with 0.6 of their density.
- **Ahead** (single player only): such zones inside the view frustum up
  to 4,000 m. They are populated at a point 5,000 m ahead with one third
  of their density, so that the place the player is flying towards is
  already inhabited.

Lane corridors and mission sites are recognised by a fixed word in the
zone's nickname (a lane corridor must also be a box at least ten times
longer than it is wide and high). The other roles are recognised by their
fields.

### 3.3 The steady loop

Let `D` be the largest density among the zones at the ship (a near zone
replaces it when `D` is below 0.6 of that zone's density). Let `n` be the
number of NPC ships within the counting radius of the player: 3,750 m in
single player, 9,200 m in multiplayer. For each zone in turn, with
respawn time `T`:

```text
if n >= D: nothing
chance = clamp((D - n) / T, 0, 1)
if random() < chance: try to spawn one encounter from this zone
stop at the first zone that produced ships
```

So at most one group appears per player per heartbeat, the rate falls as
the neighbourhood fills, and it stops at `D` ships. Because groups have
one to five ships, the count can overshoot `D`.

Worked example. A path segment in open space has density 3 and respawn
time 90. With nobody around the chance is 3/90 per heartbeat: one group
about every 90 s. With two ships present it is 1/90: about every 270 s. A
zone around a gate has density 10 and respawn time 10: the chance is 1
until fewer than ten ships are missing, so the gate refills by one group
every 3 s. A major lane has 6 and 15.

The counting radius is a property of the player's surroundings, not of a
zone. A zone's density is thus "ships near a player who is here", not
"ships in the zone". Zones can be any size without changing the load.

### 3.4 Filling the neighbourhood on arrival

When a player appears somewhere, waiting 90 s for company would show an
empty world. The arrival fill therefore creates groups immediately:

1. The budget is `D` (in multiplayer minus the NPC ships already within
   9,200 m, since another player may have populated the place).
2. Each zone gets a share of the budget proportional to `1 / T`: zones
   that respawn fast get more.
3. Encounters are drawn from each zone until its share is reached (at
   least one ship per zone with a share), without the chance roll.

These groups are placed close, 100 to 1,875 m from the player, and dock
controllers are told not to hail them for 30 s.

### 3.5 Choosing an encounter, and what can veto it

Within a zone an encounter line is drawn by its relative chance; if it
cannot spawn, it is removed from the draw and another is tried. Inside
the definition the selection rule picks a group template by weight. The
faction is drawn by weight from the factions listed under the encounter
line, restricted to those that own an NPC ship definition for every ship
line at this level. A group template may instead tie its faction to
another group's ("hostile to group 0", "same as group 0"); the data ships
only an example of this.

A group is then refused if any of these holds:

| Check | Rule |
| --- | --- |
| Battle size | The player is in combat (a hostile object within 2,500 m) and already has at least the zone's battle size of hostile contacts (typically 4). |
| Relief | The player is not in combat but the last fight ended less than the zone's relief time ago (typically 30 s). This blocks friendly traffic as well. |
| Mission switches | A mission may forbid, per player, factions hostile to the player, or all others; in single player it may forbid hostile factions globally. |
| Exclusion | The player is inside a sphere a mission has declared free of population. |
| Uniqueness | The template forbids a second copy while one is alive, or has reached its creation limit. |
| Caps by class | The zone caps how many ships of a class group may be near (authored: one patrol of each kind near a path, four lawful and four unlawful ships in an area). See open questions. |
| Placement | No arrival method finds a valid position (3.7). |

Battle size and relief are the pacing rules. Without the first, a busy
zone would feed a fight without end; without the second, the next patrol
would arrive while the player is still collecting loot.

### 3.6 Building the ships

For each ship line the number of ships is uniform between its minimum and
maximum; a minimum of 0 makes escorts optional. The line's class group
and the level tag (zone level plus the line's offset, at least 1) select
the candidate NPC ship definitions of the faction; one is chosen
uniformly. From it come the ship archetype, loadout, combat state graph
and pilot. The line's pilot job overrides the job part of the pilot's
personality (chapter 05), so the same pilot record serves as patrol
leader, escort or freighter captain. Wingman lines usually carry a level
offset of −1: leaders are a step better than their escorts.

Each ship also receives:

- a **name** and a private reputation record affiliated to its faction
  (3.11; chapter 07), so that the player's deeds are judged per faction
  but shown per individual;
- a **pilot appearance** and a **voice** drawn from the faction's lists,
  with the gender weighted by the share of male voices;
- the **formation** the faction uses for the template's formation class
  (factions fly the same encounter in different shapes; chapter 05);
- for a trade job, **cargo**: a commodity chosen by weight from what the
  two endpoint bases trade and the faction's own goods list, filling 80 %
  of the hold. Every freighter or transport behind the leader carries the
  same good. What a trader drops when destroyed is therefore what its
  route plausibly carries (chapter 07).

Only the leader gets the itinerary; the others follow in formation.

### 3.7 Placing the group

The template lists its arrival methods; one is tried at random until one
succeeds.

| Method | Where the group appears | Conditions |
| --- | --- | --- |
| Out of a lane | At a ring of a lane within 5,000 m, flying as if it had just left the lane | Not if a ship on that lane is within 15,000 m of the exit or left it under 7 s ago |
| From a dock | Launching from a docking ring, jump point, station or capital ship that the faction may use | Single-ship groups only; small ships from docks within 2,700 m, large ones from 3,500 to 4,000 m |
| Cruising in | Path patrols: on a segment of the zone's path. Others: on a sphere of radius 2,500 to 2,550 m around where the player will be in 5 s | The point must be clear of objects and outside every exclusion sphere |
| Passing close | 2,500 m plus 2.5 s of the player's speed ahead of a moving player, slightly above its course, heading back past it | As above |

No group appears within 2,500 m of another player. The distance is no
accident: 2,500 m is the radius within which the authority shows a
player objects that nobody owns (chapter 11), so ships are created at
the edge of what can be seen (inferred). A system may switch off the
test that keeps spawn points away from solars.

### 3.8 What ambient NPCs do, and how they leave

The itinerary gives the leader its next directive whenever the last one
is done. Fighting is not part of it: when the pilot's own logic engages
or flees (chapter 05), the itinerary waits and then resumes.

**Trade run.** Two dockable objects are chosen that the faction may use
and the ship fits: preferably the far ends of nearby lanes, bases along
the zone's path, or a jump point (standing for a base in the next
system).

| State | On completion | Next |
| --- | --- | --- |
| Travel to target (lanes included) | Arrived within range | Request docking |
| Docking | Docked | Wait 20 to 30 s, then launch, swap the two targets, travel |
| Docking | Not docked (refused, or target was a jump point) | Swap the targets, travel |

**Path patrol.** The leader flies the zone's path point by point between
two objects on it, turning at the ends or looping. Meanwhile a patrol of
a faction that scans cargo looks for ships within 2,500 m: it closes in,
announces the scan (if the faction does so), scans, and if it finds a
good on the faction's list it demands that the cargo be dropped, waits
for the answer, then collects the jettisoned cargo or turns hostile.
Lawful and unlawful factions use the same mechanism; one confiscates,
the other robs.

**Wander.** The leader hops 1,500 to 3,000 m to random points inside its
zone that it can reach in a straight clear line, shortening the hops when
it is boxed in.

**Lifetime.** Each ship has a lifetime `L` from its template (default
24 s) and starts with a budget of `2 L`. Once per second:

| Condition | Effect on the budget |
| --- | --- |
| A player is within 2,500 m; or, in single player, the ship is within 4,000 m inside a 70° cone of the player's view; or the ship is near the lane stretch a player is travelling | Refill to at least `L` |
| Otherwise, nearest player at distance `d` | Subtract `min(15, 1 + (d − 2,500) / 100)` |
| No player in the system | Subtract 15 |
| Budget below 0 | Remove the ship, unless (single player) the player has it targeted. A formation is removed only as a whole. |

A wingman in formation only refills; the leader's clock decides for the
group. At 3,900 m and beyond the budget drains fifteenfold, so a group
is gone 2 to 4 s after the player loses it. Removal is silent (inferred
from the kind of destruction used), and a docked ship is simply gone.
NPCs never travel between systems: one that enters a jump point leaves
play, and one "arriving" through a jump point is created there with the
jump-in effect (inferred).

### 3.9 Lane attacks

When a player enters a lane, the controller may stage an ambush:

1. At most one roll per lane per 180 s.
2. The probability is the largest "lane attack" value of the path
   segments close to the lane's rings whose factions include one hostile
   to the ring's owner (authored 5 to 80 %, median 15 %).
3. A ring ahead of the player, beyond the next one, with no player
   within 5,000 m is chosen by weight; then one such zone and one
   hostile faction.
4. The ring's shield is drained, which breaks the lane there (chapter
   04). One group from the zone's encounter is created 1,400 m beyond
   the ring; its leader waits 25 s and then patrols the zone's path.

The player is thrown out of the lane among ships of a faction that has a
reason to be there. A mission can switch lane attacks off per player.

### 3.10 Difficulty by place

Level is authored per encounter line, from 1 to 19. Each system occupies
a narrow band: in the data the starting systems span 1 to 4, most
systems a band two or three levels wide between 6 and 13, the remote
ones 15 to 19. A faction covers the levels it is met at with a ladder of
NPC ship definitions. A typical fighter ladder has three hulls, two
loadouts per hull and three or four pilot grades per loadout:

| Level | Hull | Loadout | Pilot grade |
| --- | --- | --- | --- |
| 1, 2, 3 | light | first | medium, hard, ace |
| 4, 5, 6 | light | second | medium, hard, ace |
| 7 … 12 | heavy | first, second | medium, hard, ace, twice |
| 13 … 19 | very heavy | first, second | medium to ace, then easy to ace |

A definition can serve a range of levels (a police fighter tagged for
levels 4 to 12), so a faction that stays in one region needs only a few.
Capital ships sit above the ladder at 22 and 25 and never match a zone
level; missions place them.

The ambient population is not scaled to the player. Nothing in the spawn
path reads the player's level, ship or wealth. Generated missions do
scale (chapter 08); the table that maps campaign progress to mission
difficulty is theirs.

### 3.11 Names

A name has two parts, each a text pattern filled with text identifiers
(chapter 12):

- **Group part:** the faction's short name, a word for the job (trade,
  or patrol in a lawful and an unlawful wording), a formation
  designation word and a number from 0 to 19: "‹faction› patrol ‹word›
  7".
- **Individual part:** for small ships a rank title and a first and last
  name from the faction's pools; the rank title is one of three, chosen
  by the ship's level against two thresholds per faction. Wingmen are
  "escort ‹n›" with rank and last name. Large ships are "‹ship type›
  ‹faction's ship prefix› ‹ship name›" from a pool of ship names.

The leader's designation is reused by its wingmen. Names, designations
and ship names are reserved while their owner lives and released 30 s
after it is gone, so two living NPCs do not share a name while the pool
lasts.

### 3.12 Radio chatter

Speech is assembled, not recorded per sentence. A **line** is a speaker,
a listener and a list of line segments: "this is", the faction's short
name, the designation word, the number, "to", the station's name,
"requesting", "permission to dock". Stations, systems and factions each
carry a prefix from which their segment names are formed. For each
segment the voice profile states how many recorded variants it has (1 to
8); one is drawn with a bias to the low numbers. A voice also declares
the roles it can speak (dock controller, docking ship, combat at three
intensities, scanning, idle chatter and so on); a line is not built for
a voice without the role.

A **conversation** is an ordered list of lines between two or more
speakers. The authority knows every segment's duration, plays one line,
waits until it has finished, and plays the next if its speaker still
exists. Conversations start from events:

| Source | Trigger |
| --- | --- |
| Dock control | A ship enters a 2,500 m sensor volume around a dockable solar: hail and identification; then request, clearance, stand-by or refusal as the dock queue decides (chapter 04) |
| Combat | Damage taken or dealt, a death, engaging or declining, fleeing with a reason, calls for help, friendly fire, all clear |
| Cargo scan | Each step of the patrol's scan sequence |
| Idle | Once per tick a formation leader within 2,500 m of a player may start small talk with its wingmen: status queries, rumours, trade talk |
| Player hail | The player hails a ship: it identifies itself or brushes the player off |

Every exchange has a category. A category is refused if the same
category was spoken within 10,000 m less than its interval ago (default
15 s; combat categories 3 to 6 s; idle talk 30 s), and some pass only
with a probability (5 % for one damage category). Without this a
six-ship fight would talk without pause.

### 3.13 Authority and presentation

Everything above runs on the authority. The presentation receives
ordinary ship creations, with name, faction, appearance, voice and rank,
and "comm" messages that name speaker, listener, voice and segments; it
plays the segments back to back and shows the speaker (chapter 09). It
never decides who exists.

## 4. Data that defines it

**Zone population entry** (in the system file; 53 systems, about 3,800
zones with encounters):

| Field | Meaning | Range, typical |
| --- | --- | --- |
| Density | Ships wanted near a player here | 0 to 30; 3 on paths, 5 to 6 in fields and lanes, 9 to 10 at jump points |
| Respawn time | Divisor of the spawn chance | 8 to 90; 90 on paths, 10 to 20 at hubs |
| Battle size | Hostile contacts beyond which nobody joins | 0 to 9; 4 |
| Relief time | Seconds of calm required after a fight | 0 to 50; 30 |
| Lane attack | Percentage for section 3.9 | 5 to 80, on about 780 path segments |
| Class cap | Count, class group | 1 per patrol kind; 4 lawful, 4 unlawful |
| Not additive | Do not contribute as a "near" zone | on 129 zones |
| Encounter line | Definition, level, relative chance | 1 to 10 per zone; three quarters have one |
| Faction line | Faction, weight; follows its encounter line | 1 to 11; usually one |

Path segments also carry the path label, index, use (patrol or trade)
and mission eligibility (chapters 01 and 08). Each system file declares
the encounter definitions its zones use, by local name and file (about
22 per system). The zone files also carry a zone-wide level, a
population type and raw faction weights that match the encounter lines
but are not read by the game; they look like the input of the authoring
tool that produced the lines (inferred).

```text
[zone]  example_sys_path_outlaw1_2          (a cylinder, chapter 01)
  density 5; respawn 30; battle size 4; relief 20
  path label outlaw1, index 2; use trade
  class cap 1 × each patrol kind
  encounter example_path_trader, level 7, chance 1
    faction example_outlaw_grp, weight 1
```

**Encounter definition** (41 files in use; 1 to 2 group templates each):

| Field | Meaning | Values in the data |
| --- | --- | --- |
| Ship line | minimum, maximum, class group, level offset | 1–1 leader; 0–1, 0–2 or 2–3 escorts; offset −1 on most escort lines |
| Pilot job | Job block for the line (chapter 05) | 20 jobs: patrol, assault, defend and escort leaders and members, scout, freighter, transport, trader, smuggler, scavenger, utility |
| Extra tags | Class tags added to the ships | marks patrol leaders for the class caps; marks wanderers |
| Formation class | Looked up in the faction's formations | fighters, freighters, transports, gunboats |
| Job | Itinerary | path patrol (24 templates), trade (18), wander (15) |
| Arrival | Allowed methods | "cruise" (26), "all" (13), "all but lane and jump point" (11), "lane" (6) |
| Lifetime | Seconds | default 24 |
| Simultaneous creation, times to create | Uniqueness | yes, unlimited |
| Selection rule | Template index, weight | weights 1 to 5 |

```text
example_lane_freighter
  group 0: freighter ×1 (job freighter)
           fighter ×0–1 (job escort leader), fighter ×0–2, offset −1 (job escort)
           formation class freighters; job trade; arrival lane
  group 1: freighter ×2, same escorts; formation class freighters, variant 2
  selection: group 0 weight 5, group 1 weight 5
```

The files fall into a small grid: area, path and lane variants of trade
(freighter, transport, armoured, smuggler, prisoner transport), patrol
(lawful, unlawful, bounty hunters), assault, scout, defend, scavenger
and utility ships.

**NPC ship definition** (404 shared): nickname, ship archetype, loadout,
pilot, state graph (fighter, transport, gunboat, cruiser), level, class
tags. Three quarters use the fighter graph.

**Class groups** (about 85): a nickname and member tags; groups may
contain groups. One exists per level and per ship role.

**Faction properties** (55): lawful or unlawful; preferred jump point
kind; 0 to 39 NPC ship definitions (median 8); 1 to 4 voices; up to 12
pilot appearances; name pools as ranges of text identifiers (typically
134 male and 83 female first names, 300 last names, 8 to 13 designation
words, 40 to 60 ship names); three rank titles with two level
thresholds; formation per class; and, for 25 factions, the goods they
scan for, whether they announce the scan, and a scan chance of 0.3.

**Voice properties**: per voice profile the roles and the variant count
of each segment name. The voice files list each recorded segment with
its duration. **Piloted solars**: on the object in the system file, a
faction, a loadout, a state graph and one of four pilot grades; about
1,500 of 3,600 objects have a pilot.

## 5. Connections

**Needs:** from 01, zones with their roles and spatial queries, paths,
lanes and rings; from 02, ship archetypes, loadouts and hold sizes; from
04, docking, dock queues, lane travel and lane disruption; from 05,
pilots, jobs, state graphs, formations and directives; from 07, faction
attitudes (the ±0.2 thresholds between factions, −0.6 for hostility to
the player), per-object reputation and base markets; from 11, the
content controller with heartbeat and timers, the rule that only
occupied systems run, and the visibility radii (2,500 m unowned, 5,000 m
other players, 10,000 m group members, multiplayer only); from 12,
record formats, text identifiers and voice profiles.

**Gives:** to 03 and 05, opponents with a level, a loadout and a pilot;
to 04, lane attacks and NPC dock traffic; to 07, the ships whose
destruction changes reputation, and trader cargo as loot; to 08, the
same builders (generated and scripted missions create their ships
through the faction ladder, names and formations of this chapter) and
the switches by which a mission silences or restricts the population; to
09, names, ranks, faces and comm messages for the HUD.

Base inhabitants (people in rooms) are produced by a separate controller
with its own data and belong to chapters 08 and 10.

## 6. Design notes for the builder

**Essential.**

- Population follows players, not zones. Density means "ships near a
  player standing here". This one decision makes the cost independent of
  world size and makes saving NPCs unnecessary.
- Indirection through class and level. Zone → encounter → class group →
  faction → ship definition lets about 40 encounter files and 400 ship
  definitions populate 3,800 zones for 55 factions. An encounter file
  written per faction and level would be two orders of magnitude more
  data.
- Give every group a job with a destination. Traffic that docks,
  launches and travels lanes is what makes stations look like stations.
- Pace combat with a battle cap and a relief time.
- Fill on arrival; trickle afterwards.

**Feel.** Cargo scans, lane attacks, dock chatter, idle talk, named
pilots with ranks, escorts one level below their leader, populating the
place ahead in single player. Each can be dropped singly; together they
are most of the impression of a living world.

**Accidents of the era.** Recognising zone roles by nickname; the
duplicated, unread tool fields in zone records; chances that do not sum
to one; one global set of mission switches rather than per-session state
in places; segment-spliced speech (chosen to fit thousands of lines on
disc — a builder with speech synthesis or more storage has other
options); the single-player-only "ahead" list and view-cone lifetime
test, which exist because single player shows the whole system.

**Simplifications.** A fixed density per system instead of per zone
loses the contrast between hubs and open space. Dropping levels in
favour of scaling to the player loses the geography of danger, which the
economy and the campaign lean on (chapters 07, 08). Dropping itineraries
in favour of wandering loses dock and lane traffic. A lifetime of "delete
beyond a radius" works; the graded drain only avoids ships vanishing in
view at the boundary.

## 7. Open questions

1. **Class caps.** The caps in the data name class groups, while the
   reconstructed comparison matches exact tags on ships and ship lines.
   As read, the caps never bind. Whether the original resolved groups
   here, or the caps were dead data, is not established.
2. **Trade through jump points.** What exactly happens to a trader whose
   destination is a jump point (removed on docking, or turned back) was
   inferred from the itinerary, not traced through the docking code.
3. **Unread fields.** Zone level, population type, raw faction weights,
   lane identifiers on corridors, "lane down" percentages and a
   difficulty level on solars are in the data and not read by any code
   found. Their intended use is unknown; an authoring tool is assumed.
4. Which numbered chatter category is which exchange, and the exact
   chance of idle talk per tick, were not mapped.
5. A ship flag that excludes some NPCs from the density count was not
   identified (mission ships, presumably).
6. How the faction is fixed for encounters whose factions are declared
   at system level rather than per zone (60 declarations) was not
   followed.

## 8. Basis

Design documents: `design/assessment/04-server-content.md` sections 3
and 6 (module list, data files consumed).

Data, tabulated by script: `outdir/data/MISSIONS/ENCOUNTERS/*.ini` (42
files), `MISSIONS/npcships.ini`, `shipclasses.ini`, `faction_prop.ini`,
`rankdiff.ini` (read; belongs to generated missions),
`voice_properties.ini`, `pilots_population.ini` (block kinds only),
`AUDIO/voices_space_male.ini`, `voices_space_female.ini`,
`SCRIPTS/GCS/gcsexcl.ini`, and the `[zone]`, `[Object]` and
`[EncounterParameters]` records of all files under `UNIVERSE/SYSTEMS/`,
two read in full.

Code, in `CppSource/Content/`: `Populators.cpp/.h` (the controller, zone
lists, arrival fill), `Encounters.cpp` (zone population entry, spawn
chance, table loading), `EncounterParams.cpp/.h` (definitions, faction
pick, vetoes, group building), `Cargo.cpp/.h` (faction and ship
definition matching, cargo), `SpawnPlacement.cpp` (arrival methods,
leader targets), `SystemInfo.cpp/.h` (lifetime, lane attack, zone and
lane records), `Zones.cpp` (zone parsing; the nickname words are
`Tradelane` and `vignette`), `PatrolPaths.h`, `Guides.h`,
`GuidesInternal.h`, `Guides.cpp` (itineraries), `NpcShip.cpp`,
`NpcShips.h`, `NpcCreate.cpp/.h` (piloted solars), `NpcNames.cpp/.h`,
`FactionProps.h`, `Gcs.h`, `Gcs.cpp` (admission rule, variant pick),
`Voices.h`, `RandomEncounters.h`, `RandomMissionGen.h` (voice roles),
`SpaceSupport.cpp` (heartbeat, category intervals), `MissionScript.cpp`
(population switches); `CppSource/Server/server_classes.cpp` (visibility
radii); `CppSource/Common/common_universe.cpp` (which zone keys the
shared loader reads).

Inference: the link between the 2,500 m spawn distance and the
visibility radius; silent removal; the fate of NPCs at jump points; the
authoring-tool explanation of unread fields; the ladder table in 3.10 is
one faction's ladder generalised. The patrol scan sequence and the
chatter triggers rest on the reconstructed headers' state lists and
comments rather than on a full reading of those functions.
