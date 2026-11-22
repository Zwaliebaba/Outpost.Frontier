# 01 — World and universe

Status: draft
Depends on: 00 (glossary). Refers forward to 02–12.

## 1. Purpose and player experience

The world is the board the game is played on. It tells every other system
*where* something is, *what kind of place* that is, and *how to get from
here to there*.

The player experiences it as a few dozen separate systems. Each is a wide,
mostly flat arrangement of a star, planets, stations, asteroid fields and
gas clouds, crossed by lanes that carry ships quickly between the important
places, with jump points near the rim that lead to neighbouring systems.
Flight is fully three-dimensional, but nearly everything is placed on one
plane, so a top-down map tells the truth.

Space is not uniform. Flying into an asteroid field surrounds the ship
with rocks that are in the same places on every visit. Flying into a cloud
shortens the view to a kilometre or two, cuts scanner range and may eat the
hull. Flying too close to a star kills. Volumes differ in traffic, music
and danger.

The player's map starts almost empty. Systems appear when entered, objects
and fields when the ship passes near, a base's wares once the player has
landed there. Reading a job offer, a news item or a rumour can also put a
place on the map that the player has never seen. Some things never appear
and must be found by flying. Picking a destination on the map yields a
chain of waypoints through lanes and jump points, across as many systems
as needed.

## 2. Concepts

Glossary terms (universe, system, zone, field, solar, base, lane, jump
point, connection, exclusion zone) are used as defined there. This chapter
adds:

| Concept | Definition |
| --- | --- |
| Solar archetype | The template of a kind of solar: its class (star, planet, station, gate, hole, lane ring, docking ring, weapon platform, depot, wreck, satellite, decoration), size, toughness and docking points. |
| Zone shape | Sphere, ellipsoid, box, cylinder or ring. |
| Zone role | One use a zone is put to, marked by the fields it carries (section 3.4). A zone may hold several. |
| Immersion | A number from 0 to 1 for how deep a point lies in a zone: 0 outside, 1 in the core, a linear ramp across an outer band. |
| Edge fraction | The width of that band as a fraction of the zone's radius. |
| Property set | Flags that classify a zone: field kind, object density, danger density, cloud, exclusion kind, and which effects it carries. |
| Physical zone | The smallest zone containing an object that has damage, interference or drag. Only its effects apply. |
| Field definition | The compact recipe that fills a zone with asteroids or with a nebula. |
| Field cube | One cell of the lattice an asteroid field is generated on. Its content is a pure function of its position. |
| Cube template | The few asteroids, with offsets and rotations, that every non-empty cube of a field repeats. |
| Dynamic asteroid | A small drifting rock kept around the viewer inside a field; it can be shot and is the source of mined loot. |
| System path | The precomputed shortest sequence of systems between two systems, for one connection kind. |
| Knowledge state | Per player, a few flags for each system, solar, zone and faction: seen, details known, and so on. |
| Knowledge table | An authored table that grants knowledge when the player reads a particular text. |

```text
Universe
 ├─ System ×N ── universe-map position, map scale, text ids
 │   ├─ Solar ×n ── archetype, pose; optional: base, jump destination,
 │   │               previous/next ring, parent solar, owner faction
 │   ├─ Zone ×n ── shape, pose, roles
 │   │    └─ referenced by field definitions, exclusion lists,
 │   │       population, mission generation, the map
 │   ├─ Asteroid field ×n ── (field definition, zone)
 │   ├─ Nebula ×n ────────── (field definition, zone)
 │   └─ Lights, ambient colour, backdrop, music set, dust
 └─ Base ×M ── system, name, rooms (chapter 10); shown in space by
               the solar that names it
Path tables ── per pair of systems: by any jump point, by gates, by holes
```

Solars and zones never contain each other. They are tied only by position
and, by convention, by similar nicknames. A station
inside a cloud is three records: the station, an exclusion zone around it,
and a line in the cloud's definition naming that zone.

## 3. How it works

