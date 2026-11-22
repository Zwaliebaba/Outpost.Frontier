# Concept blueprint: multi-agent prompt

Start a session at the repository root with:

> Read `blueprint/PROMPT.md` and execute it as the lead agent.

Part A is for the lead. Parts B and C bind every agent. Part D holds one
brief per chapter, Part E the reviewer brief, Part F the cold-reader brief.
Subagents are told to read the parts that apply to them from this file, so
the lead never retypes them.

---

## Part A — Lead agent

### A1. Mission

Produce a concept blueprint of the game implemented in this repository: a
space trading and combat game with a data-driven universe, a scripted
campaign, generated missions, single player and multiplayer.

The reader is a **builder agent** that will create a comparable game. It
has no access to this repository, has never played the original, and
chooses its own engine, language and platform. It reads `Blueprint.md`
first and then the chapters its current task needs.

The blueprint is a basis to start from, not a specification. It is
finished when the builder understands each part of the game well enough to
explain how it works, why it is built that way and how the parts depend on
each other, and can then design its own version. It records the concepts,
their relations, the governing rules, the main formulas and representative
numbers. It does not record every edge case, every parameter or every
exception; the builder will make those decisions itself.

### A2. Deliverables

| Path | Content |
| --- | --- |
| `Blueprint.md` | Entry point: the game on one page, the player's journey, architecture, chapter map, suggested build order, content budget, what the blueprint does not cover. At most 5,000 words. Written last. |
| `blueprint/00-glossary.md` | The shared vocabulary. Owned by the lead. |
| `blueprint/NN-<slug>.md` | One chapter per brief of Part D, 2,500 to 5,000 words each. |

Write nothing else into the repository. Scratch files go to the session's
scratch directory.

### A3. Your role

You coordinate and synthesise; the specialists extract. Do not read C++
sources yourself. Chapters are written to disk; a specialist's return
message is a short report, never the chapter text.

Ask the user only when a source tree named in B1 is missing or unreadable.
Everything else you decide.

### A4. Phases

**Phase 0 — Orientation.** Read the table of documents in
`design/README.md` (not its progress log), `design/port-map.md` and the
summary and first section of each file in `design/assessment/`. Write
`blueprint/00-glossary.md`: about 50 neutral terms for the concepts the
chapters will share, one line each. Check which chapters already exist and
resume from there.

**Phase 1 — Extraction.** Spawn one specialist per chapter (A5), in two
launches, in the background:

1. Chapters 01, 02, 11 and 12, which the others build on.
2. Chapters 03 to 10, each told to read the "Concepts" section of the
   chapters from the first launch.

After each launch merge the glossary proposals into chapter 00 and settle
conflicting terms: one concept, one word.

**Phase 2 — Spot review.** Spawn three reviewers (Part E), each with four
chapters: 01 to 04, 05 to 08, 09 to 12. Send each chapter's findings to
its specialist, continuing that agent so that it keeps its context, and
have it fix the chapter. One round.

**Phase 3 — Synthesis.** Read all chapters and write `Blueprint.md`:

1. How to use this blueprint, and what it deliberately leaves to the
   builder.
2. The game on one page: pillars, the core loops (minute to minute, hour
   to hour, whole game), modes of play.
3. The player's journey as a state diagram: front end, loading, space,
   base, cutscene, death, and the transitions between them.
4. Logical architecture: components, which side is authoritative for
   what, the tick, the principle that data drives content.
5. How the systems feed each other: one matrix or diagram.
6. Chapter map: file, scope in one line, "read this when…".
7. Suggested build order: vertical slices from first flyable ship to full
   game, each naming the chapters it draws on.
8. Content budget: how much content of each kind a comparable game needs,
   from chapter 12.
9. What can be left out, and what the game loses.
10. What the blueprint does not know: the open questions that matter most.

**Phase 4 — Acceptance.** Spawn one cold reader (Part F). Route what it
could not explain to the owning specialists, update `Blueprint.md`, and
run the neutrality lint (B3). Report to the user: what was produced, word
counts, and which parts of the game the blueprint understands least.

