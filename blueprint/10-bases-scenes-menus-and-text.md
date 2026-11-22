# 10 — Bases, scenes, menus and text

Status: reviewed
Depends on: 01, 02, 07, 08, 09, 11, 12

## 1. Purpose and player experience

Everything the player does outside the cockpit happens here: the screens
before play, the visit to a base, the map, and all text.

A base is the pause between flights. The player lands, watches the ship
set down, and is then inside a few rooms shown as staged 3D scenes with
a fixed camera, without walking. A bar of buttons moves the view from
room to room, and each room offers its services: the landing place gives
repair and launch, the bar gives people to talk to, news and a job
board, the dealers give the trade panels (3.15; their rules are chapter
07's). People stand or sit at authored spots, fidget, and answer a click
with a short acted conversation. Campaign scenes play in the same rooms
with the same machinery, with the interface hidden and the picture
letterboxed.

The front end is a column of five choices over a live scene. Multiplayer
adds a host list, a character list and a character creation page. In
play, a bar of icons opens the map, the inventory, the mission log, the
player's standing and help; a pause menu saves, loads and leaves.

## 2. Concepts

Glossary terms are used as given.

| Concept | Definition |
| --- | --- |
| Set | The 3D stage of a room with its lights, cameras and markers. It is itself a scene script that never ends. |
| Setpoint | A named marker in a set: a camera position, a place for a person, a pad for a ship. |
| Placement | A setpoint for a person. Its name encodes who may use it (player or NPC), a role and a posture (stand, sit low, sit high, special). |
| Virtual room | A function that a base stages inside another room, by a second camera and a different set of buttons, instead of giving it a room of its own. |
| Room hotspot | A record of a room that becomes one button of the room bar, with a behaviour. |
| Room bar | The row of room buttons and, above the current one, the service tabs of that room. |
| Character hotspot | The screen rectangle of an inhabitant in a room; hovering and clicking go to that inhabitant. |
| Entity, event | A thing a scene script controls; a timed instruction acting on one or two entities (3.5). |
| Sequence | Scene scripts played one after another as a unit, branching on the player's answer. |
| Costume | A person's appearance: body, head, two hands, up to eight accessories. |
| Page, panel | A front-end screen laid out by a small text file of widgets; an in-game window opened from the icon bar or a service tab. |
| Format string | A text identifier plus typed values, expanded into a sentence on the presentation side. |

```text
base ── start room, rooms ×1..5
room ── set (scene script) ── cameras, lights, setpoints
     ├─ ambient / traffic / one-shot scene scripts; ambience and theme
     ├─ placements: player figure, player ship, ships for sale
     └─ hotspots ──► room bar: room buttons + service tabs
authority ──► per room: inhabitants, their behaviours, extra scenes
          ──► per base: job offers, news            (chapter 08)
inhabitant ── costume, placement, idle script, behaviour ──► sequence
sequence ── scene scripts ×n ── entities bound to set, people, ship
text identifier ──► text catalogue ──► string or card
format string ── template identifier + values ──► sentence
```

## 3. How it works

### 3.1 A base as a set of rooms

A base definition names its rooms and its start room, normally the
landing place. Five room functions exist: landing place, bar, commodity
trader, equipment dealer and ship dealer. Of 197 bases, 125 have a
landing place and a bar, 18 all five functions as separate rooms, 16 a
landing place only, and the rest other mixes (chapter 12). Where a
dealer has no room, the landing place hosts it as a virtual room.

| Room | What it offers |
| --- | --- |
| Landing place | The player's ship on its pad; landing and launch scenes; launch; repair entry; on small bases the dealers as virtual rooms |
| Bar | Inhabitants to talk to (chapter 08), the news screen, the job board |
| Commodity trader | A desk with the trader; the commodity screen |
| Equipment dealer | The player's ship on a stand, a desk; the equipment screen and repair |
| Ship dealer | Up to three ships for sale on pads; the ship screen |

### 3.2 What a room definition holds

- The **set script**, and optional looping set animations.
- A **scene list**: scripts to run on entry, each marked ambient (loops
  for the whole stay), traffic (repeats) or once, with an optional detail
  priority (3.5).
- **Sound**: a looping ambience and an optional theme; either can be
  marked to play once.
- The **default camera**, by setpoint name.
- **Placements**: one for the player's figure, with an optional walk-in
  script; one for the player's ship, with landing and launching scripts;
  up to three for ships on sale. A room with the player's ship may name
  a script that shows stock being carted to it.
- **Hotspots**: median 7 per room, at most 15 (3.3).
- **Blinking light sets** for pad and skyline lights.

Where inhabitants may stand comes from the placement setpoints of the set,
which the authority also reads to seat its inhabitants.

### 3.3 The room bar and hotspot behaviours

On entering a room the presentation rebuilds the bar from the room's
hotspots: up to nine room buttons and nine service tabs. A hotspot has a
name (for tooltip and icon), a behaviour, and optionally a target room,
the virtual room it belongs to, and a pair of state numbers.

| Behaviour | Effect of pressing the button |
| --- | --- |
| Room switch | Fade to black over 1 s, tell the authority, load the target room, fade in |
| Virtual-room switch | Fade, cut to that function's desk camera, move the player's figure to its setpoint there, rebuild the bar; the room stays |
| News | Slide the news panel open. The tab exists only if the base has news |
| Job board | Slide the job list open |
| Repair | Open the repair panel. Enabled only while the ship needs repair; pressed elsewhere, it first takes the player to the equipment room or virtual room |
| Front desk | Camera back to the desk; closes an open dealer screen |
| Commodity, equipment or ship dealer | Camera travels to the dealer view over 2 s, eased; then the trade screen opens |
| Move along | Step the camera along a numbered track of setpoints |
| Launch | Not authored; added in the start room when the player owns a ship |

The desk and dealer tabs form a two-state pair: each hotspot declares the
state it shows in and the state it sends, so only one of the two is
offered at a time. A hotspot may name a scene script to play for a given
state change instead of the plain camera move. While a fade, a camera
move or a panel animation runs, the cursor is hidden and clicks are
ignored.

### 3.4 Inhabitants of a room

The authority sends, per room and per player, the inhabitants that
chapter 08 chooses: a key, a name identifier, a faction, a costume, an
idle script and optionally a behaviour. The presentation builds each
one from the costume, stands it on a free placement and loops the
idle script.

An inhabitant with a behaviour or a dealer role has a character hotspot.
Hovering shows the name and changes the cursor to announce the kind of
talk: brush-off, bribe, rumour, job, information, campaign, or one of
the three dealers. A click

1. marks the inhabitant's faction as known (chapter 01);
2. for a dealer standing in another virtual room, switches there first;
3. with a behaviour, tells the authority which inhabitant was selected
   and plays the behaviour as a sequence (3.6);
4. for a dealer, then opens the trade screen.

Fixed role names (bartender, trader, equipment dealer, ship dealer) tie
a person to a function; scripts address people by role or as "player".

### 3.5 Scene scripts: what a script can control

A scene script is pure data: a duration, a list of entities and a list
of events. Playback is a clock in milliseconds. Each event has a start,
a duration and optionally a parameter curve (step, linear, smooth,
spline or free-form) that shapes its progress.

| Entity | What the host binds it to |
| --- | --- |
| Scene | The script itself: ambient light, fog, reverb, a priority, an optional on-screen caption |
| Compound | A rigid model: the room set, a prop, a ship |
| Deformable | A skinned person, built from a costume |
| Camera, monitor | A view. The running script with the highest priority owns the room camera |
| Light | Point, spot or directional; colour, range, cone, on or off |
| Sound | A sound or voice line, positional or not, streamed or loaded |
| Marker, hardpoint | A named point; markers are the setpoints |
| Motion path | A spline for position and orientation |
| Particle system | A named effect (chapter 09) |
| Sub-scene | Another script started from this one (inferred) |

| Event | Effect |
| --- | --- |
| Set camera | Switch the active view |
| Start motion | Play a named animation on a person: start offset, blend time, speed, weight, loop, heading |
| Start inverse kinematics | Make a limb or the head reach for or look at a target |
| Path animation | Move an entity along a motion path |
| Spatial animation | Move or turn an entity, or keep it aimed at another |
| Attach, connect hardpoints | Parent one entity to another; join two models at named hardpoints |
| Start sound, audio animation | Play; change volume, pan and distance range over time |
| Light, camera, fog, reverb, particle animations | Change those properties over time |
| Start particle system, start sub-scene | Trigger |
| Floor height | Change the ground level a person stands on |
| Subtitle, user event | Show a text; signal the host. Neither was found in use |

Scripts stay reusable through late binding. An entity's template is
resolved when the script starts: a model nickname, a costume, a voice
line of whoever speaks, or a reserved name such as the player's ship, its
engine effect or its engine sound. A person entity names an actor (the
player, a role, or the NPC of this conversation) and takes over the
figure already in the room instead of creating a second one. Marker and
camera entities with reserved name prefixes are replaced by the room's
setpoints for the speaker's place, so one conversation clip works at
every table of every bar.

An entity may carry a named detail priority. One detail setting between
0 and 1 is compared with a threshold per name (0.17, 0.51 or 0.83), so
extras, traffic, carts, weather and secondary lights vanish on slow
machines without re-authoring. The same setting caps the frame rate
inside bases at 30 or 20 per second.

Scene scripts also drive the camera in space, for flight cutscenes,
lane rides and jumps (chapter 09).

Authored scripts by kind, with typical lengths (inferred, section 8):
168 room sets and 318 ambience and traffic loops; 70 walk-ins (8 s); 27
landings (16 s) and 27 launches (13 s); 635 conversation clips (2.6 s, at
most 7 s); 25 idle fidgets; 124 campaign scenes on bases (22 s, up to
3.5 min); about 80 scenes for flight.

A conversation is assembled from clips. An index groups them by line
kind (63 kinds), speaker sex and posture, with one to four
interchangeable takes each; a second table bars takes that a given voice
did not record. A clip is one camera angle, one body animation and one
voice line. Chapter 08 says which lines are chosen.

### 3.6 Sequences

A sequence plays up to 32 scripts in order. Its options: loop; letterbox
with cursor and hotspots locked; fade to black at the end; restore the
room camera and the player's pose afterwards; fade the room's ambience
or theme while it plays; move to another room when it stops; report a
scene number to the authority when it stops.

Each entry has a kind, and the kinds give a conversation its branches:

| Entry kind | Behaviour |
| --- | --- |
| Plain | Play, then go to the next entry |
| Offer | Play, then show an accept or decline box and wait |
| Accept branch | Played only after "accept" |
| Decline branch | Played only after "decline" |
| Information | Play, show a text box and wait for it to be closed; the map is focused on the place the text names |
| Bystander branch | In a multiplayer group offer, played for members who did not accept |
| Traffic | Repeats |

The answer to an offer goes to the authority as a mission response. In a
multiplayer group offer the script waits for the authority's verdict and
takes the accept branch only for the player who accepted. Campaign
scenes use the same form, authored as sets of offer, accept, decline,
re-offer and decision scripts.

The cancel command skips a letterboxed sequence marked skippable: each
remaining script is run to its end state at once, up to the next offer
or information entry, so the room ends as if the scene had played.

### 3.7 The base visit as a state machine

| State | Event | Next state |
| --- | --- | --- |
| Arriving | Base and start room loaded behind the loading screen; the authority's room lists complete | Conversation or scene, if an inhabitant's behaviour is marked to start on entry; else landing scene, if arriving from space and the room has one; else idle |
| Landing scene | Ends or is skipped | Idle; a caption names the base |
| Idle | Room or virtual-room button | Fade out; room exit and enter sent and room loaded, or camera cut; fade in; idle |
| Idle | Dealer or desk tab | Camera move, then trade screen or idle |
| Idle | News, job board, repair or ship tab | Panel open; closing returns to idle |
| Idle | Click on an inhabitant | Conversation |
| Conversation or scene | Offer or information entry | Waiting for the player |
| Conversation or scene | Last script ends | Idle, in another room if the sequence names one |
| Idle, start room | Launch pressed with engine, power plant and scanner mounted | Launch scene. Otherwise a spoken and written refusal |
| Launch scene | Ends | Briefing if an objective route exists, else loading and space |
| Briefing | Continue | Loading and space |

Loading a saved game on a base skips the landing scene and the caption.
The briefing is the navigation map alone with the route, the objective
text, a continue button and, for a job, an abort button. Chapter 11 has
the authority's side of landing and launch.

### 3.8 Screen flow from start to play

The presentation is always in one of five top-level states. The front
end leaves to loading when a character is chosen. Loading leaves to the
base if the character is docked, else to space. The base leaves to the
briefing or to space on launch; space leaves to the base on landing.
Base and space can also go to loading (a load), back to the front end,
or quit.

At start the program plays three films (two logos and an introduction),
then shows the main menu over one of three authored menu scenes picked
at random, run as ordinary rooms without an authority.

| Front-end step | Offers | Goes to |
| --- | --- | --- |
| Main menu | New game, load game, multiplayer (3.9), options, exit; the build version | The steps below |
| New game | Nothing: the restart template is selected | Loading |
| Load game | Saves with description and time; for the selected one ship, level, money and location; load, delete | Loading |
| Options | Interface, audio and video settings (29) and the key bindings of chapter 09 | Main menu |

The save list is the in-process authority's character list (chapter
11).

