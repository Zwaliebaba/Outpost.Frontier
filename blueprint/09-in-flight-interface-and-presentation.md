# 09 — In-flight interface and presentation

Status: draft
Depends on: 01, 02, 11, 12; refers to 03, 04, 05, 08, 10

## 1. Purpose and player experience

This chapter covers everything between the player and the simulation while
the ship is in space: how key and mouse input becomes commands, how the
commands become steering demand and requests to the authority, what the
HUD shows, how targets are found and selected, where the camera sits, what
one frame is made of, and what the effect and audio systems must do.

The player flies with one hand on the mouse and one on the keyboard. The
mouse does two jobs with the same cursor. Left free, it is a pointer:
clicking an object in space selects it, clicking a HUD button presses it.
With the steering button held, the same cursor steers: the further it sits
from the screen centre, the faster the ship turns towards it. In both
modes the cursor is also the gun sight, so the player aims by pointing and
the turrets follow. The keyboard carries throttle, strafe, thruster,
cruise, weapons, target cycling and the automatic manoeuvres.

The HUD answers four questions without opening a window: what state is my
ship in (power, shield, hull, speed), what is around me (brackets, edge
arrows, a contact list), what am I shooting at (a target panel with a
damage view and a lead marker), and what can I do next (a manoeuvre bar
that lights only the manoeuvres possible for the current target). Sound
carries the rest: music that shifts with danger, spoken warnings, and
radio chatter that fades with distance.

## 2. Concepts

Glossary and chapter 01, 02, 11 and 12 terms are used as defined. This
chapter adds:

| Concept | Definition |
| --- | --- |
| Command | A named player intention ("fire", "next target", "open map"). Input devices produce commands; nothing else in the game reads keys. |
| Binding | A key or mouse button, with modifier keys, attached to a command. A command may have two. |
| Command context | A named set of commands that is live in one situation: always, in space, flying, chase view. |
| Handler | A piece of the interface that has registered for a command and may consume it. |
| Mouse flight | The cursor mode in which the cursor's offset from the screen centre is the steering input. |
| Free cursor | The other mode: the cursor points, picks and aims but does not steer. |
| Aim point | The world position the guns converge on this frame, derived from the cursor. |
| Contact | An object the player's scanner currently reports. |
| Lead marker | The point to shoot at so that a shot meets a moving contact. |
| Sub-target | One part or equipment item of the target, chosen for aimed fire. |
| HUD element | One self-contained piece of the HUD, created from a record in the HUD definition. |
| Camera mode | One way of placing the view: cockpit, chase, turret, rear, death, docking, scripted, parked. |
| Letterbox | The cinematic state: bars on, cursor and HUD off, player commands refused. |
| Effect | A named bundle of a visual part, a sound and optionally a light, played at a point or attached to a hardpoint. |
| Effect type | A class of effects sharing culling, level-of-detail and priority rules. |
| Sound category | One of five volume groups: effects, interface, voice, music, ambience. |
| Ducking | Temporarily lowering the rest of the mix while speech plays. |
| Comm | One radio message: sender, receiver, voice profile and a list of voice lines. |
| Voice line | One recorded utterance or fragment, a member of a voice profile. |

```text
keys, buttons, wheel, cursor
        │ bindings (per context)
        ▼
    COMMANDS ──► handlers, in turn ──► first to consume wins
        │
        ├─ flight ─────► manoeuvre layer of own ship (chapter 02, 05)
        │                 direction, throttle, strafe, cruise, manoeuvre
        ├─ weapons ────► fire request with aim point (chapter 03)
        ├─ targeting ──► own ship's target and sub-target ─► authority
        ├─ view ───────► camera mode
        └─ windows ────► map, inventory, chat … (chapter 10)

simulation state ──► HUD elements, camera, effects, audio (read only)
scanner of own ship ──► contacts ──► brackets, contact list, music state
```

## 3. How it works

### 3.1 From device to command

Keyboard input arrives as key-down, key-up and auto-repeat events. Mouse
movement arrives as relative motion, which the game integrates into its
own cursor position and scales by a sensitivity option; the cursor is
drawn by the game, last in the frame. Buttons and the wheel are events
like keys.

Each command record states which events it reacts to: down, up, repeat,
and whether the shift key is ignored when matching. Hold-type commands
(strafe, thruster, brake, rear view, throttle up and down) take both down
and up. Continuous fire takes repeat.

An event is offered in this sequence:

1. A modal window, if one has taken the input focus, sees it first and
   may swallow everything.
2. The "cancel" command goes to the top of a cancel stack, so the most
   recently opened thing closes first.
3. Otherwise the bindings of every live command context are searched, and
   the resulting command is offered to each handler registered for it
   until one consumes it.

