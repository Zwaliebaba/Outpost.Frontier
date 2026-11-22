# 10 — Bases, scenes, menus and text

Status: draft
Depends on: 01, 02, 07, 08, 09, 11, 12

## 1. Purpose and player experience

Everything the player does outside the cockpit happens here: the screens
before play, the visit to a base, the map, and all text.

A base is the game's pause between flights. The player lands, watches the
ship set down, and is then inside a small set of rooms shown as staged 3D
scenes with a fixed camera. There is no walking. A bar of buttons along
the screen edge moves the view from room to room, and each room offers
its services: the landing pad gives repair and launch, the bar gives
people to talk to, news and a job board, the dealers give the trade
screens of chapter 07. People stand or sit at authored spots, fidget, and
answer a click with a short acted conversation. Campaign scenes play in
the same rooms with the same machinery, only with the interface hidden
and the picture letterboxed.

The front end is a short column of choices over a live scene: new game,
load, multiplayer, options, exit. Multiplayer adds a host list, a
character list and a character creation page. In play, one bar of icons
opens the map, the inventory, the mission log, the player's standing and
help; a pause menu saves, loads and leaves.

The reason for building a base as scenes instead of menus is pacing and
place: the same five services feel different on a planet, a station and
an outlaw hideout because the set, the people and the ambience differ,
while the rules behind the buttons stay identical.

## 2. Concepts

Room, base, scene script, information card, text identifier, knowledge
state, character, account and session are used as in chapters 00, 01 and
11; record, reference, text catalogue and asset as in chapter 12.

| Concept | Definition |
| --- | --- |
| Set | The 3D stage of a room, with its lights, cameras and markers. It is itself a scene script that never ends. |
| Setpoint | A named marker in a set: a camera position, a place for a person, a pad for a ship. |
| Placement | A setpoint for a person. Its name encodes who may use it (player or NPC), a role, and a posture (stand, sit low, sit high, special). |
| Start room | The room a visit begins in; it holds the landing pad. |
| Virtual room | A function that a base without a room of its own for it stages inside another room, by a second camera and a different set of buttons. |
| Room hotspot | A record of a room that becomes one button of the room bar, with a behaviour. |
| Room bar | The row of room buttons and, above the current one, the service tabs of that room. |
| Character hotspot | The screen rectangle of a person in a room; hovering and clicking go to that person. |
| Entity | One thing a scene script controls: set, prop, person, camera, light, sound, marker, path, particle system, sub-scene. |
| Event | One timed instruction of a scene script acting on one or two entities. |
| Sequence | Scene scripts played one after another as a unit, with rules for branching on the player's answer. |
| Costume | A person's appearance: body, head, two hands, up to eight accessories. |
| Page | One front-end screen, laid out by a small text file of widgets. |
| Panel | An in-game window opened from the icon bar or from a service tab. |
| Format string | A text identifier plus typed values, expanded into a sentence on the presentation side. |
| Rich text | Text with inline markup for style, alignment and paragraphs; cards are rich text. |

```text
base ── start room, rooms ×1..5
room ── set (scene script) ── cameras, lights, setpoints
     ├─ ambient / traffic / one-shot scene scripts
     ├─ sound: looped ambience, optional theme
     ├─ placements: player figure, player ship, ships for sale
     └─ hotspots ──► room bar: room buttons + service tabs
authority ──► per room: inhabitants, their behaviours, extra scenes
          ──► per base: job offers, news            (chapter 08)
inhabitant ── costume, placement, idle script, behaviour ──► sequence
sequence ── scene scripts ×n ── entities bound to set, people, ship
text identifier ──► text catalogue ──► string or rich-text card
format string ── template identifier + values ──► sentence
```

## 3. How it works

### 3.1 A base as a set of rooms

A base definition names its rooms and its start room. Five room functions
exist: landing place (a pad on a planet, a deck on a station), bar,
commodity trader, equipment dealer and ship dealer.

| Rooms of a base | Share of 197 bases |
| --- | --- |
| Landing place and bar | 63 % |
| Landing place, bar and one dealer room or a second deck | 12 % |
| All five functions as separate rooms | 9 % |
| Landing place only | 10 % |
| Other mixes of three or four | 6 % |

