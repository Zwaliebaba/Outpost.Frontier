# 08 — Missions and base services

Status: draft
Depends on: 01, 02, 04, 05, 06, 07, 10, 11, 12

## 1. Purpose and player experience

Missions give the open universe direction. Two kinds share one runtime.

A **scripted mission** is an authored set piece of the campaign. The player
is led to a room on a base, watches a scene, accepts, launches, and is then
carried through escorts, ambushes, chases and assaults by named NPCs who
talk over the radio. Objectives appear one at a time, a marker shows where
to go, and companions remind a player who strays. Between campaign
missions the player is released to trade and fight until a wealth target
is met.

A **generated mission** is a paid job. On most bases a job board lists two
to four offers, and people in the bar may offer one directly. Each offer
names who pays, whom to attack, where, how hard it is and what it pays.
The player accepts, flies to a marked site, fights what spawns there and
is paid on the spot.

The bar also holds people who tell rumours, sell the location of hidden
places or take a bribe to repair a reputation, and a news screen whose
items follow the campaign.

## 2. Concepts

| Concept | Definition |
| --- | --- |
| Mission definition | The record file of one scripted mission (section 3.2). |
| Label | A group name on mission objects. Conditions and actions address one object by nickname or all members of a label. |
| Directive list | Ordered orders for one scripted ship. |
| Reminder | A watch on the player's progress towards a place; a companion nags, and ignoring it fails the mission. |
| Checkpoint | A save made by an action. It names the resume trigger that rebuilds the scene on load. |
| Campaign position | A number from 0 to 42: where the player stands in the campaign. |
| Worth gate | A wealth target to reach before the next campaign mission starts. |
| On-base story scene | A record that binds scene scripts (offer, accept, decline) and a cast to one room. |
| Difficulty | A number from 0 to 100 on a geometric scale; it sizes a generated mission and sets its reward. |
| Mission site | A zone marked for generated missions. |
| Offer tree | The authored decision tree from which a generated mission is composed. |
| Base roster | Per base: factions with their shares and characters, fixtures, job-board size. |
| Fixture | A character at a fixed spot who provides a room's service: bartender, trader, dealer. |
| Job board | The job offers a base makes to one visiting player. |

```text
campaign controller ──(position, worth gate)──► scripted mission
job board, bar character ──(acceptance)──► generated mission
    both run as content controllers on the event bus (chapter 11)

scripted mission ─► cast, ships with labels, solars, loot
                 ─► directive lists ─► behaviours (chapter 05)
                 ─► objectives ─► interface and map (chapters 04, 09)
                 ─► triggers: condition ◄─ events; actions ─► services
generated mission = offer (payer, enemy, site, difficulty, reward, seed)
                  + one path through the offer tree
base roster ─► fixtures, bar characters ─► job, bribe, knowledge, rumour
```

## 3. How it works

### 3.1 A mission is a content controller

Starting a mission creates a content controller above the priority of the
support controllers, so it sees each event first (chapter 11). It belongs
to one player; the members of that player's group join it and share its
objectives and reward. A player or group holds at most one live mission.

The campaign controller is in the single-player start-up list only.
Multiplayer has generated missions and no campaign.

### 3.2 Anatomy of a mission definition

| Record | Declares |
| --- | --- |
| Header | The file of the mission's own NPC ship archetypes; optionally title, summary and reward. |
| Cast member | A named NPC: ship archetype, faction, voice, appearance, name. |
| Mission ship | A cast member placed in the world: labels, optional pose, cargo, initial directive list, and whether it follows the player through jump points. |
| Formation | A formation type, a pose, its mission ships. |
| Mission solar, mission loot | A placed solar with loadout, faction, pilot and labels; an item with amount, position and velocity. |
| Mission encounter | An ambient-style attack group the mission can switch on (chapter 06). |
| Objective | Text only; text with a waypoint; or text with a waypoint bound to an object. |
| Conversation | A sequence of lines: speaker, listener, voice line. |
| Directive list, trigger | Sections 3.6 and 3.3. |

Nothing exists in the world until an action spawns it: the definition is
a catalogue, and triggers decide when each entry appears.

### 3.3 The trigger model

A trigger has **exactly one condition** and any number of actions.

| State | Event | Next state | What happens |
| --- | --- | --- | --- |
| Inactive | activated | Active | The condition is primed (counters reset, names resolved) and probed once. |
| Active | condition true | Complete | Actions run in authored sequence. |
| Complete | flagged repeatable | Active | Primed again. |
| Active | deactivated | Inactive | Immediate; pending activations of it are dropped. |