Four contexts matter in flight: always present (pause, map, inventory,
chat, screenshot, objective recall: 29 commands), space (target and
weapon selection: 24), flying (77) and chase view (21). Entering or
leaving a state switches contexts on and off. This is how the same key
can mean different things on a base and in space.

Nearly every in-flight handler first asks one question: may the player
act now? The answer is no while a full-screen window is open, while the
cursor is hidden by a cutscene, while the game is paused, or when there
is no player ship. Key releases for held commands are still honoured, so
a thruster cannot stick on when a window opens.

Bindings are data. The shipped map is read first, then the player's own
map replaces it. The options screen lists 92 of the commands for
rebinding in three groups: ship controls (44), interface (43) and
multiplayer (5).

### 3.2 The command catalogue

The data defines 151 commands, 22 of them debug aids. The rest group as
follows.

| Group | Commands | Effect |
| --- | --- | --- |
| Steering | Mouse-flight hold (steering button), mouse-flight lock toggle, pitch/yaw/roll keys | Sets the direction input of the manoeuvre layer (3.3, 3.4) |
| Speed | Throttle up, throttle down, wheel step, brake/reverse (hold), engine kill (toggle), cruise (toggle), thruster (hold), strafe left/right/up/down (hold) | Sets throttle and the wishes of chapter 02 |
| Manoeuvres | Free flight, go to, dock, join formation, cancel | Replaces the manoeuvre running on the player's ship (chapter 04, 05) |
| Guns | Fire active weapons (hold), fire straight ahead, fire weapon 1–10, toggle weapon 1–10 | Fire request with the aim point (3.5) |
| Weapon groups | Select, fire and store group 1–6 | A group is a remembered set of active weapons |
| Launchers | Missiles, torpedoes, cruise-disrupting munitions, mines, countermeasures | One command per launcher kind, each firing every launcher of that kind |
| Targeting | Select under cursor, next/previous contact, closest enemy, next/previous enemy, clear, next/previous sub-target | Sets the ship's target (3.6) |
| Target actions | Scan cargo, tractor target, tractor all loot, hail, invite to group, request trade | Requests to the authority about the target |
| Consumables | Use hull repair, use shield repair | Spends one repair consumable |
| View | Cockpit/chase toggle, turret view toggle, rear view (hold), look left/right/up/down, reset look, level-camera toggle | Camera mode (3.9) |
| HUD | Minimise HUD, open status panel, target panel, contact list, formation list, weapon list | Shows and hides panels |
| Windows | Map, inventory, player status, current objective, chat, group window, help, pause | Opens windows of chapter 10 |
| System | Screenshot, pause, quit, save | Handled by the application itself |

Every HUD button is only another source of the same commands: clicking
the "dock" button posts the dock command. Nothing can be done with the
mouse that has no command.

### 3.3 Mouse flight against free cursor

The steering button is the primary mouse button. Because the same button
also selects targets, a press must be told apart from a hold.

| State | Event | Next state | Effect |
| --- | --- | --- | --- |
| Free | Button pressed over empty space | Arming | Remember the cursor position as origin; start a timer |
| Free | Button pressed over a HUD element | Free | The element gets the click |
| Arming | Timer reaches 0.25 s | Steering | — |
| Arming | Cursor moves further from the origin than a small radius | Steering | Timer forced to 0.25 s; steering starts next frame |
| Arming | Button released | Free | The press counts as a click: select what is under the cursor |
| Steering | Each frame | Steering | Send direction (x, y) to the manoeuvre layer |
| Steering | Button released, lock off | Free | Send direction (0, 0); the ship stops turning |
| Any | Lock toggled on | Steering | Steering continues with no button held |
| Any | Letterbox on, or riding a lane | Free | Direction reset; steering refused |

The radius is 50 pixels on an 800-pixel-wide screen and shrinks in
inverse proportion to the screen width.

The direction is the cursor offset from the centre divided by half the
screen width and half the height, so each component runs from −1 to 1.
An option inverts the vertical axis. Chapter 02 describes how this
becomes an axis throttle and then torque. The result is rate control:
holding the cursor still off-centre holds a steady turn.

Three details make the scheme workable:

- **A click never steers and a hold never selects.** Selection is
  suppressed once the hold has passed its threshold.
- **The lock** turns the mouse into a virtual stick for players who do
  not want to hold a button. With the lock on, a click still selects.
- **Turret and rear views do not steer.** A non-zero direction is not
  forwarded while either is up. Entering the turret view clears the
  direction, so the ship flies on while the player looks and shoots
  around it.

A steering arrow at the cursor shows the demand while mouse flight is
on: it points from the centre towards the cursor and grows with the
offset, with a dead zone drawn at one fifth of its full range.

