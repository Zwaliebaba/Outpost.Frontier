# 09 — In-flight interface and presentation

Status: reviewed
Depends on: 01, 02, 11, 12; refers to 03 to 08, 10

## 1. Purpose and player experience

This chapter covers what lies between the player and the simulation in
space: input and commands, the HUD, targeting, cameras, the composition
of a frame, and what the effect and audio systems must do.

The player flies with the mouse and the keyboard. One cursor does three
jobs. Left free, it points: a click selects an object in space or presses
a HUD button. With the steering button held, it steers: the further it
is from the screen centre, the faster the ship turns towards it. In both
modes it is the gun sight, and the guns follow it. The keyboard
carries throttle, strafe, thruster, cruise, weapons, target cycling and
the automatic manoeuvres.

The HUD answers four questions without opening a window: what state is
my ship in, what is around me, what am I shooting at, and what can I do
next.

## 2. Concepts

Terms of the glossary and of chapters 01, 02, 11 and 12 apply. New here:

| Concept | Definition |
| --- | --- |
| Command | A named player intention ("fire", "next target", "open map"). Devices produce commands; nothing else in the game reads keys. |
| Command context | A set of commands that is live in one situation: always, in space, flying, chase view. |
| Mouse flight | The cursor mode in which the cursor's offset from the screen centre is the steering input. The other mode is the free cursor. |
| Aim point | The world position the guns converge on this frame, derived from the cursor. |
| Contact | An object the player's scanner currently reports. |
| Lead marker | The point to shoot at so that a shot meets a moving contact. |
| Sub-target | One part or equipment item of the target, chosen for aimed fire. |
| Letterbox | The cinematic state: bars on, cursor and HUD off, commands refused. |
| Effect | A named bundle of a visual, a sound and optionally a light. |
| Ducking | Lowering the rest of the mix while speech plays. |
| Comm | One radio message: sender, receiver, voice profile and a list of that profile's recorded lines. |

```text
keys, buttons, wheel, cursor
      │ bindings, per context
      ▼
  COMMANDS ──► handlers in turn; the first to consume wins
      ├─ flight ────► manoeuvre layer of own ship (chapters 02, 05)
      ├─ weapons ───► fire request with aim point (chapter 03)
      ├─ targeting ─► own ship's target ──► authority
      ├─ view ──────► camera mode
      └─ interface ─► HUD panels; windows of chapter 10

own ship's scanner ──► contacts ──► brackets, contact list, music state
```

## 3. How it works

### 3.1 From device to command

Keys arrive as down, up and auto-repeat events; buttons and the wheel
are events like keys. Mouse movement arrives as relative motion, which
the game integrates into its own cursor position and draws itself.

A binding is a key or button, with modifiers, attached to a command. A
command record states which events it reacts to: hold commands (strafe,
thruster, brake, rear view, throttle) take down and up, continuous fire
takes repeat.

An event is offered in this sequence:

1. A modal window that holds the input focus sees it first.
2. "Cancel" goes to the top of a cancel stack, so the thing opened last
   closes first.
3. Otherwise the bindings of every live context are searched, and the
   command is offered to each handler registered for it until one
   consumes it.

Four contexts matter in flight: always present (pause, map, inventory,
chat; 29 commands), space (target and weapon selection; 24), flying (77)
and chase view (21). Game states switch contexts on and off, so one key
can mean different things on a base and in space.

Nearly every in-flight handler first asks whether the player may act:
not while a full-screen window is open, a cutscene hides the cursor, the
game is paused, or there is no player ship. Releases of held keys are
still honoured, so a thruster cannot stick on.

Bindings are data: the shipped map is read first, then the player's
own. The options screen offers 92 commands for rebinding.

### 3.2 The command catalogue

The data defines 146 commands, 22 of them debug aids.