### 3.1 Coordinates, units and scale

- Each system has its own Cartesian frame with the origin at its centre,
  usually at the star. The second axis is "up"; systems are laid out in
  the plane of the first and third axes, and almost every authored
  position has height 0.
- An object's forward direction is its negative third axis. Authored
  rotations are three angles in degrees: pitch about the first axis, yaw
  about the second, roll about the third, combined as roll · yaw · pitch.
- One distance unit behaves as one metre (inferred). Times are seconds.
  One game day is 1,800 seconds of play.

| Thing | Size or distance |
| --- | --- |
| Developed system, centre to outermost object | 80,000 to 100,000 |
| Frontier or pocket system | 15,000 to 50,000 |
| Star | body radius 1,000 to 2,000; damage sphere 5,000 to 11,000 |
| Planet | radius 1,000 to 5,000; kill sphere 75 to 100 beyond the surface |
| Station | radius 250 to 7,000 |
| Gate, hole, docking ring | radius 600 |
| Lane | rings of radius 300 standing 7,500 apart; 4 to 15 rings; length 22,000 to 105,000, median 52,000 |
| Asteroid field or cloud | median 30,000 across; most between 8,000 and 90,000 |

At ordinary speed a system is too big to cross; hence lanes and the fast
travel mode (chapters 02 and 04).

The system map is a fixed 8 × 8 grid. Its half-width in world units is
133,333 divided by the system's *map scale* (1 by default, up to 4 for
small systems). The grid cell also names any location that lies in no
named zone.

### 3.2 Loading and identity

The universe definition is read once at start-up, identically on the
authority and the presentation side. Each system record pulls in that
system's definition at once, and three things stay in memory for the whole
session: every zone of every system, every connection, and each system's
scalar properties. Solars, lights and field definitions are read when a
system becomes active (inferred from the separate loaders).

Records are addressed by the hash of their nickname. Zones live in one
universe-wide table, so zone nicknames must be unique across all systems;
the data prefixes each with its system's nickname.

### 3.3 What a system definition declares

A flat list of records of these kinds:

1. **System properties**: background colour, the faction considered local.
2. **Preload hints** for ships, solars, equipment and sounds that scripted
   events will need.
3. **Encounter references**: the encounter definitions its zones may
   spawn (chapter 06).
4. **Environment**: music set (calm, danger, battle), ambient light,
   backdrop, default dust, light sources.
5. **Fields**: pairs of (field definition, zone), separately for asteroid
   fields and nebulae. One zone may appear in both lists: a cloud with
   rocks in it.
6. **Solars** and 7. **Zones**.

A solar record carries nickname, archetype, position, rotation, name and
information-card text ids, and whichever of these apply:

- *Star appearance*: also marks the object as known from the start.
- *Base and dock target*: the base this solar represents and the base
  that docking leads to.
- *Destination*: target system, target jump point, transit effect. This
  makes it a jump point.
- *Previous and next ring*: makes it a lane ring. End rings also carry the
  name of the place the lane leads to.
- *Parent*: the solar it belongs to (a docking ring to its planet, a gun
  platform to its station).
- *Owner faction, pilot, loadout, difficulty level*: whom it sides with
  and how it defends itself (chapters 03, 05, 07).
- *Atmosphere range, spin, voice, comm appearance, initial knowledge.*

A planet the player can land on is two solars, the planet and a docking
ring that carries the base reference, usually with a lane end nearby.

### 3.4 Zones

| Shape | Size means | Bounding radius |
| --- | --- | --- |
| Sphere | radius | radius |
| Ellipsoid | three semi-axes | largest semi-axis |
| Box | three full edge lengths | half the diagonal |
| Cylinder | radius, length along its own second axis | √(radius² + (length/2)²) |
| Ring | outer radius, inner radius, height | √(outer² + (height/2)²) |

"Smallest zone" always means smallest bounding radius. It is the game's
only rule for resolving overlap, and it makes nesting work: a small
hazardous pocket inside a large calm cloud wins without either zone
knowing of the other.