### 3.4 Keyboard steering and throttle

Pitch, yaw and roll keys build a vector with components −1, 0 or 1. It
is multiplied by a ramp that rises from 0 to 1 at 2 per second while any
key is held and resets when the keys change sharply, so a tap nudges and
a hold turns fully. With shift held and a target selected, the ramp is
replaced by a factor that falls as the nose approaches the target,
`1.5 × (1 − cos²θ)` clamped to 0.1 to 1, which gives fine control for
lining up. The vector goes to the manoeuvre layer only when it changes.

Throttle keys move the throttle at 1.0 per second while held; the wheel
steps it by 0.2. Lowering the throttle, or reaching exactly zero, drops
cruise. Cruise, engine kill and the lock are toggles; brake, thruster
and strafe act while held. When one strafe key is released and another
is still down, the remaining one takes over.

Cancel returns the ship to free flight, drops cruise and plays a spoken
confirmation. In the turret view, cancel leaves the turret view instead.

### 3.5 Aim point and firing

Each frame the presentation computes one aim point and gives it to every
gun; chapter 03 describes how guns turn towards it and what they hit.

1. Cast a ray from the camera through the cursor. Note the first object
   it hits, ignoring shield bubbles and the player's own ship.
2. Find the lead markers on screen (3.6). If the cursor lies within a
   small screen radius of one, that contact is the intended target. If it
   lies very close, and aim assistance is on, the aim point snaps to the
   lead position. Otherwise the aim point is on the cursor ray at the
   marker's distance.
3. With no marker near, the aim point is on the cursor ray at the
   distance of the object hit, or else at the range of the longest-ranged
   active gun.

The cursor changes shape and colour with what it would hit: neutral,
hostile, friendly, a firing variant of each, and a "cannot fire" shape.
Firing is refused while the cursor is over the interface.

While the fire command is held, every active gun with unguided munitions
fires each frame it can; refire delay and power are chapter 03's rules.
Guided launchers, mines and countermeasures each have their own command,
so that holding fire never empties the missile rack. Weapons can be
switched on and off singly; a weapon group stores the current on/off
pattern under a number. The groups belong to the character and are kept
by the authority.

### 3.6 Contacts and targeting

**What the player can know.** The player's ship carries a scanner
(chapter 02). Each tick it lists the targetable objects within its
range. The range is

```text
effective range = scanner range × (1 − interference × immersion)
```

where interference (0 to 1, authored values 0.01 to 0.8) and immersion
come from the ship's physical zone (chapter 01), and the product is
capped at 1. A target is hidden by its own zone too: if the target's
interference is higher than the observer's, the target must also lie
within the range computed with the target's value. Cargo scanning uses a
second, shorter range with the larger of the two interferences.

Worked example: a 2,500 m scanner, the player in clear space, a ship
deep in a cloud of interference 0.7. The ship appears only within
2,500 × 0.3 = 750 m. From inside the same cloud the player sees
everything only within 750 m. Clouds are therefore places to hide and to
be ambushed in.

