# 00 — Glossary

The shared vocabulary of the blueprint. One concept, one word. Chapters
define their own terms in their "Concepts" section and use these as given.

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

## Flight and combat

| Term | Meaning |
| --- | --- |
| Tick | One step of the simulation. |
| Throttle | The player's or pilot's requested fraction of engine thrust. |
| Thruster | Equipment that adds forward thrust for a short time at an energy cost. |
| Cruise | A slow-to-engage, fast travel mode in which weapons cannot be used. |
| Engine kill | Cutting engine thrust so that the ship coasts while it turns freely. |
| Strafe | Sideways thrust. |
| Power | The ship's energy store, filled by the power plant and drawn on by weapons and thrusters. |
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
| Scene script | An authored timeline that drives cameras, characters, lights and sound in a room or cutscene. |
| HUD | The in-flight interface overlay. |
| Information card | A marked-up text describing an object, good or place. |
| Text identifier | The number by which the game refers to a piece of localised text. |
