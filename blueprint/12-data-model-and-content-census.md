# 12 — Data model and content census

Status: reviewed
Depends on: 00 (glossary). Every other chapter depends on this one for where its records live and how many there are.

## 1. Purpose and player experience

The player never sees this part of the game, and it shapes everything the
player does see. The program holds the rules: how a ship flies, how a
trigger fires, how a price is formed. Everything that has a name comes from
data: each system, base, ship, gun, good, faction, NPC group, mission,
room, effect, voice line and piece of text. A new ship is a record, a
model and a collision hull, and no program change. A new system is one
record file and a line in the universe file.

About 1,250 plain-text record files hold the definitions, about 7,100
binary assets hold the shapes, sounds and timelines, and eight text
catalogues hold the words. One manifest starts the loading. Section 3
explains how the files are found and how they refer to each other.
Section 4 lists each kind of record file and asset and gives the census:
counts and distributions from which a comparable game can be budgeted.
Section 6 proposes a modern form of the same model.

## 2. Concepts

| Term | Meaning |
| --- | --- |
| Manifest | The one list, in the game's configuration file, that names the record files of each kind to load at start, in sequence. |
| Record file | A text file of records. One file usually holds one kind of content (ships, goods, one system, one room). |
| Record | One block in a record file: a kind and a list of entries. It defines one thing, such as an archetype, a zone or a trigger. |
| Entry | One line of a record: a key and one or more values. A key may repeat inside a record. |
| Reference | An entry whose value is the nickname of another record, the path of a file, a hardpoint name or a text identifier. Nickname and text identifier are glossary terms. |
| Text catalogue | One table of localised text: plain strings, information cards, or both. |
| Asset | A binary file that a record names by path: model, collision hull, material or texture library, animation library, effect library, scene script, sound, voice archive, image, movie. |
| Voice profile | A named speaker with the list of lines it can say. Its recordings sit in one archive that carries the profile's nickname. |

How the pieces refer to each other:

```text
configuration file
 ├─ manifest ──► record files by kind (archetypes, goods, markets, factions, effects, sounds …)
 │                └─ universe file ──► one record file per system ──► field definitions, encounter definitions
 │                                └─► one record file per base ──► one per room ──► scene scripts
 ├─ text catalogue list
 └─ the parts of the authority to start (campaign, population, base services) ──► their own record files, by fixed path

record ──nickname──► record        (loadout ► ship archetype, zone ► faction, good ► equipment)
record ──path──────► asset         (archetype ► model; the collision hull is found beside the model)
record ──number────► text catalogue (name, description)
record ──hardpoint─► a named node inside a model
```

## 3. How it works

### 3.1 The record file

All definitions use one text format, a sectioned key-and-value file. A
block opens with its kind in brackets. Each following line is one entry:
a key, an equals sign, and values separated by commas. A semicolon starts
a comment. Values are integers, decimals or bare words; the reader gives
each value the type that the consuming code asks for. An illustration,
with invented values inside the ranges of section 4.3:

```text
[ship]
nickname    = example_light_fighter
name_text   = 131073                        ; a text identifier
model       = ships/example/light_fighter.model
hit_points  = 1300
hold        = 25
hardpoint_class = gun_grade_2, hp_weapon_01, hp_weapon_02
hardpoint_class = shield_light_grade_2, hp_shield_01
```

Four properties of the format matter for anyone who reproduces the model:

1. **Kinds and keys repeat.** A file holds many records of one kind, and a
   record may hold many entries with one key. Lists are written that way:
   one line per equipped item, per market row, per reputation value.
2. **Hierarchy is expressed by adjacency.** The format is flat. A child
   record belongs to the nearest parent record above it. A ship record is
   followed by its parts, a fuse by its timed actions, a base roster (the
   record of who is on a base, chapter 08) by its rooms and characters, a
   voice profile by its lines. The
   sequence of records in a file therefore carries meaning.
3. **Unknown content is skipped.** Readers pass over keys and kinds they
   do not know; some log a warning. Typing errors in data do not stop the
   game; they quietly remove content.
4. **A compiled form exists.** The reader also accepts a binary form of
   the same file: typed values and a shared string table (per design
   notes). None of the 1,256 record files in this data set uses it.

Only two kinds of record can inherit: a light may name a parent light,
and a mission's voice profile may extend a base profile. All other
records are written out in full, which makes some files long and
repetitive.