Four spatial questions are asked of zones, through a spatial index per
system:

- *Is this point inside, with a margin?* An exact test per shape.
- *How immersed is this point?* Let `c` be the point's distance from the
  centre divided by the centre-to-surface distance in that direction.
  Immersion is 0 if `c ≥ 1`, 1 if `c ≤ 1 − edge`, else `(1 − c) / edge`.
  With edge 0 the zone has a hard border.
- *Which is the smallest zone here*, optionally only among physical zones
  or zones with a given property?
- *Which properties hold around this point?* The property sets of all
  zones touching a small sphere are merged, except that among field zones
  only the smallest contributes. A ship is in one field at a time but can
  be in a field, a cloud, a hazard and a patrol corridor at once.

A zone is passive data; everything it does is done by a system that
queries it. The roles found in the data (shares overlap):

| Role | Marked by | Used by | Share |
| --- | --- | --- | --- |
| Path segment | path label and index, usage (patrol, trade); a cylinder of radius 750 | NPC routes are chains of segments; mission generation (06, 08) | 56 % |
| Mission site | site type (open, field, exclusion), lawful or unlawful | Generated missions (08) | 13 % |
| Population area | density, respawn time, battle size, encounters with faction weights | Ambient traffic (06) | 12 % |
| Exclusion | exclusion flag; optional edge fraction and tint | Field generation (3.6–3.8) | 10 % |
| Field or cloud body | field-kind, density and cloud flags; dust, music, tint | Field generation, AI caution, map, presentation | 5 % |
| Hazard | damage rate, interference, drag modifier | Section 3.5 | 4 % |
| Named region | name and information card | Map and location read-out (09) | 3 % |
| Lane corridor | lane number; a long thin box | Lane traffic and lane attacks (06) | 2 % |

Three conventions follow from zones being passive:

- Population code recognises a lane corridor as a box at least ten times
  longer than wide whose nickname contains a fixed word, and a mission
  site by its nickname likewise. Naming is part of the schema.
- A path is the set of cylinders sharing a label, ordered by index. A
  segment's two ends are the ends of the cylinder's axis.
- The AI does not see asteroids ahead of time. It reads the density,
  danger and cloud flags of the zones around a ship and flies more
  carefully there (chapter 05).

### 3.5 Hazards

Each update a ship looks up its physical zone and its immersion `m` there.

- **Damage.** The zone's rate `d` is in hit points per second. The ship
  accumulates `d × m × dt`. Nothing is applied until more than a second
  has passed and the sum exceeds 1 % of the ship's total hit points; then
  it is applied as that fraction of its health (chapter 03). An
  invulnerable ship accumulates nothing. Rates in the data: 5 to 300 in
  hazardous clouds and fields, 10,000 near a star, 2,000,000 in the sphere
  hugging a planet, which is simply a kill volume. The presentation side
  samples the same flag every two seconds to raise a warning.
- **Scanner interference.** With the zone's value `i` (0.4 to 0.8),
  effective interference is `min(1, i × m)` and scanner range is
  multiplied by `1 − interference`. A target in stronger interference than
  the observer is seen only within the range its *own* interference
  allows, so clouds hide what is inside them.
- **Drag.** A drag modifier of 1 or more raises linear drag in proportion
  to immersion. One zone in the data uses it.

### 3.6 Asteroid fields

A field must look dense to the horizon, be identical for everyone on
every visit, and cost nothing to store or transmit. The answer is a
lattice whose cells generate themselves.

A field definition gives a cube size (250 to 500, usually 400), a fill
distance (1,000 to 2,500), an empty-cube frequency (0 to 1), a cube
template (five asteroids on average, each with archetype, an offset in
−1…1 per axis, a rotation and an optional marker "mine" or "instant
kill") and optionally four permitted rotation angles per axis (default 0°,
90°, 180°, 270°).