Commit after phases 1, 2 and 4 on the current branch, with a message that
starts with `blueprint:`. Push only if your session's instructions name
the branch to push to. Do not open a pull request.

### A5. Spawning

Use general-purpose subagents; they must be able to write files. Use this
prompt, filled in, and add nothing that Parts B to F already say:

```text
You are the specialist for chapter NN (<title>) of a game concept
blueprint. Working directory: the repository root.

Read these parts of blueprint/PROMPT.md completely before anything else:
A1 (mission and reader), Part B (ground rules), Part C (chapter contract)
and brief D<NN> in Part D. Then read blueprint/00-glossary.md.

<Second launch only:> Read the "Concepts" section of chapters 01, 02, 11
and 12 and use their terms.

Write blueprint/NN-<slug>.md. Finish with the report that C5 asks for,
and nothing else.
```

Reviewers and the cold reader get the same form with Part E or Part F in
place of Part C and the brief.

---

## Part B — Ground rules for every agent

### B1. Sources, and what each is for

1. **`design/` — structure.** The assessments, decision records, facts and
   `sim-records.md` tell you which subsystems exist, where their code is
   and how they connect. Start here. Ignore everything that concerns the
   Rust port: destinations, work packages, gates, crate names, the
   progress log.
2. **`outdir/` — concepts and numbers.** The INI files at its top and
   under `outdir/data/` name every concept of the game with its
   parameters. Tabulate sections, keys and value ranges with a script;
   read a few records of each kind in full.
3. **`CppSource/` — behaviour, on demand.** Read code when the documents
   and the data name a concept or a parameter but do not tell you what it
   does. Your brief lists the files that carry the rules. Read the parts
   that answer your question, not your whole scope; expect to open ten to
   thirty files, most of them in part.

When sources disagree, code outranks data and data outranks documents.

Not sources: `crates/`, `apps/`, `xtask/` (a port in progress) and
anything outside the repository. **Do not fill a hole from what you
remember about any published game.** If the three sources do not show it,
it is an open question.