| Group | Commands |
| --- | --- |
| Steering | Mouse-flight hold, mouse-flight lock, pitch, yaw and roll keys |
| Speed | Throttle up and down, wheel step, brake/reverse (hold), engine kill, cruise, thruster (hold), strafe in four directions (hold) |
| Manoeuvres | Free flight, go to, dock, join formation, cancel: each a directive to the manoeuvre layer (chapters 04, 05) |
| Guns | Fire active weapons (hold), fire straight ahead, fire weapon 1–10, toggle weapon 1–10 |
| Weapon groups | Select, fire and store group 1–6; a group is a remembered set of active weapons |
| Launchers | Missiles, torpedoes (chapter 03's heavy guided munitions), cruise-disrupting munitions, mines, countermeasures: one command per kind, firing every launcher of that kind |
| Targeting | Select under cursor, next and previous contact, closest enemy, next and previous enemy, clear, next and previous sub-target |
| Target actions | Scan cargo, tractor target, tractor all loot, hail, invite to group, request trade |
| Consumables | Use a hull or shield repair consumable |
| View | Cockpit/chase toggle, turret view toggle, rear view (hold), look keys, level-camera toggle |
| Interface | Minimise the HUD; open its status panel, target panel, contact, formation or weapon list; open map, inventory, objective, chat and the other windows of chapter 10 |

A HUD button is only another source of the same commands.

### 3.3 Mouse flight against the free cursor

The steering button is the primary mouse button, which also selects
targets, so a press must be told apart from a hold.

| State | Event | Next | Effect |
| --- | --- | --- | --- |
| Free | Press over empty space | Arming | Store the cursor position; start a timer |
| Arming | Timer reaches 0.25 s | Steering | — |
| Arming | Cursor leaves a small radius | Steering | Steering starts on the next frame |
| Arming | Release | Free | The press was a click: select under the cursor |
| Steering | Each frame | Steering | Send direction (x, y) |
| Steering | Release, lock off | Free | Send (0, 0); the ship stops turning |
| Any | Lock toggled on | Steering | Steers with no button held |
| Any | Letterbox, or riding a lane | Free | Direction reset; steering refused |

The radius is one sixteenth of the screen width at the original's
reference resolution; it does not scale properly (section 6).

The direction is the cursor offset from the centre divided by half the
screen width and height, so each component runs from −1 to 1. An option
inverts the vertical axis. Chapter 02 turns this into an axis throttle
and torque. The result is rate control: a cursor held still off-centre
holds a steady turn.

- **A click never steers and a hold never selects.** A press over a HUD
  element goes to the element.
- **The lock** makes the mouse a virtual stick. With it on, a click
  still selects.
- **Turret and rear views do not steer.** The ship flies on while the
  player looks and shoots around it.

A steering arrow that grows from the centre towards the cursor shows the
demand while mouse flight is on.

### 3.4 Keyboard steering and throttle

Pitch, yaw and roll keys build a vector with components −1, 0 or 1. It
is multiplied by a ramp that rises from 0 to 1 at 2 per second while a
key is held, so a tap nudges and a hold turns fully. With shift held and
a target selected, the ramp is replaced by `1.5 × (1 − cos²θ)`, clamped
to 0.1 to 1, where θ is the angle between nose and target.

Throttle keys move the throttle at 1.0 per second; the wheel steps it by
0.2. Two rules end cruise from here. Pressing the throttle-down key
drops cruise at once, whatever the throttle value. And any throttle of
exactly zero, however reached, drops it; a wheel step down therefore
ends cruise only when it arrives at zero. Both rules live in the
interface, not in the engine equipment (chapter 02, 3.8).

Cancel returns the ship to free flight and drops cruise. In the turret
view it leaves that view instead.

### 3.5 Aim point and firing

Each frame the presentation computes one aim point and gives it to every
gun; chapter 03 describes how guns turn to it and what they hit.

1. Cast a ray from the camera through the cursor and note the first
   object hit, ignoring shield bubbles and the player's ship. Only an
   object slower than 5 m/s counts, so the aim does not jump onto
   passing ships.
2. If the cursor is within about 4.5 % of the screen size of a lead
   marker (5.5 % for the selected target), that contact is the intended
   target. Within about 3 %, unless one setting of the aim option
   forbids it, the aim point snaps to the lead position. Otherwise it
   lies on the cursor ray at the marker's distance.
3. With no marker near, it lies on the cursor ray at the distance of the
   object hit, or else at the range of the longest-ranged active gun.

The cursor changes shape and colour with what it would hit (neutral,
hostile, friendly). Fire is refused while it is over the interface.

While fire is held, every active gun with unguided munitions fires each
frame it can (chapter 03). Guided launchers, mines and countermeasures
have their own commands, so holding fire never empties the missile rack.
A weapon group stores the current on/off pattern under a number; groups
belong to the character and are kept by the authority.

### 3.6 Contacts and targeting

**What the player can know.** Each tick the ship's scanner (chapter 02)
lists the targetable objects within

```text
effective range = scanner range × (1 − interference × immersion)
```

Interference (authored 0.01 to 0.8) and immersion come from the ship's
physical zone (chapter 01); the product is capped at 1. A target is
hidden by its own zone too: if its interference is higher than the
observer's, it must also lie within the range computed with its own
value. Cargo scanning uses a second, shorter range and the larger of the
two interferences. Scanner ranges in the data are 2,500 and 5,000 m.

Worked example: a 2,500 m scanner in clear space, a ship deep in a cloud
of interference 0.7. The ship appears only within 2,500 × 0.3 = 750 m.
From inside that cloud the player sees everything only within 750 m.
Clouds are therefore places to hide and to be ambushed in.

In multiplayer the authority tells a presentation only about objects
inside its visibility radii (chapter 11: 2,500 m for objects no player
owns, 5,000 m for another player's, 10,000 m for a group member's), so
those radii cap what a scanner can report (inferred).

**Marking.** Every contact gets a bracket sized to its screen extent and
coloured by attitude; hostile brackets also differ in shape. An
off-screen contact becomes an arrow at the screen edge. The target gets
a heavier reticle with shield and hull bars, a name, a range read-out
and a lead marker.

**Selecting.**

| Method | Rule |
| --- | --- |
| Click | The bracketed contact under the cursor; else the object the cursor ray hits; else the nearest pickable object whose enlarged bracket holds the cursor. A hostile contact under the cursor wins over a nearer object. |
| Closest enemy | The nearest hostile contact; contacts on a watch list first (set by missions, inferred) |
| Next, previous | Cycle through hostiles, or through the contact list in its current filter |
| Double-click a list row | Select, and switch the panel to the target view |

Selecting sets the target on the player's ship and reports it to the
authority (chapter 11). The player can then step through the target's
parts and external equipment, or click one in the target panel; guns
aim at that sub-target.

**Collecting loot.** Two commands drive the tractor (chapter 03 has the
rule). "Tractor target" pulls in the selected loot, within the tractor's
authored reach of 1,500 m. "Tractor all" addresses every loot object
within 1,000 m, a figure fixed in the interface and shorter than the
reach, most valuable first: items without volume, then repair
consumables, then by price. The status panel shows its loot button only
while some loot within those 1,000 m still fits in the hold, the ship is
not in cruise and not in a lane; when the hold is full the button
disappears. A pull shows as a beam and ends with a sound. A failed one
is announced by the interface voice: "out of range", or a general
tractor failure for the other causes, a full hold presumably among them
(inferred).

**Learning the map by flying.** Once a second the presentation checks
what the player has come near and updates the knowledge state, by the
rules of chapter 01 (3.10): a solar whose surface is closer than the
larger of 7 radii and 10,000 m, among solars within 20,000 m; a zone
within 7,500 m; and the faction of any ship within scanner range. Each
new fact is reported to the authority.

### 3.7 HUD elements

The HUD is a list of elements named in one definition file. Each
watches the player's ship and draws itself. All hide in letterbox and
with no player ship.

| Element | Shows |
| --- | --- |
| Gauges | Power pool, shield and hull as three bars of 16 segments; speed along the nose, shown as 300 in cruise; thrust pool in percent, in a warning colour below 25 % |
| Markers and reticle | 3.6; this element also computes the aim point and resolves clicks |
| Alerts | Incoming guided munition; ship on your tail within 500 m; radiation; network lag above 600 ms, red above 1,200 ms. The munition alert blinks with a period of distance × 0.001 s, at least 0.1 s, and triggers one spoken warning. |
| Target panel | A wireframe view of the target, its parts coloured by damage in four steps; name, faction, range; buttons for scan, tractor, next enemy, trade. Switches to the contact list or the formation list. |
| Status panel | The same wireframe for the player's ship; weapons with ammunition and on/off state; repair consumable counts; a loot button (3.6) |
| Manoeuvre bar | One button per manoeuvre, lit only if the manoeuvre layer says it is possible for the current target; a cruise indicator |
| Damage direction | Four edge flashes that light on the side a hit came from and fade in 0.7 s |
| Waypoint | A marker at the next point of the plotted route; advances within 150 m (chapter 04) |
| Comm window | An animated portrait of the speaker with name and caption (3.14) |
| Captions | Cruise charge in percent; the active view or cursor mode, with the key that leaves it |

Text that names a key is built from the live binding. The contact list
refreshes every 3 s.

### 3.8 The contact list

Eight visible rows, each with an icon for the kind of object (about 16
kinds), the range and the name, coloured by attitude. Five filters
choose the rows: important (hostile ships, other players, hostile solars
close by), ships, solars (stations, jump points, lane rings, planets),
loot, and all.

Ships must be within scanner range; other players' ships are listed at
any range. Solars and loot come from a 14,000 m sphere and from the list
of large solars; most kinds of solar are listed only once visited.

### 3.9 Cameras

One camera is active at a time.

| Mode | Placement | Entered by |
| --- | --- | --- |
| Cockpit | At the hull's cockpit hardpoint, a cockpit model around the view; the head lags under acceleration and turn | View toggle |
| Chase | Behind and above the ship (below) | View toggle; default |
| Turret | On a tether behind the ship, looking anywhere within ±60° of pitch; aimed by keys or cursor | Turret toggle |
| Rear | 5 m up, 20 m back, looking backwards | Held key |
| Death | Shakes for 2 s with falling amplitude while drifting with the wreck at up to 200 m/s | Loss of the ship |
| Docking | A fixed outside pose that keeps aiming at the ship; letterbox; fade to black timed to end with the sequence | Docking sequence (chapter 04) |
| Scripted | Driven by a scene script, or parked at a pose it gives; letterbox (chapter 10) | Mission or cutscene |

"Player view" is cockpit or chase by stored preference. Rear and turret
are overlays that return to it.

**The chase camera** makes mouse flight readable.

- **Offset.** Authored per hull, from 6 m up and 25 m back to 13 m up
  and 52 m back.
- **Speed lag.** The offset grows by 0.05 s of travel at a smoothed
  copy of the ship's speed, plus a slack that builds while the ship
  out-accelerates the camera. Acceleration shows as the ship pulling
  away.
- **Turn look-ahead.** Its orientation is the ship's, turned further by
  the axis throttle times an authored angle: 17 to 23° sideways, 5 to
  10° up, 20 to 30° down. The view swings into the turn ahead of the
  ship (direction inferred). An authored angular acceleration limits the
  camera's own turn rate.
- **Levelling.** The camera keeps its own "up" and gives it to the
  manoeuvre layer. Banking (chapter 02) is active only in this mode,
  relative to the camera's up. A command switches levelling off.

Flight cameras shake on damage with a small random rotation: amplitude
0.02 for a hull hit, 0.005 for a shield hit, decaying by 0.001 per frame.

### 3.10 What a frame is composed of

1. **Advance the presentation's copy of the world**: objects, animation,
   physics, beams, collisions (chapters 02, 11).
2. **Update the camera**, then the system's ambience and music state.
3. **Run the audio mixer** with the camera as listener.
4. **Update mouse flight**; begin the picture.
5. **Draw the scene through the camera:**
   - Decide whether the camera is outside, partly inside or deep inside
     a field (chapter 01). Deep inside a nebula the star background is
     skipped, the far limit shrinks to the fog distance and the clear
     colour is the fog colour.
   - Background: a star sphere in up to three layers by quality option,
     and distant stand-ins for asteroid fields, with a far limit of
     250,000 m.
   - Collect objects: everything within 10,000 m of the camera from a
     spatial index, plus the short list of large solars (stars, planets)
     at any distance, plus ships, munitions and debris. The default draw
     limit is 20,000 m; a system may set its own.
   - Pick a level of detail per object from projected size and authored
     distances; sort.
   - Draw solid objects, each lit by the system ambient blended towards
     the nebula's colour plus the few nearest lights from a grid.
   - Draw effects, fogged inside a field.
   - In the cockpit view, draw the cockpit model in its own slice of the
     depth range.
6. **Clear depth and draw the HUD's three-dimensional elements**:
   brackets, reticle, arrows, wireframes.
7. **Draw windows**: HUD panels, any open window, then the cursor.
8. **Draw the fade overlay**; remove dead objects; apply a pending
   change of system or a landing.

During a jump, step 5 draws only the player's ship, its effects and the
jump tunnel.

Requirements on any engine: a sorted transparent pass after the opaque
one; per-object light selection; distance fog with colour and range per
zone; a background pass independent of world scale; an interface pass
with its own camera and cleared depth; small viewports that render a
model with another camera (target view, comm portrait); a full-screen
fade. The original has no post-processing.

### 3.11 What the effect system must do

An effect record names a visual from a particle-effect library, a sound,
and for explosions a light. Code and data refer to effects by nickname
only: a gun names its muzzle flash, trail and impact; a shield its hit
effect; an engine its flame; a fuse (chapter 03) its explosions.

The system must:

- play an effect once at a point, or attach it to a hardpoint and loop
  it until stopped;
- stop softly, letting particles die out;
- take an intensity from 0 to 1 at run time (flame with throttle, dust
  with speed);
- draw beams and bolts from a compact description of lengths, widths
  and colours;
- start a positional sound and a temporary light with the visual;
- cull and degrade by rule, per effect type.

About 45 effect types carry the rules:

| Field | Meaning | Range |
| --- | --- | --- |
| Priority | Skipped when the detail option is below it | 0.1 to 1 |
| Radius | Bounding size for culling | 5 to 1,250 m |
| Off screen | Destroyed or kept; frozen or simulated | Two flags |
| Run time | Lifetime; negative means until stopped | −1, 0.5 to 20 s |
| Distance pair | Near and far limit; the detail option slides the cut-off between them | 50/150 to 2,000/7,000 m |
| Detail class | Maps projected size in pixels to a detail blend, and to nothing below a minimum | Six classes |

The rule to keep: effects are numerous and cheap to drop, each judged by
its type every frame.

### 3.12 Sound

A sound record gives a file, a category, an attenuation in dB, a pair of
distances (full volume inside the first, silent beyond the second),
whether it is positional or streamed, and optional pitch variation.
Typical pairs are 20 to 70 m and 200 to 500 m around ships, and
1,750/6,000 m for the loudest effects.

- **Five categories**, each with a master volume: effects, interface,
  voice, music, ambience.
- **A voice budget.** Each frame every playing source computes a score.
  The mixer keeps the best N (8 to 128 by option); the rest fall silent
  but keep their place in time.
- **The listener** is the active camera, with its velocity for Doppler.
- **Engine sound follows throttle** in pitch and volume (inferred from
  constant names).
- **Ducking.** Speech lowers everything else: by 12 dB for a comm to the
  player, 9 dB for other in-flight speech, 10 dB in cutscenes, falling
  in 0.2 to 0.25 s and recovering in 0.75 to 0.85 s.

### 3.13 Music follows the situation

Each system names a calm, a danger and a battle track. A zone may
replace the calm track while the player is inside it. A mission may
override any of the three or force one track (chapter 08).

| State | Condition |
| --- | --- |
| Calm | None of the below |
| Danger | The scanner lists a hostile contact (checked every 2 s) |
| Battle | This frame the player damaged a hostile, was damaged by one, or selected one |
| Forced | A mission set an override track |
| Silent | The ship is in a jump or docking sequence (inferred) |

A state change fades to the new track (3 s) and locks
the state for 10 s, after which the conditions are evaluated again. In
battle each new battle event restarts the 10 s, so battle music holds
until ten quiet seconds have passed. The lock prevents flicker when one
enemy drifts in and out of range.

### 3.14 Voice lines and comms

A voice profile is a speaker with a set of voice lines. A reusable
flight profile holds 400 to 500 lines: docking requests and replies,
combat calls, acknowledgements, and many fragments: numbers, formation
call signs, and one spoken reference per faction, ship class and base.

The authority composes a comm (chapters 06 and 08 decide who says what):
sender, receiver, voice profile, caption and up to 16 line identifiers,
such as "this is" + call sign + number + a docking request. The
presentation only plays it.

- **Two channels.** Comms sent or received by the player's ship use the
  channel with the portrait and caption. Everything else is overheard
  chatter on a second channel with no window, panned by the speaker's
  direction. Each channel queues 16 comms.
- **Priority.** Each line has a priority, from −6 for background chatter
  to 1 for mission speech; a comm takes its highest. A low-priority comm
  arriving while something more important plays is dropped; one that is
  playing is cut off by a more important arrival; an important one
  waits.
- **Distance.** Chatter fades linearly from −2 dB to −16 dB at 3,500 m,
  background chatter from −8 dB to −24 dB; beyond that it is not played.
  Comms to the player are not faded.
- **Completion.** In single player the presentation reports each comm
  as finished or cancelled, because scripted missions wait for speech to
  end (chapter 08).

### 3.15 Authority and presentation

Everything in this chapter runs on the presentation. It reports to the
authority: fire requests with the aim point; target changes; manoeuvre,
dock and formation requests; use of consumables, tractor and cargo scan;
knowledge gained; weapon groups; comm completion. The player's ship is
moved by the presentation's own simulation and its state is sent up
(chapter 11), so steering has no network delay.

## 4. Data that defines it

| Record | Principal fields |
| --- | --- |
| Command | Nickname, name and help text identifiers, events it reacts to, default bindings |
| Command context, options group | Nickname or title, list of commands |
| HUD element | Mostly empty: the record's presence creates the element. The comm window carries positions and sizes; the manoeuvre bar lists (behaviour name, text identifiers, two icons). |
| Camera mode | Field of view (70° in flight), near distance |
| Ship archetype, camera fields | Chase offset, three turn angles, angular acceleration |
| Cockpit, per hull family | Cockpit model, head-turn limits, turret tether and speeds, rear-view position |
| Effect type, detail class | 3.11 |
| Effect | Nickname, effect type, visual, sound, optional light |
| Visual effect | Nickname, library file, identifier inside it, textures |
| Sound | 3.12 |
| Audio configuration | Ducking amounts and times, fade time, Doppler factor |
| Voice profile | Nickname, or the profile it extends; portrait animations; gender; supported roles |
| Voice line | Nickname, duration, attenuation, priority |
| Global constants | Comm distances and attenuations, music lock |

## 5. Connections

- **From 01:** zones with interference, immersion, fog, ambience and
  music; the knowledge state.
- **From 02:** the manoeuvre layer's inputs; scanner, pools, hardpoints;
  banking.
- **From 03:** guns against launchers, lead position, damage events.
- **From 04:** manoeuvres and their availability; waypoints; sequences
  that take the camera.
- **From 06:** composed comms (its section on radio chatter).
- **From 07:** the attitude of each contact.
- **From 08:** objectives, music overrides, scripted cameras.
- **From 11:** visibility radii; the messages for fire, target and
  manoeuvre.
- **To 03, 04, 05:** the aim point; fire and manoeuvre requests.
- **To 10:** the command system and focus rules, shared by base screens.

## 6. Design notes for the builder

**Essential.**

- The command layer: devices produce commands, buttons post them,
  handlers consume them, contexts scope them. Rebinding, HUD buttons and
  key hints all follow from it.
- One cursor for pointing, steering and aiming, with a hold threshold to
  separate click from steer. This is the game's control identity: it
  frees the gun sight from the ship's nose.
- Rate control with a chase camera that looks into the turn.
- The scanner as the single source for brackets, list, alerts and
  music. The interference rule turns clouds into gameplay with no
  further mechanic.
- A manoeuvre bar that lights only what is possible.

**Feel.** Banking, camera lag and shake. The three-level music and its
lock. Fragment-assembled radio speech: whole recorded lines would do,
provided traffic talks and distance fades it.

**Accidents of the era.** Pixel constants tied to an 800-pixel screen,
including the click radius of 3.3: 50 pixels × 800 / width, so it
shrinks as resolution grows. No gamepad
or joystick support; the lock mode is the natural basis for a stick
mapping. A shake decay per frame instead of per second.

**Simplifications.**

- Drop the cockpit view: little is lost; banking exists only in chase.
- Drop sub-targets and the wireframe: fights with large ships lose their
  tactical layer (chapter 03).
- Compute contacts on the authority: easier to secure in multiplayer, at
  the price of delayed brackets.

## 7. Open questions

1. The manoeuvre layer exposes a flag that vetoes mouse flight and hides
   the steering arrow. Its conditions were not established; probably an
   automatic manoeuvre or scripted sequence owns the ship (inferred).
2. The score a sound source computes for the mixer's ranking was not
   read.
3. Zoom commands have default keys, but no in-flight handler was found.
   A "trail" and several evasive manoeuvres have handlers but no key.
4. How the authority picks the fragments of a comm, and how often NPCs
   chatter, belong to chapter 06 and were only skimmed.
5. What starts the docking camera, and with what pose, was not traced.
6. The format of the particle-effect libraries was not read; 3.11 states
   only what game code asks of them.

## 8. Basis

Design: assessments 06 (1, 2), 05 (9), 07 (3, 7).

Data: `INTERFACE/keymap.ini`, `keylist.ini`, `hud.ini`; `mouse.ini`,
`cameras.ini`, `constants.ini`; the INI files in `AUDIO/` and `FX/`;
camera fields of `SHIPS/shiparch.ini`; one file under `COCKPITS/`;
`[Music]` and `interference` in system files; scanner and tractor
records.

Code, client: `menu_keymap_config.h`, `hud_command_dispatch.h`,
`main_command_handler.cpp`, `input_manager.h`, `player_behavior.cpp`,
the `hud_*` files, `space_state.cpp`, the `space_camera_*` files,
`space_scene_obj_draw.cpp`, `star_sys.cpp`, `menu_sound_arch.*`,
`sound_config.cpp`, `voice_profiles.cpp`, `effect_type_info.h`,
`particle_effect.*`, `client_equip_fx.cpp` (tractor feedback).

Code, shared and authority: `Common/common_cescanner.cpp`,
`common_csimple.cpp`, `common_behaviormanager.cpp`,
`common_faceidea.cpp`; `Server/server_classes.cpp`; `Content/Gcs.cpp`
(skimmed).

Inferred: the multiplayer limit on contacts; the full-hold feedback; the
watch list's source; the direction of the chase look-ahead; the
engine-sound rule; the "silent" music state. The capability list of 3.11 comes
from the callers, not from the particle library.
