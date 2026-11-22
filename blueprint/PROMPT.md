# Blueprint extraction: multi-agent prompt

Start a session at the repository root with:

> Read `blueprint/PROMPT.md` and execute it as the lead agent.

Part A is for the lead. Parts B and C bind every agent. Part D holds one
brief per chapter, Part E the reviewer brief, Part F the cold-reader brief.
Subagents are told to read the parts that apply to them from this file, so
the lead never retypes them.

---

## Part A — Lead agent

### A1. Mission

Produce a blueprint of the game implemented by the C++ code base in
`CppSource/`: a space trading and combat game with a data-driven universe, a
scripted campaign, generated missions, single player and multiplayer.

The reader of the blueprint is a **builder agent** that will create a
comparable game. Assume the builder:

- has no access to this repository, the original game or its data;
- has never played the original;
- chooses its own engine, language and platform;
- reads `Blueprint.md` first and then only the chapters its current task
  needs.

The blueprint is finished when that builder can implement each subsystem
without having to guess a rule, a formula, an ordering or a data shape.

### A2. Deliverables

| Path | Content |
| --- | --- |
| `Blueprint.md` | Entry point: the game on one page, architecture, chapter map, build order, content budget, gaps. At most 6,000 words. Written last. |
| `blueprint/00-conventions-and-glossary.md` | Conventions of Part C and the shared vocabulary. Owned by the lead. |
| `blueprint/NN-<slug>.md` | One chapter per domain of Part D. |
| `blueprint/A-original-technology.md` | What the original was built on and which of its traits are accidents of that technology. |
| `blueprint/B-gaps.md` | Everything that could not be established, with what was tried. |
| `blueprint/evidence/NN-<slug>.evidence.md` | Per chapter: where each statement comes from. For audit; the builder does not read it. |
| `blueprint/evidence/NN-<slug>.review.md` | Per chapter: the reviewer's findings and their resolution. |

Write nothing else into the repository. Scratch files go to the session's
scratch directory.

### A3. Your role

You coordinate; you do not extract. Keep your own context for synthesis:

- Do not read C++ sources beyond what you need to assign scope or settle a
  dispute between two agents.
- Chapters are written to disk by the specialists. Their return message to
  you is a short report, never the chapter text.
- You own `00-conventions-and-glossary.md`, `B-gaps.md` and `Blueprint.md`.

Ask the user only when a source tree named in B1 is missing or unreadable,
or when a scope decision would change the structure of more than one
chapter. Everything else you decide, and record in `B-gaps.md` when the
decision hides an uncertainty.

### A4. Phases

**Phase 0 — Orientation.** Read `design/README.md` (the table of documents,
not the progress log), `design/port-map.md` and the summary and first
section of each file in `design/assessment/`. Then:

1. Write `blueprint/00-conventions-and-glossary.md`: the conventions of
   Part C restated for the builder, the unit system, and a seed glossary of
   neutral terms (about 60) for the concepts the chapters will share.
2. Run the coverage check: list every directory under `CppSource/` that B1
   does not exclude and every directory directly under `outdir/data/`, and
   confirm that each is named in at least one brief of Part D. Assign any
   orphan to the nearest chapter and say so in that agent's spawn prompt.
3. Check which chapters already exist and read their `Status:` line. Resume
   from there; never redo a chapter whose status is `accepted`.

**Phase 1 — Extraction, in three waves.** Spawn one specialist per chapter
(A5). Launch a wave's agents in a single message, in the background, at
most six at a time.

| Wave | Chapters | Why this order |
| --- | --- | --- |
| 1 | 01, 02, 03, 17, 18 (census step) | Everything else refers to the world, the object model, flight, the authority split and the data shapes. |
| 2 | 04, 05, 06, 07, 09, 10 | Built on the object model and flight. |
| 3 | 08, 11, 12, 13, 14, 15, 16, A | Built on AI, reputation, economy and travel. |

After each wave, merge the glossary proposals into chapter 00, resolve
conflicting terms (one concept, one word), and pass interface requests from
one chapter to the chapter that must answer them.

**Phase 2 — Review, after each wave.** For every chapter of the wave spawn
a fresh reviewer (Part E). It writes `NN-<slug>.review.md` and does not
edit the chapter. Send the review to the chapter's specialist, continuing
that agent so that it keeps its context; if it can no longer be reached,
spawn a new specialist with the chapter, the evidence file and the review.
The specialist fixes the chapter and answers each finding in the review
file. Send the fixed chapter back to the reviewer once. A chapter with no
open finding of severity *wrong* or *missing* gets `Status: reviewed`. Stop
after two rounds; what is still open goes to `B-gaps.md`. Start the next
wave only when the current one is reviewed.

**Phase 3 — Reconciliation.** Continue the chapter 18 specialist with its
reconcile step (brief D18). Route each finding to the owning chapter's
specialist. Then run, as scripts of your own:

- every source file in scope that no evidence file cites: hand the list to
  the owning specialist to read or to declare irrelevant with a reason;
- every statement ID referenced from another chapter resolves;
- every chapter stays within its word budget.

**Phase 4 — Synthesis.** Read all chapters (they are now your main input)
and write `Blueprint.md`:

1. How to use this blueprint: reading order, the fidelity tags, statement
   IDs, what the evidence directory is.
2. The game on one page: pillars, the core loops (minute to minute, hour
   to hour, whole game), modes of play.
3. The player's journey as a state diagram: front end, loading, space,
   base, cutscene, death, and every transition between them.
4. Logical architecture: components, which side is authoritative for what,
   the tick, the principle that data drives content.
5. System interaction matrix: which chapter provides what to which.
6. Chapter map: file, scope in one line, "read this when…", word count.
7. Build order: vertical slices from first flyable ship to full game.
   For each slice the chapter sections it needs and the acceptance checks
   (by ID) that prove it.
8. Content budget: how much content of each kind a comparable game needs,
   from the census of chapter 18, and what must be authored by hand.
9. Scope reduction menu: what can be left out, and what the game loses.
10. Gaps: the ten most consequential entries of `B-gaps.md`.

**Phase 5 — Acceptance.** Spawn two cold readers (Part F) with different
tasks. Route every blocking question to the owning specialist, then update
`Blueprint.md`. Run the neutrality lint (B3) over `Blueprint.md` and
`blueprint/*.md`. Set `Status: accepted` on chapters that pass. Report to
the user: what was produced, word counts, the open gaps, and which
subsystems the blueprint knows least about.

Commit after each wave and after phases 3, 4 and 5, on the current branch,
with a message that starts with `blueprint:`. Push only if your session's
instructions name the branch to push to. Do not open a pull request.

### A5. Spawning

Use general-purpose subagents; they must be able to write files. Use this
prompt, filled in, and add nothing that Parts B to F already say:

```text
You are the specialist for chapter NN (<title>) of a game blueprint.
Working directory: the repository root.

Read these parts of blueprint/PROMPT.md completely before anything else:
A1 (mission and reader), Part B (ground rules), Part C (chapter contract)
and brief D<NN> in Part D. Then read
blueprint/00-conventions-and-glossary.md.

<Wave 2 and 3 only:> These chapters exist and are reviewed. Read their
"Interfaces" and "Concepts" sections and refer to their statement IDs
instead of restating them: <list>.

<Any orphan directories or interface requests assigned to this chapter.>

Write blueprint/NN-<slug>.md and blueprint/evidence/NN-<slug>.evidence.md.
Finish with the report that C7 asks for, and nothing else.
```

Reviewers and cold readers get the same form with Part E or Part F in
place of Part C and the brief.

---

## Part B — Ground rules for every agent

### B1. Sources and their precedence

1. **`CppSource/` — the behaviour.** The code is the authority on what the
   game does.
2. **`outdir/` — the content.** The INI files at its top and the tree in
   `outdir/data/` show which features the shipped game uses, with what
   values and in what quantities. A code path that no data exercises is
   *dormant*; say so.
3. **`design/` — a head start.** The assessments, facts, decision records
   and `sim-records.md` tell you where things are and what earlier readers
   concluded. Verify every fact you take from them in the code. When they
   disagree with the code, the code wins and the evidence file records the
   disagreement. Ignore everything in `design/` that concerns the Rust
   port: destinations, work packages, gates, crate names.

Not sources: `crates/`, `apps/`, `xtask/` (a port in progress, with its own
decisions) and anything outside the repository. Do not use what you
remember about any published game; if the code and data do not show it, it
is a gap.

Excluded from reading, except where a brief says otherwise:
`CppSource/DX8/` (a vendor SDK), `CppSource/zlib/`, `CppSource/qhull/`,
`CppSource/SceneScript/lua32/`, `CppSource/luaprofile/`,
`CppSource/MemoryDiagnostics/`, `CppSource/RenderTrace/`, project and
resource-script files.

Know what you are reading:

- The C++ was reconstructed from binaries. Function boundaries and exported
  names are reliable; many fields are named `Unknown…`, `Pad…` or `Field…`
  and many virtual methods `SlotN`. Establish their meaning from how they
  are used, or report a gap. Never present a guess as a fact.
- Code marked `port-only` (widescreen support, the Direct3D 9 renderer,
  walk mode, the profiler) and the test instrumentation (`CppSource/Oracle/`
  and every `oracle_*` file) were added during reconstruction and are not
  part of the original design. Leave them out, or tag a port-only feature
  [I] where the builder benefits from knowing it exists.
- The one exception: `CppSource/Oracle/scenarios/` holds scripted
  simulation scenarios. Read them as seeds for acceptance checks.
- The names in code and data were partly changed by a rebrand
  (`design/rebrand-names.md`). File and directory names keep the older
  spelling. Neither set of names goes into the blueprint (B3).

### B2. Read only