Do not read: `CppSource/DX8/`, `zlib/`, `qhull/`, `SceneScript/lua32/`,
`luaprofile/`, `MemoryDiagnostics/`, `RenderTrace/`, `Oracle/` (except
`Oracle/scenarios/`, short scripted flight scenarios that show the
simulation's inputs), any `oracle_*` file, project and resource-script
files.

Know what you are reading:

- The C++ was reconstructed from binaries. Function boundaries and
  exported names are reliable; many fields are named `Unknown…`, `Pad…`
  or `Field…`. Establish meaning from use, or leave an open question.
- Code marked `port-only` (widescreen, the Direct3D 9 renderer, walk
  mode, the profiler) is not part of the original design. Leave it out.
- Names in code and data were partly changed by a rebrand
  (`design/rebrand-names.md`); file and directory names keep the older
  spelling. Neither set of names goes into the blueprint (B3).

### B2. Read only

Change nothing outside `Blueprint.md` and `blueprint/`. Do not build or
run anything in the repository. You may run your own read-only scripts
over the sources; keep them and their output in the scratch directory.

### B3. Neutrality: mechanics in, setting out

The blueprint serves the making of a comparable game, not a second copy
of this one.

- **No proper nouns of the setting**, in either spelling: no faction,
  character, star system, planet, base, ship, weapon, commodity or
  mission names, and no nickname that encodes one. Use role descriptions
  ("a major house", "its police faction", "an outlaw faction", "the
  campaign's antagonist") and placeholder nicknames that start with
  `example_`.
- **No narrative.** No plot, dialogue, briefing, news, rumour or
  info-card text. Describe the campaign as structure: how many missions,
  the kinds of objective they string together, what gates the next one.
- **No look and sound.** Say what the presentation must convey and how
  it behaves, not the art direction, the interface skin or the music.
- **Rules and global constants as they are; item lists in shape.** For
  per-item content (ships, guns, goods, bases) give the schema, the value
  ranges and the progression, with at most three anonymised example
  rows. Never reproduce a table of the original's items.
- **No code.** Pseudocode in your own words. C++ identifiers and file
  names appear only in a chapter's final "Basis" section.

The lint in Phase 4 searches the blueprint, outside the "Basis" sections,
for the names in `design/rebrand-names.md`, the faction nicknames in
`outdir/data/initialworld.ini`, the system and base nicknames in
`outdir/data/UNIVERSE/universe.ini` and the directory names under
`outdir/data/SHIPS/`, `BASES/` and `AUDIO/`.

### B4. Technology neutrality

Describe behaviour, not the implementation: no DLL boundaries, COM
interfaces, class hierarchies, Direct3D or Windows concepts. Where an
implementation trait shapes what the player experiences (a fixed
simulation rate, an update order), state it as a rule. The rest of the
original technology belongs to one section of chapter 11.

### B5. Honesty

Every number and every formula in a chapter was read in the data or the
code. What you reasoned out instead is marked "(inferred)"; what you took
from a design document without checking is marked "(per design notes)".
What you could not establish is an open question, never a plausible
invention. A chapter with honest gaps is acceptable; one with confident
errors is not.

---

## Part C — Chapter contract

### C1. Header

```markdown
# NN — <Title>

Status: draft | reviewed
Depends on: <chapters>
```

### C2. Sections, in this order

1. **Purpose and player experience.** What this part of the game is for
   and what the player sees and does. Half a page.
2. **Concepts.** Each concept defined once, and how the concepts relate:
   a table or a small diagram of what contains, references or drives
   what.
3. **How it works.** The governing rules in prose, the main formulas with
   units, the main state machines as tables (state, event, next state),
   and what runs on the authoritative side against the presentation side.
   Cover the normal path thoroughly and the important exceptions briefly.
4. **Data that defines it.** Which kinds of record an author writes to
   create this content, their principal fields, and representative values
   and ranges.
5. **Connections.** What this chapter needs from others and gives to
   others, by chapter number.
6. **Design notes for the builder.** What is essential to the game, what
   merely gives it its feel, what is an accident of its era, and what can
   be simplified at what loss.
7. **Open questions.**
8. **Basis.** The design documents, data files and source files the
   chapter rests on, and which parts rest on inference.

### C3. Depth

The test: after reading the chapter, could the builder explain this part
of the game to someone else, including why a rule exists and what would
break without it? Prefer the reason behind a rule to one more parameter.
Prefer one worked example with numbers to an exhaustive table.

### C4. Method

1. Read the design documents your brief names, and search `design/` for
   the terms of your chapter.
2. Tabulate the data your brief names; read sample records in full.
3. List the concepts and parameters you cannot yet explain. Read the code
   your brief names until you can, following it elsewhere when needed.
4. Answer every "key question" of your brief, or list it as open.

### C5. Report to the lead

At most 200 words: file written and word count; glossary terms you
propose; what you found that belongs to another chapter; your three most
important open questions.

---

## Part D — Chapter briefs

Paths are relative to `CppSource/` for code and `outdir/data/` for data
unless they start with `design/` or `outdir/`. A stem after `Common/`
stands for the files `Common/common_<stem>*`; other stems are file name
prefixes as written. "Client" is `OutpostFrontier/Client/`. Most headers
are in `include/` under the same stem. Assessments are the files in
`design/assessment/`.

### D01 — World and universe

Design: assessment 02 sections 1 and 3.
Data: `UNIVERSE/` (the universe file, two system files in full, the
shortest-path files), `SOLAR/` (the archetype files, one asteroid field
and one nebula definition).
Code: `Common/universe`, `zone`, `exclusionzone`, `casteroidfield`;
`Content/SystemConnections`, `Zones`; `x86math/` for axis and rotation
conventions only.

Key questions: the hierarchy from universe to system to zone and object
to base; coordinate system, units and typical scales; what a system
definition declares; what zones are and everything they are used for; how
asteroid fields and nebulae are generated from a compact definition; how
systems connect and how routes are found; what the player's map knows and
how discovery works.

### D02 — Ships, equipment and flight

Design: `design/adr/0001-object-model.md`, `design/sim-records.md`,
`design/adr/0003-simulation-oracle.md`, assessment 02 sections 1 and 4.
Data: `SHIPS/shiparch.ini`, the loadout files, `EQUIPMENT/engine_equip.ini`,
`misc_equip.ini`, `select_equip.ini`, `constants.ini`.
Code: `Common/cship`, `ceqobj`, `cequip`, `ceengine`, `cethruster`,
`csteering`, `physys`, `phycontroller`, `thrustmanager`, `cecargo`;
`Oracle/scenarios/free_flight.txt`.

Key questions: the kinds of world object; archetype against instance;
hardpoints and the rules for mounting; the equipment categories and what
each contributes; the flight model (the forces and damping terms, how top
speed and turn rate emerge from them, the simulation step); throttle,
strafe, thruster, engine kill and cruise; how the player's steering input
becomes motion; collisions in outline.

### D03 — Combat and damage

Design: search `design/` for shields, fuses, projectiles and damage.
Data: `EQUIPMENT/weapon_equip.ini`, `weaponmoddb.ini`, `st_equip.ini`,
`FX/explosions.ini`, `FX/fuse.ini`, `MISSIONS/lootprops.ini`,
`constants.ini`.
Code: `Common/cegun`, `celauncher`, `cprojectile`, `cmine`, `cedroppers`,
`ceshield`, `cepower`, `fuses`, `damagelist`, `repairmanager`;
`Server/server_guided`, `server_loot`, the damage path in
`server_gameobject`.

Key questions: the weapon classes and their firing cycle (refire, energy,
speed, range); guided munitions and what defeats them; mines; the path of
damage from hit to shield to hull to component, with the weapon-type
against shield-type modifiers; how shields and power regenerate; area
damage; fuses as timed destruction sequences; death and dropped loot;
repair consumables; weapon groups.

### D04 — Travel, docking and navigation

Design: `design/adr/0007-server-world.md`, assessment 04 section 1.
Data: the docking fields in `SOLAR/solararch.ini`, the shortest-path
files in `UNIVERSE/`, `constants.ini`.
Code: `Common/dockbehavior`, `launchaction`, `laneaction`,
`cruisebehavior`, `gotobehavior`; `Content/DockQueues`,
`TradelaneRequests`; Client `best_path`; the docking, lane and launch
scenarios in `Oracle/scenarios/`.

Key questions: the manoeuvres a player can order; the docking sequence
(request, permission, queue, approach, the kinds of dock); launching;
travel along a chain of lane rings and what interrupts it; transfer
between systems; route planning and waypoints across systems; what
refuses a dock.

### D05 — AI and pilots

Design: assessment 02 section 1 (the AI row) and section 2.
Data: `AI/state_graph.db`, `MISSIONS/pilots_population.ini`,
`MISSIONS/formations.ini`.
Code: `Common/behaviormanager`, `behaviortypes`, four behaviours read
closely (`buzzbehavior`, `evadebehavior`, `formationbehavior`,
`strafebehavior`) and the others by their headers, `aisteer`,
`obstacleavoidance`, `aifirecontrol`, `targetmanager`, `iustategraph`,
`pub`; `Content/PilotPersonalities`; the dogfight and formation scenarios
in `Oracle/scenarios/`.

Key questions: the layers of the AI (who decides what to do, who turns
that into steering); the list of behaviours with one paragraph each; how
the state graph selects the next combat manoeuvre; what a pilot's
personality is made of and what each block modulates; target selection;
fire control and accuracy; when a pilot uses consumables, flees or drops
cargo; formations; how difficulty scales pilots.

### D06 — Population and encounters

Design: assessment 04 sections 3 and 6.
Data: `MISSIONS/ENCOUNTERS/`, `MISSIONS/npcships.ini`, `shipclasses.ini`,
`faction_prop.ini`, `rankdiff.ini`, and the population keys of the zones
in two system files.
Code: `Content/Encounters`, `EncounterParams`, `Populators`,
`SpawnPlacement`, `PatrolPaths`, `NpcCreate`, `NpcNames`, `Gcs`.

Key questions: how a zone declares who lives in it; the population loop
(density, caps, repopulation, distance to players, spawning and removal);
what an encounter definition contains; how a spawned NPC's ship, loadout,
pilot and level are chosen; how difficulty varies by place; what ambient
NPCs do with their time; how names and radio chatter are assembled, as a
mechanism.

### D07 — Factions, economy and progression

Design: search `design/` for reputation, goods, market and rank.
Data: `initialworld.ini`, `MISSIONS/empathy.ini`, `faction_prop.ini`,
`EQUIPMENT/goods.ini`, the `market_*.ini` and `*_good.ini` files,
`commodities_per_faction.ini`, `MISSIONS/ptough.ini`,
`RANDOMMISSIONS/diff2money.ini`.
Code: `Common/reputation`, `vibe`, `goodinfo`, `tradeoffer`,
`repairmanager`; `Content/CombatReputation`, `BaseDB`;
`Server/server_basedb`.

Key questions: the reputation scale and its thresholds; how an event
changes standing with one faction and propagates to its friends and
enemies; everything reputation affects; the kinds of good; how a price at
a base is formed, buying against selling; what restricts a purchase;
buying a ship and trading in the old one; the sources and sinks of money;
how the player's level is computed and what it unlocks; contraband. In
shape: number of commodities, spread of prices, profit of typical routes.

### D08 — Missions and base services

Design: assessment 04 section 3.
Data: two mission definitions under `MISSIONS/` read in full for their
form, the two `mission1 readme.doc` files as format notes,
`RANDOMMISSIONS/`, `MISSIONS/mbases.ini`, `MISSIONS/news.ini`.
Code: `Content/MissionScript` (the tables of conditions and actions),
`MissionCnd` and `MissionAct` (for the meaning of each), `MissionObjList`,
`Story`, `RandomMissionGen`, `RandomMissionVignette`, `BaseNpcRecords`.

Key questions: the anatomy of a mission definition; the trigger model;
the conditions and actions as a catalogue grouped by purpose, one line
each, in your own words; the directive language for scripted ships; how
objectives reach the player; how missions chain and what gates the next;
failure and retry; how a job offer is generated (who, against whom,
where, how hard, for how much); how a generated mission is composed and
run; who the player meets on a base and what each offers; the campaign
as structure only (B3).

### D09 — In-flight interface and presentation

Design: assessment 06 sections 1 and 2, assessment 05 section 9,
assessment 07 sections 3 and 7.
Data: `INTERFACE/hud.ini`, `keymap.ini`, `keylist.ini`, `mouse.ini`,
`cameras.ini`, the INI files in `AUDIO/`, `FX/effects.ini` and
`effect_types.ini`.
Code: Client `input_manager`, `main_command_handler`, `hud_target`,
`hud_contact_list`, `hud_maneuvers`, `hud_status_core`,
`space_camera_third`, `space_camera_game`, `space_scene_draw`,
`menu_sound_arch`.

Key questions: the catalogue of player commands, grouped; mouse flight
against free cursor; each HUD element and what it tells the player;
targeting and the contact list; the camera modes; what a frame in space
is composed of, as requirements on any engine; what the effect system
must be able to do; the sound categories, how music follows the
situation, how voice lines are assembled and queued.

### D10 — Bases, scenes, menus and text

Design: assessment 06 sections 1 (rows 8 to 11) and 5, assessment 07
sections 1 and 6.
Data: one base under `BASES/` in full, the kinds of script under
`SCRIPTS/`, the INI files in `CHARACTERS/`, the rest of `INTERFACE/`,
the layout of `text/`.
Code: Client `gameflow`, `client_state`, `room_control`, `gf_script`,
`engine_character`, `nn_navmap`, `nn_load_save`, `page_character_select`,
`ids_strings`; `Common/roomdata`, `costume`, `fmtstr`; the interface
header of `SceneScript/`.

Key questions: a base as a set of rooms and what each room offers; how
rooms, hotspots and characters are defined; what a scene script can
control, as a capability list; the state machine of a base visit from
landing to launch; the screen flow from start to play; the navigation
map and what it reveals when; the multiplayer front end; what the UI
toolkit must provide; how text is identified, formatted and localised.

### D11 — Architecture, authority, networking and persistence

Design: `design/adr/0002-single-player-world.md`,
`0006-controller-bus.md`, `0007-server-world.md`, assessments 01, 03, 04
(sections 1 and 2), 06 (section 2) and 08; decisions T17 to T19 in
`design/decisions.md` as hints only.
Data: the files at the top of `outdir/`.
Code: `include/rpc_iserver.h`, `include/rpc_iclient.h`,
`Server/IServerImpl`, `server_savegame`, `server_pub`; Client
`client_frame_tick`; `Content/Content.cpp`, `PlayerState`.

Key questions: the logical components and which is authoritative for
what; the order of work within a tick on each side; the messages in both
directions, grouped by purpose, with what each means; how object state
reaches other players and what the receiver predicts; single player as
the same authority in process, and how it differs from multiplayer; what
a saved game contains and when saving is allowed; accounts and
characters; multiplayer rules; what a dedicated host can administer.
Close with a section on the original technology: what it stood on, which
behaviours of the game follow from it, and what a modern builder should
not copy.

### D12 — Data model and content census

Design: `design/facts.md`, assessment 08 section 8.
Data: every text file under `outdir/`; binary assets by kind and count.
Code: `Common/dataconfig`, `ini`, and readers only where a file's
meaning is unclear.

Deliver: the manifest that says which configuration file names which
data files; for each kind of data file its purpose, its sections and its
principal keys; for each kind of asset what the game needs from it (a
model's parts and hardpoints, a collision hull, an animation, an effect,
a scene script, a voice line), without binary layouts; the census as
numbers and distributions, never names (systems, bases per system,
connections per system, zones per system, ships by class, weapons by
class, commodities, factions, encounter definitions, missions, scripts,
voice lines); a proposal for expressing the same data model in a modern,
neutral form.

---

## Part E — Reviewer brief

You review four chapters that you did not write. Read A1 and Part B, then
each chapter. Do not edit them; return your findings to the lead, grouped
by chapter, in at most 500 words in total.

1. **Correctness.** In each chapter pick the eight statements a builder
   would lean on most (formulas, state machines, governing rules) and
   check them against the code or data named in the chapter's "Basis".
   Report what is wrong, with what the source says.
2. **Understanding.** After reading, is there a concept you could not
   explain to someone else, or a rule whose reason is missing?
3. **Rules.** Violations of B3 (names, narrative, item tables, code), of
   B4 (implementation concepts) and of B5 (guesses stated as facts,
   anything that looks recalled rather than read).

---

## Part F — Cold-reader brief

You are the builder of A1. You may read `Blueprint.md` and
`blueprint/*.md` and nothing else in the repository. Do not use what you
know about any published game.

Write, for yourself, an account of one complete play session as the
systems experience it: starting a new game, launching from a base,
flying to another system, meeting and fighting an NPC group, picking up
loot, docking, selling, taking a generated mission and completing it. At
each step name the rules and data that make it happen.

Return, in at most 500 words: every step you could not explain from the
blueprint, the contradictions between chapters, terms used without a
definition, and anything that reads as belonging to a particular setting
or technology. Do not return the account itself.