### 3.2 How a file is found

There are three routes.

**Route 1: the manifest.** One section of the configuration file lists 99
entries under 34 keys. They name 96 record files. The key says what kind
of content the file holds. Several keys repeat, so that one kind is split
over several files:

| Manifest key (neutral name) | Files | Loaded by |
| --- | --- | --- |
| Solar archetypes; asteroid archetypes | 1 + 1 | both sides |
| Star archetypes | 1 | presentation, which opens the file by fixed path |
| Universe (which names systems and bases) | 1 | both |
| Global constants | 1 | both |
| Equipment archetypes | 7 | both |
| Ship archetypes | 2 | both |
| Goods | 5 | both |
| Loadouts | 4 | both |
| Markets | 3 | both |
| Factions and their mutual reputations | 1 | both |
| Fuses | 16 | both |
| Explosions and debris | 1 (named twice) | both; debris by presentation |
| Weapon-type against shield-type modifiers | 1 | both |
| Character body parts; costumes | 1 + 1 | both |
| Set pieces (logical name to model file, for rooms and props) | 1 | both |
| New-character choices; cutscene detail levels | 1 + 1 | both |
| Bases (no file: the key only fixes when base data is built) | 0 | both |
| Sound definitions | 7 | presentation |
| Voice profiles and lines | 18 | presentation |
| Effects, effect types, beams, effect library indexes | 12 | presentation |
| Fonts and text styles | 2 | presentation |
| HUD, intro screens, effect texture shapes, named curves, jump tunnel, jump effect | 6 | presentation |
| List of models whose collision hull is concave | 1 | reader not established |

