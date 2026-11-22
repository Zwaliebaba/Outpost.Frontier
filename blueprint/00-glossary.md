# 00 — Glossary

The shared vocabulary of the blueprint. One concept, one word. Chapters
define their own terms in their "Concepts" section and use these as given.
"Controller" is never used alone: a force controller acts on a body, a
content controller is a rules module of the authority.

## World

| Term | Meaning |
| --- | --- |
| Universe | The whole playable map: all systems and the connections between them. |
| System | One self-contained region of space with its own objects, zones and simulation. The player is in exactly one system at a time. |
| Zone | A named volume inside a system that carries properties: who lives there, what hazards apply, what it looks and sounds like. |
| Exclusion zone | A volume cut out of a field so that a base, lane or route stays clear. |
| Field | An asteroid field or a nebula, generated from a compact definition rather than placed object by object. |
| Solar | A fixed world object placed by a system definition: star, planet, station, lane ring, jump point, depot, wreck, buoy. |
| Base | A place the player can dock at and visit on foot. A base belongs to a solar and consists of rooms. |
| Room | One location inside a base, with a function: landing pad or deck, bar, equipment dealer, commodity trader, ship dealer. |
| Lane | A chain of rings that carries ships quickly between two points of one system. |
| Lane ring | One element of a lane; ships enter and leave at rings. |
| Jump point | A solar that transfers a ship to another system. A regulated one is a gate, an unregulated one a hole. |
| Connection | The pairing of two jump points that links two systems. |
| Solar archetype | The template of a kind of solar: its class, size, toughness and docking points. |
| Immersion | A number from 0 to 1 for how deep a point lies in a zone: 0 outside, 1 in the core, a linear ramp across an outer band. |
| Physical zone | The smallest zone containing an object that has damage or interference. Only its effects apply. |
| Field definition | The compact recipe that fills a zone with asteroids or with a nebula. |
| Field cube | One cell of the lattice an asteroid field is generated on. Its content is a pure function of its position. |
| Dynamic asteroid | A small drifting rock kept around the viewer inside a field; it can be shot and is the source of mined loot. |
| System path | The precomputed shortest sequence of systems between two systems, for one connection kind. |
| Knowledge state | Per player, flags for each system, solar, zone and faction that say what the player has seen and learned. |

## Objects and equipment

| Term | Meaning |
| --- | --- |
| World object | Anything simulated in a system: ship, solar, loot, munition, mine, asteroid. |
| Archetype | The authored template of a kind of object or equipment. Shared and immutable. |
| Instance | One live object created from an archetype, with its own state. |
| Nickname | The text key by which data refers to an archetype, object or other record. Hashed to a numeric identifier. |
| Hardpoint | A named mounting position on a model, with a type that limits what can be mounted. |
| Loadout | A list of equipment and cargo, with the hardpoint each item is mounted on. |
| Equipment | An item mounted on or carried in a ship that has behaviour: engine, power plant, shield, weapon, scanner, tractor and so on. |
| Internal equipment | Equipment with no hardpoint; it cannot be hit separately. |
| External equipment | Equipment on a hardpoint; it can be damaged and destroyed on its own. |
| Cargo | Items carried in the hold. They take volume and have no behaviour while carried. |
| Part | A separately damageable piece of a model's hierarchy. |
| Loot | A world object floating in space that a ship can collect. |
| Simple object | A world object with hit points, an owning player (or none) and a current zone. |
| Equipped object | A simple object that carries equipment, a power pool, parts, docking points and a pilot: a ship or a solar. |
| Body | The rigid body of an instance: mass, inertia, linear and angular velocity, one convex collision shape per part. |
| Hardpoint class | A label on an item and on a hardpoint; an item mounts only where its class is accepted. |
| Sub-object identifier | A small number, unique within one ship, naming the hull, a part or one equipment item. |

## Flight and combat