Every base still trades (chapter 07 decides what). Where a dealer has no
room, the landing place hosts it as a virtual room. Only 31 bases have a
ship dealer room; the rest sell no hulls or stage the dealer on the deck.

| Room | What it offers |
| --- | --- |
| Landing place | The player's ship on its pad; landing and launch scenes; launch; repair entry; on small bases the dealers as virtual rooms |
| Bar | People to talk to (chapter 08), the news screen, the job board |
| Commodity trader | A desk with the trader; opens the commodity screen |
| Equipment dealer | The player's ship on a stand, a desk; opens the equipment screen and repair |
| Ship dealer | Up to three ships for sale on pads; opens the ship screen |

### 3.2 What a room definition holds

- **Set script** and optional looping set animations.
- **Scene list**: scripts to run on entry, each marked ambient (loops for
  the whole stay), traffic (repeats) or once, with an optional detail
  priority (3.6). A base may add exterior terrain scripts by size.
- **Sound**: a looping ambience and an optional theme, either of which
  can be marked to play once.
- **Default camera**, by setpoint name.
- **Placements**: one for the player's figure, with an optional
  walk-in script; one for the player's ship, with landing and launching
  scripts; up to three for ships on sale. A dealer room with a ship stand
  may name a script that plays stock being carted to the ship.
- **Hotspots** (median 7 per room, up to 15), described next.
- **Blinking light sets**: lists of model hardpoints with colour, size,
  blink time and pause, for pad and skyline lights.

Where NPCs may stand is not in the room definition. It comes from the
placement setpoints of the set; the authority reads the same set to seat
its inhabitants.

### 3.3 The room bar and hotspot behaviours

On entering a room the presentation rebuilds the bar from that room's
hotspots: up to nine room buttons and nine service tabs. A hotspot has a
name (a text identifier for its tooltip and icon), a behaviour, and
optionally a target room, a virtual room it belongs to, a virtual room it
sets, and a pair of state numbers.

