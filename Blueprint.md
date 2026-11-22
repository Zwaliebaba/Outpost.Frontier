# Blueprint

A concept blueprint of a space trading and combat game: a data-driven
universe of star systems, a scripted campaign, generated missions, single
player and multiplayer. It was extracted from an existing game's code and
data and describes how that game works, so that a comparable game can be
designed and built from it.

## 1. How to use this blueprint

Read this file first. It gives the whole game on a few pages, the
architecture, a map of the chapters and an order in which to build. Then
read the chapters your current task needs; each is self-contained apart
from the terms in [the glossary](blueprint/00-glossary.md).

What the blueprint is:

- **A basis, not a specification.** It records the concepts, their
  relations, the governing rules, the main formulas and representative
  numbers. It does not record every edge case. Where it is silent, design
  your own answer.
- **Mechanics without a setting.** It names no faction, place, ship or
  character and tells no story. Roles stand in their place ("a major
  house", "an outlaw faction") and example nicknames start with
  `example_`. The setting, the look and the sound are yours to make.
- **Technology neutral.** It describes behaviour. Chapter 11, section 7,
  says what the original was built on and which of its traits are
  accidents of that technology.
- **Honest about its footing.** A number or formula in a chapter was read
  in the original's data or code. "(inferred)" marks what was reasoned
  out, "(per design notes)" what was taken from earlier analysis
  unchecked. Each chapter ends with its open questions and its basis.

Every chapter has the same sections: purpose and player experience,
concepts, how it works, the data that defines it, connections, design
notes for the builder, open questions, basis. The design notes sort each
chapter's content into what is essential, what gives the feel, what is an
accident of the original's era, and what can be simplified at what loss.
Read them before you cut scope.

Units throughout: metres, seconds, metres per second. Money is one
integer currency.

## 2. The game on one page

The player owns one ship and flies it through a universe of about fifty
star systems, earning money by fighting, trading and taking jobs, spending
it on better ships and equipment, and following a campaign that opens the
map step by step.

### Pillars

1. **One ship, one cursor.** Flight is drag-dominated: the ship goes where
   its nose points, reaches top speed in under a second and stops as
   fast. One cursor points, steers and aims; the guns follow the cursor,
   not the nose. This is the game's control identity (chapters 02, 09).
2. **Combat in layers.** A regenerating shield absorbs everything until it
   collapses, then stays down for seconds; in that window hull, parts and
   mounted equipment are hit. Energy limits sustained fire. A fight is:
   break the shield, make the window count (chapter 03).
3. **Speed regimes with prices.** Normal flight (80 m/s) allows fighting.
   A thruster (200 m/s) burns a short reserve. Cruise (300 m/s) takes
   five seconds to engage, forbids weapons and can be knocked out. Lanes
   carry ships at 2,500 m/s between the important places of a system and
   can be broken. Travel and combat are distinct states, and the seams
   between them are where ambushes happen (chapters 02, 04).
4. **Place matters.** Space is divided into zones that say who lives
   there, how dangerous it is and what it does to ship and sensors.
   Danger grows outward from the starting region; prices differ by base
   and never by time, so profit comes from knowing the map (chapters 01,
   06, 07).
5. **Every kill is political.** A few dozen factions hold views of each
   other and of the player. A change in standing with one spills over to
   its friends and enemies. Standing decides who shoots, who lets the
   player dock and who sells what (chapter 07).
6. **A world that seems to exist without the player.** No ambient ship is
   placed by hand. Traffic is generated around players from what zones
   declare, and each group has somewhere to go: a patrol route, a trade
   run, a base to dock at (chapter 06).
7. **Bases are places.** Docking is a ceremony the world can refuse. A
   base is a few staged rooms with people in them; trade, repair, jobs,
   rumours and campaign scenes all happen there (chapters 04, 10).
8. **Authors build the game, not programmers.** Everything that has a name
   is data: systems, ships, goods, factions, missions, scenes, text.
   Missions are triggers over events; scenes are timelines (chapters 08,
   10, 12).

### Core loops

**Seconds: the fight.** Steer with the cursor, lead the target, hold fire
while the power pool lasts, break a shield and press the hull, spend a
repair consumable, break off with the thruster, or run in cruise and hope
nobody disrupts it.

**Minutes: the sortie.** Launch from a base. Plot a route; ride lanes and
cruise. Meet traffic whose regard for the player decides whether it
talks, scans the hold or attacks. Fight or flee; tractor in the loot.
Dock; sell; repair; refit; read the job board; talk in the bar. Launch
again.

**Hours: the climb.** Worth (money plus the value of ship and equipment)
is the single measure of progress. It sets the player's level, which
unlocks better hulls and equipment and harder, better-paid jobs. Better
ships allow more dangerous places, which hold better profit and loot.

**The whole game: the campaign.** Thirteen scripted missions lead the
player across the map. Between missions the game waits until the player's
worth has grown by a set amount, which pushes the player into trading and
jobs. Missions open locked jump points, so the story paces exploration.
After the campaign the universe stays open.

### Modes of play

Single player and multiplayer run the same rules through the same message
catalogue. Single player adds the campaign, pausing and saved games.
Multiplayer adds other players, chat, groups, trade between players and a
character kept on a host; it has no campaign and no save command.

## 3. The player's journey

```text
            ┌────────── films ──────────┐
            ▼                           │
       MAIN MENU ── options             │
        │   │   └── multiplayer: host list ─ account ─ character
   new  │   │ load                            select or create
        ▼   ▼                                       │
        LOADING ◄───────────────────────────────────┘
        │     │
 docked ▼     ▼ in space
      BASE ─ launch scene ─ briefing ─ loading ─► SPACE
        ▲                                          │
        └── landing scene ◄── docking sequence ◄───┤
                                                   │
        jump: docking sequence at a jump point ─ tunnel ─ loading ─► SPACE
        death: single player ─► load a save; multiplayer ─► last BASE
```

| State | What the player does there | Leaves by |
| --- | --- | --- |
| Front end | New game, load, multiplayer, options | Choosing a character or a save |
| Loading | Nothing | Completion: to the base if the character is docked, else to space |
| Base | Moves between rooms; trades, repairs, refits, buys ships; reads news and jobs; talks to inhabitants; watches scenes | Launch (refused without engine, power plant and scanner); loading a save |
| Briefing | Sees the map with the route and the objective | Continue |
| Space: free flight | Flies, fights, scans, loots, talks | Ordering a manoeuvre; dying |
| Space: manoeuvre | Lets the ship fly itself: go to, dock, join formation | Completion, or taking the controls back |
| Space: lane ride | Is carried from ring to ring | Lane end, leaving, a ring shot out |
| Space: docking sequence | Watches the approach | Completion: base, lane, or next system |
| Cutscene | Watches, letterboxed, interface hidden; in space or in a room | Its end, or skipping |
| Pause (single player) | Saves (only while docked), loads, changes options | Resume |
| Death | Watches the ship break up | Single player: a saved game. Multiplayer: the last base, with losses |

Saving follows place, not time: single player saves itself on every
launch and on arrival after a jump; the player saves by hand only while
docked; missions can set checkpoints. Details: chapter 10 (sections 3.7,
3.8) and chapter 11 (3.10, 3.11).

## 4. Logical architecture

```text
PRESENTATION (one per player)
   input, cameras, HUD, rooms and scenes, sound, effects
   simulates its own player's ship; mirrors everything else
        │ up:   commands, own-ship state, hit reports, base requests
        │ down: object creation and state, damage, inventory, missions
TRANSPORT (in-process loopback or network; one message catalogue)
        │
AUTHORITY
   sessions, accounts, characters, saved games
   systems: world objects, dock queues, per-player observers
   base database: rooms, inhabitants, offers, news
        │ events down (synchronous, by priority); service calls up
CONTENT (content controllers on the event bus)
   support, population, campaign, one per running mission
```

**Who decides what.** The authority owns the world: which objects exist,
what NPCs do, all damage, reputation, docking, missions and every saved
record. The presentation owns what the player would feel as lag: its own
ship's movement, its own aim and hit detection, and the arithmetic of
buying and selling. The original pays for that with trust in the
player's machine and an audit afterwards; chapter 11 (section 6) says how
a modern builder should tighten it.

**Shared simulation.** Both sides read the same data and run the same
object, equipment and flight model. A player's ship, an NPC and a station
are one kind of object with different equipment and a different pilot.
The player's autopilot is the NPC manoeuvre layer run on the player's
ship.

**The tick.** The authority aims at thirty ticks a second and passes the
measured time as the step; the step is variable. One tick: drain
messages, step every system that holds a player (ships: authority state,
equipment, pilot; then other objects; deletions; dock queue), update the
base database, run content heartbeats and timers, then replicate to each
observer. A system without a player does not run.

**Content on an event bus.** The authority knows nothing about missions or
population. It announces events (a ship destroyed, a dock requested, a
room entered); content controllers listen in priority order and answer by
calling services. Starting a mission means creating a content controller.

**Replication.** Discrete facts go at once to everyone who has the object.
Motion is sent only when the receiver's extrapolation would be wrong by
more than a tolerance that grows with distance. The player's own ship is
never corrected.

**Data drives content.** The program holds rules; records hold everything
with a name. A manifest lists the record files; records refer to each
other, to assets and to text by name. Chapter 12 describes the model and
proposes a modern form of it.

Read chapter 11 before designing any subsystem; it fixes the seams the
others rely on.

## 5. How the systems feed each other

| System | Takes | Gives |
| --- | --- | --- |
| World (01) | Records of systems, zones, fields | Positions, hazards and sensor interference to combat and HUD; who lives where to population; sites to missions; connections, lanes and system paths to travel; knowledge flags to the map |
| Ships and flight (02) | Archetypes, loadouts; steering demand from player or pilot | One object model to everything; predictable motion to the AI; energy pools and hardpoints to combat; hardpoint classes as the thing money buys |
| Combat (03) | Fire commands, hit reports, zone hazards | Damage and death events to reputation, missions and population; loot to the economy |
| Travel (04) | Dock and route requests; locks from missions; regard from factions | Arrival and departure events; lane traffic and ambush sites to population; the route to the map and HUD |
| AI and pilots (05) | Directives from population, missions and the player's interface; regard for target selection | Steering and fire for every NPC; the player's automatic manoeuvres |
| Population (06) | Zone population entries, faction ship definitions, player positions | NPC groups with itineraries; cargo scans; lane attacks; radio chatter |
| Factions and economy (07) | Kills, job outcomes, bribes, purchases and sales | Regard to AI, docking, markets and offers; worth and level to dealers, job difficulty and the campaign's gates |
| Missions and base services (08) | Events from the bus; inhabitants and rosters per base | Spawns and directives; objectives, comms and scenes; locks and unlocks; pay and standing; checkpoints |
| In-flight interface (09) | Devices; contacts from the scanner; state from the own ship | Commands to flight, combat and travel; brackets, lists, alerts, music and speech to the player |
| Bases, scenes, menus, text (10) | Room lists and offers from the authority; scene scripts; text identifiers | The base visit, the trade screens, conversations, the map, the front end |
| Architecture (11) | Everything | Tick order, event bus, messages, replication, persistence, sessions |
| Data (12) | Authored records and assets | Archetypes and definitions to every system, on both sides |

Three couplings carry most of the design:

- **Regard is read everywhere.** One function gives how one object views
  another, from lasting standings and temporary personal adjustments.
  Target selection, docking permission, market rows, job offers and the
  colour of a contact all read it.
- **Zones are the single spatial vocabulary.** Hazards, traffic, mission
  sites, fields, music and the map all ask the same question of the same
  primitive: which zone is this point in, and how deep.
- **Directives are the one way to tell a ship what to do.** Population,
  missions and the player's manoeuvre buttons all issue directives to a
  ship's manoeuvre layer.

## 6. Chapter map

| Chapter | Scope | Read this when |
| --- | --- | --- |
| [00 Glossary](blueprint/00-glossary.md) | The shared vocabulary | A term is unclear |
| [01 World and universe](blueprint/01-world-and-universe.md) | Systems, zones, hazards, generated asteroid fields and nebulae, connections, discovery | Designing the map, a system, or anything that asks "where" |
| [02 Ships, equipment and flight](blueprint/02-ships-equipment-and-flight.md) | Object model, archetypes, hardpoints, equipment, the flight model, speed regimes | Building the first flyable ship; tuning how flight feels |
| [03 Combat and damage](blueprint/03-combat-and-damage.md) | Firing cycle, munitions, shields, hull, parts, fuses, death, loot, consumables | Building weapons and damage; balancing equipment |
| [04 Travel, docking and navigation](blueprint/04-travel-docking-and-navigation.md) | Manoeuvres, docking and launch, lanes, jumps, routes, locks | Connecting space to bases and systems to each other |
| [05 AI and pilots](blueprint/05-ai-and-pilots.md) | Manoeuvre layer, behaviours, state graph, targeting, fire control, personalities, formations | Building opponents and the autopilot |
| [06 Population and encounters](blueprint/06-population-and-encounters.md) | How zones generate traffic around players, encounters, itineraries, chatter | Making a system feel inhabited |
| [07 Factions, economy and progression](blueprint/07-factions-economy-and-progression.md) | Reputation and its propagation, goods, markets, prices, worth, level, money | Designing factions, trade and the progress curve |
| [08 Missions and base services](blueprint/08-missions-and-base-services.md) | Triggers, conditions and actions, directive lists, the campaign's structure, generated jobs, inhabitants, news | Building the mission language, the job board or the bar |
| [09 In-flight interface and presentation](blueprint/09-in-flight-interface-and-presentation.md) | Commands, mouse flight, HUD, targeting, cameras, frame composition, effects, sound, music, speech | Building controls and HUD; setting requirements for rendering and audio |
| [10 Bases, scenes, menus and text](blueprint/10-bases-scenes-menus-and-text.md) | Rooms, inhabitants, scene scripts, the base visit, front end, map, UI toolkit needs, text | Building bases, cutscenes, menus or localisation |
| [11 Architecture, authority, networking and persistence](blueprint/11-architecture-networking-and-persistence.md) | Authority and presentation, tick, event bus, messages, replication, saves, sessions, multiplayer rules, original technology | First, and again before multiplayer or saving |
| [12 Data model and content census](blueprint/12-data-model-and-content-census.md) | Record files, references, load sequence, asset kinds, counts of all content, a modern data model | Designing the content pipeline; budgeting content |

Each chapter is about 5,000 words.

## 7. Suggested build order

Vertical slices, each playable. Keep the split between authority and
presentation from the first slice, with an in-process transport: it costs
little then and is expensive to add later.

| Slice | Result | Draws on | It works when |
| --- | --- | --- | --- |
| 1. A ship that flies | One ship in empty space, steered by cursor, with throttle, strafe, thruster, engine kill and cruise; chase camera | 02 (3.1 to 3.3, 3.5, 3.7 to 3.10); 09 (3.1 to 3.5, 3.9); 11 (3.1 to 3.4); 12 (3.1, 3.4, the proposal) | Top speed and turn rate come out of thrust and drag as chapter 02 predicts; a ship is a record, not code |
| 2. A place to be | One system with a star, planets, a station, zones and a hazard; dock, a one-room base, launch; save on launch | 01 (3.1 to 3.5); 04 (3.1 to 3.5); 10 (3.1 to 3.3, 3.7); 11 (3.10) | A full loop of launch, fly, dock and resume from a save |
| 3. A fight | Guns, shields, hull, parts, death and loot; one opponent that attacks, evades and flees; targeting and the combat HUD | 03 (3.1 to 3.5, 3.9); 05 (3.1 to 3.8); 09 (3.6 to 3.8) | A duel has the rhythm of shield break and window; the opponent's skill is visible |
| 4. A living system | Factions and regard; zone population with patrols and traders that dock and ride lanes; formations; lanes; comms and music states | 06 (3.2 to 3.8); 07 (3.1 to 3.6); 05 (3.9 to 3.11); 04 (3.6); 09 (3.12 to 3.14) | Shooting one faction's ship changes how two others treat the player |
| 5. An economy | Goods, markets, cargo, dealers, ship purchase, repair; worth and level; tractor and loot sale | 07 (3.8 to 3.15); 10 (3.10); 03 (3.9, 3.10) | A trade route and a fight both raise worth; level opens a better hull |
| 6. A universe | Several systems; jump points, system paths, route planning, locks; asteroid fields, nebulae, exclusions; the map and discovery | 01 (3.6 to 3.10); 04 (3.7 to 3.10); 10 (3.11) | A route across three systems can be plotted and flown; the map fills as the player explores |
| 7. Jobs and the bar | Job board, generated missions, inhabitants, rumours, bribes, news; acted conversations | 08 (3.11 to 3.14); 10 (3.4 to 3.6) | An offer names payer, enemy, site, difficulty and pay drawn from where it is made, and pays on completion |
| 8. A campaign | Triggers, conditions, actions, directive lists, objectives, checkpoints; scene scripts in rooms and in space; worth gates | 08 (3.1 to 3.10); 10 (3.5, 3.6) | An author builds an escort-and-ambush mission from data alone, and a checkpoint resumes it |
| 9. Multiplayer | Network transport, observers and replication, accounts and characters, chat, groups, player trade, host administration | 11 (3.6 to 3.9, 3.11 to 3.13) | Two players meet, fight the same NPCs and trade; rules the original left to the presentation are checked by the authority |

Slices 1 to 3 prove the feel and should be tuned before anything else is
built; chapters 05 and 06 assume the flight model of chapter 02 and lose
their closed-form predictions if it changes character. Slices 4 to 6 can
be reordered. Slice 9 is cheap only if slice 1 kept the split.

Rendering, effects and audio are not a slice. Chapter 09 (3.10 to 3.14)
states what any engine must provide; choose the technology and build to
those requirements alongside the slices.

## 8. Content budget

What the original shipped, from the census in chapter 12 (section 4.3).
The right-hand column says what drives each count, because much of the
volume is systematic.

| Kind | Original | What a comparable game needs to know |
| --- | --- | --- |
| Systems | 53, with a median of 4 neighbours | Authored by hand. 47 form the connected map; 6 are tiny special-purpose spaces |
| Objects per system | Median 47, up to 438 | Mostly lane rings, buoys and scenery; stations and planets are few |
| Zones per system | Median 98 | 70 % carry population. Zones are the main authoring effort of a system |
| Lanes | 124, about 8 rings each | Two per system at the median; 24 systems have none |
| Bases | 197, median 3 per system | 125 are just a bar and a landing deck; 18 have the full five rooms |
| Rooms | 461 | Hotspots per room: median 7 |
| Ship archetypes | 69 | 31 packages the player can buy: 25 fighters, 6 freighters |
| Guns | 513 | Families written out per grade across 10 grades; 266 fit player ships. Price doubles per grade, damage per second grows about 1.35 times |
| Shields, engines, power plants | 123, 60, 32 | Grids of family by class |
| Commodities | 40 | Markets: median 9 rows per base, of which 3 are sold |
| Factions | 55 (30 lawful, 25 unlawful) | A full matrix of mutual reputation |
| NPC ship definitions | 404, on 52 ship archetypes and 52 pilots | Median 8 per faction, levels 1 to 25 |
| Loadouts | 803 | About 20 lines each |
| Encounter definitions | 42 | Shared by all zones; indirection through class and level keeps this small |
| Inhabitants | 1,642, median 8 per base | Assembled from records; conversations from 635 reusable clips |
| Campaign | 13 missions; 2,984 triggers | Median 189 triggers and 57 scripted ships per mission; 4.8 actions per trigger |
| Generated missions | One mission type; an offer tree of 219 nodes | Variety comes from payer, enemy, site and difficulty |
| Scene scripts | 1,506 | 635 room sets and ambience, 635 reusable character scenes, 206 story scenes |
| Voice | 109 profiles; about 24,000 line entries; 6.4 hours | Ambient speech is spliced from fragments, 420 to 680 per pilot voice |
| Text | 12,948 strings, 5,252 information cards | 7,139 of the strings are names |

Hand-made and irreducible: the systems and their zones, the bases'
character, the campaign, the story scenes, the information cards.
Systematic and generable: equipment grids, loadouts, NPC ship
definitions, markets, room sets shared between bases, inhabitants, voice
fragments. A comparable game needs the grids and the generators, not
thousands of hand-written records; chapter 12 (section 6) proposes
templates and inheritance for exactly this.

A smaller game keeps its character if it keeps the ratios: a few bases
per system, a lawful and an unlawful bloc per region, danger rising with
distance, price spread growing with distance, and enough ship and gun
grades for the climb to take hours.

## 9. What can be left out

Each chapter's design notes give the full list. The cuts with the best
ratio of saving to loss:

| Cut | Saves | Loses |
| --- | --- | --- |
| Multiplayer | Network transport, observers, accounts, audit, social features | Nothing in single player. Keep the authority and presentation split anyway |
| A physics library | Integration and contacts can be two formulas and swept simple shapes (02) | Tumbling debris |
| Damage to parts and equipment | Per-part collision shapes, most fuse content (03) | Crippling a ship, stripping a station's turrets |
| Approach flight when docking | Authored approach poses, door animations, docking camera (04) | The sense of arrival; exposure of a slow ship near a base under fire |
| Lanes as real travel | Ring-to-ring simulation (04) | Interdiction, traffic seen in passing, leaving mid-lane. A lane that cannot be broken is a loading screen |
| Most behaviours and pilots | Attack pass, evade, trail, formation, flee and travel carry most of the experience; four tiers of one pilot per role would be hard to tell from the original's 64 (05) | Large-ship combat, some variety |
| Itineraries | NPCs wander instead of travelling (06) | Dock and lane traffic, which makes stations look like stations |
| Empathy between factions | The propagation table (07) | Bloc politics; every kill becomes a private matter |
| Staged rooms | Sets, placements and acted conversations become backdrops and text boxes (10) | The sense of place; none of the rules |
| The offer tree | A flat list of job templates (08) | Little; the original tree has few leaves |
| Fragment-spliced speech | Whole recorded lines, or text (06, 09) | Named, specific chatter at low recording cost |
| Two connection networks | One kind of jump point (01) | The lawful and unlawful geography that factions build on |
| Discovery | The map is known from the start (01, 10) | Exploration as a reward; rumours and news that reveal places |

Do not cut: drag-dominated flight with a cursor-aimed gun sight; the
shield collapse window; the costs of cruise; zones as the spatial
vocabulary; population that follows players; regard read by everything;
worth as the measure of progress; content as data with checked
references.

## 10. What the blueprint does not know

The blueprint understands best the simulation both sides share (flight,
combat, zones, reputation), the data model and the authority's structure.
It understands least the inside of rendering, effects and audio, where
chapter 09 states requirements only, and the binary assets, which the
census counts and does not open. The original's code is a reconstruction
with unnamed fields; where meaning could not be established the chapters
say so. The open questions that matter most to a builder:

1. **Failure in the campaign.** No restart logic for a failed scripted
   mission was found; that the player reloads a save is inferred (08).
2. **Trust.** The authority accepts the player's position, hits, mining
   results and purchases, and does not check a player's fire rate or
   energy. The blueprint documents this and recommends against copying it
   (03, 11); the stricter rules are yours to design.
3. **Cruise disruption for players.** The authority's cancellation was
   found for NPC ships only; how a player's own cruise is knocked out was
   not traced (03).
4. **Death in multiplayer.** What a player's ship drops, and where a
   character resumes after logging out in space, were not established
   (03, 11).
5. **Turn feel.** A turn-sensitivity factor and a per-ship multiplier on
   angular drag default to no effect; what sets them was not traced (02).
6. **Fields and NPCs.** Whether asteroids have collision bodies on the
   authority away from players is unknown; if not, NPCs are kept out of
   fields only by target-selection rules (05). Nebula exterior generation
   was read from parameter names, not code, and three code paths disagree
   about drag inside zones (01).
7. **Dead or dormant data.** Zone class caps appear never to bind as
   read; zone level, population type and several other authored fields
   have no reader; 85 guns are used by nothing found; a commodity field
   has no consumer (01, 03, 06, 07). Treat counts of unused content as
   lower bounds on what was cut, not as features.
8. **Host rules that the reconstruction lost.** The presentation never
   sends a host's password, and the host reads a death-penalty percentage
   and discards it; whether the original behaved so is unknown (11).
9. **Scenes.** How an inhabitant chooses a placement, several script and
   sequence flags including the one that gates skipping, and how text was
   fitted per language are not recovered (10).
10. **AI detail.** The state tables of five behaviours were read in part,
    and several job fields have no visible effect (05).

None of these blocks a build. Each is a decision the builder makes
instead of inheriting.