Change nothing outside `Blueprint.md` and `blueprint/`. `CppSource/` is
frozen and `outdir/` is the only copy of the data. Do not build or run the
C++ or the Rust workspace. You may run your own read-only scripts over the
sources (counting, grepping, tabulating INI keys); keep scripts and their
output in the scratch directory.

### B3. Neutrality: mechanics in, setting out

The blueprint serves the making of a comparable game, not a second copy of
this one. It records how the game works and how much of everything there
is. It does not carry the setting.

- **No proper nouns of the setting**, in either the older or the rebranded
  spelling: no faction, character, star system, planet, base, ship, weapon,
  commodity or mission names, and no nickname that encodes one. Use role
  descriptions ("a major house", "the police faction of a house", "an
  outlaw faction", "the campaign's antagonist faction") and placeholder
  nicknames that start with `example_`.
- **No narrative.** No plot summary, dialogue, briefing, news, rumour or
  info-card text. Describe the campaign as structure: how many missions,
  the sequence of objective types in each, which script features each
  uses, what gates the next one.
- **No look and sound.** Describe what the presentation must convey and
  how it behaves, not the art direction, the interface skin or the music.
- **Rules and global constants exactly; item tables in shape.** Formulas,
  thresholds, state machines and global tuning values are recorded
  exactly. For per-item content (the list of ships, of guns, of goods, of
  bases) record the schema, the value ranges, the distribution and the
  progression curve, with at most three anonymised example rows. Never
  reproduce a table of the original's items.
- **No code.** Express algorithms as language-neutral pseudocode in your
  own words. C++ identifiers, file names and line numbers belong in the
  evidence file only.

The lint in Phase 5 searches the blueprint for the names in
`design/rebrand-names.md`, the faction nicknames in
`outdir/data/initialworld.ini`, the system and base nicknames in
`outdir/data/UNIVERSE/universe.ini` and the directory names under
`outdir/data/SHIPS/`, `BASES/` and `AUDIO/`.

### B4. Technology neutrality

Describe behaviour, not the implementation. No DLL boundaries, COM
interfaces, class hierarchies, Direct3D or Windows concepts in a chapter.
Where an implementation trait shapes what the player experiences (a fixed
simulation rate, the order in which systems update, a quantised network
value), state it as a rule and tag it. Everything else about the original
technology goes to appendix A.

### B5. Honesty about what you know

Every normative statement has a basis, recorded in the evidence file:
`code` (read in the code), `data` (observed in the data), `both`,
`inferred` (reasoned from code or data, not read directly) or `unknown`.
An `inferred` statement says so in the chapter text as well. What you could
not establish is an open question in the chapter and never a plausible
invention. A chapter with honest gaps is acceptable; a chapter with
confident errors is not.

---

## Part C — Chapter contract

### C1. Header

```markdown
# NN — <Title>

Status: draft | reviewed | accepted
Prefix: XXX
Depends on: <chapters>
Words: <count>
```

### C2. Statement IDs and fidelity tags

Every rule, formula, state machine, parameter and acceptance check carries
an ID, `XXX-nnn`, with the chapter's prefix from Part D. IDs are stable:
never renumber, retire instead. Other chapters refer to a rule by its ID.

Every statement carries one fidelity tag:

- **[E] Essential.** Defines the game. A comparable game has it.
- **[C] Characteristic.** Gives the game its particular feel. Reproduce it
  unless there is a reason not to.
- **[I] Incidental.** An accident of the implementation or its era,
  recorded so that the builder can drop or modernise it knowingly.

### C3. Sections, in this order

1. **Purpose and player experience.** What this subsystem is for and what
   the player sees of it. Half a page.
2. **Concepts.** Terms this chapter introduces, each defined once.
3. **Data model.** Entities, their fields, types, units, ranges, defaults
   and relations. Tables. Distinguish authored data from run-time state.
4. **Rules and algorithms.** Numbered. Pseudocode for anything with more
   than one branch. Formulas with units and with the order of operations
   where it changes the result.
5. **State machines.** One table each: state, event, guard, action, next
   state. Include the entry state and every exit.
6. **Timing, ordering and authority.** What runs when, how often, on which
   side (authoritative simulation or presentation), and what must be
   ordered before what.
7. **Tuning parameters.** Name, value, unit, what it changes, where the
   builder would feel a change.
8. **Interfaces.** What this chapter needs from others and offers to
   others, by statement ID.
9. **Edge cases and failure behaviour.** What happens at the limits and
   when inputs are missing or contradictory.
10. **Acceptance checks.** Observable tests in the form "given, when,
    then", with numbers, that a builder can run against its own
    implementation.
11. **Simplification options.** What may be left out, and what is lost.
12. **Open questions.** What you could not establish, and what you tried.

A section that does not apply is kept with one line saying why.

### C4. Depth

The test for depth: if the builder asks "what happens when…" and the code
answers it, the answer is in the chapter. Prefer one precise table to three
paragraphs. A chapter runs to 4,000 to 10,000 words; above 12,000, propose
a split to the lead instead of cutting detail.

### C5. Method

1. Start from the design documents your brief names, to learn the layout.
2. Read the headers and the data readers of your scope, then the update
   and message-handling functions, then the rest. Read the files that
   carry rules in full; do not infer a rule from a function's name.
3. Read the data your brief names: every key your code reads, the range of
   its values across all files, and the keys present in data that your
   code never reads.
4. Write the chapter and the evidence file together, statement by
   statement.
5. Before you finish, answer every "must answer" item of your brief, or
   list it as an open question.

Your brief's file lists are starting points. Follow the code where it
leads; when it leads into another chapter's scope, refer to that chapter
and tell the lead in your report.

### C6. Evidence file

```markdown
# NN — <Title>: evidence

## Statements
| ID | Basis | Sources | Note |
| --- | --- | --- | --- |
| FLT-012 | both | CppSource/Common/<file>.cpp:120-188; outdir/data/constants.ini [Section] key | |

## Coverage
Read in full: … / Read in part: … / In scope but not read, with reason: …

## Disagreements with design/
…
```

### C7. Report to the lead

At most 300 words: files written and word count; coverage in one line;
glossary terms you propose (term and one-line definition); what you need
from other chapters; what you found that belongs to another chapter; your
five most important open questions.

---

## Part D — Chapter briefs

Paths are relative to `CppSource/` for code and `outdir/data/` for data
unless they start with `design/` or `outdir/`. A stem after `Common/`
stands for the files `Common/common_<stem>*`; other stems are file name
prefixes as written. "Client" is `OutpostFrontier/Client/`. Most headers
are not beside their sources but in `include/` (flat, same stem) and its
subdirectories `engine/`, `render/` and `audio/`; they belong to the
chapter that owns the source.

### D01 — World and universe (WLD)

Code: `Common/universe*`, `zone`, `exclusionzone`, `casteroid*`, `spatial`,
`basedata`, `baseproperties`, `pathfind`; `Content/SystemInfo`,
`SystemConnections`, `Zones`, `SpacePaths`; Client `star_sys`,
`space_solar_world`, `cli_asteroid*`, `cli_nebula`, `nebula_volume`,
`mission_created_solars`; `x86math/` for the handedness and the vector,
matrix and rotation conventions only.
Data: `UNIVERSE/`, `SOLAR/`. Design: assessment 02 section 1.

Must answer: the hierarchy from universe to system to zone and object to
base; the coordinate system, units and typical scales; what a system
definition declares; zone shapes, properties and the queries made against
them; how asteroid fields and nebulae are generated from a field
definition (tiling, density, exclusion zones, dynamic and lootable
asteroids, hazards); how systems connect and how routes between them are
computed, including the legal and illegal variants; what is discovered or
hidden from the player's map and how discovery is recorded.

### D02 — Objects, archetypes and equipment (OBJ)

Code: `Common/archetype*`, `carchgroup`, `loadout`, `equipdesc`,
`hardpointsummary`, `hierarchy`, `cobject`, `engineobject`, `cobjdb`,
`csimple`, `cship`, `csolar`, `cloot`, `cattachment`, `cattachedequip`,
`cequip`, `ceqobj`, `cesmallequip`, `cescanner`, `cecargo`,
`iobjinspectimpl`, `id`, `subobjectid`; `Server/server_gameobject`,
`server_createinfo`, `server_classes`; Client `client_gameobj`,
`client_cliobj`, `client_objdb`.
Data: `SHIPS/` and `SOLAR/` archetype files, `EQUIPMENT/*_equip.ini`, the
loadout files. Design: `design/adr/0001-object-model.md`, assessment 02.

Must answer: the kinds of world object and what each can do; archetype
against instance; identifiers and how names map to them; hardpoints,
their types and the mounting rules; every equipment category with its
state and its per-tick behaviour in outline (chapter 04 details the combat
ones); internal against external equipment; the cargo hold; parts, groups
and the hierarchy of destructible sub-objects; the life cycle of an object
from creation to removal; what an observer can inspect about an object.

### D03 — Flight and physics (FLT)

Code: `Common/physys*`, `physicalsystem`, `phyarch`, `phycollisionstate`,
`phycontroller`, `phantom`, `controllergolem`, `remotephysics`,
`deadreckonedvector`, `sphereintruder`, `csteering`, `ceengine`,
`cethruster`, `thrustmanager`, `constants`; `IVP/` only as far as needed to
state how bodies are integrated and how collisions are resolved; Client
`player_behavior`; the free-flight and collision scenarios in
`Oracle/scenarios/`.
Data: `constants.ini`, `EQUIPMENT/engine_equip.ini`, the physical fields of
ship archetypes. Design: `design/sim-records.md`,
`design/adr/0003-simulation-oracle.md`, assessment 02 sections 4 and 6.

Must answer: the equations of motion, linear and angular, with every
force, torque and damping term; how top speed and turn rate emerge; the
integration step and its order; throttle, reverse, strafe, thruster,
engine kill and cruise, each with its charge, cost, limits and what
interrupts it; how steering input becomes torque; collision shapes,
detection, response and collision damage; how remote objects are
corrected and extrapolated; what differs for very large objects.

### D04 — Combat and damage (CMB)

Code: `Common/cegun`, `celauncher`, `cprojectile`, `cmine`, `cedroppers`,
`ceshield`, `cepower`, `cecloak`, `cetractor`, `tractorarm`, `fuses`,
`damagelist`, `gunconvergence`, `repairmanager`, `sysmgr_gungroups`,
`archetype_weapons`; `Server/server_guided`, `server_mine`,
`server_countermeasure`, `server_fuse`, `server_loot`, and the damage path
in `server_gameobject`; `Content/LootProps`; Client `client_fuse`,
`client_explosion`, `client_debris`, `cli_projectile_fx`, `beam_effect`.
Data: `EQUIPMENT/weapon_equip.ini`, `weaponmoddb.ini`, `st_equip.ini`,
`misc_equip.ini`, `FX/` explosion and fuse definitions,
`MISSIONS/lootprops.ini`.

Must answer: the firing cycle of each weapon class (refire, energy,
muzzle velocity, lifetime, dispersion, convergence); guided munitions,
their seeking and what defeats them; mines; the damage pipeline from hit
to shield to hull to component, with every modifier between weapon type
and shield type; shield capacity, regeneration, collapse and rebuild;
power capacity and regeneration; area damage; fuses as timed sequences of
effects and destruction; death, wreckage and dropped loot; the tractor
beam; repair consumables and what they restore; weapon groups.

### D05 — Travel, docking and navigation (NAV)

Code: `Common/dockbehavior`, `launchbehavior`, `launchaction`,
`laneaction`, `cruisebehavior`, `gotobehavior`, `pathfind`;
`Content/DockQueues`, `TradelaneRequests`, `SystemConnections`,
`SpacePaths`, `GotoGuides`, `Guides`; the base-enter, base-exit, launch
and system-switch handlers in `Server/IServerImpl`; Client `best_path`,
`gate_tunnel`, `jump_effect`, `tradelane_fx`, `client_land_action`,
`player_spawn`, `hud_maneuvers`, `hud_waypoint`; the docking, lane and
launch scenarios in `Oracle/scenarios/`.
Data: docking fields of the solar archetypes, the shortest-path files in
`UNIVERSE/`. Design: `design/adr/0007-server-world.md`.

Must answer: the manoeuvres a player can order and what each does; the
docking sequence as a state machine (request, permission, queue, approach
points, each kind of dock); the launch sequence; travel along a chain of
lane rings (entry, speed, interruption, exit); transfer between systems
and what happens on each side during it; route planning within and across
systems and how waypoints are followed; what refuses a dock.

### D06 — AI: manoeuvres and steering (AIM)

Code: `Common/behavior`, `behaviormanager`, `behaviortypes`, every
`Common/*behavior` and `*idea` file, `aisteer`, `aiintercept`,
`airaycast`, `obstacleavoidance`; the follow, formation, fly-to-point,
guide, large-ship and dogfight scenarios in `Oracle/scenarios/`.
Data: `MISSIONS/formations.ini`. Design: assessment 02 section 1 (AI row).

Must answer: the architecture (who owns a behaviour, how behaviours are
requested, stacked, interrupted and finished); for every behaviour its
purpose, inputs, the steering and throttle it produces as an algorithm,
its parameters and its exit conditions; interception and lead pursuit
mathematics; obstacle avoidance; formation slots and station keeping; how
large ships manoeuvre differently; update rates and level of detail for
distant ships.

### D07 — AI: combat decisions and pilots (AIC)

Code: `Common/aifirecontrol`, `aitarget`, `targetmanager`,
`systemmanager`, `sysmgr_*`, `iustategraph`, `pubstategraph`, `pub`;
`Content/PilotPersonalities`, `BehaviorCmds`, and `NpcBase`, `NpcShip` as
far as they drive the AI.
Data: `AI/state_graph.db`, `MISSIONS/pilots_population.ini`,
`pilots_story.ini`.

Must answer: the state graph, its format and how the next combat
manoeuvre is chosen; every block of a pilot's personality and what each
parameter modulates; target selection and threat assessment; fire control
(lead, burst, accuracy, weapon choice by range and target); decisions
about missiles, mines, countermeasures, repair consumables, thruster and
cruise; reactions to damage; fleeing and cargo dropping; the jobs a pilot
can hold; how difficulty scales pilots.

### D08 — NPC population and encounters (POP)

Code: `Content/Encounters`, `EncounterParams`, `Populators`,
`RandomEncounters`, `SpawnPlacement`, `PatrolPaths`, `PatrolGuides`,
`NpcCreate`, `NpcShips`, `NpcShip`, `NpcBase`, `NpcNames`, `Voices`,
`Zones`, `GenericScripts`, `Support`, `SpaceSupport`, `GameSupport`.
Data: `MISSIONS/ENCOUNTERS/`, `npcships.ini`, `shipclasses.ini`,
`faction_prop.ini`, `rankdiff.ini`, `voice_properties.ini`, and the
population keys of zones in `UNIVERSE/SYSTEMS/`. Design: assessment 04
sections 3 and 6.

Must answer: how a zone declares who lives in it; the population loop
(density, caps, repopulation timers, distance to players, spawn and
despawn rules); what an encounter definition contains and how its
permutations are chosen; how a spawned NPC's ship, loadout, pilot and
level are selected; how difficulty varies by place; what ambient NPCs do
(patrol, trade runs, docking, lane travel, scanning for contraband);
how NPC names and radio chatter are assembled, as a mechanism.

### D09 — Factions and reputation (REP)

Code: `Common/reputation`, `vibe`; `Content/CombatReputation`,
`FactionProps`; reputation handling in `Server/`; Client `nn_player_info`.
Data: `initialworld.ini`, `MISSIONS/empathy.ini`, `faction_prop.ini`,
`factionsets.ini`.

Must answer: the range of a reputation value and its thresholds; the
faction-to-faction matrix and the player's standing; per-object attitude
overrides; every event that changes reputation, with the propagation to
allied and opposed factions as a formula; every effect of reputation
(hostility, docking, offers, visibility of information); how the player's
apparent affiliation is derived.

### D10 — Economy, trade and progression (ECO)

Code: `Common/goodinfo`, `tradeoffer`, `repairmanager`, `cecargo`;
`Content/BaseDB`, `BaseSupport`, `Cargo`; `Server/server_basedb` and the
purchase, sale and equip handlers in `Server/IServerImpl`; Client
`client_base_market`, `commodity_by_base`, `nn_trade`, `nn_dealer`,
`nn_ship_trader`, `nn_inventory`, `nn_repair`, `trade_equip_list`,
`dealer_equip_stats`.
Data: `EQUIPMENT/goods.ini`, the `*_good.ini` and `market_*.ini` files,
`commodities_per_faction.ini`, `MISSIONS/ptough.ini`,
`RANDOMMISSIONS/diff2money.ini`.

Must answer: the kinds of good; the price model per base, with buying,
selling and resale; the conditions on a purchase (stock, level,
reputation); cargo volume and limits; buying a ship, the trade-in and
what transfers; mounting, unmounting and selling equipment; repair cost;
every source and sink of money; how the player's level or rank is
computed and what it unlocks; contraband; the validation the authority
performs on each transaction. As shape, not as tables: the number of
commodities, the spread of prices, the profit of typical routes.

### D11 — Mission scripting and campaign structure (MIS)

Code: `Content/MissionScript`, `MissionCnd`, `MissionAct`,
`MissionObject`, `MissionObjList`, `ObjListDirective.h`, `MissionDialogs`,
`MissionSave`, `Story`, `StoryLauncher`, `StoryMissions`, `RtcScripts`,
`Gcs`, `ContentMessageRouter`, `PlayerState`; Client `mission_briefing`,
`nn_mission_message`, `nn_story_star`, `hud_comm`.
Data: `MISSIONS/M*/` for structure only, the two `mission1 readme.doc`
files as format documentation, `MISSIONS/mshipprops.ini`,
`MISSIONS/specific_npc.ini`. Design: assessment 04 section 3.

Must answer: the anatomy of a mission definition (declared ships, solars,
characters, formations, object lists, objectives, dialogue hooks,
triggers); the trigger model; the complete catalogue of conditions and of
actions, each with parameters and semantics (count the ones the code
registers and make the catalogue match that count); the directive
language that drives scripted ships; objectives and how they reach the
player; how missions chain and what gates the next one; cutscene and
base-scene integration; checkpoints, saving, failure and retry; the
campaign as structure, without its story (B3).

### D12 — Generated missions and base services (RND)

Code: `Content/RandomMissionDB`, `RandomMissionGen`,
`RandomMissionMachine`, `RandomMissionRuntime`, `RandomMissionTriggers`,
`RandomMissionVignette`, `RandomMission.h`, `DestroyMission`,
`BaseNpcRecords`; Client `nn_mission_vendor`, `nn_news`,
`nn_offer_dialog`.
Data: `RANDOMMISSIONS/`, `MISSIONS/mbases.ini`, `news.ini`, and the
vignette zones in `UNIVERSE/SYSTEMS/`.

Must answer: how job offers are generated (who offers, against whom,
where, at what difficulty, for what reward, as formulas); the mission
types and the tree that composes a mission from parts; how a location is
chosen; the run-time state machine (arrival, waves, objectives, timers,
success, failure, payout, reputation); the characters met on a base and
what each can offer (jobs, information, bribes), with pricing and effect;
when a news item is shown.

### D13 — Bases, rooms and cinematics (BAS)

Code: Client `gf_*`, `gameflow`, `room_control*`, `room_camera`,
`base_walk`, `engine_character`, `anim_script_set`, `fade_to_black`;
`SceneScript/` without `lua32/`; `SceneRuntime/` at requirement level;
`Deformable2/` at requirement level; `Common/roomdata`, `roominfo`,
`costume`, `animdb`, `actiondb`, `newchar`.
Data: `BASES/`, `SCRIPTS/`, `CHARACTERS/`, `petaldb.ini`. Design:
assessment 06 section 1 (row 11), assessment 07 section 1.

Must answer: a base as a set of rooms and what each room type offers;
room definitions, hotspots and movement between rooms; how characters
are placed and composed from parts; the scene-script model as a
capability catalogue (entity types, event types, timeline, what a script
can control) without reproducing any script; ambient against one-shot
scenes; landing and launch sequences; cutscenes in space; the state
machine of a base visit from touchdown to launch.

### D14 — HUD, controls and cameras (HUD)

Code: Client `hud_*`, `wire_controls`, `chat_*`, `input_manager`,
`menu_keymap`, `main_command_handler`, `space_camera*`,
`ship_camera_params`, `space_state`, `nav_bar`, `button_mover`,
`cargo_bar_control`, `space_letterbox`.
Data: `INTERFACE/hud.ini`, `INTERFACE/HUD/`, `keymap.ini`, `keylist.ini`,
`mouse.ini`, `cameras.ini`. Design: assessment 06 sections 1 and 2,
assessment 07 section 7.

Must answer: the complete catalogue of player commands with their default
binding and the mode they apply in; mouse flight against free cursor and
how each maps to steering (with chapter 03); every HUD element, the data
it shows, its update rule and its interactions; targeting commands and
selection rules; the contact list and its filters; lead indicators and
brackets; every camera mode with its parameters and transitions; the
chat interface in multiplayer.

### D15 — Menus, UI toolkit and text (UIX)

Code: Client `page_*`, `menu_*`, the `nn_*` files not claimed by chapters
10, 11, 12 and 14 (navigation map, inventory frame, load and save,
preferences, modals and shared controls), `ui_*`, `win3d*`, `win_*`,
`wincontrol_*`, `window`, `list_box`, `scroll_control`, `text_win`,
`rich_text_win`, `font_*`, `ids_*`, `infocard_map`, `knowledge_map`,
`loading_screen`, `intro_*`, `game_options`, `perf_options`, `login_mgr`,
`server_*`, `connect_*`; `Common/fmtstr`, `rdl*`, `renderdisplaylist`,
`textrender`; `imeui/`; the structure of `Resources/` (which kinds of
text exist and how they are keyed, never the text).
Data: the rest of `INTERFACE/`, `FONTS/`, `SCREENS/`, the layout of
`text/`. Design: assessment 06 section 5.

Must answer: the screen flow from start to play, as a graph, and what
each screen does; the navigation map (layers, filters, what is revealed
when, information panels, route plotting); the load and save interface
and the options catalogue; the multiplayer front end (server list,
account, character creation and selection); what the UI toolkit must
provide (control types, layout, model-viewing widgets, rich text); the
localisation model (text identifiers, formatted strings with
substitutions, marked-up information cards).

### D16 — Presentation: visuals and audio (PRS)

Code: Client `cli_*`, `space_scene*`, `star_sphere`, `space_dust`,
`asteroid_billboard`, `engine_light`, `lights_camera`, `effect_*`,
`particle_effect`, `vis_effect_arch`, `client_equip_fx`, `flash_light`,
`texture_*`, `cockpit_load`, `menu_sound_arch`, `sound_config`,
`voice_profiles`; at requirement level only: `particlefx/`, `RendComp/`,
`Shading/`, `surfacematerials/`, `ximage/`, `DX8RENDERER/`,
`DX9RENDERER/`, `SoundManager/`, `SoundStreamer/`, `movie/`.
Data: `FX/`, the INI files in `AUDIO/`, the visual definitions in
`SOLAR/`, `COCKPITS/`, `MOVIES/` (as a count), `igraph.ini`. Design:
assessments 05 and 07.

Must answer, as requirements on any engine: what a frame in space
contains and in what order it is composed; visibility ranges and levels
of detail; the capabilities the effect system needs (emitters, fields,
appearances, attachment to hardpoints); what lighting, fog, nebula
interiors and asteroid fields must convey; the sound categories and
volume groups, distance attenuation, the rules that select and change
music, how voice lines are assembled from segments and queued; video
playback. Most statements here are [C] or [I]; say which few are [E].

### D17 — Architecture, authority, networking and persistence (ARC)

Code: `Server/`, `rpclocal/`, `RemoteCommon/`, `RemoteClient/`,
`RemoteServer/`, `OutpostServer/`, `NeuronRuntime/`, `neuroncomponents/`,
`system/`, `ReadFile/`; `Common/marshalbuffer`, `playeraccount`,
`dataconfig`, and the server headers in `include/`; Client
`client_iclient*`, `client_frame_tick`, `client_state`, `main`,
`engine_init`; `Content/Content`, `ContentMessageRouter`, `PlayerState`,
`MissionSave`.
Data: the files at the top of `outdir/` (the INI files and the two new
player files). Design: `design/adr/0002-single-player-world.md`,
`0006-controller-bus.md`, `0007-server-world.md`, assessments 01, 03, 04
and 06 section 2.

Must answer: the logical components and which is authoritative for what;
the order of work within one tick on each side; the message catalogue in
both directions, grouped by purpose, each with its trigger and the
meaning of its payload (not its byte layout); how object state is
replicated, at what rates, and what the receiving side predicts; single
player as the same authority in process, and every difference between
single player and multiplayer; sessions, accounts and characters;
everything a saved game contains, by field group, and when saving is
allowed; the multiplayer rules (death, respawn, groups, chat, player
trade, the checks against cheating); the administration functions of a
dedicated host; start-up, data loading and shutdown order.

### D18 — Data schema and content census (DAT)

Code: `Common/ini`, `dataconfig`, `archetypeloader`, and every data reader
in `Common/`, `Content/`, `Server/` and the client, found by searching for
section and key literals. Data: every text file under `outdir/`; binary
assets by kind only. Design: `design/facts.md`, assessment 08 section 8.

Census step (wave 1). Deliver: the manifest that says which configuration
file names which data files; for every kind of data file its sections and
keys with value types, units, multiplicity, defaults and the chapter that
consumes them; for every kind of asset what the game needs from it (for a
model: parts, hardpoints, levels of detail; likewise collision hulls,
animations, effect libraries, materials, scene scripts, audio, fonts),
without its binary layout; the content census as numbers and
distributions, never names (systems, bases per system, connections per
system, zones per system, ships by class, weapons by class, commodities,
factions, encounter definitions, missions, scene scripts, voice lines);
the validation rules the loaders apply. Close with a proposal for
expressing the same schema in a neutral, modern form.

Reconcile step (phase 3). Compare your catalogue with every chapter's
data model. Report to the lead: keys the data holds that no chapter
explains, fields chapters describe that no data holds, and chapters that
contradict each other about the same key.

### DA — Appendix A: original technology (APX)

Sources: `design/assessment/01-foundation.md`, `05-rendering.md`,
`08-cross-cutting.md`, `design/decisions.md` for the numeric model only,
and the code they point to, `x86math/` among it. The section order of C3 does not apply; write prose and tables.

Must answer: the platform, libraries and middleware the original stood
on; which behaviours of the game follow from them (numeric precision,
fixed rates, limits on counts and sizes, 32-bit assumptions); what the
reconstruction added that the original did not have; a list of traits a
modern builder should not reproduce, each with what to do instead.

---

## Part E — Reviewer brief

You review one chapter. You did not write it and you do not trust it.
Read A1, Part B and Part C, then the chapter, its evidence file and its
brief in Part D. Do not edit the chapter. Write
`blueprint/evidence/NN-<slug>.review.md`.

1. **Correctness.** Re-derive from the code, without looking at the
   chapter's wording first: every formula, every state machine, every
   global tuning value, and a sample of at least a third of the remaining
   statements (at least fifteen). Prefer the statements tagged [E] and the
   ones with basis `inferred`.
2. **Completeness.** Go through the brief's "must answer" list. For each
   source file in scope that the evidence never cites, skim it and name
   the behaviour the chapter lacks. Search the data named in the brief for
   keys the chapter does not explain.
3. **Buildability.** Read the chapter as the builder of A1. List every
   point where you would have to guess.
4. **Rules.** Violations of B3 (names, narrative, tables of items, code),
   of B4 (implementation concepts in the text) and of B5 (guesses stated
   as facts).

Record each finding as: statement ID or section, severity (*wrong*,
*missing*, *unclear*, *rule*), what the source says with path and line,
and the correction you propose. End with a verdict line. Report to the
lead in at most 150 words.

---

## Part F — Cold-reader brief

You are the builder of A1. You may read `Blueprint.md` and
`blueprint/*.md` and nothing else: not `blueprint/evidence/`, not this
prompt beyond A1 and this part, no other file in the repository. Do not
use what you know about any published game.

The lead gives you one of these tasks:

- **Slice plan.** Write an implementation plan for the first two slices of
  the build order: modules, data structures, the order of work, and the
  tests you would write.
- **Subsystem build.** For three chapters named by the lead, write the
  data structures and the update loop in pseudocode.

While you work, record every decision you could not take from the
blueprint. Return, in at most 600 words: the blocking questions (you had
to guess), the contradictions between chapters, terms used without a
definition, references that do not resolve, and anything that reads as
belonging to a particular setting or technology. Do not return the plan
itself.