Each condition kind subscribes to one event kind (one kind to three).
Fourteen subscribe to the heartbeat and are in effect polled: 30 times a
second in a scripted mission, once a second in a generated one. An
arriving event tests every active trigger that subscribes to it.

"Activate trigger" does not run the target. It adds it to a pending set
that is drained after the pass, repeatedly, until empty. The set is keyed
by trigger identifier, so triggers activated together start in identifier
sequence, not as written. An author may rely on the sequence of actions
inside a trigger and on nothing between triggers.

Because the "always true" condition fires on activation, such a trigger is
a subroutine. AND is a chain (A activates B) or the condition that watches
another trigger; OR is several triggers that activate the same successor.

An action sets the mission's desired end state; it is applied when the
current event has been handled. Missions also fail without an action when
the player's ship is destroyed, the player aborts, or a reminder runs out.

A leg as an author writes it:

```text
trigger example_leg      when: player has finished launching
   spawn formation example_convoy
   give example_lead the directive list example_route
   show objective example_obj_escort
   activate example_lost, example_arrived
trigger example_lost     when: 1 member of label example_vip destroyed
   end mission: fail, with text
trigger example_arrived  when: example_lead within 2,000 m of example_dock
   deactivate example_lost
   start conversation example_thanks
   activate example_thanked
trigger example_thanked  when: voice line example_thanks_3 has finished
   checkpoint, resume at example_next_leg_resume
```

Five condition kinds make up 70% of the shipped triggers: line finished,
always true, timer, destroyed, distance between two objects (totals in
chapter 12).

### 3.4 Conditions

44 kinds are registered; 40 occur in the shipped data.

**Where the player is**

- Lands on, or leaves, a given base.
- Enters, or leaves, a given room.
- Enters, or leaves, space in a given system.
- Arrives in, or leaves, a system, optionally from or to a given one.
- Begins a launch; a ship has finished launching, optionally from a given base.
- Arrival through a jump point has completed.
- Polled: the player is, or is not, in space; has, or has not, a live mission.

**Where ships are**

- Two objects are nearer, or farther, than a distance; optionally for a time, counted always, only while closing or only while parting.
- An object is nearer to, or farther from, a point; same options.
- Any, or all, members of a label are nearer to, or farther from, a point.
- An object passes forwards through the opening of a ring-shaped object.
- Two ships stay farther apart than a distance for a time (an escort abandoned).
- All listed ships, the player included, have arrived in a system; arrival by jump point adds 10 s.
- A ship enters a lane at a given ring, or leaves one.
- Polled: a ship is, or is not, riding a lane.
- Polled: a ship is inside a zone of a given class (asteroid field, exclusion, nebula, dust).
- Unused: an object is on one side of a plane; listed ships left a system; a jump gate was activated.

**Combat and cargo**

- A count of a label's members, or all, have been destroyed. Filters: exploded; removed silently; either; either, but not while the player is docked. Optionally by a given killer.
- An object's health is at or below a fraction, optionally by a given attacker.
- An object, or any member of a label, has been hit N times, optionally by a given shooter.
- A given ship scanned a given ship's cargo; a ship collected a given mission loot.
- Polled: one object's attitude towards another is at most, at least or equal to a level.
- A mission encounter has spawned.

**Speech and interface**

- A given voice line has finished; a given on-base story scene has finished.
- The player accepted, or rejected, the offer.
- The player selected a given character in a room.
- The player answered a pop-up with a given button.
- The player's ship is, or is not, performing a given manoeuvre (go to, dock, formation), optionally on a given target.
- Unused: the player heard a given rumour.

**Flow**

- Always true.
- A time has passed since activation, fixed or drawn once from a range; optionally it stands still while docked.
- Polled: another trigger is in a given state.

Conditions that count (destroyed, hits, loot, timer, dwell) save their
progress.

### 3.5 Actions

85 kinds are registered; 78 occur. Ten make up 63% of all uses: activate
and deactivate trigger, give directive list, three attitude setters, set
objective, send line, spawn ship, protect.

**Flow**

- Activate a trigger; deactivate a trigger.
- End the mission as succeeded or failed, with a text.
- Checkpoint: save, naming the resume trigger and a description.
- Author's note; does nothing.