In multiplayer a second limit applies: the authority tells a
presentation only about objects inside its visibility radii (chapter 11:
2,500 m for objects no player owns, 5,000 m for another player's ship,
10,000 m for a group member's). A scanner cannot report what the
presentation has not been told about (inferred).

**Marking.** Every contact gets a bracket sized to its screen extent and
coloured by attitude: hostile, neutral, friendly, with separate colours
for group members and mission-critical objects. Hostile brackets have a
different corner shape, so colour is not the only cue. A contact off
screen becomes an arrow on the screen edge. Other players' ships carry a
name label. The current target gets a heavier reticle with small shield
and hull bars, a name, a range read-out ("M" up to 2,000, then "K") and
a lead marker.

**Selecting.**

| Method | Rule |
| --- | --- |
| Click | The bracketed contact under the cursor; else the object the cursor ray hits; else the nearest pickable object whose enlarged bracket holds the cursor. A hostile contact under the cursor wins over a nearer object behind it. |
| Click on a waypoint marker | Selects the waypoint |
| Closest enemy | The nearest hostile contact |
| Next / previous enemy | Cycle through hostiles |
| Next / previous contact | Step through the contact list in its current filter |
| Double-click a list row | Select, and with a second action start the default manoeuvre (inferred from the list's separate double-click path) |
| Clear | No target |

Selecting sets the target on the player's ship and reports it to the
authority, which fans it out (chapter 11). Selecting a hostile also
counts as entering combat for the music (3.13).

**Sub-targets.** With a target selected, the player can step through its
parts and external equipment or click one in the target panel's damage
view. Guns then aim at that item.

**Learning the map by flying.** Once a second the presentation checks
what the player has come near and updates the knowledge state (chapter
01): a solar within seven times its radius, but at least 10,000 m; a
zone within 7,500 m; and the faction of any ship within scanner range.
Each new fact is reported to the authority.

### 3.7 HUD elements

The HUD is a list of elements named in one definition file. Each element
is built by a factory registered under the record's name, reads its own
settings, watches the player's ship and draws itself. All elements hide
together in letterbox, when the HUD option is off, and when there is no
player ship.

| Element | Shows | Notes |
| --- | --- | --- |
| Gauges | Power pool, shield and hull as three bars of 16 segments; forward speed; thrust pool in percent | Shield is the sum over all shields above their offline threshold. Speed is the velocity component along the nose, shown as 300 in cruise. The thrust read-out changes colour below 25 %. |
| Contact markers and reticle | 3.6 | Also computes the aim point and resolves clicks |
| Alerts | Incoming guided munition, ship on your tail, radiation, network lag | The munition alert blinks faster as the munition closes, with a period of √distance × 0.001 s and at least 0.1 s, and triggers a spoken warning once. The tail alert fires within 500 m. Lag lights above 600 ms and turns red above 1,200 ms. |
| Target panel | Wireframe view of the target seen from the player's side, parts coloured by damage in four steps; name, faction, pilot, range, shield and hull; buttons for scan, tractor, next enemy, trade, invite | Switches between target view, contact list and formation list |
| Contact list | 3.8 | Shares the panel |
| Status panel | The same wireframe for the player's own ship; weapon list with ammunition and on/off state; repair consumable counts; a "loot in reach" button | Weapon rows are clickable |
| Manoeuvre bar | One button per manoeuvre: free flight, go to, dock, formation; a cruise indicator | A button is lit only if the manoeuvre layer says the manoeuvre is possible for the current target; the active one is marked |
| Steering arrow | 3.3 | — |
| Damage direction | Four edge flashes that light on the side a hit came from and fade in 0.7 s | A hit from dead ahead lights all four faintly |
| Cruise progress | Percentage while cruise charges | — |
| Waypoint | A marker object at the next point of the plotted route, with distance | Advances to the next point within 150 m (chapter 04) |
| Comm window | A small animated portrait of the speaker with name and caption | 3.14 |
| Mode captions | "Turret view", "rear view", "mouse flight" with the key that leaves the mode | The key name is read from the live bindings |

Two rules hold across elements. Text that names a key is always built
from the current binding. And expensive lists refresh on timers, not
every frame: the contact list every 3 s, status counts every second.

### 3.8 The contact list

The list has eight visible rows, each with an icon for the kind of
object (about 16 kinds: fighter, freighter, capital ship, station, jump
point, lane ring, planet, loot, waypoint, other player and so on), the
range and the name, coloured by attitude.

| Filter | Contents |
| --- | --- |
| Important | Hostile ships; other players; the selected target; mission and waypoint objects; hostile solars close by |
| Ships | Every ship contact |
| Solars | Stations, jump points, lane rings, planets |
| Loot | Loot, and wrecks that can be looted |
| All | Everything above |

Ships must be within scanner range; other players' ships are listed at
any range the presentation knows them. Solars and loot are taken from a
14,000 m sphere and from the list of large solars. Solars the player
has not yet visited appear with a kind label only, not a name. The
formation list replaces the filters when the player is in a formation or
a player group.

### 3.9 Cameras

One camera is active at a time. A camera has a field of view, a watched
object, an update step, and a say in which audio mode applies (inside or
outside the ship).

| Mode | Placement | Entered by | Left by |
| --- | --- | --- | --- |
| Cockpit | At the hull's cockpit hardpoint, with a cockpit model drawn around the view; the head lags slightly under acceleration and turn | View toggle | View toggle |
| Chase | Behind and above the ship; see below | View toggle; default | View toggle |
| Turret | On a tether behind the ship, free to look anywhere within ±60° of pitch; aimed by keys or by the cursor | Turret toggle | Turret toggle, cancel |
| Rear | 5 m up and 20 m back, looking backwards | Held key | Key release |
| Death | Shakes for 2 s with amplitude falling as the square of time left, then drifts with the wreck's velocity, capped at 100 to 200 m/s | Player ship destroyed | Hands over to the "ship lost" flow (chapter 10) |
| Docking | Fixed outside point that keeps aiming at the ship; letterbox on; fade to black timed to end with the docking sequence | Authority starts a dock or lane entry (chapter 04) | Sequence end |
| Scripted | Driven by a scene script (chapter 10) | Mission or cutscene | Script end or cancel |
| Parked | Fixed pose given by a script; letterbox on | Script | Cancel returns to the player |

"Player view" means cockpit or chase according to a stored preference.
Rear and turret are overlays that return to it.

**The chase camera** is what makes mouse flight readable, and has four
behaviours.

- **Offset.** It sits at an authored offset per hull, for example 9 m up
  and 35 m back for a fighter and 13 m up and 50 m back for a freighter.
- **Speed lag.** It follows a smoothed copy of the ship's speed and
  stretches back by 0.05 s of travel plus the accumulated lag, so
  acceleration is seen as the ship pulling away.
- **Turn look-ahead.** It rotates towards the ship's current axis
  throttle by an authored angle: 17 to 23° sideways, 5 to 10° upward and
  20 to 30° downward. The ship therefore slides towards the screen edge
  it is turning to and the space it is turning into opens up. The turn
  rate of the camera is limited by an authored angular acceleration.
- **Levelling.** The camera keeps its own "up" and tells the manoeuvre
  layer about it. Banking (chapter 02) is active only in this mode: the
  ship rolls into turns relative to the camera's up and rolls back when
  the input is released. In the cockpit view the camera reports "normal"
  and there is no banking. A command switches levelling off.

All ship cameras shake on damage: amplitude 0.02 for a hull hit, 0.005
for a shield hit, decaying by 0.001 per frame. A jump adds its own shake.

### 3.10 What a frame is composed of

In space the presentation does this each frame, in sequence.

1. **Advance its copy of the world**: objects, animation, physics,
   beams, collisions, lane and docking sequences (chapter 02, 11).
2. **Update the active camera**, then the system's ambience and the
   music state.
3. **Run the audio mixer** with the camera as listener.
4. **Update mouse flight** and begin the picture.
5. **Draw the scene through the active camera:**
   - Decide whether the camera is outside, partly inside or deep inside
     a field (chapter 01). Deep inside a nebula the star background is
     skipped, the far limit shrinks to the fog distance and the clear
     colour becomes the fog colour.
   - Far background: star sphere in up to three layers, selected by a
     quality option, and distant stand-ins for asteroid fields, drawn
     with a far limit of 250,000 m.
   - Collect objects: everything within 10,000 m of the camera from a
     spatial index, plus the short list of large solars (stars and
     planets) whatever their distance, plus ships, munitions, debris and
     the waypoint marker. The default draw limit is 20,000 m and a
     system may set its own.
   - Choose a level of detail per object from its projected size and
     authored switch distances; sort.
   - Draw solid objects with per-object lighting: system ambient blended
     towards the nebula's colour, plus the few nearest lights taken from
     a grid.
   - Draw effects with fog applied when inside a field: particles,
     beams, trails, flares.
   - In the cockpit view the cockpit model is drawn in its own slice of
     the depth range, so it never intersects the world.
6. **Clear depth and draw the three-dimensional HUD elements** through
   the HUD camera: brackets, reticle, arrows, wireframe views.
7. **Draw windows**: HUD panels, any open window, the pause menu, then
   the cursor.
8. **Draw the fade overlay**, finish, remove dead objects, and apply a
   pending change of system or a landing.

During a jump, step 5 draws only the player's ship, its effects and the
jump tunnel.

As requirements on any engine: a depth-sorted transparent pass after the
opaque one; per-object light selection; distance fog with a per-zone
colour and range; a background pass independent of world scale; a second
scene pass for the interface with its own camera and cleared depth; small
viewports inside the HUD that render a model with another camera (target
view, comm portrait); and a full-screen fade. The original has no
post-processing and no render-to-texture.

### 3.11 What the effect system must do

An effect record names up to three parts: a visual effect from a
particle-effect library, a sound, and for explosions a light with radius
and range. Game code and data refer to effects only by nickname: a gun
names its muzzle flash, munition trail and impact; a shield its hit
effect; an engine its flame and trail; a fuse (chapter 03) its
explosions.

The system must be able to:

- play an effect once at a point, or attach it to a hardpoint and follow
  it, looping until stopped;
- stop an attached effect softly, letting particles die out;
- scale intensity from 0 to 1 at run time (engine flame with throttle,
  dust with speed);
- draw beams and bolts from a compact description (lengths, widths, tip,
  core and tail colours);
- trigger a positional sound and a temporary light with the visual;
- cull and degrade by rule, per effect type.

Each effect belongs to one of about 45 effect types, which carry the
rules:

| Field | Meaning | Range |
| --- | --- | --- |
| Priority | The effect is skipped when the detail option is below this | 0.1 to 1 |
| Radius | Bounding size for culling | 5 to 1,250 m |
| Visibility when off screen | Destroyed, or kept alive | Two values |
| Update when off screen | Frozen, or simulated | Two values |
| Run time | Lifetime in seconds; negative means until stopped | −1, 0.5 to 20 |
| Visible distance pair | Near and far limit; the detail option slides the cut-off between them | 50/150 to 2,000/7,000 m |
| Level-of-detail class | Maps projected size in pixels to a detail blend, and to nothing below a minimum size | Six classes |

The rule to keep is that effects are numerous and cheap to drop. A
battle spawns hundreds; each is judged by type every frame, and the
player's detail setting moves one threshold.

### 3.12 Sound

A sound record gives a file, a category, an attenuation in dB, a pair of
distances (full volume inside the first, inaudible beyond the second),
whether it is positional, whether it is streamed from disk, and optional
pitch variation. Typical distance pairs are 20 to 70 m near and 200 to
500 m far for ship sounds, and 1,750/6,000 m for the largest explosions.

- **Five categories**, each with a master volume from the options:
  effects, interface, voice, music, ambience.
- **A voice budget.** Each frame every playing source computes a score.
  The mixer sorts them and keeps the best N, 8 to 128 by option; the
  rest fall silent but keep their place in time.
- **The listener** is the active camera, with its velocity for Doppler.
  Camera modes report an inside or outside audio mode; the player's own
  engine and thruster use different recordings inside.
- **Engine sound follows throttle**: pitch bends with it, and volume
  rises while the throttle is changing and settles when it is steady.
- **Ducking.** Speech lowers everything else: 12 dB for a comm addressed
  to the player, 9 dB for in-flight speech, 10 dB in cutscenes, falling
  in 0.2 to 0.25 s and recovering in 0.75 to 0.85 s.
- **Ambience** is a looping sound per zone or system (chapter 01),
  cross-faded like music.
- **Pause** freezes all playing sources.

### 3.13 Music follows the situation

Each system names three tracks: calm, danger and battle. A zone may
replace the calm track while the player is inside it. A mission may
override any of the three, or force one track (chapter 08).

| State | Condition |
| --- | --- |
| Calm | None of the below |
| Danger | The player's scanner lists at least one hostile contact (checked every 2 s) |
| Battle | This frame the player damaged a hostile, was damaged by one, or selected one as target |
| Forced | A mission set an override track |
| Silent | The player's ship is in a jump or docking sequence |

A state change starts a cross-fade (3 s) and locks the state for 10 s,
after which the conditions are evaluated again. While in battle, each
new battle event restarts the 10 s, so battle music holds until ten
quiet seconds have passed and then falls to danger or calm. The lock is
what prevents flicker when a single enemy drifts in and out of range.

### 3.14 Voice lines and comms

A voice profile is a speaker with a set of voice lines. Reusable flight
profiles hold about 400 to 500 lines each: requests and replies for
docking, combat calls, acknowledgements, and a large vocabulary of
fragments: numbers, formation call signs, and one spoken reference per
faction, ship class and base. Fragments come in sentence-medial and
sentence-final intonations.

The authority composes a comm (chapter 05 and 08 decide who says what):
sender, receiver, voice profile, a caption text and up to 16 line
identifiers, such as "this is" + call sign + number + "requesting
permission to dock". The presentation only plays it.

- **Two channels.** Comms sent or received by the player's ship go to
  the channel with the portrait window and a caption. Everything else is
  overheard chatter on a second channel with no window, panned by the
  speaker's direction.
- **Queue.** Each channel has a queue of 16. Lines of one comm play back
  to back. The channel stays busy for a short linger afterwards.
- **Priority.** Each line has a priority (−6 for background chatter up
  to 1 for mission speech). A comm takes the highest of its lines. A
  low-priority comm that arrives while something more important plays is
  dropped; a low-priority comm that is playing is cut off by a more
  important arrival; an important one waits.
- **Distance.** Chatter fades linearly with distance from −2 dB to
  −16 dB at 3,500 m, background chatter from −8 dB to −24 dB, and beyond
  that is not played at all. Comms to the player are never faded.
- **Completion.** In single player the presentation reports each comm
  to the authority as finished or cancelled, because scripted missions
  wait for speech to end before continuing (chapter 08).
- **Status voice.** A separate interface voice announces ship events:
  incoming munition, hazards entered, manoeuvre accepted or cancelled.

### 3.15 Authority and presentation

| Presentation alone | Reported to the authority |
| --- | --- |
| Bindings, contexts, cursor mode, aim point | Fire requests with aim point; activation of equipment |
| HUD, brackets, contact list, alerts | Target and sub-target changes |
| Camera, shake, letterbox | Manoeuvre changes; dock and formation requests |
| Effects, mixer, music state | Use of consumables, tractor, cargo scan |
| Scanner evaluation for the HUD | Knowledge gained; weapon groups; comm completion |

The player's own ship is moved by the presentation's simulation, and its
state is sent up (chapter 11). So steering has no network delay, at the
cost of the authority having to trust or check that state.

## 4. Data that defines it

| Record file | Record | Principal fields |
| --- | --- | --- |
| Key map | Command | Nickname, name and help text identifiers, events it reacts to, default bindings (key plus modifiers; mouse buttons as negative codes) |
| Key map | Context | Nickname, list of commands |
| Key list | Options group | Group number, title, commands in display sequence |
| Cursor definition | Shape, cursor | Image region; hotspot, scale, colour, spin |
| HUD definition | One per element | Mostly empty: the record's presence creates the element. The comm window and steering arrow carry positions and sizes; the manoeuvre bar lists manoeuvres as (behaviour name, tooltip and name text identifiers, two icons) |
| Camera definition | One per camera mode | Field of view (70° for flight modes), near distance |
| Ship archetype | Camera fields | Chase offset, turn angles (sideways, up, down), angular acceleration, look-ahead blend |
| Cockpit definition | Per hull family | Cockpit model, head-turn limits, turret tether and rotation speeds, rear-view position |
| Effect types | Effect type, level-of-detail class | 3.11 |
| Effects | Effect | Nickname, effect type, visual effect, sound, optional light |
| Visual effects | Visual effect | Nickname, particle library file, identifier inside it, texture libraries |
| Beam effects | Beam, bolt | Lengths, widths, colours, brightness, textures |
| Sound files | Sound | 3.12 |
| Audio configuration | One record | Ducking amounts and times, fade time, Doppler factor, pan range, category masters |
| Voice files | Profile, line, properties | Profile nickname or "extend" an existing one; portrait animation scripts; line nickname, duration, attenuation, priority; gender and the roles the voice supports |
| System definition | Music | Calm, danger, battle tracks (chapter 01) |
| Global constants | Comm, engine sound, music | Distances and attenuations of 3.14; cross-fade lock |

Scale, for orientation: about 600 effects over 45 types and 1,200 visual
effects; about 1,800 non-voice sound records; some 60 music tracks in
calm/danger/battle sets plus mood cues; 33 reusable flight voices.
Chapter 12 has the census.

## 5. Connections

- **Needs from 01:** zones with interference, immersion, field
  enclosure, fog, ambience and music; the knowledge state.
- **Needs from 02:** the manoeuvre layer's inputs; scanner, power and
  thrust pools; hardpoints for cockpit and effects; banking.
- **Needs from 03:** which weapons are guns or launchers, lead position,
  damage events for flashes, shake and music.
- **Needs from 04:** manoeuvres and their availability; waypoints;
  docking and jump sequences that take the camera.
- **Needs from 05:** attitude of each contact; composed comms.
- **Needs from 08:** objectives, mission music overrides, scripted
  cameras, the wait for comm completion.
- **Needs from 11:** visibility radii; the messages that carry fire,
  target and manoeuvre requests.
- **Gives to 03:** the aim point and the fire requests.
- **Gives to 04 and 05:** the player's manoeuvre requests.
- **Gives to 10:** the command system and input focus rules, which base
  screens share; the window commands.
- **Gives to 12:** the record kinds of section 4.

## 6. Design notes for the builder

**Essential.**

- The command layer. Devices produce commands, buttons post commands,
  handlers consume them, contexts scope them. Rebinding, HUD buttons and
  key hints in text all follow from it.
- One cursor for pointing, steering and aiming, with the hold threshold
  to separate click from steer. This is the game's control identity. It
  makes a mouse-only space shooter playable and frees the ship's nose
  from the gun sight.
- Rate control with a chase camera that looks into the turn. Without the
  look-ahead the player steers towards a screen edge they cannot see
  past.
- The scanner as the single source for brackets, list, alerts and music.
  The interference rule turns clouds into gameplay without any further
  mechanic.
- The manoeuvre bar that lights only what is possible. It teaches the
  travel system without a tutorial.

**Feel.**

- Banking, camera lag and shake, the head lag in the cockpit.
- The 10 s music lock and the three-level music.
- Fragment-assembled radio speech. A builder can replace it with whole
  recorded lines or synthesised speech; what matters is that traffic
  talks and that distance fades it.
- The damage-direction flashes and the blink rate of the munition alert.

**Accidents of the era.**

- A cursor drawn by the game from relative mouse motion.
- Pixel constants tied to an 800-pixel-wide screen, including a
  click-versus-drag radius that shrinks as resolution grows.
- No game controller support at all; a modern builder needs a stick
  mapping, for which the "lock" mode is the natural basis.
- A voice budget taken from sound hardware, and environmental reverb
  tied to one vendor's extension.
- A shake decay per frame instead of per second.

**Simplifications and their cost.**

- Drop the cockpit view: little is lost, since the HUD does not depend
  on it and banking exists only in the chase view.
- Drop sub-targeting and the wireframe damage view: combat against large
  ships loses its tactical layer (chapter 03).
- Drop the comm portrait: keep the caption and the voice.
- Reduce the contact list to "hostile" and "all": workable, but trading
  and navigation lean on the solar filter.
- Compute contacts on the authority instead: simpler to secure in
  multiplayer, at the price of a delay in the brackets.

## 7. Open questions

1. The manoeuvre layer exposes a flag that vetoes mouse flight and hides
   the steering arrow. Its exact conditions were not established; it is
   probably set while an automatic manoeuvre or a scripted sequence owns
   the ship (inferred).
2. The exact score a sound source computes for the mixer's ranking
   (distance, category, attenuation) was not read.
3. The key map defines zoom commands with default keys, but no in-flight
   handler for them was found.
4. A "trail" manoeuvre with an adjustable distance (20 to 2,000 m, in
   20 m steps) and several evasive manoeuvres have commands and handlers
   but no button and no default key. Whether players could reach them is
   not established.
5. How the authority picks the fragments of a comm, and the per-category
   chatter intervals, belong to chapter 05 and were only skimmed here.
6. The comm window's open and close animation: a fly time is authored
   but nothing was found that uses it.
7. Whether the contact list's double-click starts a manoeuvre is
   inferred, not traced.
8. The internal format of the particle-effect libraries was not read;
   3.11 states only what game code asks of them.

## 8. Basis

Design documents: `design/assessment/06-client-app.md` sections 1 and 2;
`05-rendering.md` section 9; `07-media-script-resources.md` sections 3
and 7.

Data: `outdir/data/INTERFACE/keymap.ini`, `keylist.ini`, `hud.ini`;
`outdir/data/mouse.ini`, `cameras.ini`, `constants.ini`
(`[CommConsts]`, `[EngineEquipConsts]`, `MUSIC_CROSS_FADE_DELAY`);
`AUDIO/soundcfg.ini`, `sounds.ini`, `engine_sounds.ini`, `music.ini`,
`ambience_sounds.ini`, `interface_sounds.ini`, `voices_space_*.ini`,
`voices_recognizable.ini`, `voices_mission*.ini`; `FX/effect_types.ini`,
`effects.ini`, `effects_explosion.ini`, `beam_effects.ini`, the
`*_ale.ini` files; camera fields of `SHIPS/shiparch.ini`; one file under
`COCKPITS/`; `[Music]` and `interference` in the system files; scanner
records under `EQUIPMENT/`.

Code, client: `menu_keymap_config.h`, `menu_keymap_nicknames.inc`,
`hud_command_dispatch.h`, `main_command_handler.cpp`, `input_manager.h`,
`player_behavior.cpp` (`UpdateMouseFlightDirection`), `hud_maneuvers.cpp`,
`hud_status_core.cpp` (`HUD_Core::Draw`, `HUD_Status::OnEvent`),
`hud_target.h`, `hud_target_panel.cpp`, `hud_targetable_objects.cpp`
(`UpdateAimPoint`, `OnEvent`), `hud_contact_list.cpp`
(`RebuildContacts`), `hud_comm.h`, `hud_comm.cpp` (`UpdateSlot`,
`Dispatch`), `hud_damage_indicator.h`, `hud_cruise_progress.h`,
`hud_waypoint.h`, `hud_weapon_groups.h`, `hud_element_list.h`,
`space_state.cpp` (`UpdateSpaceState`, the view commands,
`SpaceAppArmDeathCamera`), `space_camera_game.cpp` (`HudCameraUpdate`,
`HudCameraScanVisits`, shake), `space_camera_third.*`, the other
`space_camera_*.h`, `space_scene_obj_draw.cpp` (`SpaceObjDrawPass`),
`star_sys.cpp` (music state), `menu_sound_arch.*`
(`TickClientSoundMixer`), `sound_config.cpp`, `voice_profiles.cpp`,
`effect_type_info.h`, `particle_effect.*`.

Code, shared and authority: `Common/common_cescanner.cpp`,
`common_csimple.cpp` (`get_scanner_interference`, `update_zones`),
`common_behaviormanager.cpp` (camera level status), `common_faceidea.cpp`
(banking gate); `Server/server_classes.cpp` (visibility radii);
`Content/Gcs.cpp` (skimmed).

Inference: the multiplayer limit on contacts in 3.6; the double-click
rule in 3.6; the meaning of the "silent" music state as jump or docking;
open question 1. The contents of the "important" filter were read from
code in part and completed from the element's structure. The effect
capability list in 3.11 was assembled from the callers' interfaces, not
from the particle library itself. Counts in section 4 are from scripts
over the data and are rounded.