### 3.9 The multiplayer front end

| Step | Content |
| --- | --- |
| Account | Shown only when no valid local account identifier exists: an explanation and the identifier field |
| Host list | A sortable, filterable table: name, address, visited before, ping, players (current / maximum), version, on the local network, options (password required; players can damage players). For the selected host its description and restrictions (no new accounts, password). Hosts of another version are greyed and cannot be selected |
| Connect | Log in with the account identifier; a password prompt where required |
| Character list | The account's characters, at most five: name, level, money, ship type, location. Also the host's message of the day and details. Load, new, delete with confirmation, back |
| New character | A name of up to 23 characters, a preview of the pilot's figure, and the start base, money and ship package. The data allows several pilots, packages and starting factions but ships one of each, so the name is the only choice |

### 3.10 In-game panels, pause and saving

One icon bar is present on bases and in space: map, inventory, mission
log, player status (level, standing with each known faction), help, and
in multiplayer chat and group.

The pause menu offers resume, load, options and exit, plus save in
single player and "change character" and "change host" in multiplayer.
Save is offered only while docked and while no unskippable scene runs.
The save window lists saves by name and date, takes a description and
asks before overwriting; the automatic save and the restart slot cannot
be overwritten by hand. The presentation only asks; the authority writes.

### 3.11 The navigation map