**Mission objects**

- Spawn a mission ship, optionally with a directive list and a pose; a formation; a mission solar; a mission loot.
- Unused: spawn a ship relative to another object.
- Destroy an object, exploding or silently.
- Light a named fuse on an object (chapter 03).
- Change the health of a label's members by a fraction.
- Protect an object from damage, optionally only from non-player damage, with a cap on the health it may lose.
- Cloak or reveal a label's members.
- Move a ship to a pose; turn a ship. Unused: move a formation.
- Mark an object as important on the player's displays.
- Set whether a ship follows the player through jump points; set its lifetime.

**Orders to ships**

- Give a ship, or the player's autopilot, a directive list.
- Give a ship a behaviour that guides the player to a point.
- Set whether a label's members hold to their directives.
- Clear the player's current autopilot order.
- Lock or unlock all of the player's manoeuvre commands; enable or disable one.
- Unused: put the player into formation with a ship; set a pilot's flee mode; make a ship flee from a point or object.

**Attitudes and reputation**

- Set the attitude between two objects; between two labels; from a label to a ship; from a ship to a label. Seven named levels run from hostile maximum to friendly maximum.
- Set the reputation of the player or an object with a faction.
- Make a base friendly to the player so that the offer can be reached.
- Excuse the player's fire on listed labels.
- Bound the number of enemies attacking the player at once.
- Unused: offer, on entering a system, to reset reputations with listed factions.

**The ambient world** (chapter 06)

- Switch ambient population on or off.
- Add or remove a sphere that population stays out of.
- Forbid or allow: ambient attacks on the player; ambushes on lanes; hostility in general; ambient radio chatter; companions' reminders.
- Enable or disable a mission encounter.
- Disable a lane ring.

**Access**

- Lock or unlock a jump point or dock, for the player or for everyone.
- Allow or forbid the player to dock, or to use lanes, with exceptions.

**The player's ship and account**

- Pay or charge money; remove a mission loot from the hold.
- Replace the player's ship and loadout.
- Move the player's ship if it is farther than a radius from a point (used by resume triggers).
- Set where the player appears on the next launch.
- Land the player on a base at once; request docking for the player.

**Objectives and guidance** (section 3.7)

- Show an objective, hiding the others; optionally log it.
- Set an objective's state; show or hide one or all; resend the list.
- Show or log a text without a waypoint.
- Plot a route to an object in a system as the objective.
- Set the mission's title; its summary.
- Unused: play a spoken interface message.

**Speech** (section 3.8)

- Send one voice line from a ship or solar to the player or anyone.
- Send a line from an unseen speaker with a given face.
- Start a conversation, optionally after a delay.
- Start a reminder towards an object or point; one against leaving a place; a greeting. Stop a reminder.
- Open a pop-up with a button set.

**Presentation**

- Set or cancel the situation music; play a sound.
- Run a scene script in space on an object or at a point; this protects the player and silences reminders and chatter.
- Fix the camera at a pose; give it back.
- Add or remove an on-base story scene; add or remove an ambient scene in a room.

### 3.6 Directive lists

A ship's pilot runs its directive list top to bottom through the
behaviours of chapter 05. 23 directives are registered; 15 occur.

| Purpose | Directives |
| --- | --- |
| Travel | Go to a ship or solar, to a point, or along a four-point spline, with a stand-off range. A mode word demands cruise, forbids it, or leaves it to the pilot. |
| Company | Follow a target at an offset; trail it; stay within, or outside, a radius of an object or point; guide the player to a point or ship. |
| Formation | Form a named formation with listed ships; form up around the player; break formation. |
| Stations | Dock at a target; launch from one; ride a lane at once. |
| Timing | Wait some seconds; idle; wait until the player performs a manoeuvre. |
| Settings | Hold to directives or allow distraction; collision avoidance on or off; lifetime; face an object or point; cancel. |

The player's ship can receive a list too: so a script flies the player.

### 3.7 How objectives reach the player

The authority keeps an ordered objective list per player and sends it
whole, or one changed entry, to the presentation. An entry is a text
identifier, a state, a hidden flag and optionally a waypoint (system and
position) and an object whose name and standing the text shows.

Scripts usually show one objective at a time. A text can be queued as a
banner, written to the mission log, or both. The route action asks the
route planner (chapter 04) for the legs to a distant object and shows them
as waypoints, rebuilt as the player advances. Success and failure have
their own message kinds; failure opens the mission log.