```text
centre = position snapped to the lattice of pitch cube_size
id     = pack(centre quantised to 128 units)      # 0 if out of range
if id == 0 or immersion of centre in the field zone == 0: no cube
freq   = empty frequency, or the override of an exclusion zone here
if centre is in an exclusion zone without override:       no cube
rng    = generator seeded with id
if rng.next() < freq:                                     empty cube
r      = one permitted angle per axis, chosen by rng
for each template entry:
    place an asteroid at centre + rotate(r, offset × cube_size / 2),
    oriented rotate(r) × entry rotation, id derived from cube id + offset
```

The generator is a fixed linear congruential one, so a position yields the
same cube on every machine. The identifier packs position in 128-unit
steps, so cubes exist only within about ±131,000 units of the origin
horizontally and ±65,000 vertically.

What is drawn and what can be hit are separate populations:

- *Presentation.* Every lattice cell within the fill distance and in view
  is drawn as one prebuilt mesh of the template, rotated per cube, fading
  out over the last half cube.
- *Simulation.* Collidable asteroid objects exist only in the eight cubes
  nearest to each observer and one cube 1.25 cube sizes ahead of it.
  Observers are all ships in the system that are not riding a lane.

| Cube state | Event | Next state |
| --- | --- | --- |
| absent | an observer is in range; cube neither empty nor excluded | active, asteroids created |
| active, near | an observer is still in range | near, timer reset |
| near | no observer for more than 3 seconds | far |
| far | next update | removed, asteroids destroyed |

**Worked example.** Cube size 400, fill distance 1,700, empty frequency
0.6, five asteroids per cube. A sphere of radius 1,700 holds about 320
cells; 40 % are filled, so about 130 cubes or 640 rocks may be drawn.
Around the player at most nine cubes, on average three or four, hold some
18 real asteroids. A field 40,000 across costs the simulation the same as
one 4,000 across.

Three presentation-only layers hide the lattice: flat sprites beyond the
meshes (100 to 1,000, from 500 to 1,200 units out), a textured band that
shows the field from afar, and dynamic asteroids: 5 to 30 rocks kept
within 90 to 500 units of the camera, drifting at up to 50 units per
second and recycled as the camera moves.

**Mines and mining.** A template entry flagged as a mine uses a mine
archetype with a detection radius (35 to 150), an explosion and a recharge
time (1.5 to 3 s); a field of such cubes is a minefield. A field
definition may also declare loot: a commodity, a container, a count range
and a difficulty `D` (1 to 20). When the player's shot destroys a dynamic
asteroid there, loot appears with probability `1 / D`. A loot declaration
may name a sub-zone, so a rich seam can sit inside a poor field.

### 3.7 Nebulae

Gameplay comes entirely from the nebula's zone: interference, damage,
population, and the cloud flag that AI and map read. The definition adds
presentation:

- **Fog**: colour, near and far distance (far is 1,000 to 2,000 typically,
  up to 19,500). Inside, the far distance is the limit of sight.
- **Interior clouds**: 10 to 30 soft sprites within 300 to 1,200 units of
  the camera, drifting, with optional lightning.
- **Exterior**: from outside, the zone is wrapped in 2 to 14 large
  textured pieces of radius 3,000 to 15,000 (inferred from the
  parameters).
- **Light**: ambient colour inside, and how strongly the star shows
  through.

The transition follows the zone's immersion, so the edge fraction decides
whether a cloud has a wall or a haze.

### 3.8 Exclusion zones

A field definition lists the zones cut out of it. An exclusion is an
ordinary zone tested by immersion, so one zone can be cut from an asteroid
field and a nebula at once.

- In an **asteroid field** it suppresses cubes whose centre it contains.
  Options: also suppress distant sprites; also suppress dynamic asteroids;
  or give the volume its own empty-cube frequency instead, which thins or
  thickens the field locally.
- In a **nebula** it must be a sphere centred in the cloud's core. Inside,
  the fog distance is replaced by the exclusion's own (1,500 to 50,000),
  and an optional tinted shell at its surface makes the clearing read as a
  bubble.