"Both" means the authority and the presentation read the same file with
the same code. A dedicated host does not load presentation-only content
(inferred). The table of AI state graphs (the tables that choose a
pilot's next behaviour, chapter 05) is loaded right after the manifest,
from a fixed path.

**Route 2: named by other data.** About 1,000 of the record files are
reached from a record already loaded. The universe file names one file
per system and one per base. A base file names one file per room. A
system file names the definition file of each asteroid field and nebula,
and the encounter definitions used by its zones. A ship archetype names
its cockpit definition. A scripted mission names its file of NPC ship
records.

**Route 3: fixed paths.** About 150 files are opened by paths written
into the program. These are the files of Content (the NPC ship table,
pilots, formations, faction properties, loot tables, base rosters, news,
the campaign's mission definitions, the generated-mission tables, the
path tables) and those of
the interface (key map, option lists, map and info-card tables, camera
settings).

The split between routes follows no principle. It reads as history: the
shared object model uses the manifest, and the mission and interface
code have their own readers (inferred).

### 3.3 Load sequence

The manifest is processed top to bottom, and the sequence matters in
several places. The data file itself states two of them in comments:
solar archetypes must load before the universe, because the universe
reader inspects each placed object's type; effect types and beam
definitions must load before the effects that use them. Markets load
after goods and bases, because a market row for an unknown good or with
no current base is dropped. The profiles of recurring voices load before
the mission-specific line files that extend them.

A third comment in the same file states a network rule. Multiplayer
messages carry an archetype as a 16-bit value instead of its 32-bit
identifier, and for that to work both sides must initialise the shared
archetypes in the same sequence. The comment warns that a mismatch gives
unpredictable results.

Loading that depends on sequence is fragile. One example (per design
notes): the systems place stations with a faction entry, but the
universe loads before the faction table, so that reference cannot
resolve at load time. Where placed objects get their faction is an open
question.

### 3.4 Identity and references

- **Nicknames.** Every record that others refer to has one. At load it is
  folded to lower case and hashed to a 32-bit number, with the top bit
  set; objects created at run time get counted numbers with the top bit
  clear, so the two never collide. Faction nicknames use a 16-bit hash,
  in which collisions can occur (all per design notes).
- **Paths.** Assets are named by a path relative to the data root. Some
  paths are derived, not written. The collision hull is the file beside
  the model with the same name and another extension; a sphere model has
  none. A voice profile's archive is the file with the profile's
  nickname in the audio folder.
- **Hardpoints and parts.** Loadouts, fuses and part records name nodes
  inside a model by text. Nothing links them except equal spelling.
- **Text identifiers.** The upper half of the number selects a catalogue,
  the lower half an entry in it. The catalogue number is the position in
  the configuration file's list, after one catalogue that the program
  registers first (per design notes). A record usually carries two
  identifiers: a name and an information card.
- **Effects.** A gameplay record names an effect by nickname. The effect
  record names an effect type, a visual effect, and optionally a sound
  and a light. The visual effect's index record gives the library file,
  a numeric key of the effect inside it, and the textures it needs.

A worked example shows how deep a chain goes. A patrol appears in a zone:

1. The zone record (system file) names encounters by a local name, each
   with a level and a chance, factions with weights, and a density.
2. The system file maps the local name to an encounter definition file.
3. The encounter definition names a ship class and a formation class per
   slot, and a job for the pilots.
4. The ship class table lists member classes. The faction's property
   record lists the NPC ship records that this faction can field.
5. An NPC ship record of the right class and level names a ship
   archetype, a loadout, a pilot and a state graph.
6. The ship archetype names a model, material libraries, a cockpit, an
   explosion and fuses. The collision hull is found beside the model.
7. The loadout names equipment archetypes and the hardpoint for each.
   Each gun names its munition, the munition its hit effect, the effect
   its visual and sound.
8. Name and voice: the faction's property record lists name ranges in
   the text catalogue and voice profiles; the voice profile lists lines.

Ten kinds of record file take part before the first shot, and every
step is a reference by nickname. Chapter 06 gives the choice rules.

### 3.5 Which side needs what

| Data | Authority | Presentation |
| --- | --- | --- |
| Archetypes, loadouts, goods, markets, factions, universe, fuses, constants | yes | yes, the same files |
| Models | hierarchy and hardpoints only (inferred) | everything |
| Collision hulls | yes | yes |
| NPC tables, pilots, encounters, missions, rosters, news, loot tables | yes | no; results arrive as messages |
| Effects, sounds, voices, scene scripts, interface, fonts | no | yes |
| Text catalogues | a small one for its own console | yes |

In single player both sides run in one process and read the shared files
once.

## 4. Data that defines it

### 4.1 Kinds of record file

Counts are records in this data set. Fields are given by meaning, not by
their original keys.

**World** (chapter 01)

| Kind | Records | Principal fields |
| --- | --- | --- |
| Universe | 53 systems, 197 bases, 1 clock | system: file, map position, name, card, map scale; base: system, file, name; seconds per game day |
| System | 53 files; 3,430 objects, 5,428 zones | background, ambient colour, music set, light sources; object: archetype, position, rotation, names, faction, loadout, pilot, base, jump target, neighbouring lane rings; zone: shape, size, position, sort key, population entries, hazard and visual properties |
| Solar archetype | 321, with 238 parts | type, model, radius, mass, hit points, docking points, loadout, fuse, explosion |
| Star, asteroid archetypes | 38; 86 static, 26 dynamic, 9 mines | glow and flare shapes; model, explosion, detection radius |
| Asteroid field definition | 153 | cube size and contents, fill distance, colours, exclusion zones, dynamic asteroids, distant sprites, lootable zones |
| Nebula definition | 59 | fog range and colour, exterior shapes, interior puffs, lightning, exclusion zones |
| Path tables | 3 files, 131 records | the system path for each pair of systems: by any jump point, by gates only, by holes only |

**Objects, equipment, economy** (chapters 02, 03, 07)

| Kind | Records | Principal fields |
| --- | --- | --- |
| Ship archetype | 69, with 246 parts and 157 simple-object archetypes; 46 cutscene stubs | type, model, mass, hit points, hold, drag and torque vectors, hardpoint classes with the hardpoints of each, cockpit, camera values, consumable limits, fuses, explosion |
| Equipment archetype | about 1,800 in 24 kinds | common: names, model, mass, volume, hit points, lootable; then per kind (engine thrust and drag; power capacity and charge; shield capacity, regeneration and type; gun refire, power use, muzzle speed and munition; munition damage, lifetime, seeker; and so on) |
| Loadout | 494 for ships, 309 for solars | archetype; item and hardpoint per line; cargo and count |
| Good | 855 | category (commodity, equipment, hull, ship package), price, item, icon; a package lists a hull and its fitted items |
| Market | 413 base records, 14,694 rows | base; per row: good, minimum level, minimum reputation, stock range, sell-and-buy or buy-only, price multiplier |
| Weapon modifiers | 21 weapon types, 9 shield types each | damage multiplier per pair |
| Explosion, debris | 70 + 86; 41 | effect, radius, damage, impulse, pieces; lifetime, drag |
| Fuse | 209, with about 1,960 timed actions | lifetime; actions at a fraction of it: start effect at hardpoint, destroy group, destroy attachment, ignite another fuse, impulse, destroy root |

**Factions, NPCs, missions** (factions chapter 07; pilots, formations, state graphs 05; NPC ships, encounters 06; the rest 08)

| Kind | Records | Principal fields |
| --- | --- | --- |
| Faction | 55 | names; one reputation value towards each faction (55 by 55) |
| Reputation effects | 55 | change per event kind; share passed on to each other faction |
| Faction properties | 55 | lawful or not, NPC ship records, name ranges, voices, costumes, formation names, cargo scanning |
| NPC ship | 404 shared, 218 mission-local | ship archetype, loadout, pilot, state graph, level, class tags |
| Ship class | 78 | member tags |
| Pilot | 64 ambient, 255 story | references to behaviour blocks of 16 kinds (239 blocks for the ambient pilots) |
| Formation | 93 | slot positions |
| Encounter definition | 42 files | per formation: ship classes with counts, formation class, pilot job, behaviour, arrival methods; size permutations |
| Loot tables | 434 + 124 | per item: the conditions for a drop and the amounts |
| Base roster | 194 bases, 1,642 characters | faction shares on the base, room densities, mission offers; per character: appearance, voice, room, rumours, bribes |
| News | 403 items | text, icon, bases, campaign stage |
| Scripted mission | 14 files | NPCs, ships, formations, solars, loot, objective lists, objectives, dialogues, triggers |
| On-base story scene | 69 files | location, scene scripts for offer, accept, decline; actors and reserved spots |
| Generated-mission tables | 7 files | offer tree (219 nodes), reward by difficulty (23 rows), destroyable solar templates (126), bonus loot |
| AI state graphs | 1 file | 10 graphs over 21 behaviours |

**Presentation and interface** (chapters 09, 10)

| Kind | Records | Principal fields |
| --- | --- | --- |
| Sound | 1,817, of which 962 cutscene speech lines | file, category, attenuation, range, pitch variation, streamed or not |
| Voice | 109 profiles, 23,995 lines | profile: idle animations, roles; line: identifier, duration, priority, attenuation |
| Effect | 705, 45 types, 55 beams, 1,218 library index entries | type (priority, visibility radius, run time), visual, sound, light |
| Base and room | 197 base files, 446 room files | rooms; per room: set script, ambient scripts, sound, camera, placements, hotspots with behaviour and target |
| Costume, body parts | 154; 104 heads, 88 bodies, 12 hands, 106 accessories | parts by nickname; meshes |
| Cockpit | 33 | model, head-turn limits, turret camera |
| Interface tables | 15 files | 146 commands with default keys, option lists, HUD layout, fonts, cursors, text-to-knowledge map (4,752 rows) |
| New character, new game | 1 file + 2 templates | starting ship package, money, base, reputations; the templates have the form of a saved game |

### 4.2 Kinds of asset

| Asset | Files | What the game needs from it |
| --- | --- | --- |
| Compound model | 511 | A tree of named parts joined by fixed, hinged or sliding joints; named hardpoints with position and orientation, hinged ones with axis and limits; several levels of detail (switch distances are in the archetype record); material names; optional built-in animations such as bay doors (per design notes) |
| Single-part model | 1,341 | The same without the tree. Also used for equipment, loot, interface widgets and icons |
| Sphere model | 86 | Planets and stars: radius and materials; collision is the sphere itself (inferred) |
| Collision hull | 798 | Per part of the model: convex pieces, extents, whether the part is fixed, and which hardpoints have a hull of their own (per design notes) |
| Skinned mesh | 204 | Heads, bodies, hands: vertices weighted to bones, face groups by material, detail levels (per design notes) |
| Animation library | 11 | Named clips that drive joints by keyframe channels; scene scripts and records refer to clips by name |
| Material and texture libraries | 107 + 104 | Named materials (type, colours, texture names, flags) and named textures; shared by many models |
| Effect library | 596 | Particle effects as graphs of emitters, force fields and appearances with parameter curves (per design notes) |
| Scene script | 1,506 | A duration, a list of entities (set, prop, character, camera, light, sound, marker, path, particle system, sub-scene) and a list of timed events. Stored compiled; every script is a pure data table without control flow (per design notes) |
| Sound | 1,700 | One recording, uncompressed or compressed, with optional loop points |
| Voice archive | 109 | All recordings of one voice profile, found by line identifier |
| Movie, image | 3 + 27 | Start-up films; splash and loading images |
| Text catalogue | 8 | Strings and information cards by number |

No font files ship; the data names faces that the platform must supply
(per design notes).

### 4.3 Census

All numbers were counted from the data by script. "Median" and "max" are
over the 53 systems unless stated.

**Volume.** The data is 876 MB in 8,385 files: models 313 MB, sound and
voice 228 MB, character meshes and animation 176 MB, textures and
materials 52 MB, films 45 MB, record files 19 MB, scene scripts 17 MB,
collision hulls 7 MB.

**Universe.**

| Measure | Value |
| --- | --- |
| Systems | 53; 47 form one connected map, 5 a separate cluster, 1 stands alone; 6 are tiny (at most 7 objects) and serve special purposes (inferred) |
| Connections | 90 linked pairs, one of them inside a single system; 232 jump points: 86 gates, 140 holes, and 6 doors between a system and the interior of a structure in it (role inferred), which link two pairs |
| Neighbours per system | median 4, max 7; 21 systems have exactly 4, 5 are dead ends |
| Bases | 197; per system median 3, quartiles 2 and 5, max 18; 4 systems have none |
| Objects per system | median 47, quartiles 26 and 72, max 438 |
| Object mix | lane rings 981, buoys and satellites 1,010, scenery 590, stations 220, weapon platforms 168, depots 77, planets 73, stars 53, docking rings 26 |
| Lanes | 124 with 981 rings, about 8 per lane; per system median 2, max 10; 24 systems have none |
| Zones per system | median 98, quartiles 47 and 131, max 414 |
| Zone use | 70% carry population, 56% a patrol-path label, 13% are sites for generated missions, 3% do damage, 1% carry sensor interference |
| Fields per system | asteroid: median 3, max 11; nebula: median 1, max 3 |
| Rooms | 461 room records in the base files, which name 446 room files (some rooms share a file); per base median 2, max 5. 125 bases are a bar and a landing deck; 18 have the full set of five; 16 have only a deck. Hotspots per room: median 7 |

**Ships and equipment.**

| Measure | Value |
| --- | --- |
| Ship archetypes | 69: 36 fighters, 8 freighters, 8 haulers, 4 gunboats, 4 cruisers, 8 capital ships, 1 mining ship |
| Ships the player can buy | 32 hull goods and 31 ship packages. The player buys a package, which names a hull and its fitted items: 25 fighters, 6 freighters. 59 bases sell ships, 1 or 3 packages each |
| Progression of bought ships | hull price 6,000 to 658,000, apart from one hull at 1,000; hit points 1,100 to 12,600; fighter hold 20 to 70, freighter hold 80 to 275; required level 0 to 32 |
| Guns | 513, each with its own munition. 266 fit player ships: 166 energy guns, 81 turrets, 15 guided launchers, 4 heavy launchers. 247 have no hardpoint class and serve NPCs and solars (inferred) |
| Gun grades | 10. These are the gun hardpoint classes only; chapter 02 gives the full table of hardpoint classes. Mountable guns per grade: 16, 13, 18, 27, 35, 52, 40, 33, 14, 18 |
| Gun progression | median price per grade from 500 to about 120,000, roughly doubling each grade; median damage per second from 136 to about 2,150, about 1.35 times per grade |
| Other weapons | 10 mine droppers, 3 countermeasure droppers, 71 seeking munitions |
| Shields | 123, in three families of hardpoint class and three shield types of about 40 each; capacity 450 upwards, median 2,360 |
| Other equipment | 60 engines, 32 power plants, 6 thrusters, 2 scanners, 1 tractor |
| Goods | 855: 752 equipment, 40 commodities, 32 hulls, 31 packages |
| Commodity base price | 2 to 1,200, median 120 |
| Commodity markets | 178 bases; rows per base median 9, max 38; a base sells a median of 3 and only buys the rest; multiplier quartiles 0.67 and 3.0 |
| Equipment markets | 176 bases; rows per base median 64; 366 distinct goods listed |

**Factions and NPCs.**

| Measure | Value |
| --- | --- |
| Factions | 55: 30 lawful, 25 unlawful |
| Initial reputations | 3,025 values from −0.65 to 0.91; 15% below −0.3, 18% above 0.3, the rest in between |
| NPC ship records per faction | median 8, max 39 |
| NPC ship records | 404, drawing on 52 ship archetypes, 151 loadouts and 52 pilots; levels 1 to 25, most between 4 and 13 |
| Loadouts | 803; about 20 equipment lines each |
| Encounter definitions | 42; 1 or 2 formations each; behaviours: patrol 24, trade 18, wander 15. A system uses a median of 26 |
| Base characters | 1,642; per base median 8, max 24; 15,127 rumour references; 643 characters carry a mission entry |

**Missions and scripts.**

| Measure | Value |
| --- | --- |
| Campaign | 13 missions in 14 definition files of 31 to 214 KB |
| Per mission file | triggers 79 to 408 (median 189); scripted ships 21 to 181 (median 57); objectives 17 to 58; objective lists 14 to 119 |
| Campaign totals | 2,984 triggers with on average 1.0 conditions and 4.8 actions; 78 kinds of action and 40 kinds of condition in use |
| Generated missions | one mission type on offer; an offer tree of 219 nodes; 126 destroyable solar templates |
| Scene scripts | 1,506: 635 for rooms and their ambience, 635 reusable character scenes, 206 story scenes, 30 others. Median size 4 KB, largest 170 KB |

**Sound and text.**

| Measure | Value |
| --- | --- |
| Voice profiles | 109 |
| Voice lines | 23,995 entries with 3,268 distinct identifiers. An ambient pilot voice has 420 to 680 fragments. Recorded durations total 6.4 hours for the 17,323 timed fragments; median 1.1 s |
| Campaign speech | 13 in-flight line files of 62 to 175 lines; 962 cutscene lines as single sound records |
| Other sound records | 855, of which 188 music; 108 music files, 568 effect files |
| Text | 12,948 strings and 5,252 cards, about 2.2 million characters; 7,139 of the strings are names |

Three anonymised rows show the shape of a per-item table: bought ships
at the bottom, middle and top of the range.

| Nickname | Type | Hull price | Hit points | Hold | Hardpoint class lines | Level |
| --- | --- | --- | --- | --- | --- | --- |
| `example_starter_fighter` | fighter | 6,000 | 1,100 | 20 | 9 | 0 |
| `example_mid_freighter` | freighter | 53,400 | 3,600 | 175 | 21 | 10 |
| `example_top_fighter` | fighter | 658,000 | 12,600 | 70 | 35 | 32 |

## 5. Connections

- **Gives to every chapter:** where its records live, their principal
  fields and their counts. The meaning of each field belongs to the
  chapter named in the headings of section 4.1.
- **Chapter 11** owns what follows from the load sequence (archetype
  indexes in messages), the form of the saved game and the per-player
  files. This chapter takes from it which component is the authority and
  which the presentation.

## 6. Design notes for the builder

**Essential.** Content is data, and every cross-reference is a name. One
archetype table is shared by authority and presentation. Models carry
named hardpoints and parts that data refers to. Text is separated from
data by identifiers. Without these, content of the size shown in
section 4.3 could not have been authored.

**Feel, not essence.** The exact split into files, the folder layout and
the three routes of section 3.2.

**Accidents of the era.** The flat format with hierarchy by adjacency.
Hashing without collision checks, and a 16-bit hash for factions.
Dependence on load sequence, including for network indexes. Numeric text
identifiers bound to the position of a catalogue in a list. Silent
skipping of unknown keys. Fixed paths in the program. Repetition instead
of inheritance: the 513 guns read as families written out once per
grade (inferred).

**What the counts hide.** Much of the volume is systematic. Guns, shields
and loadouts are grids of family by grade. Most scene scripts are reusable
character scenes and room ambiences. Voice lines are fragments
recorded once per voice. 125 of 197 bases have the same two rooms. A
comparable game needs the grids and the generators, not thousands of
hand-made records.

### Proposal: the same model in a modern, neutral form

1. **One schema per kind of record.** Typed fields with units, defaults
   and ranges. Validate when content is built, and fail on unknown keys.
2. **Nested documents.** Write children inside their parent: ship with
   its parts, fuse with its actions, base with rooms, hotspots
   and roster. Any structured text format will do.
3. **Tables as tables.** The reputation matrix, markets, the weapon and
   shield modifiers and the level curves are tabular. Keep markets
   sparse, with defaults.
4. **Stable textual identifiers with a kind prefix**, such as
   `ship:example_light_fighter`. Derive run-time numbers at build and
   check them for collisions.
5. **Typed, checked references.** A build step resolves every reference
   to a record, asset, hardpoint or text. It reports dangling and unused
   content.
6. **No load sequence.** Read everything, then resolve. Discover files by
   folder convention. Keep a manifest only for packages, layers and
   versions.
7. **Three sets by consumer:** simulation data for both sides,
   presentation data, authoring-only data. Exchange a hash of the
   simulation set when a session starts, instead of relying on equal load
   sequence.
8. **Templates and inheritance for every kind**, so that a family of guns
   is one template and a table of grades.
9. **Text by string key**, one catalogue per language, with a simple
   markup for cards. Keep generated names as part lists.
10. **Assets in open interchange formats.** Models with named nodes for
    hardpoints and parts. Collision hulls generated from the model at
    build, with an authored override. Scene scripts as timeline
    documents, data only. One voice bank per profile with a line list
    that holds duration and priority.
11. **Bake for shipping.** Text is the source; a build produces compact
    packs, as the original reader already allowed.

Points 1 and 5 matter most. With thousands of references by spelling, an
unchecked typing error removes content without a message.

## 7. Open questions

1. Where do placed solars get their faction? The system files name one,
   but the reference does not resolve at load (per design notes).
2. Which records are dead? One fuse file is in the data and not in the
   manifest, and 247 guns have no hardpoint class. How many of those guns
   the NPC and solar loadouts use was not counted.
3. What exactly do the per-system preload lists (ships, equipment,
   sounds, voices) do? They read as loading hints (inferred).
4. The census does not look inside binary assets. Clips per animation
   library, textures per library and polygon counts are unknown.
5. Did the shipped game use the compiled form of record files? This data
   set is all text.
6. Whether the five-system cluster and the lone system are reached by
   mission scripts only was not established.

## 8. Basis

**Design documents.** `design/facts.md` (F1 to F3, F6, F9, F15, F17);
assessment 08 section 8; assessment 02 (identifier hashes, record
formats, collision file); assessment 05 sections 7 and 8 (model, skinned
mesh, effect library); assessment 07 section 1 (scene script form).

**Data.** Every `.ini` file under `outdir/` was tabulated by script
(kinds of section, keys, counts). The census was computed from
`OutpostFrontier.ini` (`[Data]`, `[Resources]`), `UNIVERSE/universe.ini`
and the system, base and room files it leads to, `SOLAR/`, `SHIPS/`,
`EQUIPMENT/`, `initialworld.ini`, `MISSIONS/`, `RANDOMMISSIONS/`,
`AUDIO/*.ini`, `FX/*.ini`, `CHARACTERS/*.ini`, `INTERFACE/*.ini`,
`AI/state_graph.db`, `text/README.md` and the eight `.toml` catalogues,
`newplayer.fl`, `mpnewcharacter.fl`. Asset counts are file counts by
extension (`.cmp`, `.3db`, `.sph`, `.sur`, `.dfm`, `.anm`, `.mat`,
`.txm`, `.ale`, `.thn`, `.wav`, `.utf`).

**Code.** `Common/common_dataconfig.cpp` (manifest dispatch, shared
kinds); `OutpostFrontier/Client/dataconfig_progress.cpp` and
`startup_config.cpp` (presentation-only kinds); `include/common_ini.h`
(reader interface); `Common/common_basedata.cpp` (market row, transaction
type in `include/common_goods.h`); `Common/common_archetype.cpp`
(collision file beside the model); `Client/voice_profiles.cpp` (voice
archive by nickname); `Client/knowledge_map.h`, `infocard_map.h`,
`graph_library.h` (interface tables); string literals of fixed data
paths in `Content/` and `Client/`.

**Inference.** The purpose of the six tiny systems; the grid structure of
guns and shields; sphere collision for sphere models; the role of the
set-piece table and of the preload lists; the historical reason for the
three routes in section 3.2. What the binary assets contain is taken
from the design documents, not from reading the files.