### 3.8 Speech and reminders

A line is a voice-profile entry (chapter 12) sent from a speaker to a
listener; the presentation plays it with the speaker's face and reports
its end. That report is the commonest condition: missions pace themselves
by speech. A conversation is a list of lines played in turn.

A reminder names a speaker and a destination. While the player is farther
away than a set distance, a 20 s timer runs. If the player has not closed
by a threshold when it expires, the speaker says a stock line chosen by
the kind of destination and the timer restarts. Four unanswered reminders
fail the mission; progress resets the count; arriving ends it.

### 3.9 Saving, failure and retry

Mission state is saved in single player only. A normal save writes the
ambient switches, title, pending scenes, objectives, mission ships to
re-create, and every active trigger with its condition's progress.

A checkpoint differs. The action records the resume trigger and asks the
authority to save; the file is then copied to the autosave slot (chapter
11). The saved mission lists that one trigger in place of the active
ones. On load it alone is activated, and its actions move the player,
respawn companions and activate the triggers of the leg. The script
rebuilds the scene; nobody serialises a battle. The campaign makes 36
checkpoints, two or three per mission.

There is no retry logic. Of the campaign's 118 end-mission actions, 104
are failures. A failed scripted mission ends its controller and leaves the
campaign position unchanged with nothing to restart it; the player returns
by loading a save (inferred).

### 3.10 The campaign as structure

13 missions, the first split over two definitions that chain directly.
Each mission owns three campaign positions: started, accepted, and free
time after it.

| After mission | Rule |
| --- | --- |
| 1 to 5 | Worth target = worth at completion + 3,500; 5,200; 7,000; 10,000; 16,000 |
| 6 | Next mission starts at once |
| 7, 8 | + 36,000; + 30,000 |
| 9 to 12 | Next mission starts at once |
| 13 | Position 42: campaign over, level follows worth alone (chapter 07) |

Worth is cash plus the value of ship and equipment; the interface shows
the target. On every change of worth or of the live mission, the campaign
controller starts the next mission if the target is met and no generated
mission is live.

A started mission is not yet accepted. Its entry trigger guides the player
to a room; an on-base story scene plays there and ends in a choice. Reject
swaps in a repeatable re-offer scene. Accept ends any generated mission,
advances the position, and by default stops ambient ships from attacking
or scanning the player until the script says otherwise.

The second gate is the map. The new-game template is a saved game with
campaign position 1 and 32 locked jump points; missions unlock them (118
unlock actions against 64 locks). Missions also pay directly: 16 payments
of 750 to 25,000 credits. Mission sequence and increments are fixed in
the program, not in data.

### 3.11 How a job offer is generated

The board is built when a player enters a base and rebuilt after a bribe;
a bar character's job is drawn when the player enters the room.

1. **How many.** A uniform draw from the base's range: 2 to 4 on 140 of
   194 bases, none on 33, 8 to 12 on the largest.
2. **Who.** A faction on the base with a mission entry and not hostile to
   the player, drawn by its share; then a mission type by weight. The
   shipped data has one type.
3. **How hard.** The entry gives a range. In single player the campaign
   position gives a centre `d` (0.08 at the start, 1.23 before the ninth
   mission, 39 after the campaign) and a window `[d / s³, d · s³]` with
   `s = 1.3689`. The pick is `high − cos(u · π/2) · (high − low)` with `u`
   uniform, which favours the low end. No overlap means no offer. In
   multiplayer the entry's range is used; two times in three its top is
   raised by `s²`, `s⁴` or `s⁶`.
4. **Where.** A mission site within a travel budget of 90 units from the
   base: a unit is 300 m of flight, a lane ride costs 10, a jump 45
   (multiplayer: 30 more per other player in the system, at most 300).
   The site must suit the payer (lawful or unlawful), carry no other
   offer and not lie behind a locked jump point. Drawn uniformly.
5. **Against whom.** A faction that populates zones within 7,500 m of the
   site, failing that anywhere in its system, and that the payer regards
   at −0.2 or worse. Drawn uniformly.
6. **For how much.** A 23-row table maps difficulty to money, from
   0 → 1,800 to 100 → 247,065, interpolated linearly and truncated.
   Difficulty 1.0 lies between 0.900 → 7,264 and 1.233 → 9,001 and pays
   7,784.
7. **What.** Section 3.12. The offer keeps a random seed.