Every base, jump point, lane and mission site inside a field has one.
Without them stations would be buried in rock, lanes would run through
asteroids, and anything in a cloud would be invisible until collision.

### 3.9 Connections, lanes and routes

**Connections.** A jump point names its destination system and the jump
point there. Loading collects per system a list of connections with their
kind. Links are authored on both sides; nothing enforces symmetry. Of some
95 linked pairs of systems, a fifth are joined by a gate only, half by a
hole only and a quarter by both. Gates are the regulated network that
lawful traffic uses, holes the unregulated one, and holes reach places
gates do not.

**Path tables.** For each kind (any, gates only, holes only) a table
gives, for every ordered pair of systems, the systems to pass through. It
comes from a breadth-first search over connections not locked for
everyone: fewest jumps, first found wins. The tables ship with the data
and are rebuilt only if missing. The *next hop* from A towards B is the
jump point in A that leads to the second system on the path.

**Lanes.** A lane is a doubly linked chain of rings; riding it is chapter
04. For routing it is a shortcut between its two end rings.

**Routes.** The authority answers route requests. Within one system it
plans over the lanes and returns legs, each "ride this lane from ring X to
ring Y" or "fly free to this point"; a trip under 500 units needs no
route. Across systems it takes the gates-only path and plans, in each
system on it, from the arrival point to the next jump point. Jump points
locked for this player are taken into account; a route is capped at 99
waypoints. The same planner gives NPCs travel orders and missions their
waypoints; missions prefer the gate path and fall back to any path.

### 3.10 The map and discovery

The map draws only what the knowledge state allows. A thing's effective
state is its authored initial flags combined with the player's stored
flags.

| Flag | Meaning |
| --- | --- |
| Seen | Drawn on the map; card readable. For a faction: listed in the reputation screen. |
| Extra text | A second part of a solar's card is shown. |
| Goods, equipment, ships known | A base's card lists what it sells, per kind. |
| Holds loot, looted | On wrecks; a looted wreck does not pay out twice. |
| Never | Cannot be discovered. |

Authoring sets the starting picture: stars and the starting system are
known; a handful of systems, a dozen holes and several hundred zones are
marked "never".

| Event | Effect |
| --- | --- |
| Entering a system | System seen. |
| Each second in flight: a solar within 20,000 whose surface is closer than max(7 × its radius, 10,000) | Solar seen. Wrecks with loot use 7,000. |
| Same scan: inside a zone grown by 7,500 | Zone seen. |
| Same scan: a ship within scanner range | Its faction seen. |
| Landing on a base | Extra text, goods, equipment and ships known. |
| Reading a text listed in the knowledge table | Every entry under that text gains its flags. |

