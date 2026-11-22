# 11 — Architecture, authority, networking and persistence

Status: draft
Depends on: 00 (glossary). Read with 02, 03, 06, 08, 10 and 12.

## 1. Purpose and player experience

This chapter is the skeleton the others hang on: which part of the
program owns the truth, the order of work in a tick, what travels between
the parts, and what survives between sessions. The player sees only its
consequences:

- **One game, two ways to play.** Single player and multiplayer run the
  same world rules. Single player adds the campaign, pausing and saved
  games; multiplayer adds other players, chat, groups, trade between
  players and a character kept on somebody's host.
- **A responsive ship.** The player's own ship answers input in the same
  frame, in both modes, and is never corrected by the network.
- **A world alive only near players.** Systems without a player do not
  run.
- **Saving as a place, not a moment.** Single player saves itself on every
  launch and jump; the player can save by hand only while docked.
  Multiplayer has no save command; the character is simply kept.
- **A host anyone can run:** one program with a small administration
  window.

## 2. Concepts

| Concept | Definition |
| --- | --- |
| Authority | Owns the universe, its objects, the rules and every saved file ("server" in the original). |
| Presentation | Shows one player the world, collects input, runs sound and interface ("client"). |
| Content | The game rules layered on the authority as a set of controllers: population, missions, campaign, base inhabitants. |
| Controller | One content module on the authority's event bus, with a priority, an optional heartbeat and timers. A running mission is a controller. |
| Event bus | The synchronous channel by which the authority tells controllers what happened. They answer by calling its service functions. |
| Transport | What carries messages between presentation and authority: an in-process loopback or a network connection. |
| Session | One player's connection, numbered from 1 to the host's player limit. |
| Account | The identity a session logs in with; holds up to five characters. |
| Character | One persistent pilot, one text file. A single-player saved game is the same thing. |
| Observer | The authority's record of one session in one system: which objects that presentation knows, and what it is assumed to extrapolate for each. |
| Host | The program that runs an authority for remote sessions, with an administration interface. |
| Player group | Up to eight players who see each other from further away and share a chat channel. |

```
PRESENTATION (one per player)
   up:   commands, own-ship state, hit reports, base requests, chat
   down: object creation and state, damage, inventory, missions, base lists
TRANSPORT (loopback or network)
AUTHORITY
   sessions, accounts, characters, save files
   systems: objects, dock queues, observers
   base database: rooms, inhabitants, offers, news
   events down (synchronous, by priority), service calls up
CONTENT
   support controllers, population, campaign, one controller per mission
```

Both sides read the same data and run the same simulation code (objects,
equipment, physics).

Presentation and authority never call each other. They exchange messages
from one catalogue, the same for both transports.

## 3. How it works

### 3.1 Who is authoritative for what

| Subject | Authority | Presentation |
| --- | --- | --- |
| Existence of objects (NPC ships, solars, loot, mines, guided munitions, countermeasures) | Creates and destroys | Mirrors; builds the local player's ship on instruction |
| NPC decisions and movement | Owns | Extrapolates between updates |
| The player's own ship movement | Accepts the reported state, relays it | Owns: integrates input and physics |
| Hits by the player's shots; the player's collisions | Applies damage | Detects and reports |
| NPC shots and collisions (multiplayer) | Detects and applies | Shows the result |
| Hull, shield, part and equipment damage; destruction; loot | Owns | Plays the effects |
| Inventory, money, hull type | Stores; in multiplayer accepts changes only while docked, then audits | Computes each purchase and sale, requests the change |
| Prices and stock | Static shared data; not simulated | Reads the same data |
| Reputation; docking permission, dock queues, lanes, jumps | Owns | Requests; plays sequences and reports completion |
| Missions, objectives, comm, music cues, cutscene triggers | Owns (content) | Plays and reports completion |
| Base inhabitants, job offers, news | Owns (content fills a per-base database) | Receives lists on entering a room |
| Saved files | Writes and reads | Never touches them |
| Weapon groups, visit flags, mission log, interface flags | Stores verbatim | Owns the meaning |