An empty board says why: no willing faction, no difficulty in range, or
no site.

### 3.12 How a generated mission is composed and run

The offer tree has decision nodes with two children and data nodes that
add parameters. Each decision is a yes-or-no fact: is the objective a
large solar, are there defensive solars, a fight on the way, a named
target, friendly ships after 30 s, reinforcements, an item. Data nodes
carry offer-text fragments with placeholders, an objective text per step,
radio sequences keyed to run-time moments, permitted payers, enemies and
site kinds, a difficulty window and a leaf weight.

Composition takes the leaves that fit payer, enemy and difficulty, draws
one by weight, then one root-to-leaf path whose windows admit the
difficulty. The path's facts split the difficulty into shares. A second
table turns a share into an NPC level and a formation size of 1 to 8:
level 5 with four ships is 0.475. Solars come from templates with a
difficulty, hit points and a choice of pilots.

A saved generated mission stores payer, enemy, site, difficulty, reward
and seed; loading reseeds the generator and composes it again.

A state machine runs it:

| State | What happens |
| --- | --- |
| Travel | A waypoint leads to the first fight or straight to the main one. Ambient population is kept out of 10,000 m around it. |
| First fight (optional) | Starts within 2,500 m. Ends when its waves are dead, or on a timer, ship count or health level; survivors run to the main fight. |
| Main fight | Solars spawn at 7,500 m, ships at 2,500 m. Ends when the flagged ships and solars are destroyed. |
| Item (optional) | The target drops an item to destroy, collect or bring back. |
| Success | Reward split in the group; standing with the payer rises. |

Later waves arrive on a delay or when few ships are left. Leaving the
combat area (3,500 m, hysteresis 250 m) starts a 30 s countdown, 45 s in
multiplayer; returning cancels it. The mission fails when the countdown
ends, when the players are destroyed, or in single player when the player
docks after reaching the site. Failure and abort lower standing with the
payer (chapter 07).

### 3.13 Who the player meets on a base

**Fixtures** stand at authored spots and provide the room's service. Some
also carry rumours and bribes.

**Roster characters** fill a room when the player enters, up to the room's
density (1 to 7). Each draw takes a faction by its share on the base, one
of its characters allowed in that room, and a free spot. The character
offers this player one thing, drawn by weight among what applies:

| Offer | Applies when | Weight | Effect |
| --- | --- | --- | --- |
| Job | The character has a mission entry | 30 | A generated mission, started on acceptance |
| Bribe | Standing with a listed faction is below 0.6 | 20 | Pay; standing rises by the shortfall |
| Knowledge | Standing with the character's faction is at least 0.2, 0.4 or 0.6; not yet bought | 15 | Pay 300 to 55,000; a place is revealed (chapter 01) |
| Rumour | Campaign position inside the rumour's window; standing high enough; not yet heard | 64 if tied to a campaign stretch, else 10 to 20 | A text; some reveal a place |

A bribe costs `10 × base cost × (0.6 − standing)`, rounded to two leading
digits. Every shipped base cost is 10,000: 60,000 at standing 0, 90,000 at
−0.3. A rumour once told is recorded for the player and withdrawn from the
others in the room.

### 3.14 News

A news item has a campaign-position window, a list of bases, an icon, an
image and texts. A player entering a base receives the items that list
the base and whose window holds the player's position. About half the
shipped items are always available.

### 3.15 Authority and presentation

All of this runs on the authority. The presentation receives objective
lists, messages, voice lines, music, camera and scene requests, offer and
news lists, and answers with events: offer or pop-up answered, line or
scene finished, character selected.

## 4. Data that defines it

| Record | Principal fields | Shipped values |
| --- | --- | --- |
| Mission definition | Section 3.2 | 14 files; 79 to 408 triggers each (chapter 12) |
| On-base story scene | Base and room; scripts for offer, decision, accept, decline; cast; reserved spots | 69 |
| Base roster | Local faction; board size; per faction: share, mission type with difficulty range and weight, characters | 194 bases |
| Roster character | Appearance, voice, faction, room; bribes; knowledge with price and standing level; rumours with window and level; mission entry | 1,642, of which 560 fixtures; median 10 rumours |
| News item | Window, bases, icon, image, texts | 403; median 5 bases, at most 40 |
| Difficulty tables | Campaign position to difficulty; difficulty to money; NPC level and formation size to difficulty | 9, 23 and 25 rows |
| Offer tree | Section 3.12 | 219 decision and data nodes, 6 weighted leaves |
| Destroyable solar | Kind, faction, archetype, loadout, hit points, difficulty, zone restriction | 126, in 11 layouts |
| Mission item, bonus loot | Item, kind or faction, difficulty band, weight | 62 and 143 |
| Zone (chapter 01) | Site marking, lawful or unlawful, kind | 13% of zones |