The map has three views: the universe (a marker per known system, lines
for known connections, the player's position), a system, and a zoomed
part of a system that can be dragged. A system is drawn on an 8 × 8
lettered and numbered grid; text names an unnamed place by its grid cell.

| Mode | Shows |
| --- | --- |
| Physical | Solars, lanes, fields and clouds |
| Political, patrol | Population areas, or patrol paths, tinted by their faction's attitude to the player |
| Mining | Zones that yield loot |
| Mission | The zones the current objective names |
| Known bases | Every visited base of every visited system, sortable by base or system; choosing one zooms the map on it |
| Legend, card | The key to the symbols; the information card of the chosen object |

Clicking a marker offers tools around the cursor: zoom, go to that
system, show its card, set a waypoint (up to 99), plot the best path,
select it as the flight target (own system only), clear waypoints. Three
rows list the waypoints or the steps of the current route. In
multiplayer the map marks group members.

What is drawn follows the knowledge state of chapter 01: unseen systems,
solars and zones are absent, lanes are hidden until something in the
system is known, and a base's card lists commodities, equipment and
ships only once the player has docked there. Knowledge also arrives by
text. When the interface shows a text whose identifier is in the
knowledge table (a job offer, a news item, a rumour, an information
box), every object listed under it gains the listed flags, and the
authority is told so that it is saved. A second table appends a further
text to an object's card once the flag for it is set.

### 3.12 What the UI toolkit must provide

- A tree of named windows with position, size, draw layer, visibility
  and enable state, created by class name from layout data.
- Layout in fractions of the screen; text height as a fraction of screen
  height (0.015 to 0.06 in the 21 shipped styles).
- Widgets: static text, text button, mesh or image button, toggle,
  single-line text edit with filter, length limit and composed input for
  East Asian text, list box, table with sortable columns and rich text
  per cell, scroll bar, cycle selector, rich-text window, a view of a
  rotating ship, a view of a person.
- Stacked modal dialogs: confirm, error, prompt, waiting for the network.
- Tooltips, and longer texts mapped from a caption's identifier.
- Move and scale animators for panels; a software cursor with named
  shapes; mouse capture by one window.
- One command registry shared with the key bindings, so a button and a
  key trigger the same command.
- Layout from data: a per-window record (frame, offsets, child list) or
  a page file (one line per widget: class, name, rectangle, caption).

### 3.13 Text: identification, formatting, localisation

**Identification.** All visible text is referred to by a text
identifier, which names a catalogue and an entry in it. A catalogue
holds strings, cards, or both under separate number spaces. The shipped
set has seven catalogues with about 12,800 strings and 5,250 cards;
names (7,100 strings) and bar-talk cards (3,090) are the largest.
Records and messages carry identifiers only; the authority never
resolves text.

**Format strings.** Text composed at run time (job offers, objectives,
news) travels as a template identifier plus a list of typed values:
system, base, faction, zone, object, good, installation kind, loot kind,
number, another text identifier, or a nested format string. In the
template a placeholder is a percent sign, a kind letter, the index of
the value among those of its kind and, optionally, a variant digit. The
variant selects an alternative form of the same name by a fixed
identifier offset; the templates use it to fit a name into different
grammatical positions (inferred). Because only identifiers travel, each
player reads the text in the language of their own installation.

**Rich text.** Markup has text runs, paragraph breaks, alignment, a
horizontal position, style changes (bold, italic, underline, font by
index, colour), named styles, and push and pop of the style state. Code
can also insert images and other loaded text. Layout wraps, aligns and
can truncate with an ellipsis.

**Localisation.** Replace the catalogues and the voice archives; nothing
else holds language.

### 3.14 Authority and presentation

The authority decides a room's inhabitants, behaviours, extra scenes,
job offers and news; it reacts to "inhabitant selected", offer answers
and "scene done"; it stores knowledge flags and the character's
location; it owns markets and saving. The presentation owns room sets,
cameras, scene playback, the room bar and every screen; it raises
knowledge flags and reports them.

The authority does not run scenes, so a scene's length is known only to
the presentation; that is why "scene done" exists as a message.

### 3.15 The service and inventory panels

Chapter 07 owns the rules; this is what the player handles.

**Inventory and the two dealers.** One window serves the inventory icon,
the commodity trader and the equipment dealer. It shows the player's
ship in 3D with its hardpoints, a scrolling list of five rows, five
filter buttons (weapons, ammunition, equipment, cargo-bay items,
commodities), the money and a bar for hold space. At a dealer a second
such window shows the base's stock beside it, with what this player may
buy listed first.

A row shows icon, name, quantity and, at a dealer, the unit price; a
health bar when damaged; a mounted toggle for mountable items; a label
on mission cargo. The commodity trader adds a three-level marker (good,
average, poor) that rates the local price against authored thresholds,
separately for buying and selling. Selecting a row opens the item's
card: its description and, for equipment, a two-column table of its
statistics. No comparison with the mounted item was found. For a
commodity, a "trade routes" button lists its price at each known base;
choosing a base opens the map there.

Selecting a row on either side opens a transfer strip between the lists:
the item, a buy or sell arrow, the unit price, a quantity (slider or
typed number, from 1 to what fits and can be paid for, or to the stack
size when selling) and the total. Where nothing can be bought the strip
states why: no hold space, not enough money, no ship, level too low
(with the level needed), standing too low, no hardpoint of that class,
no free hardpoint, or a carrying limit reached. Mission cargo and the
engine cannot be sold.

There is no basket and no confirmation. Each press of buy or sell
changes the local lists and is sent to the authority at once, as a
market request, an item change and a money change. Equipment bought with
a free matching hardpoint is mounted immediately; otherwise it goes to
the hold. The mounted toggle, and dragging an item onto a hardpoint of
the ship view, send a modify request the same way. A jettison button
asks for a quantity when the stack is larger than one.

**Ship dealer.** A panel lists up to three ships on sale and the
player's own: icon, name, class, price, and a lock when level or standing
is too low. Picking one shows it rotating in 3D with three panes of
facts. Buying opens the two-list window with the old ship beside the
new. The old equipment is moved across automatically, most valuable
first, as far as it fits; the player may move items either way, except
engines. The price shown is the package less the trade-in of the old
ship and of what stays on it. "Continue" sends ship, loadout, hull state
and money together; "back" abandons the purchase.

**Repair.** A list of the hull, damaged parts and damaged equipment,
each with a check box, name, price and health bar. The checked rows are
totalled and charged on confirmation, which needs enough money.

**Player trade (multiplayer).** A window between two players' cargo
lists: each puts up items and money and sees the other's offer.

## 4. Data that defines it

| Record | Principal fields | Volume |
| --- | --- | --- |
| Base | nickname, start room, list of (room nickname, room file) | 197; 461 room entries over 446 room files |
| Room | 3.2 | 446 |
| Room hotspot | name, behaviour, target room, virtual room, virtual room set, state shown, state sent, transition scripts | 3,581; 43 % room switches |
| Scene script | duration, entities, events; stored compiled (chapter 12) | about 1,500 |
| Conversation index | line kind, sex, posture, clips; exclusions per voice | 617 clip entries in 64 groups |
| Body part | kind (body, head, left hand, right hand), mesh; skeleton by sex; detail-switch table | 88 bodies, 104 heads, 12 hands |
| Costume | body, head, hands, accessories (106, on a hat or an eyewear hardpoint) | 154 |
| New-character options | pilot (figure, comm appearance, voice, preview animation); package (ship, loadout, money, name, description); start (reputation group, base, package, pilot) | 1 each |
| Page, window, font and style tables | widget lines; frame and child controls; role, face, height, colour | 8 pages, 21 font roles, 15 styles |
| Tooltip map; card extension map | caption and long text identifiers; card identifier, extra text, stock lists to append | 298; 174 |
| Knowledge table | text identifier, nickname, flags | 4,752 |

Example: a two-room station's deck has a room switch to the bar, a
virtual-room switch `example_equipment_room`, and in that virtual room a
desk hotspot (shown in state 1, sends 2) and a dealer hotspot (shown in
state 2, sends 1).

## 5. Connections

Needs: the knowledge state and route planning (01); ship models and
mounted equipment (02); the rules of trade and repair (07); who is in a
room, what they say and offer, news, job offers, campaign scenes (08);
cursor, commands, key bindings, sound, voice, effects, the scripted
camera in flight (09); messages for base, room, lists, launch, saving,
accounts and characters (11); asset kinds and catalogues (12).

Gives: the rooms and scenes in which 07 and 08 appear; the screen flow
and panels to 09; enter and exit, selection and scene-done reports to 08
and 11; text identifiers and format strings to every chapter.

## 6. Design notes for the builder

Essential:

- A base as a few staged rooms with services on buttons.
- Late-bound, data-only scene scripts. One mechanism serves sets,
  ambience, landing, conversations and campaign scenes; 635 of about
  1,500 scripts are reusable clips.
- Sequences with offer, accept and decline branches: the whole
  conversation system.
- Text by identifier and format strings by typed values, which keep the
  authority free of language.
- Knowledge granted by reading, which ties bar talk and news to the map.

Feel: landing and launch scenes, walk-ins, fidgets, carts, traffic and
blinking lights; the cursor that announces the kind of talk; the camera
that travels to the dealer.

Accidents of the era:

- Virtual rooms save sets on the 125 bases with two rooms (inferred). A
  builder may give every function a panel on the landing place.
- An interface drawn as 3D meshes before its own camera, for a 4:3
  screen only (per design notes). Any modern toolkit that meets 3.12
  will do.
- Text identifiers packed as catalogue index × 65,536 + entry number,
  the index being the catalogue's place in the load list; name variants
  reached by numeric offsets. Use explicit keys.

Simplifications: static backdrops instead of scenes lose the sense of
place and none of the rules; text boxes instead of acted conversations
keep offers, rumours and bribes.

## 7. Open questions

1. How an inhabitant record selects among the free placements of a room,
   and what happens when a room has fewer placements than inhabitants.
2. Several flags of sequences and scripts have no recovered meaning; one
   gates skipping together with a second.
3. Whether the authored pages for ship choice, tutorial and credits are
   reachable. The flow traced here never opens them.
4. How text was fitted per language. The original measured text with the
   operating system's font engine (per design notes).
5. What sub-scenes, user events and subtitle events were meant for.

## 8. Basis

Design documents: assessment 06 sections 1 and 5 (toolkit, layout, text
path); assessment 07 sections 1 and 6 (script form, curve kinds,
catalogue scheme); `design/facts.md` F3, F6, F15;
`outdir/data/text/README.md`.

Data: `UNIVERSE/universe.ini` (base records); all base and room files
under `UNIVERSE/SYSTEMS/*/BASES/`, tabulated by script; `SCRIPTS/` (file
census; string constants of the compiled scripts), `SCRIPTS/rtcslider.ini`,
`SCRIPTS/GCS/genericscripts.ini`, `gcsexcl.ini`; `CHARACTERS/bodyparts.ini`,
`costumes.ini`, `newcharacter.ini`; `INTERFACE/BASESIDE/navbar.ini`,
`INTERFACE/INTRO/*.page`, `intro.ini`, `optlist.ini`, `rollover.ini`,
`infocardmap.ini`, `knowledgemap.ini`; `FONTS/fonts.ini`,
`rich_fonts.ini`; `outdir/data/text/*.toml`; `outdir/OutpostFrontier.ini`
(startup films).

Code, `CppSource/OutpostFrontier/Client/`: `client_state`, `gameflow`,
`menu_enter_state`, `mission_briefing`, `menu_universe_base`,
`room_control*`, `nav_bar`, `gf_hotspot`, `gf_character_object`,
`gf_script*`, `engine_character`, `fade_to_black`, `page_server_select`,
`server_filter`, `login_mgr`, `page_character_*`, `nn_main_button`,
`nn_pause`, `nn_load_save`, `nn_navmap*`, `nn_inventory`, `nn_dealer`,
`nn_ship_trader`, `nn_repair`, `nn_trade`, `dealer_equip_stats`,
`commodity_by_base`, `knowledge_map`, `infocard_map`, `ids_strings`.
`CppSource/Common/`: `common_roomdata`, `common_costume`,
`common_fmtstr`, `common_rdlio`, `common_rtcslider`.
`CppSource/include/`: `scenescript_iface.h`, `scenescript_controls.h`,
`common_goods.h`, `common_text.h`. `CppSource/Server/server_classes.cpp`
(five-character limit).

Inference: script lengths (the constant after the duration key in each
compiled script, not playback); name variants as grammatical forms; the
purpose of virtual rooms; "skippable" as the meaning of the flag that
gates skipping; sub-scenes. Map modes are named from their buttons.