The pattern: what the player would feel as lag is decided where the player
sits (own movement, own aim, buying and selling); what another player must
agree with is decided by the authority. The price is trust (3.8).

### 3.2 The authority's tick

The host loop aims at 30 ticks per second (adjustable 1 to 60, per design
notes), measures real elapsed time, sleeps the rest of the period and
passes the measured time as the step. In single player the presentation's
frame calls the same two entry points with its frame time.

One tick, in order:

1. **Drain the transport.** Each arrived message is handled to completion.
2. **Advance the world clock** by wall-clock time.
3. **Step systems.** Walk the active sessions; step each distinct system
   that holds one. A system without a player is not stepped.
4. **Late space events:** volume enter and leave events collected by the
   step; ships that left through a jump point (inferred).
5. **Base database** update.
6. **Heartbeats.** Destroy controllers queued for destruction, then give
   each controller its accumulated time in whole multiples of its interval.
7. **Timers.** Fire at most 128 expired one-shot timers, each to the
   controller that set it.

One system step. In multiplayer a step longer than 0.2 s is cut into 0.2 s
sub-steps; single player takes one.

| # | Sub-step |
| --- | --- |
| 1 | Update ships: authority-side state (death sequence, dock requests), then pilot behaviour, equipment, power, shield. |
| 2 | Update a second list, probably solars (inferred); then a ship-proximity pass. |
| 3 | Update loot, unguided projectiles and two further lists, probably guided munitions, mines and countermeasures (inferred). |
| 4 | Update asteroid fields, then an always-update list. |
| 5 | Delete objects destroyed during this step. Destruction only marks; deletion waits until here. |
| 6 | Process the dock queue. For each finished docking: tell every observer, raise the event; remove an NPC ship; take a player's session out of the system. |
| 7 | Multiplayer only: move projectiles, step physics, resolve collisions. |

After the sub-steps: the replication pass for each observer (3.7).

An object appended to a list during its walk is updated in the same walk;
content relies on that when it spawns ships from inside an event.

### 3.3 The presentation's frame

Measure the frame time (capped at 1 s; frame rate capped, default 200 per
second); read input; update the interface; drain the transport and, in
single player, run the authority's tick; update the current state (space,
base, menus, cutscene), which draws its own picture.

The space state, in order: objects, animations, physics, projectiles,
collisions (reported to the authority), docking sequences and camera,
audio, scene, HUD, removal of dead objects, the own-ship report, a pending
system switch. (Subsection per design notes.)

### 3.4 Time

- **The step is variable.** There is no fixed simulation rate.
- **Physics has its own clamp.** Single player advances physics once per
  frame, by at most 0.0999 s. Multiplayer accumulates time on both sides
  and advances in 0.0999 s slices plus the remainder. The solver inside
  works at 16 steps per second.
- **Pause** exists only in single player: it zeroes the frame time and
  skips the authority's tick (per design notes).
- Three clocks coexist: the step, a saved wall-clock world clock, and
  content's clock fed by heartbeats (per design notes). Have one.

### 3.5 The event bus

The authority knows nothing about missions or population. It announces
events; content listens.

- **Registration.** Controllers are created from a configuration list at
  start-up and later by other controllers; starting a mission means
  creating its controller. Each has a priority.