| Behaviour | Effect of pressing the button |
| --- | --- |
| Room switch | Fade to black over 1 s, tell the authority, load the target room, fade in |
| Virtual-room switch | Fade, cut the camera to that function's desk camera, move the player's figure to its setpoint there, rebuild the bar; the room does not change |
| News | Slide the news panel open; the tab exists only if the base has news |
| Job board | Slide the job list open |
| Repair | Open the repair panel; enabled only while the ship needs repair. Pressed elsewhere, it first takes the player to the equipment room or virtual room |
| Front desk | Camera back to the desk; closes an open dealer screen |
| Commodity, equipment or ship dealer | Camera moves to the dealer view over 2 s (eased), then the trade screen opens; the base's stock becomes known to the map |
| Move along | Step the camera along a numbered track of setpoints (the equipment room's ship stand) |
| Launch | Added automatically in the start room when the player owns a ship |

The desk and the dealer tabs form a two-state pair: each hotspot declares
the state it shows in and the state it sends, so only one of the two is
offered at a time. A hotspot may also name a scene script to play for a
given state change instead of the plain camera move. While a fade, a
camera move or a panel animation runs, the cursor is hidden and input is
ignored.

### 3.4 People in a room

The authority sends, per room, the inhabitants (chapter 08 chooses them):
a key, a name identifier, a faction, a costume, an idle script and
optionally a behaviour. The presentation builds each person from the
costume, stands them on a free placement and loops the idle script.

A person with a behaviour or a dealer role has a character hotspot.
Hovering shows the name and changes the cursor to tell the player what
kind of talk to expect: brush-off, bribe, rumour, job, information,
campaign, or one of the three dealers. Clicking

1. marks the person's faction as known (chapter 01),
2. if the person is a dealer standing in another virtual room, switches
   there first,
3. tells the authority which inhabitant was selected, and
4. plays the behaviour as a sequence (3.7), or opens the dealer's screen.

A fixed role name ties a person to a function: bartender, trader,
equipment dealer, ship dealer. Scripts address people by these roles or
as "player".

### 3.5 Scene scripts: what a script can control

A scene script is pure data: a duration, a list of entities and a list
of events. Playback is a clock in milliseconds; each event has a start,
a duration and optionally a parameter curve (step, linear, smooth or
spline) that shapes progress over its span.

| Entity | What the host binds it to |
| --- | --- |
| Scene | The script itself: ambient light, fog, reverb, a priority |
| Compound | A rigid model: the room set, a prop, a ship |
| Deformable | A skinned person, built from a costume |
| Camera, monitor | A view; the script with the highest priority owns the room camera |
| Light | Point, spot or directional; colour, range, cone, on or off |
| Sound | A sound or voice line, positional or not, streamed or loaded |
| Marker | A named point: a setpoint |
| Motion path | A spline for position and orientation |
| Particle system | A named effect (chapter 09) |
| Sub-scene | Another script started from this one |

| Event | Effect |
| --- | --- |
| Set camera | Switch the active view |
| Start motion | Play a named animation on a person: start offset, blend time, speed, weight, loop, heading |
| Start inverse kinematics | Make a limb or the head reach or look at a target |
| Path animation | Move an entity along a motion path |
| Spatial animation | Move or turn an entity, or keep it aimed at another |
| Attach, connect hardpoints | Parent one entity to another; join two models at named hardpoints |
| Start sound, audio animation | Play; fade volume, pan and distance range |
| Light, camera, fog, reverb, particle animations | Change those properties over time |
| Start particle system, start sub-scene | Trigger |
| Floor height | Change the ground level a person stands on |
| Subtitle | Show a text identifier for a span |
| User event | An opaque signal to the host |

Scripts stay reusable through late binding. An entity's template is
resolved when the script starts: a model nickname, a costume, a voice
line of whoever speaks, or a reserved name such as "the player's ship",
its engine effect or its engine sound. A person entity names an actor
(the player, a role, or "the NPC in this conversation") and takes over
the figure already in the room instead of creating a second one. Marker
and camera entities with reserved name prefixes are replaced by the
room's setpoints for the speaker's place, so one conversation clip works
at every table of every bar.

Each entity may carry a named detail priority. A single detail setting
between 0 and 1 is compared against per-name thresholds (0.17, 0.51 and
0.83 in the shipped table), so extras, traffic, carts, weather and
secondary lights disappear on slow machines without re-authoring. The
same setting caps the frame rate inside bases at 30 or 20 per second.

Scene scripts also drive the camera and one "main object" in space, for
flight cutscenes, lane rides and jumps (chapter 09).

### 3.6 Kinds of script, as authored

| Kind | Count | Typical length (inferred from the scripts' constants) |
| --- | --- | --- |
| Room sets | 168 | endless |
| Room ambience and traffic | 318 | looping |
| Player walk-in | 70 | 8 s |
| Landing, launching | 27 each | 16 s, 13 s |
| Stock-cart scenes | 15 | 9 s |
| Conversation clips | 635 | 2.6 s, at most 7 s |
| Idle fidgets, staff idles | 25 | 25 s to 4 min |
| Campaign scenes on bases | 124 | 22 s, up to 3.5 min |
| Campaign scenes in space | about 80 | not measured |
| Front-end scenes | 4 | up to 12 min loops |

A conversation is assembled from clips. An index groups them by line
kind (63 kinds), speaker sex and posture (standing or sitting), with one
to four interchangeable takes each; a second table bars takes that a
given voice did not record. Each clip is one camera angle, one body
animation and one voice line. Chapter 08 says which lines are chosen.

### 3.7 Sequences

A sequence plays up to 32 scripts in order. Options per sequence: loop;
letterbox with cursor and hotspots locked; fade to black at the end;
restore the room camera and the player's pose afterwards; fade out the
room's ambience or theme while it plays; move to another room when it
stops; report a scene number to the authority when it stops.

Each entry has a kind, and the kinds give a conversation its branches:

| Entry kind | Behaviour |
| --- | --- |
| Plain | Play, then go to the next entry |
| Offer | Play, then show an accept or decline box and wait |
| Accept branch | Played only after "accept" |
| Decline branch | Played only after "decline" |
| Information | Play, show a text box, wait for it to be closed; the map is focused on the place the text names |
| Bystander branch | In a multiplayer group offer, played for members who did not answer |
| Traffic | Repeats |

The answer to an offer goes to the authority as a mission response. In a
multiplayer group offer the script waits for the authority's verdict and
then takes the accept branch only for the player who accepted. Campaign
scenes use the same form, authored as sets of offer, accept, decline,
re-offer and decision scripts.

The cancel command skips a letterboxed sequence that is marked skippable:
each remaining script is run to its end state at once, stopping at the
next offer or information entry, so a skipped scene leaves the room as
the played scene would.

### 3.8 The base visit as a state machine

| State | Event | Next state |
| --- | --- | --- |
| Arriving | Base and start room loaded behind the loading screen; the authority's room lists are complete | Authority scene, if an inhabitant's behaviour is marked "start on entry"; else landing scene, if arriving from space and the room has one; else idle |
| Landing scene | Ends or is skipped | Idle; a caption names the base |
| Idle | Room button | Fade out, room exit and enter sent, room loaded, fade in, idle |
| Idle | Virtual-room button | Fade, camera cut, bar rebuilt, idle |
| Idle | Dealer or desk tab | Camera move, then trade screen or idle |
| Idle | News, job board, repair or ship tab | Panel open; closing returns to idle |
| Idle | Click on a person | Conversation |
| Conversation or scene | Offer or information entry reached | Waiting for the player |
| Conversation or scene | Last script ends | Idle, in another room if the sequence names one |
| Idle, start room | Launch pressed; engine, power plant and scanner are mounted | Launch scene; otherwise a spoken and written refusal |
| Launch scene | Ends | Briefing if an objective route exists, else loading and space |
| Briefing | Continue | Loading and space |

Loading a saved game on a base skips the landing scene and the caption.
The briefing is the navigation map alone on screen, showing the route and
the objective text, with a continue button and, for a job, an abort
button. Launch and landing on the authority's side are chapter 11's.

Per frame on a base: advance sequences by whole milliseconds (the
remainder is carried), advance a queued room change, update room objects,
letterbox and fade, re-test the cursor against character hotspots, draw
the room, then the interface on top, then the fade.

### 3.9 Screen flow from start to play

The presentation is always in one of six top-level states.

| State | Leaves to |
| --- | --- |
| Front end | Loading, when a character is chosen; quit |
| Loading | Base, if the character is docked; else space |
| Base | Briefing or space on launch; front end; quit |
| Briefing | Space |
| Space | Base on landing; front end; quit |
| Reload | A single-player load requested from inside the game reconnects to the in-process authority and goes to base or space |

At start the program plays up to three films (two logos and an
introduction, skippable), then shows the main menu over one of three
authored menu scenes picked at random, which run as ordinary rooms
without an authority.

| Front-end step | Offers | Goes to |
| --- | --- | --- |
| Main menu | New game, load game, multiplayer, options, exit; build version | The steps below |
| New game | Nothing: the restart template is selected | Loading |
| Load game | List of saves with description and time; for the selected one ship, level, money and location; load, delete | Loading |
| Options | Three groups (interface, audio, video; 37 settings) and the key bindings of chapter 09 | Main menu |
| Multiplayer | See 3.10 | Loading |

A single-player session is started as soon as the main menu appears, so
the save list is the in-process authority's character list (chapter 11).

### 3.10 The multiplayer front end

| Step | Content |
| --- | --- |
| Account | Shown once, when no local account identifier exists or it fails verification: an explanation and the identifier field |
| Host list | A sortable table with a filter dialog and a refresh button. Columns: name, address, visited before, ping, players (current / maximum), version, on the local network, options (password required; players can damage players). Below: the host's description and its restrictions (no new accounts, password). Hosts of another version are greyed and cannot be selected |
| Connect | Log in with the account identifier; a password prompt where required |
| Character list | The account's characters (at most five): name, level, money, ship type, location; the host's message of the day and details; load, new, delete with confirmation, back |
| New character | A name of up to 23 characters; a preview of the pilot's figure; text stating start base, money and ship package. The data model allows several pilots, packages and starting factions; the shipped data has one of each, so the name is the only choice |

From the in-game pause menu a multiplayer player can return to the
character list or to the host list without restarting.

### 3.11 In-game panels, pause and saving

One icon bar is present on bases and in space: map, inventory, mission
log, player status (level, standing with each known faction), help, and
in multiplayer chat and group. An icon slides out and its panel scales
open; opening one closes another. Information panels overlay play without
stopping it.

The pause menu offers resume, load, options and exit, plus save in single
player and "change character" and "change host" in multiplayer. Save is
offered only while docked and while no unskippable scene runs. The save
window lists saves by name and date, takes a description, and asks before
overwriting; the automatic save and the restart slot cannot be
overwritten by hand. The presentation only asks; the authority writes
(chapter 11).

### 3.12 The navigation map

The map has three views: the universe (one marker per known system,
lines for known connections, the player's position), a system, and a
zoomed part of a system that can be dragged. A system is drawn on an
8 × 8 lettered and numbered grid; text refers to a place without a name
by its grid cell.

| Mode | Shows |
| --- | --- |
| Physical | Solars, lanes, fields and clouds |
| Political | Population areas, tinted by that faction's attitude to the player |
| Patrol | Patrol paths, tinted the same way |
| Mining | Zones that yield loot |
| Mission | The zones the current objective names |
| Legend | The key to the symbols |
| Known bases | Every visited base in every visited system, sortable by base or system; choosing one opens its system |
| Card | The information card of the chosen object |

Clicking a marker offers tools around the cursor: zoom, go to that
system, show its card, set a waypoint (up to 99, chained in order), plot
the best path, select it as the flight target (own system only), clear
waypoints. Three rows list the current objective steps. In multiplayer
the map also marks group members.

What is drawn follows the knowledge state of chapter 01: unseen systems,
solars and zones are absent, lanes are hidden until something in the
system is known, and a base's card lists commodities, equipment and
ships only once the player has docked there. Knowledge can also arrive
by text: when the interface shows a text whose identifier is in the
knowledge table (a job offer, a news item, a rumour, an information box),
every object listed under it gains the listed flags, and the authority is
told so that it is saved. A second table extends a base's card with a
further text once that flag is set.

### 3.13 What the UI toolkit must provide

- A tree of windows with position, size, draw layer, visibility, enable
  state and a name by which other code finds them; creation by class
  name from layout data.
- Resolution-independent layout: positions and sizes as fractions of the
  screen, text height as a fraction of screen height (0.015 to 0.06 in
  the 21 shipped styles). The original is 4:3 only (per design notes).
- Widgets: static text, text button, mesh or image button with tooltip,
  toggle, single-line text edit with filter and length limit (including
  composed input for East Asian text), list box, table with sortable
  columns and per-cell rich text, scroll bar, cycle selector, rich-text
  window, a window that shows a rotating ship, one that shows a person.
- Modal dialogs with a stack: confirm, error, prompt, "waiting for the
  network".
- Tooltips after a hover delay and longer "rollover" texts mapped from a
  caption's identifier (298 pairs).
- Move and scale animators for sliding panels.
- A software cursor with named shapes, and capture of the mouse by one
  window.
- One command registry shared with the key bindings, so a button and a
  key trigger the same command.
- Three layout sources: a per-window record (frame, offsets, child
  list), a page file (one line per widget: class, name, rectangle,
  caption, identifiers), and code.

### 3.14 Text: identification, formatting, localisation

**Identification.** All player-visible text is referred to by a text
identifier: catalogue index × 65,536 + number within the catalogue. The
order of the catalogue list in the configuration fixes the index, so it
is part of the data contract. A catalogue holds plain strings, rich-text
cards, or both under separate number spaces. The shipped set has seven
catalogues: about 12,800 strings and 5,250 cards (interface 3,650
strings; names 7,100; equipment 780 names and 780 cards; mission and job
text 1,270; place and faction cards 890; news, rumours and conversation
3,090 cards; interface cards 500). Records, the authority and messages
carry identifiers only; the authority never resolves text.

**Format strings.** Text composed at run time (job offers, objectives,
news) travels as a template identifier plus a list of typed values:
system, base, faction, zone, object, good, installation kind, loot kind,
number, another text identifier, or a nested format string. In the
template, a placeholder is a percent sign, a kind letter, the index of
the value of that kind, and optionally a variant digit. The variant
selects an alternative form of the same name by a fixed identifier
offset, which is how one template yields grammatical sentences ("the X",
"X's", "an X base") in any language. Numbers are grouped by thousands.
Because only identifiers travel, each player reads the text in the
language of their own installation.

**Rich text.** Markup has a root, text runs, paragraph breaks, alignment
(left, centre, right), a horizontal position, style changes (bold,
italic, underline, font by index, colour), named styles, and push and
pop of the style state. Code can also insert images and references to
other loaded text. Layout wraps, justifies and can truncate with an
ellipsis.

**Localisation.** Replace the catalogues and the voice archives; nothing
else holds language. Fonts are named in data by role.

### 3.15 Authority and presentation

| Authority | Presentation |
| --- | --- |
| Which inhabitants a room has, their behaviours, extra scenes, job offers, news | Room sets, cameras, all scene playback, the room bar, every screen |
| Reacts to "inhabitant selected", offer answers, "scene done" | Decides when a scene has finished |
| Stores knowledge flags and the character's location (base and room) | Raises knowledge flags and reports them |
| Markets, repair, purchases, saving | Asks, then shows the result |

The authority reads room sets only for their placements. It does not run
scenes; a scene's length is known only to the presentation, which is why
"scene done" exists as a message.

## 4. Data that defines it

| Record | Principal fields | Volume |
| --- | --- | --- |
| Base | nickname, start room, list of (room nickname, room file) | 197; 461 room entries over 446 room files |
| Room | 3.2 | 446 |
| Room hotspot | name, behaviour, target room, virtual room, virtual room set, state shown, state sent, transition scripts | 3,581; 43 % room switches |
| Blinking light set | hardpoints, colour, size, blink, pause | 852 |
| Scene script | duration, entities, events; binary, compiled (chapter 12) | about 1,500 |
| Conversation index | line kind, sex, posture, list of clips; exclusions per voice | 617 entries, 64 groups |
| Detail table | priority name, threshold, "at or above" or "below" | 44 |
| Body part | kind (body, head, left hand, right hand), mesh, skeleton by sex, detail-switch table | 88 bodies, 104 heads, 12 hands |
| Accessory | mesh, hardpoint (hat or eyewear) | 106 |
| Costume | body, head, hands, accessories | 154 |
| New-character options | pilot (figure, comm appearance, voice, preview animation); package (ship, loadout, money, name, description); start (reputation group, base, package, pilot) | 1 each |
| Page layout | one line per widget | 8 pages |
| Window layout | frame, offset, child controls, icon per hotspot name | per panel |
| Option list | caption, tooltip, default, range, flags | 37 in 3 groups |
| Font and style tables | role, face, height; style: font, weight, colour | 21 roles, 15 styles |
| Tooltip map | caption identifier, long text identifier | 298 |
| Card extension map | card identifier, extra text, which stock lists to append | 174 |
| Knowledge table | text identifier, nickname, flags | 4,752 |
| Text catalogue | number, string or card | 7 |

Example hotspots of a two-room station's deck:

| Name | Behaviour | Target or state |
| --- | --- | --- |
| `example_bar` | room switch | room "bar" |
| `example_equipment_room` | virtual-room switch | virtual room "equipment" |
| `example_equipment_dealer` | open equipment dealer | in virtual room "equipment"; shown in state 2, sends 1 |

## 5. Connections

Needs: base and solar identity, the knowledge state and its flags, route
planning (01); ship models and hardpoints for ships on pads, mounted
equipment for the launch check (02); what each trade and repair screen
does (07); who is in a room, what they say and offer, news, job offers,
campaign scenes and their triggers (08); cursor shapes, commands and key
bindings, sound and voice playback, effects, the in-flight scripted
camera (09); messages for base, room, lists, launch, saving, accounts and
characters (11); asset kinds and catalogues (12).

Gives: the rooms and scene mechanism in which 07 and 08 appear; the
screen flow and the panels to 09; the room and base enter and exit
events, selection and scene-done reports to 08 and 11; text identifiers
and format strings to every chapter that names or describes anything.

## 6. Design notes for the builder

Essential:

- A base as a few staged rooms with services attached to buttons. The
  rules are in the panels; the room is a place to stand.
- Late-bound, data-only scene scripts. One mechanism serves sets,
  ambience, landing, conversations and campaign scenes, and 635 of about
  1,500 scripts are reusable clips.
- Sequences with offer, accept and decline branches. This is the whole
  conversation system.
- Text by identifier and format strings by typed values. It keeps the
  authority free of language and makes multiplayer text local.
- Knowledge granted by reading. It ties bar talk and news to the map.

Feel:

- Landing and launch scenes, walk-ins, fidgets, carts, traffic and
  blinking lights. They can be cut by priority, as the original does.
- The cursor that announces the kind of talk before the click.
- Panels that slide out of their icons; the camera that travels to the
  dealer.

Accidents of the era:

- Virtual rooms exist to save sets on the 63 % of bases with two rooms.
  A builder may give every function a panel on the landing place.
- No walking, fixed cameras, a frame cap on bases.
- Interface drawn as 3D meshes before a dedicated camera, with many
  layouts in code; 4:3 only. Use any modern toolkit that meets 3.13.
- Scripts stored as compiled chunks of an old scripting language though
  they are pure data (chapter 12 proposes a neutral timeline form).
- Text identifiers that depend on the load order of catalogues, and
  name variants reached by fixed numeric offsets. Use explicit keys and
  named variants.
- The account identifier as the only credential (chapter 11).

Simplifications and their cost: dropping scenes for static backdrops
loses most of the sense of place but none of the rules; dropping acted
conversations for text boxes loses the bar's character but keeps offers,
rumours and bribes; dropping the knowledge table loses the link between
talk and exploration.

## 7. Open questions

1. How an inhabitant record selects among the free placements of a room,
   and what happens when a room has fewer placements than inhabitants.
2. The exact start and end rules of the landing caption and of music
   hand-over between a room's theme and a scene's own music.
3. Several per-sequence and per-script flags have no recovered meaning
   (one gates skipping together with a second flag).
4. Whether the authored front-end pages for ship choice, tutorial and
   credits are reachable; the flow traced here never opens them.
5. How text height and wrapping were tuned per language; the original
   measured text with the operating system's font engine (per design
   notes), so line breaks are not reproducible from data alone.
6. The meaning of the "user event" entries and of the sub-scene entity in
   shipped scripts; no use was traced.
7. Script lengths in 3.6 come from constants read out of compiled
   scripts, not from playback.

## 8. Basis

Design documents: `design/assessment/06-client-app.md` sections 1 and 5
(toolkit model, layout mechanisms, text path: taken per design notes
where marked); `design/assessment/07-media-script-resources.md` sections
1 and 6 (script form, catalogue scheme); `design/facts.md` F3, F6, F15;
`outdir/data/text/README.md`.

Data: `UNIVERSE/universe.ini` (base records); all base and room files
under `UNIVERSE/SYSTEMS/*/BASES/` (tabulated by script);
`SCRIPTS/` (file census; string constants of compiled scripts for
keywords and durations), `SCRIPTS/rtcslider.ini`,
`SCRIPTS/GCS/genericscripts.ini`, `gcsexcl.ini`;
`CHARACTERS/bodyparts.ini`, `costumes.ini`, `newcharacter.ini`;
`INTERFACE/BASESIDE/navbar.ini`, `INTERFACE/INTRO/*.page`, `intro.ini`,
`optlist.ini`, `rollover.ini`, `infocardmap.ini`, `knowledgemap.ini`;
`FONTS/fonts.ini`, `rich_fonts.ini`; `outdir/data/text/*.toml`;
`outdir/OutpostFrontier.ini` (startup films).

Code, `CppSource/OutpostFrontier/Client/`: `client_state`, `gameflow`,
`menu_enter_state`, `mission_briefing`, `menu_universe_base`,
`room_control`, `room_control_hotspot`, `nav_bar`, `gf_hotspot`,
`gf_character_object`, `gf_script`, `gf_script_object`,
`gf_script_spawn`, `engine_character`, `fade_to_black`,
`page_server_select`, `server_filter`, `login_mgr`,
`page_character_select`, `page_character_select_single`,
`page_character_create`, `nn_main_button`, `nn_pause`, `nn_load_save`,
`nn_navmap*`, `knowledge_map`, `infocard_map`, `ids_strings`.
`CppSource/Common/`: `common_roomdata`, `common_costume`,
`common_fmtstr`, `common_rdlio`, `common_rtcslider`.
`CppSource/include/`: `scenescript_iface.h`, `scenescript_controls.h`,
`common_goods.h`, `common_text.h`. `CppSource/Server/server_classes.cpp`
(five-character limit).

Inference: script durations; the reading of name variants as grammatical
forms (the code shows only the identifier offsets, the templates show
their use); the purpose given for virtual rooms; "skippable" as the
meaning of the flag that gates skipping. The map modes are named from
their button names and drawing code. Toolkit requirements combine the
assessment with the widget classes and page files read.