A zone is recognised as a site by a fixed substring in its nickname; it
is lawful unless a key says otherwise.

## 5. Connections

Needs: zones, jump points and the knowledge state (01); ships and
loadouts (02); fuses and damage (03); docking, lane and route services
(04); behaviours, pilots and formations (05); population switches and
encounters (06); reputation, worth, level and event rates (07); banners,
log, markers and voice playback (09); rooms and scene scripts (10); the
event bus, heartbeat, timers and saving (11); the census (12).

Gives: the campaign position to 06, 07 and 10; locked jump points to 04;
checkpoints to 11; job boards, bar offers and news to 10.

## 6. Design notes for the builder

**Essential.** Triggers over events, labels, spawn-on-demand and directive
lists form a small language in which authors, not programmers, build set
pieces. Checkpoints as resume triggers are the cheapest robust save for
scripted fights. The ambient switches matter as much as the spawns: a set
piece fails if traffic wanders through it. A generated mission feels part
of the world because payer, enemy and site come from the place where the
offer is made.

**Feel.** Pacing by finished voice lines, companions who nag, and failure
as the usual exit give the campaign its guided tone. The worth gates push
the player into the economy.

**Accidents of its era.** One condition per trigger. Activation in
identifier sequence. Mission order, increments and per-mission hooks in
the program. Ambient switches as process-wide flags, tolerable only in
single player. A generated mission rebuilt from a seed, which ties saved
games to the generator's exact draw sequence. Sites found by substring.

**Simplifications.** Twenty actions and ten conditions cover most of the
campaign. A modern trigger language would allow boolean expressions and
queue activations as written. Store a composed generated mission, not its
seed. The offer tree could be a flat list of templates; it has six leaves.

## 7. Open questions

1. What follows a failed scripted mission. No restart was found; that the
   player must load a save is inferred.
2. What "hold to directives" and "lifetime" do exactly; their consumers
   in the NPC code were not traced.
3. Chapter 02 says mission scripts can engage cruise instantly. The
   authority offers that service to content, but no caller was found in
   the actions or directives, which only demand or forbid cruise.
4. How mission ships written by a checkpoint combine with what the resume
   trigger spawns.
5. Whether the ±0.15 reputation reward and penalty stored on each offer
   are used, or only the per-faction event rates.

## 8. Basis

Design: `design/assessment/04-server-content.md` sections 3, 6 and 7.

Data: `MISSIONS/M08/m08.ini` read for form, all 14 definitions
tabulated; the two `mission1 readme.doc`; one on-base scene file of
`M08`; `MISSIONS/mbases.ini`, `news.ini`, `rankdiff.ini`, `empathy.ini`;
`RANDOMMISSIONS/*.ini`; `outdir/OutpostFrontier.ini`; `outdir/newplayer.fl`.

Code: `Content/MissionScript.cpp` (`GCndFactory`, `GActFactory`),
`MissionCnd.cpp`, `MissionAct.cpp`,
`MissionObjList.cpp`, `MissionObject.cpp`, `MissionSave.cpp`, `Story.cpp`,
`StoryLauncher.cpp`, `StoryMissions.cpp`, `RtcScripts.cpp`, `BaseDB.cpp`,
`BaseNpcRecords.cpp`, `BaseSupport.cpp`, `Populators.cpp`, `LootProps.cpp`,
`DestroyMission.cpp`, `RandomMissionDB.cpp`, `RandomMissionGen.cpp`,
`RandomMissionVignette.cpp`, `RandomMissionMachine.cpp`, `Zones.cpp`;
`Server/server_pub.cpp` (`Save`, `SetInvincible`, `ActivateEquipment`);
`OutpostFrontier/Client/nn_story_star.cpp`.

Inferred: retry by loading; the reading of the unused conditions and
actions and of "excuse fire", "bound enemies", "lifetime" and "hold to
directives", which rests on parameters and names. The run-time states of
a generated mission were read from the reconstruction's comments. The
worked reward was computed from the table.