- **Order.** A broadcast visits controllers by priority, newest first
  among equals. Shipped order: game support (each player's content record),
  running missions, the campaign, base support, space support (docking,
  lanes, NPC bookkeeping, loot), base population, space population.
  Content depends on it: missions see a destroyed NPC before space
  support deletes its record (per design notes).
- **Synchronous and nested.** A handler finishes before the sender
  continues, and may change the world and raise further events.
- **Stable list.** A controller created during a broadcast joins when the
  outermost walk ends; a destroyed one leaves at the next heartbeat pass.
- **Some events are questions:** the receiver edits the payload, for
  instance which loot a destroyed ship drops.

About 60 events come from the authority and 18 from content:

| Purpose | Events |
| --- | --- |
| Session and location | login, logout; enter and exit of system, base, room and space; launch and jump-in complete; system loaded and unloaded |
| Flight services | dock request, cancel, complete; lane request, release, enter, exit; formation request and leave |
| Combat and objects | object damaged, death fuse lit, object destroyed, equipment dropped, loot spawned and acquired, cargo jettisoned and scanned, volume entered and left |
| Economy and base | good bought, sold, destroyed; base object selected; job offer answered; worth changed; hull changed |
| Dialogue and interface | hail, comm complete, cutscene done, interface item used, dialog answered, path and statistics requested, mission aborted |
| Housekeeping | heartbeat, timer, controller destroyed, player save and load, world save and load |
| Content to content | mission start, combat state, NPC turned hostile, flee request, reputation damage, formation leader changed, encounter launched, NPC ship created, NPC enters or leaves a system |

### 3.6 Messages

The catalogue has 162 kinds: 21 travel both ways, 81 from authority to
presentation, 60 the other way (per design notes).

**Both ways.** Whoever caused the fact sends it; the authority relays a
presentation's message to every observer that has the object.

| Purpose | Messages |
| --- | --- |
| Object state | position, orientation, timestamp, throttle, a one-byte flight state |
| Controls | fire weapons (hardpoints, aim point), activate equipment, cruise, thruster, set target, tractor, enter and leave lane, jettison |
| Opaque state | weapon groups, visit flags, mission log, interface state |
| Queries | best path, player statistics, rank, group positions |
| Social | chat; player trade: request, open, set money, add and remove item, accept, close |

**Presentation to authority.**

| Purpose | Messages |
| --- | --- |
| Session | login; list, create, select, delete character; skip next autosave |
| Docked changes | set hull type, hull status, part status, equipment list; add, remove, modify item; set and change money; good bought, sold, destroyed (for content) |
| Changing place | enter and leave base and room; request base and room lists; request a ship identifier; launch; launch, jump-out and jump-in complete |
| Space reports | munition hit, collision, field-hazard collision, asteroid mined, use consumable, docking invincibility, scan cargo |
| Requests | dock, formation and similar, and their cancellation; hail; set manoeuvre |
| Mission and scene | job offer answer, abort mission, cutscene done, comm finished, interface item used, dialog answered, checkpoint acknowledged |
| Single player only | save game, difficulty scale (0 to 2) |

**Authority to presentation.**

| Purpose | Messages |
| --- | --- |
| Session | session number, login result, character list, character verified with world time |
| Player state | hull type and status, part status, equipment, cargo, item added and removed, money, kill and mission counts, reputation, faction feelings |
| World objects | create ship, solar, loot, mine, guided munition, countermeasure; destroy; activate; damage list; repair; burn fuse; cargo update; guided target |
| Changing place | launch (where, from what), land, system switch out and in, start room, reply to a dock or formation request |
| Base lists | per room or base: update, remove and "list complete" for inhabitants, their behaviours, ambient scenes, job offers, news |
| Missions and scenes | mission message, objectives, mark object, comm line, scripted scene, camera, story cue, music, sound, dialog, checkpoint request, directive for the player's ship |
| Social | player list; other players' rank, group, system, affiliation; message of the day; scan result |

A list is a stream of items closed by "complete", so the receiver knows
when a room is fully described.

Two sequences show how the groups combine.

*Launch.* Leave base (autosave), request a ship identifier, launch. The
authority adds the session to the system, sends "launch" with the launch
point of the base's docking solar, creates the ship from the stored hull
status and equipment, invincible while undocking, and raises "space
enter". The presentation plays the undocking and reports "launch
complete".

*Jump.* Docking at a jump point completes; the authority sends "system
switch out"; the presentation plays the tunnel and reports "jump-out
complete". The authority removes the ship from the old system, moves the
session, asks the destination jump point for an arrival position, re-adds
the ship, raises "system enter", autosaves and sends "system switch in".
The presentation loads the system and reports "jump-in complete".

### 3.7 How object state reaches other players

After its system's step each observer runs five phases. Before each phase
and each item the authority checks the session's **link saturation**,
derived from the bytes still queued for it; at 0.9 it stops for this tick
(about 1,800 queued bytes, per design notes). Skipped work is
reconsidered next tick, so a slow link gets fewer, later updates and never
a growing queue.

1. **Advance** the extrapolation model of every object already sent.
2. **Create.** For each unsent, visible object: send its creation message
   with everything needed to build it.
3. **Destroy and check.** For each sent object: if no longer visible, send
   "destroy". Otherwise, if no player owns it, compare truth with the
   model and queue it if they disagree.
4. **Update.** Send queued objects, longest-waiting first.
5. **Deferred damage** lists.

**Visibility** is a sphere around the observer's ship: 2,500 m for
objects no player owns, 5,000 m for another player's, 10,000 m for a
group member's; the observer's own always.

**Fan-out.** Discrete facts about an object (fire, equipment, cruise,
target, fuse, comm, cargo, lane use) go at once to every observer that
has it.

**The extrapolation model**, run identically by sender and receiver:

- It fits velocity and acceleration through the last three position
  samples. If two samples coincide, or the path folds back by more than
  90°, it uses constant velocity from the last two.
- Orientation is extrapolated the same way, as two unit axes.
- A new sample does not snap the object: the shown track converges on the
  new prediction over the time between the last two samples (at most
  0.25 s in the constant-velocity case).
- The receiver steers the physical body towards the predicted pose with a
  capped correction per step.

**When to send.** The owner of the truth keeps, per receiver, a copy of
the model fed only with what it sent. It sends a sample when

```
position error    > 2.0 m × scale, or
either axis error > 0.5 × scale     (distance between unit vectors)
scale = clamp(distance to observer × 0.002, 1, 10)
```

or when the time since the last sample exceeds a back-off that starts at
5 s and doubles each time it expires; a forced send resets it.

*Worked example.* An NPC flies a steady curve 1,800 m from a player.
Scale is 3.6, so 7.2 m of position error is tolerated; the axis tolerance,
1.8, is near the possible maximum of 2, so orientation alone hardly ever
triggers. While the fit holds, only the refreshes at 5, 10, 20 s are
sent. When the NPC starts combat manoeuvres the error soon passes 7.2 m
and samples flow as fast as the saturation gate allows. At 200 m the same
NPC is held within 2 m.

**Players' ships** go the other way. Each presentation runs the check on
its own ship with scale 1 and reports when it trips. The authority
applies the report to its copy and relays it to the other observers;
nothing returns to the owner. Its only test: a position more than 750 km
from the system's origin destroys the ship.

**What the receiver predicts:** motion only. Unguided shots are not
replicated as objects; each side flies its own from the fire message
(inferred). Damage, destruction, loot and reputation are never predicted.

### 3.8 Hits, trust and checks

- A presentation reports a munition hit only for its own player's shots
  and a collision only for its own player's objects. A hit report names
  the munition archetype, shooter, target, part and hit point; the
  authority takes the damage from the archetype and applies chapter 03. A
  collision report carries a damage value the reporter computed from
  relative speed and both masses.
- The authority's own collision pass finds NPC hits and collisions.
- With player-versus-player off, the authority drops any hit or collision
  whose source and target are both players.
- **Worth audit** (multiplayer). On entering a base the authority values
  all the character owns at that base's prices and remembers the total;
  income is added to it. Before every save it refuses, logs and announces
  a character whose equipment exceeds what the hull can mount, who gained
  items while docked that the base does not sell, whose holdings are
  worth more than the remembered total, or whose hold is overfull.
  Valuation: stackable items at full price, other equipment at 0.3, the
  hull at 0.8.
- Money is clamped to 0 … 999,999,999.

### 3.9 Single player against multiplayer

Single player is the same authority hosted in the player's process, with
the loopback as transport: messages are queued as typed values and drained
by the same pumps.

| Aspect | Single player | Multiplayer |
| --- | --- | --- |
| Content | Support, population, campaign | Support, population |
| Physics and collision detection | Presentation, for every object | Each side for its own (3.8) |
| Visibility | The whole system, sent at once | By range, gated by saturation |
| Systems | Unloaded on leaving or landing | Kept loaded until shutdown |
| Inventory requests | Accepted anywhere | Only while docked; audited |
| Saving | Manual, autosave, checkpoints | Automatic only |
| World clock, faction-to-faction feelings | In the saved game | From the initial-world data |
| Pause, difficulty scale | Yes | No |
| Death | Load a saved game (inferred) | Back to the last base with a loss (3.12) |

In the original the two sides also share one set of objects in single
player. That is an optimisation, not a concept (section 7).

### 3.10 Persistence

A character and a saved game are the same text record:

| Part | Content |
| --- | --- |
| Header | Description, time stamp |
| Identity | Name, rank, appearance, voice, affiliation |
| Standing | One reputation per faction (55 shipped), kill and mission counts |
| Location | System, and a base or a position and orientation in space; room; last base |
| Ship | Hull archetype and status, damaged parts, mounted items with hardpoints, cargo with counts |
| Base snapshot | Hull, parts and equipment as at the last base |
| Interface state | Weapon groups, visit flags, mission log, interface flags |
| Content, per player | Campaign position; the running mission's triggers, ships, solars and objectives; a pending generated mission; locked jump points; bar-inhabitant state; rumours; docking and lane permissions; statistics |
| World (single player) | World clock, faction-to-faction feelings, content's world state |

Content writes its own parts: the authority raises "player save" and
"world save" and each controller appends its sections. The authority
stays ignorant of missions.

| Save | Trigger | Allowed when |
| --- | --- | --- |
| Manual (single player) | The player | Only while docked. A named or a new slot. |
| Autosave (single player) | Leaving a base; arriving after a jump | Unless the base forbids it or the presentation asked to skip one. One slot. |
| Checkpoint (single player) | A mission action; the presentation acknowledges first | Anywhere, space included. Copied to the autosave slot. |
| Character (multiplayer) | Leaving a base, landing, arriving after a jump, logout | Passes the worth audit. Written to a temporary file, then renamed. |

A new single-player game copies a template to a restart slot. The shipped
one starts in space with 500 credits, a ship with 16 mounted items, the
first campaign mission and 32 locked jump points.

### 3.11 Sessions, accounts and characters

| State | Event | Next state |
| --- | --- | --- |
| Connected | Login accepted | Logged in |
| Logged in | Character selected | Docked or in space, as saved |
| Docked | Launch | In space |
| In space | Docking completes | Docked |
| In space | Jump-out complete | In space, next system |
| In space | Ship destroyed | Dead, then a loaded game (single player) or the last base (multiplayer) |
| Any | Disconnect | Gone: room and base exits are raised, the character is saved (multiplayer) |

- An account is a directory on the host named by a hash of its
  identifier. An unknown identifier creates one if the host admits new
  players. Login fails when the account is already logged in, missing and
  not creatable, or banned.
- The identifier is generated on the player's machine and sent in clear,
  without a password (per design notes).
- At most five characters per account; names are unique on the host.
- A new multiplayer character is a template with placeholders: name,
  money, appearance, voice, home system and base, and a package of ship
  and loadout. The shipped data offers one pilot, one package (2,000
  credits) and one starting faction, presets reputations between −0.65
  and 0.91 and marks 25 objects as visited.

### 3.12 Multiplayer rules

- **Capacity:** 1 to 128 sessions, default 16 (per design notes).
- **Death.** The character returns to its last base with the equipment
  mounted there. Of unmounted items it keeps shield batteries, repair
  kits, ammunition, mines and countermeasures; commodities and spare
  equipment are lost.
- **Chat scopes:** universe, system, local (everyone who currently has the
  sender's ship), group, one player. Text commands invite to, join and
  leave a group of up to eight.
- **Trade between players:** each side offers money and cargo. When both
  accept, the authority checks that both still own what they offered and
  that each hold has room, then swaps. Firing or jettisoning cancels it.

### 3.13 What the host can administer

Settings: name, description, password, admit new players, list publicly,
player limit, player-versus-player, port, tick rate, logging. At run time:
ban, kick or delete an account; delete a character; send text to one
player, one system or everyone; edit the message of the day; watch tick
load, player count and per-session bandwidth. A host advertises its name,
description, player count and limit, its password, player-versus-player
and new-player flags and its build version; a presentation refuses
another version (per design notes).

## 4. Data that defines it

| Record | Author | Principal fields |
| --- | --- | --- |
| Game configuration | Developer | Data root; single-player transport; initial-world file; controller lists per mode (name, priority); text modules in order; the data manifest (chapter 12); log filters |
| Engine configuration, one per program | Developer | Engine modules to load; renderer, sound and particle settings. The host's lists no sound or effects modules. |
| Initial world | Designer | Per faction its feelings towards every other; jump points locked at the start of a multiplayer universe |
| New-player template | Designer | A complete single-player character (3.10) |
| New-character template and options | Designer | A character with placeholders; pilots, packages (ship, loadout, money), factions (home base, package, pilot) |
| Host settings; character files | Operator; the authority | 3.13; 3.10 |

The constants of 3.2 to 3.8 are compiled in, except the physics rate.

Data order matters across the network: archetypes travel as short indices
valid only if both sides loaded the same files in the same order. Only
the build version guards this.

## 5. Connections

**Needs:** 02 (the object and flight model both sides run); 03 (damage
rules for a reported hit); 04 (docking, lanes and jumps, whose sequences
straddle the boundary); 05 and 06 (pilots and population, run in the ship
update and as controllers); 07 (reputation and markets, stored in the
character and used by the audit); 08 (missions as controllers,
checkpoints); 10 (the front end that drives login, load and save); 12
(formats of the records in section 4).

**Gives:** to all, the tick order and the variable step; to 03, who
detects which hit; to 06, that only systems with a player run, and the
visibility radii; to 08, the event catalogue and save hooks; to 09, that
own-ship data is current and the rest delayed; to 10, session states and
account rules.

## 6. Design notes for the builder

**Essential**

- One message catalogue for both modes. It makes the two modes one game
  and gives a seam for testing.
- The authority runs only where players are; the universe of chapter 01
  assumes an empty system costs nothing.
- Content as listeners on an event bus, so missions never touch the core.
- The player's own ship is never corrected; the flight feel of chapter 02
  depends on it.
- A character is one self-contained record, and content saves its own
  part.

**Feel**

- Autosave on launch and jump, manual save only when docked: bases become
  safe harbours and combat cannot be save-scummed.
- Send-on-divergence replication: quiet traffic costs almost nothing.

**Accidents of its era**

- Trusting the presentation for position, hits, mining and purchases, and
  compensating with an audit. Today: compute purchases on the authority,
  bound reported positions by top speed, check fire rate and energy,
  confirm hits against the authority's positions with a tolerance.
- Nested events with untyped payloads, and list walks that break when a
  handler removes the visited element. Keep the synchronous order, but
  pass typed events and refer to objects by identifier.

**Simplifications and their cost**

- *Fixed tick:* easier to reason about and test; costs a retune of
  chapter 02's constants.
- *Clean loopback* (two real worlds in single player): costs memory and
  one frame of latency to hide.
- *No multiplayer:* drops social messages, observers and accounts; keep
  the split anyway.
- *Snapshot interpolation* instead of extrapolation: more traffic,
  simpler code.

## 7. The original technology

**What it stood on.** A 32-bit desktop program of the early 2000s made of
about thirty dynamically loaded modules, wired at run time by a component
registry that reads its module list from configuration. Fixed-function
graphics with a per-adapter workaround table; a licensed rigid-body
physics library; cutscenes as compiled
scripts in an embedded scripting language; text in resource modules; game
data as text files of sections with `key = value` lines. Networking used
the platform vendor's client/server session library over datagrams, plus
a third-party list service for public hosts. The host is a separate
desktop program.

**Behaviours that follow from it**

- *Everything reliable and ordered.* The session library was used in one
  mode: guaranteed, sequenced delivery on one stream, position updates
  included. Hence the saturation gate: with no way to drop a stale
  update, the only congestion control is not to queue it.
- *Batching one way.* Messages to a presentation are batched to about
  1 KiB and compressed; the other direction goes raw.
- *Compact encodings.* Orientation as four signed bytes, fractions as one
  byte, hardpoint names through a fixed dictionary, archetypes as 16-bit
  load-order indices, positions as full floats. No length field, no
  protocol version, no integrity check, no encryption.
- *One thread.* Simulation, content and interface share one thread; audio
  workers and network callbacks only queue. Nested events are safe only
  because of this.
- *Shared objects in single player.* Both halves load the same shared
  module, so the presentation wraps the authority's objects instead of
  mirroring them, steps physics for both, and loopback messages omit
  fields the receiver can read from the object. Each half tests "is this
  single player?" in over a hundred places.
- *Numbers tied to one processor family.* Timing, physics and message
  encoders depend on the precision mode of its floating-point unit.
- *Saved games are text* behind a byte-wise obfuscation.
- *Identity from the machine:* derived from the operating system's product
  identifier and the user name.
- *Hosting needs a full install:* the host loads renderer and material
  modules too.

**What a modern builder should not copy**

- The component registry: compose statically.
- A reliable, ordered channel for state updates: send object state
  unreliable and sequenced, events reliable.
- Wire formats without length, version or authentication; identifiers that
  are bearer tokens.
- Shared-object single player and its mode tests.
- Presentation-side authority over money and inventory.
- File handles lent through events for saving: give content a structured
  save interface.
- A host bound to graphics modules and a desktop toolkit: make it headless.

## 8. Open questions

1. **Validation of player fire in multiplayer.** Whether the authority
   checks refire delay, energy and ammunition for a player's weapons was
   not read.
2. **Host password.** The reconstructed presentation never sends it, so
   protected hosts look unjoinable; original behaviour or reconstruction
   gap is unknown (per design notes).
3. **Death penalty.** The host configuration has a percentage (100 as
   shipped); the reconstructed host reads and discards it and the code
   implements the fixed rule of 3.12.
4. **Where a multiplayer character resumes** after logging out in space:
   the file records the position; the resume path was not traced.
5. **Which objects the unnamed tick lists hold** (3.2, sub-steps 2 to 4).
6. **The list service protocol:** its module is not in the sources.
7. **Mining trust.** The presentation tells the authority which loot a
   mined asteroid yields; no check was found.

## 9. Basis

Design: `design/adr/0002-single-player-world.md`, `0006-controller-bus.md`,
`0007-server-world.md`; assessments `01-foundation.md`,
`03-network-stack.md`, `04-server-content.md` (1, 2), `06-client-app.md`
(2), `08-cross-cutting.md` (2, 4, 6); `design/facts.md` (F4, F5, F14);
`design/decisions.md` T17 to T19 as hints.

Data: `outdir/OutpostFrontier.ini`, `NeuronComponents.ini`,
`NeuronComponentssrv.ini`, `flconfigdatabase.txt`, `newplayer.fl`,
`mpnewcharacter.fl`; `outdir/data/initialworld.ini`, `constants.ini`,
`CHARACTERS/newcharacter.ini`.

Code, read in part: `include/rpc_iserver.h`, `rpc_iclient.h`,
`common_server_controller.h`; `Server/IServerImpl.cpp`,
`server_observer.cpp`, `server_classes.cpp`, `server_savegame.cpp`,
`server_pub.cpp`, `server_chat.cpp`; `Common/common_remotephysics.cpp`,
`common_deadreckonedvector.cpp`, `common_physicalsystem.cpp`; Client
`client_frame_tick.cpp`, `client_objdb.cpp`, `client_gameobj.cpp`;
`Content/Content.cpp`, `ContentPlayerState.h`, `PlayerState.cpp`;
`OutpostServer/OutpostServer.cpp`.

From design notes, unchecked: message counts, wire encodings, batching,
the saturation formula, tick-rate range, player limits, the account
identifier, the space-frame order, frame caps, pause, heartbeat and
controller-order details, the three clocks, host settings, and the
networking, threading and save-container details of section 7.

Inferred: the unnamed tick lists; unguided shots flown separately on each
side; the single-player death path; what the audit counts as income.