| Term | Meaning |
| --- | --- |
| Tick | One step of the simulation. |
| Throttle | The player's or pilot's requested fraction of engine thrust. |
| Thruster | Equipment that adds forward thrust for a short time, paid from the thrust pool. |
| Cruise | A slow-to-engage, fast travel mode in which weapons cannot be used. |
| Engine kill | Cutting engine thrust so that the ship coasts while it turns freely. |
| Strafe | Sideways thrust. |
| Power pool | The ship's energy store, filled by the power plant and drawn on by weapons, shield rebuilding and cruise. |
| Thrust pool | A second, separate store that pays for the thruster and for strafing. |
| Force controller | A contributor of force or torque to a body, run once per tick: engine, thruster, steering, strafe, nudge, scripted movers. |
| Axis throttle | The steering demand: a vector whose direction is the wanted rotation axis and whose length (0 to 1) is the fraction of full torque. |
| Nudge | A free thrust in any direction that pilots use for fine positioning. |
| Shield | A regenerating layer that absorbs damage before the hull does. |
| Hull | The hit points of the ship itself. |
| Munition | A projectile in flight, guided or unguided. |
| Countermeasure | A decoy released to mislead guided munitions. |
| Fuse | An authored, timed sequence of effects and destruction steps that plays when a damage threshold is crossed. |
| Repair consumable | A carried item spent to restore hull or shield. |
| Tractor | Equipment that pulls loot into the hold. |

## Characters and factions

| Term | Meaning |
| --- | --- |
| NPC | A ship or character not controlled by a player. |
| Pilot | The authored personality that drives an NPC ship's decisions. |
| Behaviour | One manoeuvre the AI can execute, such as following, evading or docking. |
| Formation | A set of ships that keep fixed stations relative to a leader. |
| Encounter | An authored group of NPC ships with a purpose, spawned together. |
| Population | The ambient NPC traffic that zones generate around players. |
| Faction | A group that owns bases, fields NPCs and holds attitudes towards other factions and the player. |
| Reputation | A number that states how one party regards another. |
| Attitude | The class a reputation falls into: hostile, neutral or friendly. |

## Economy and missions

| Term | Meaning |
| --- | --- |
| Good | Anything that can be bought or sold: commodity, equipment, ship hull, ship package. |
| Commodity | A good that exists only to be traded. |
| Market | The list of goods a base sells and buys, with its prices and conditions. |
| Level | The measure of the player's progress that gates purchases and missions. |
| Campaign | The fixed sequence of scripted missions. |
| Scripted mission | A mission authored as a definition of objects, objectives and triggers. |
| Trigger | A rule in a mission definition: when its conditions hold, its actions run. |
| Generated mission | A mission composed at run time from templates, offered for a reward. |
| Objective | What the player is currently asked to do, shown in the interface and on the map. |
| Job offer | A generated mission as presented to the player before acceptance. |

## Architecture and presentation

| Term | Meaning |
| --- | --- |
| Authority | The part of the program whose state is the truth: it owns the world, the rules and the saved game. |
| Presentation | The part that shows the world to one player and collects input. |
| Session | One player's connection to an authority. |
| Character | A player's persistent identity: ship, equipment, cargo, money, reputation, progress. |
| Account | The identity a session logs in with; it holds several characters. |
| Content | The game rules layered on the authority as a set of content controllers: population, missions, campaign, base inhabitants. |
| Content controller | One content module on the authority's event bus, with a priority, an optional heartbeat and timers. A running mission is one. |
| Event bus | The synchronous channel by which the authority tells content controllers what happened. |
| Transport | What carries messages between presentation and authority: an in-process loopback or a network connection. |
| Observer | The authority's record of one session in one system: which objects that presentation knows and what it extrapolates. |
| Host | The program that runs an authority for remote sessions, with an administration interface. |
| Player group | A small set of players who see each other from further away and share a chat channel. |
| Scene script | An authored timeline that drives cameras, characters, lights and sound in a room or cutscene. |
| HUD | The in-flight interface overlay. |
| Information card | A marked-up text describing an object, good or place. |
| Text identifier | The number by which the game refers to a piece of localised text. |

## Data

| Term | Meaning |
| --- | --- |
| Manifest | The one list, in the game's configuration file, that names the record files of each kind to load at start, in sequence. |
| Record file | A text file of records. One file usually holds one kind of content. |
| Record | One block in a record file: a kind and a list of entries. It defines one thing, such as an archetype, a zone or a trigger. |
| Entry | One line of a record: a key and one or more values. A key may repeat inside a record. |
| Reference | An entry whose value is the nickname of another record, the path of a file, a hardpoint name or a text identifier. |
| Asset | A binary file that a record names by path: model, collision hull, material, animation, effect library, scene script, sound, image, movie. |
| Text catalogue | One table of localised text: plain strings, information cards, or both. |
| Voice profile | A named speaker with the list of lines it can say. |