Known solars appear as icons, known field and cloud zones as textured
areas (pattern by field kind, colour from the cloud's tint). Filters show
patrol paths, population areas and an authored class of marked zones.

### 3.11 Authority and presentation

Both sides hold the same universe, zone and field data. The authority
owns collidable cubes around every ship, zone damage, path tables and
route planning, mined loot, and each player's stored knowledge state. The
presentation side owns drawn cubes and all dressing, fog, dust, zone
music, hazard warnings and the map. Because fields are generated from
position alone, no asteroid is ever transmitted. In the original the
presentation side also detects discoveries and rolls the mining chance,
then reports to the authority; see section 6.

## 4. Data that defines it

| Record | Principal fields | Volume and ranges |
| --- | --- | --- |
| Universe: system | nickname, definition, position on a 16 × 16 universe map, name and card ids, map scale, initial knowledge | about 50; 8 never shown, 1 known at start |
| Universe: base | nickname, system, name id, room list, "no automatic save here" | about 200; 1 to 18 per system, median 4 |
| System: solar | section 3.3 | about 3,400; median 31 per system, 3 to 440 |
| System: zone | nickname, shape, pose, size, property set, edge fraction, tint, damage, interference, drag, dust, music, name ids, knowledge flags; plus fields owned by chapters 06 and 08 | about 5,400; median 80 per system, up to 400 |
| Solar archetype | class, model, mass, radius, hit points or indestructible, loadout, docking points, map icon, destruction effects | about 320 over 13 classes |
| Asteroid archetype | model, material; mines add detection radius, explosion, recharge | 86 static, 9 mines, 26 dynamic |
| Asteroid field definition | section 3.6 | about 150 |
| Nebula definition | section 3.7 | about 60 |
| Path table | from, to, systems between | 3 tables, up to 2,100 paths each |
| Knowledge table | text id, nickname, flags | about 4,750 rows |

Solars by class, rounded: lane rings 30 %, buoys and other satellites
24 %, untargetable decoration 17 %, stations 6 %, weapon platforms 5 %,
wrecks 4 %, holes 4 %, depots 3 %, gates 3 %, planets 2 %, stars 2 %,
docking rings 1 %. Zone shapes: cylinder 57 %, sphere 33 %, ellipsoid
6 %, box 4 %, ring 8 zones.

| Example zone | Shape, size | Roles |
| --- | --- | --- |
| `example_sys_rock_field` | ellipsoid 15,000 × 9,000 × 15,000 | rock field, low density, dust, music, named, population area |
| `example_sys_path_patrol1_3` | cylinder radius 750, length 23,000 | segment 3 of path "patrol1", patrol, mission-eligible |
| `example_sys_planet_death` | sphere 2,075 around a planet of radius 2,000 | damage 2,000,000 per second |

The fourteen most developed systems each hold 1 star, 2 to 4 planets, 5
to 14 bases (median 8), 3 to 10 lanes (median 6), 5 to 9 jump points, 2 to
11 asteroid fields, up to 3 nebulae, 54 to 267 path segments (median 103),
20 to 56 mission sites and 4 to 39 exclusions. A frontier system may be
one base, three holes and one field that fills it.

## 5. Connections

Needs: scanners and fast travel (02); how zone damage and mine explosions
are applied (03); which systems are simulated when, and how knowledge
state is stored and sent (11).

Gives:

- 02, 05: axis conventions; zone drag; density and cloud flags.
- 03: hazard zones, minefields, kill volumes.
- 04: jump points, connections, lane ring chains, dockable solars, path
  tables and the route planner's inputs.
- 06: population areas, path segments, lane corridors, per-system
  encounter lists.
- 07: owner faction of solars and bases; mineable fields as income.
- 08: mission sites; system paths for placing and reaching missions.
- 09: zone names and grid cells, hazard flags, zone music, the map.
- 10: the list of bases and the solar that stands for each.
- 12: the record kinds and counts of section 4.

## 6. Design notes for the builder

**Essential.**

- *Zones as the single spatial vocabulary.* One primitive, a posed shape
  with roles, serves hazards, traffic, missions, fields, music and the
  map. Authors compose a place by overlapping zones; systems stay
  independent because each asks only its own question. Keep "smallest zone
  wins" and the immersion ramp: together they give nesting and soft
  borders for free.
- *Fields generated from position.* A seeded lattice gives unlimited
  density with no storage and no synchronisation. Keep the split between
  what is drawn (far) and what collides (very near), and give collidable
  content to every ship, or NPCs will fly through rock.
- *Exclusions.* Fields are usable as places only because holes can be cut
  in them cheaply.
- *Two connection networks* with separate path tables, so that lawful and
  unlawful traffic, missions and players route differently over one map.
- *Knowledge as flags per thing,* gained by proximity, by visiting and by
  reading. Exploration, information and rumours become one mechanism.

**Feel.** The flat layout with free 3D flight; ring spacing, system sizes
and fog distances, which decide how long the player is alone between
events; dust, bands, sprites and interior clouds, which affect nothing but
sell scale and motion.

**Accidents of the era.**

- Roles recognised by substrings of nicknames, and field kinds repeated as
  named flags in field definitions that no code reads (inferred: no reader
  found). Give roles explicit fields.
- One universe-wide zone table, hence globally unique zone nicknames.
- Path tables shipped as data; they are cheap to compute at load.
- Discovery and mining rolls decided on the presentation side. In
  multiplayer that trusts the player's machine; move both to the
  authority.
- Cube identifiers limited to a fixed range of positions.

**Simplifications and their cost.** Spheres and boxes only: lose elongated
fields; paths become point lists. No dynamic asteroids: lose mining and
near-field motion. No nebula exclusions: bases inside clouds become hard
to find. One connection kind: lose the lawful/unlawful geography that
factions and smuggling build on (chapter 07). Full knowledge from the
start: lose exploration as a reward.

## 7. Open questions

1. **Who owns a solar.** System definitions name an owner faction on most
   solars, but the loader does not apply it (per design notes). Where
   stations and platforms get their allegiance was not established.
2. **In-system route choice.** How the planner picks among several lanes
   and how player-specific locks alter a route was read only at the level
   of inputs and outputs. Chapter 04 should settle it.
3. **Nebula exterior.** The placement of the exterior pieces was taken
   from parameter names and ranges, not from code.
4. **Drag in zones.** Three code paths combine the modifier with drag
   differently; with one use in the data the intent is unknown.
5. **Local faction of a system** and the **game day**: both are loaded;
   their consumers were not traced.
6. **Marked zones.** About 90 zones carry a flag the map can filter on;
   what the class means to the player is unknown. Ring zones are used
   eight times, for planetary rings; a mesh shape is declared but its
   containment test always fails.
7. **Field-kind flags.** Which property flag means which field kind was
   matched by counts against field definitions and by the map's pattern
   choice (inferred); the density levels were read in the AI code.

## 8. Basis

Design: `design/assessment/02-simulation-core.md` sections 1 and 3;
`design/facts.md` F17 (open question 1); `design/sim-records.md`
(asteroid records); `design/rebrand-names.md`.

Data, tabulated by script: `UNIVERSE/universe.ini`; all 58 files under
`UNIVERSE/SYSTEMS/*/` (a pocket system and a mid-sized one read in full);
the three shortest-path files and `missioncreatedsolars.ini`;
`SOLAR/solararch.ini`, `stararch.ini`, `asteroidarch.ini`; all of
`SOLAR/ASTEROIDS/` and `SOLAR/NEBULA/` (one of each read in full);
`INTERFACE/knowledgemap.ini`. One system file duplicates another, so
counts are rounded.

Code: `Common/common_universe.cpp`, `common_universe_system.cpp`,
`include/common_universe.h`; `common_zone.cpp`; `common_exclusionzone.cpp`;
`common_casteroid.cpp`, `common_casteroidfield.cpp`; `common_csys.cpp`;
`common_csolar.cpp`; `common_csimple.cpp`, `common_cship.cpp`,
`common_cescanner.cpp`, `Server/server_gameobject.cpp` (physical zone,
drag, interference, damage); `Content/SystemConnections.cpp`, `Zones.cpp`,
`SpacePaths.h`, `GameSupport.cpp`, `RandomMissionGen.cpp` (path tables,
roles, routes, grid cell); `Client/cli_asteroid.cpp`,
`cli_asteroid_dynamic.cpp`, `cli_nebula.cpp` (keys only);
`Client/space_camera_game.cpp`, `character_select_equip.cpp`,
`knowledge_map.cpp`, `nav_bar.cpp`, `menu_universe_base.cpp`,
`nn_navmap_build.cpp`, `nn_navmap_draw.cpp`, `nn_navmap_input.cpp`,
`hud_target.cpp`, `hud_targetable_objects.cpp` (discovery, map, hazard
warning); `Server/server_classes.cpp` (stored knowledge, wrecks).

Inferred, not read: the metre; when solars are loaded; the nebula
exterior; the flag-to-field-kind mapping; that named flags in field
definitions are unused; the meaning of the marked zone class; the average
cube occupancy in the worked example (arithmetic).
