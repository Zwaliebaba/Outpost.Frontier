# 0008 — Content in game-logic

Status: accepted by the owner on 2026-11-22 (P3.4 to P3.7); decision O47.
Questions 1 to 10 answered as recommended; question 4 (the NPC loot
preference) on 2026-11-24. Amended by the owner on 2026-11-25 with
P3.7a (Context's random missions, points 1, 5, 8 and 10). Builds on records
0006 (controller bus, decision O40: its "Content" section and Appendix A)
and 0007 (server world, decision O43), and on P3.3's port of `pub::` in
`game-logic::api`.

## Context

Paths are relative to `CppSource/`.

### What Content.dll is

- **Size.** 57 `.cpp` and 53 `.h` files, 78,522 lines (`Content/`).
- **The factory.** `CreateInstance` (`Content/Content.cpp:94-135`) matches
  23 names with `_stricmp`, first match wins. The objects are small
  (`Content.cpp:88-93`): GameSupport 8 bytes, SpaceSupport 4, BaseSupport 8,
  SpacePop 8, BasePop 0x1c, StoryLauncher 8. Story is 0x3c, DestroyMission
  0x29c, Mission_01a 0x440, Mission_01b 0x330, and Mission_02 to 13 and
  GenericMission 0x32c. Only the missions and the story carry real
  instance state. The support controllers are facades over process
  globals (record 0006, "One shared state").
- **The static tables.** GameSupport's constructor (`GameSupport.cpp:450-458`)
  sets its heartbeat, creates the router and calls `LoadContentGameTables`
  (`Encounters.cpp:497-591`). That function reads, in this order, through
  one `INI_Reader` whose open result is never tested (`:495-496`):
  - the three voice files and their voice properties;
  - `missions\empathy.ini` and `missions\voice_properties.ini`;
  - `formations.ini`, `pilots_population.ini`, `pilots_story.ini`,
    `npcships.ini` and `npcships_test.ini`;
  - `faction_prop.ini` and `equipment\commodities_per_faction.ini`;
  - `shipclasses.ini` and the mission properties of `ships\shiparch.ini`;
  - `lootprops.ini` (the loot properties, then the phantom loot);
  - the GCS exclusions, `PTough.ini`, `RankDiff.ini` and `news.ini`.

  The destructor unloads them (`:594-606`). Other loaders run elsewhere:
  - `mBases.ini` (`BaseDB.cpp:269`, `:427`);
  - the three shortest-path files and `universe.ini`
    (`SystemConnections.cpp:350-368`, `:463`), rebuilt on every `LoadGame`
    (`GameSupport.cpp:657-668`);
  - `specific_npc.ini` (`StoryLauncher.cpp:43`);
  - each system's `[EncounterParameters]` files, read when the system
    record loads (`SystemInfo.cpp:84-114`);
  - the GCS scripts (`GenericScripts.cpp:87`, `:736`) and the RTC scripts
    (`RtcScripts.cpp:48`);
  - the seven `RandomMissions\*.ini` files (`RandomMissionGen.cpp:319-370`).
- **The live state.** Each of these is a namespace-scope global:
  - the player states (`PlayerState.cpp:37`);
  - the system records (`SystemInfo.cpp:32`), built only by
    `LoadDynSysInfo` on `LoadGame` (`SystemInfo.cpp:2076-2079`);
  - the dock queues and terminals (`DockQueues.cpp:35-39`);
  - the conversation manager and the GCS tables (`Gcs.cpp:49-79`);
  - the NPC ship events (`NpcShip.cpp:1000`) and SpaceSupport's guided
    ships (`SpaceSupport.cpp:50`);
  - the gate locks (`SpaceSupport.cpp:70`, `:1184`);
  - the objective zones of the live random missions (`DestroyMission.cpp:43`);
  - the bar scenes (`BaseDB.cpp:491-498`), news and base visits
    (`BaseSupport.cpp:36`, `:487`);
  - SpacePop's switches and exclusion spheres (`Populators.cpp:36-56`);
  - the controller ids kept as globals (`GSpaceSupportControllerId`,
    `GSpacePopControllerId`, `GBasePopInstance`, `GStoryMissionController`).

  A grep for namespace-scope containers finds about 70 of these, as the
  "about 60 STL tables" of record 0006 estimated.
- **The id counter.** `GContentNextUniqueId` (`Content.cpp:14`, starts at 1)
  hands out timer parameters.
  - Dynamic initialisers take five ids at DLL load:
    - the music timer (`SpaceSupport.cpp:61`);
    - three faction-name timers (`FactionProps.cpp:44-46`);
    - the dock-hail timer (`Gcs.cpp:64`).

    The order follows the linker's object order, which the project lists
    as SpaceSupport, FactionProps, Gcs (`Content.vcxproj`). That order is
    to be confirmed by the oracle.
  - At run time the counter serves:
    - the router (`ContentMessageRouter.cpp:27`);
    - the NPC ship timers (`NpcShip.cpp:1125`, `:1161`, `:1198`);
    - the mission nags (`MissionAct.cpp:1559`);
    - the NN path timers (`MissionObject.cpp:4296`).

  Each value comes back as a `TimerExpired`'s first word, so it can be
  observed in the dispatch trace.
- **Heartbeats.** Each `SetHeartbeatInterval` draws one `rand()` (record
  0006):
  - GameSupport: 1 s (`GameSupport.cpp:453`);
  - SpaceSupport: 1 s (`SpaceSupport.cpp:55`, `:306`);
  - SpacePop: 3 s (`Populators.cpp:240`);
  - BasePop and StoryLauncher: none (`Populators.cpp:973`, `StoryLauncher.cpp:41`);
  - Story: 5 s (`Story.cpp:115`);
  - a mission object: 1/30 s (`MissionObject.cpp:101`), set a second time
    by Mission_01a only (`StoryMissions.cpp:100`; corrected with P3.5a1);
  - the random mission object: 1 s (`RandomMissionDB.cpp:1041`).
- **Game time.** `GetContentGameTime` is a `double` that GameSupport's
  heartbeat adds to (`GameSupport.cpp:35`, `:594-599`, `:782`). The
  router's delivery times and the GCS intervals read it.

### How the server reaches it

- **Order.** Record 0006 fixed the broadcast order and listed where Content
  depends on it and where it re-enters itself. The shipped rows are
  `outdir/OutpostFrontier.ini:36-61`.
- **SpaceSupport's handler** (`SpaceSupport.cpp:560-984`) runs in four
  steps, each over every message:
  1. the GCS handler set for 15 message ids (`:566-588`);
  2. the NPC ship events for `CommComplete`, `ObjectDestroyed` and
     `TimerExpired` (`:590-592`);
  3. the main switch. The trade-lane cases are at `:669-676`. On a
     `TimerExpired`, the music timer is tested first, then the
     faction-name timers, then the router (`:923-937`);
  4. the tail: the GCS set again on load and unload (`:965-966`), the dock
     terminal states except on `JumpgateAct` (`:967-968`), and each guided
     ship forwarding the message to its guide, with the null entries
     erased afterwards (`:970-983`).
- **GameSupport's handler** (`GameSupport.cpp:544-765`):
  - keeps the player states' ship, system, base and location records;
  - writes `[mPlayer]` on `PlayerSave` and reads it on `PlayerLoad`;
  - reads the base, gate-lock and system sections on `LoadGame` and writes
    the gate locks on `SaveGame` (`:657-678`). `SaveContentBases` writes
    nothing (`BaseDB.cpp:479-485`).
- **The multiplayer start-up.** `Server_LoadWorldState` broadcasts
  `LoadGame` over `InitialWorld.ini` after the initial controllers exist
  (`Server/IServerImpl.cpp:318-323`, `:2950-2979`). So the system records
  exist before any system loads, in both modes.

### The router

- **Its parts** (`Content/ContentMessageRouter.h:37-63`):
  - a list of pending messages sorted by delivery time;
  - a timer id from the counter (`ContentMessageRouter.cpp:27`);
  - a handler-id counter;
  - a list of handlers.
- **Routing** (`RouteMessage`, `:49-89`):
  - a delay of 0 or less delivers at once, as a nested call to the first
    handler whose id matches (`:52-64`);
  - any other delay clones the message and stamps it with
    `float(game time + delay)`. The clone goes in before the first queued
    message with a later time, and a server timer is armed on
    SpaceSupport's id with `{TimerId, 0}` (`:66-89`).
- **Firing** (`OnTimerExpired`, `:131-147`): it delivers the *head* of the
  queue, with no emptiness test.
- **Users.** Only the random missions and DestroyMission use the router:
  - `RandomMissionRuntime.cpp:377`;
  - `RandomMissionMachine.cpp:629`, `:1021`;
  - `RandomMissionDB.cpp:1036`;
  - `DestroyMission.cpp:140`, `:394-513`.
- **The Common classes it serves** (`StateMachineMessage`, `MessageHandler`,
  `StateMachineMessageHandler`, `StateMachineGroup`, `include/common_net.h:381-540`)
  are already ported in `neuron-core::util::state_machine`:
  - its `MessageRouter` trait routes without a context;
  - its `StateMachineGroup` defers removals to the C++'s removal points
    (D16, D18).

  The second point settles one of record 0006's open items: the groups do
  defer the deletion of machines.

### What Content sends back

- **Call sites.** Content calls about 240 distinct `pub::` names and sends
  through `_SendMessage` from 32 sites. P3.3 ported every family into
  `game-logic::api`, in parts 14 and 16 to 18 (README P3.3 row):
  - `space_obj`, `system`, `player`, `gf`, `reputation`, `zone`, `ai`,
    `phantom`, `audio` and `controller`;
  - the getters and `pub::Save` (`misc`);
  - `pub::Market` (`bases`).
- **Signatures.**
  - A getter takes `&Cx` (for example `api::space_obj::location`, `:1500`).
  - A function that can change state or dispatch takes `&mut Cx`.
  - A C++ result code is a `PubError` (`api/mod.rs:25-42`).
  - The enumerators return their lines and do not call back (D696, D701).
  - A reader takes the `DataSource` its host gives it (D690).

### Mission scripts

- **The trigger** (`MissionScript.h:48-57`) is:
  - a state (ACTIVE 0, INACTIVE 1, COMPLETE 2);
  - one condition;
  - a list of actions;
  - the message ids the condition listens to;
  - a repeatable flag;
  - its id.

  The script mission (`FMissionScript`, `MissionScript.h:110-248`) adds
  the trigger map, the active list, the pending activations, the delayed
  actions, the nags, the NN path and the story state.
- **The factories.**
  - 44 `Cnd_*` entries, each with its listen ids (`MissionScript.cpp:88-133`).
    `Cnd_NPCSystemEnter` listens to three ids (`:113-114`); every other
    condition listens to one.
  - 85 `Act_*` entries (`:146-230`).
  - `LoadTrigger` (`:240-297`) tests the conditions first, then the
    actions, then `InitState`, `repeatable` and `system`.
  - A second `Cnd_*` line replaces the first and leaks it. The data does
    this once: `extra_static_cam_time` in `MISSIONS/M04/m04.ini`.
- **The shipped scripts.** The 14 story missions (`StoryMissions.cpp:102-437`)
  hold 2,984 triggers and 14,203 action lines. They use 118 of the 129
  names. Never used: `Cnd_CmpToPlane`, `Cnd_JumpgateAct`,
  `Cnd_NPCSystemExit`, `Cnd_RumorHeard`, `Act_PilotParams`,
  `Act_PlayerForm`, `Act_PlayNN`, `Act_RelocateForm`,
  `Act_RepChangeRequest`, `Act_SetFlee` and `Act_SpawnShipRel`.
- **Parsing is not pure.** The parsers call `CreateID` (30 sites) and these
  getters:
  - `pub::GetSystemID`, `GetBaseID`, `GF::FindLocation`, `GF::GetBodyPart`,
    `GF::GetAccessory` and `Reputation::GetReputationGroup`;
  - `pub::System::EnumerateObjects`, in `Act_NNPath`
    (`MissionAct.cpp:3742-3774`).

  `Cnd_Timer` with a range draws `rand()` while it parses
  (`MissionCnd.cpp:2635-2656`), and so do the `[MsnRandEnc]` populators
  (their `time` activations and `num_forms`), which parse before the
  triggers (found with P3.5a1). The constructors before the parse draw
  the heartbeat phase (`MissionObject.cpp:101`), and Mission_01a a second
  one (`StoryMissions.cpp:100`).
  The parse path does not test `pub::SinglePlayer()`.
- **Running a trigger.** `DispatchMissionTriggerEvent` (`MissionScript.cpp:433-487`)
  runs the active pass of record 0006. `ActivateMissionTrigger` (`:358-402`)
  probes the condition with `Matches(-1, null)` when the trigger is
  activated.
- **What conditions and actions call.** No condition sends, creates or
  submits anything: `MissionCnd.cpp` calls only `pub::` getters and the
  mission's own lookups. Actions dispatch freely.

  The survey found no action that writes its own fields in `Execute`; the
  assignments there are to locals. Conditions do keep run-time state: a
  timer's elapsed time, a circle's last position, `Cnd_NPCSystemEnter`'s
  pending list. That state is saved through `Cnd::Save` and `Load`
  (`MissionCnd.h:51-53`).
- **Saves.** Each section is written only in single player.
  - The script mission writes `[MissionState]`, `[TriggerSave]` and
    `[BStoryMissionDone]` on `PlayerSave` (`MissionScript.cpp:1053-1065`,
    `:1698-1942`).
  - The mission object writes a snapshot of its live ships, wings,
    solars, marks, invulnerables, cloaks and vibes
    (`MissionObject.cpp:1510-1529`; `MissionSave.cpp:223-1199`). It does
    so on `PlayerSave` and on `SpaceExit`.
  - Story writes `[StoryInfo]` (`Story.cpp:335-343`).
  - The player state writes `[mPlayer]` (`PlayerState.cpp:260-298`).
  - A random mission writes `[RandomMission]` (`DestroyMission.cpp:409`,
    `:575`).
  - The gate locks are written as `[locked_gates]` (`SpaceSupport.cpp:1342`),
    and the system paths as `[SystemConnections]` (`SystemConnections.cpp:161`).

  26 of the `fprintf` formats use `%f`, beside `%g`, `%d`, `%u` and
  `%s`. P3.3's writer lends the open text to the `PlayerSave` and
  `SaveGame` broadcasts (`game-logic/src/save_writer.rs:481-485`, `:523`),
  and P3.2's selection lends the reader to `PlayerLoad` and `LoadGame`
  (`selection.rs:177`, `:204`). `neuron-core::msvcrt` has the UCRT `%g`
  (`format_g`) but no `%f`.

### Story

- **Creation.** StoryLauncher creates Story at NORMAL on `PlayerLogin`
  (`StoryLauncher.cpp:64-77`).
- **Running the missions.** Story creates the chapter's mission at
  ABOVE_NORMAL and sends it `MissionStart` (`Story.cpp:162-169`). It
  destroys missions (`:176`, `:272`, `:291`), and restores one from
  `[StoryInfo]` at NORMAL (`:360-380`).
- **The missions call back synchronously.** A mission calls Story directly
  through the `IStoryMissionEvents` facet it got in its create
  parameters (`Story.cpp:255-330`, record 0006 Appendix A).
- **The create parameters on load** (`FStoryOnLoadCreateParms`, `Story.cpp:37-42`)
  carry the `GenericMission` path in a buffer that the C++ fills past its
  bound.
- **Ranks.** A 43-entry threshold table is filled by the constructor
  (`Story.cpp:25`, `:91-103`).

### Population and NPCs

- **The parts.**
  - SpacePop and BasePop (`Populators.cpp`);
  - zone encounters and encounter parameters (`Encounters.cpp`,
    `EncounterParams.cpp`), with the zones of the system record
    (`Zones.cpp`, `SystemInfo.cpp`);
  - spawn placement (`SpawnPlacement.cpp`);
  - the NPC ship and its archetypes (`NpcShip.cpp`, `NpcShips.cpp`);
  - the guides (`Guides.cpp`, `GotoGuides.cpp`, `PatrolGuides.cpp`);
  - the conversation system, GCS (`Gcs.cpp`). With 76 `rand()` calls it
    is the module's largest user of the generator;
  - the dock queues (`DockQueues.cpp`) and the combat reputation
    (`CombatReputation.cpp`).
- **Solar NPCs.** When a system record fully loads on `SystemLoaded`
  (`SpaceSupport.cpp:914-917`, `SystemInfo.cpp:367-380`), it reads the
  system file's `[Object]` sections again (`SystemInfo.cpp:313-366`). Every
  named object with a behaviour, a base, a docking target, a `reputation`
  or a `msg_id_prefix` becomes a solar NPC. One with a `reputation` gets
  the vibe of its nickname and that faction as its affiliation
  (`NpcCreate.cpp:70-104`). This is where the stations get their factions,
  the question fact F17 left open.
- **Callbacks.**
  - `FContentShipBase` is a `pub::AI::DirectiveCallback`
    (`BehaviorCmds.h:66`, `:84`).
  - `FContentNpcShip` and `FSolarNpc` are `pub::AI::ContentCallback`s
    (`NpcShip.h:99`, `NpcCreate.h:49`).
  - Both are handed over in `SetPersonalityParams`
    (`MissionScript.cpp:1999-2000`, `NpcShip.cpp:360-361`, `NpcCreate.cpp:147-148`).
  - The content callback has ten slots (`include/common_pub.h:1254-1330`).
    The NPC's implementations send controller messages (`NpcShip.cpp:41-90`).
  - `request_flee_destination` returns an answer that the behaviour uses
    in the same tick. It may replace the NPC's directive, and it always
    sends `FleeRequest` (`NpcShip.cpp:642-729`). The guides' answers only
    read positions (`Guides.cpp:346`, `:740`; `GotoGuides.cpp:245`;
    `PatrolGuides.cpp:188`, `:1211`).
- **Loot preference** (the owner's question of 2026-11-18). The parser
  tests `LT_ALL`, `LT_NONE`, `LT_COMMODITIES`, `LT_EQUIPMENT` and
  `LT_POTIONS` with the case-sensitive `strstr` (`PilotPersonalities.cpp:783-799`).
  - `pilots_population.ini` writes all 37 values in lower case: 11
    `lt_all`, 9 `lt_commodities`, 7 `lt_none` and 10 `lt_potions`.
  - `pilots_story.ini` writes 13 `LT_NONE` and one `lt_all`.

  So every pilot gets 0, and no NPC loots.
- **The oracle already runs part of Content.** `Oracle.exe sim` runs the
  real GameSupport, SpaceSupport and BaseSupport (`Oracle/oracle_sim.cpp:2364-2405`).
  Since P3.6a the Rust scenario run starts the same three controllers and
  runs `LoadGame`, so SpaceSupport runs on both sides
  (`crates/game-logic/tests/scenario_hooks.rs`).

### Random missions

- **The mission data** (`FRandomMissionData`, `RandomMission.h:21-90`) holds
  the offer query's values, a seed, the tree path and the offers. It is
  the reference-counted create parameter of the `DestroyMission`
  controller.
- **Creating a mission.** `CreateMission` (`DestroyMission.cpp:1041-1120`)
  draws the seed with `rand()` and seeds the generator with it
  (`:1051-1052`).
  `FinalizeMission` seeds it again from the saved seed and builds the
  offer (`:620-650`). A save rebuilds the mission the same way from
  `[RandomMission]` (`:661-689`; `Support.cpp:360-380`).
- **There is one generator.** The CRT's `rand` belongs to `ucrtbase.dll`,
  which every game module binds to ([sim-records.md](../sim-records.md),
  `load_rand.jsonl`). Content's `srand` therefore reseeds the generator
  that the server and Common draw from too. The port's bus already draws
  from the world's generator (`game-logic/src/server.rs:376-378`).
- **Building an offer.** `BuildOffer` (`RandomMissionGen.cpp:3048-3093`)
  validates the query, then picks an offer node, chooses the tree path and
  builds from the path. `RemapRmOfferDifficulty` takes `std::log`
  (FYL2X) and rounds the result to an integer rank (`:1876-1898`).
- **State outside the seed.** An offer's build reads and changes state
  that is not part of its seed (amended by the owner on 2026-11-25, with
  P3.7a):
  - three counters: the vignette solar and ship names
    (`RandomMissionGen.cpp:1857`, `:1998`) and the ship groups
    (`RandomMissionRuntime.cpp:199`);
  - the faction name reservations an assassination target takes
    (`FFactionNames::PickPilotName` and `PickFormationDesig`,
    `FactionProps.cpp:457-616`), which `~FRmVignetteOffer` releases
    (`RandomMissionRuntime.cpp:458-461`, through `ReleaseTargetName`,
    `:463-521`);
  - `GRmDestSystems`, the systems loaded for random missions
    (`RandomMissionDB.cpp:395-410`);
  - `GRmObjectiveZones`, the zones other missions hold as objectives
    (`DestroyMission.cpp:43`), which `PickObjectiveZone` reads (`:952`,
    the test at `:970`) and `CreateMission` adds to (`:1087`).

  The vignette cookies (`RandomMissionVignette.cpp:43`) are not among
  them: they are state of the runtime machines.
- **Where offers come from.** The bar offers (`CreateBarMissionOffer`,
  `LootProps.cpp:195`) and the vendor offers (`GenerateVendorMissions` and
  `BuildMissionVendorOffers`, `:278-520`) are built in TU35, which the
  reconstruction put in `LootProps.cpp`. They are accepted in
  `BaseSupport.cpp:420-435` (vendor) and `BaseDB.cpp:900-921` (bar,
  `FBarOffer::Accept`). (References corrected by the owner on 2026-11-25,
  with P3.7a.)
- **The run time.** `FRandomMissionObject` is the mission object plus a
  state-machine group (`RandomMissionObject.h:39`). Its machines are at
  `RandomMissionMachine.h:205` and `RandomMissionRuntime.h:102`, `:185`.
  Its ten message classes are at `RandomMissionMachine.h:35-98` and
  `RandomMissionRuntime.h:82`, `:222`, `:233`.

### The Rust side today

- **game-logic** (`crates/game-logic/src/server.rs`):
  - `ControllerKind` has 12 variants over the 23 names (`:25-98`);
  - `CreateParams` is `None` or `Player` (`:103-109`);
  - `Logic` has no Content state;
  - `deliver` runs `tradelane::space_support` for SpaceSupport, then the
    test hook (`:336-350`);
  - `instantiate` only names the kind (`:352-360`), and
    `destroy_instance` is empty (`:362`).
- **The messages.** `events.rs` has the 81 messages, with `NpcShipKey` as
  Content's key for an NPC ship (`:32`).
- **The callbacks in game-sim.**
  - `CallbackArg::Set(u32)` carries a content callback's creation type or
    a directive callback's handle (`api/ai.rs:85-94`).
  - The manager keeps a content callback as a flag plus a creation type
    (`game-sim/src/ai/manager.rs:602-609`).
  - It queues directive results in `ManagerState::completed` (D194) and
    the content-callback reports as `CombatEvent` (`ai/target_manager.rs:327-345`).
  - The launch action holds the reserved handle `u32::MAX`
    (`object/world/launch_action.rs:60`, D722).
- **Personalities.** The pilot loader is Content's
  (`game-sim/src/ai/personality.rs:5-6`). The scenario driver takes the
  pilots' personalities from the oracle's `ai.jsonl` (`scenario/driver.rs:376-381`).
- **Dispatch trace.** The `dispatch.jsonl` trace of record 0006 point 10
  is not written yet: no file in `Oracle/` or `Server/` names it.

## Decision

1. **Content's state is one value in `Logic`, in three parts: immutable
   tables, live state and the controllers' own state.** Content lives in `Logic` as record 0006
   point 3 says, so `Cx { world, logic }` is unchanged:

   ```rust
   pub struct Content {
       pub tables: Arc<ContentTables>,
       pub state: ContentState,
       pub instances: BTreeMap<ControllerId, Instance>,
   }
   ```

   - `ContentTables` holds what the C++ never changes after loading. It is
     built by GameSupport's constructor and dropped by its destructor. A
     handler that needs a table across a call clones the `Arc`. Nothing in
     it is mutable, so this is shared immutable data, not the shared
     mutable state T10 rules out.
   - `ContentState` holds the live globals listed in Context: player states,
     system records, dock queues, conversations, NPC ships, gate locks, the
     router, the counter, and the controller ids the C++ keeps as globals.
     Tables that the C++ rebuilds on `LoadGame`, such as the system paths,
     and the faction-name reservations live here too. So do the tables
     whose lifetime the C++ ties to another controller or to `LoadGame`:
     the bases of `mBases.ini` (built on `LoadGame` and BaseSupport's first
     visit) and the specific NPCs of `specific_npc.ini` (built by
     StoryLauncher's constructor, cleared by its destructor), each behind
     its own `Arc` (owner, 2026-11-23, with P3.4c).
   - `instances` holds each controller's own state. A handler is
     `fn(cx: &mut Cx, me: ControllerId, msg: &mut ControllerMessage)`.
   - The random-mission tables are a fourth field, `rm`, beside `tables`
     (point 5; amended by the owner on 2026-11-25, with P3.7a).

   The borrow rules are record 0006's, applied to Content:
   - A borrow of `cx.logic.content` ends before any call that takes
     `&mut Cx`. Whatever the C++ holds by pointer is held by key and looked
     up again.
   - A call that cannot dispatch may take an item out of its slot and put
     it back. A condition's `matches`, a guide's flee answer and the
     label lookups are such calls. This is not the rejected option C of
     record 0006: that option was rejected because a nested message could
     find the slot empty, and no message can arrive during a call that
     cannot dispatch. The callee takes `&Cx`, where a getter's port allows
     it. The bus gains a dispatch counter in debug builds, and an
     assertion checks that it is unchanged across every take.
   - No Content item is moved out of its slot across a call that
     dispatches.

2. **Each controller kind maps onto a game-logic module and one instance
   variant.**

   | Kind | Module | Instance |
   | --- | --- | --- |
   | `GameSupport` | `content::game_support` | unit |
   | `SpaceSupport` | `content::space_support` | unit |
   | `BaseSupport` | `content::base_support` | unit |
   | `SpacePop`, `BasePop` | `content::population` | unit, `BasePop` state |
   | `StoryLauncher`, `Story` | `content::story` | unit, `Story` |
   | `Mission01a`, `Mission01b`, `Mission(n)`, `GenericMission` | `content::mission` | `MissionScript` with a `StoryMission` variant for the overrides of `StoryMissions.cpp` |
   | `DestroyMission` | `content::random_mission` | `RandomMission` |

   - **Delivery.** `Cx::deliver` calls `content::deliver`, which matches the
     kind. Each handler runs its parts in the C++ order; SpaceSupport, for
     example, runs the four steps of Context. `tradelane::space_support`
     (part 20) moves to its case in SpaceSupport's switch, after the GCS
     and NPC-event steps. `deliver_hook` stays as a test seam.
   - **Constructors and destructors.** `instantiate` runs the C++
     constructor (heartbeats, router, table loading, parse).
     `destroy_instance` runs the destructor. P3.5 and P3.7 check each one
     they port against D594 and D595.
   - **Create parameters.** `CreateParams` gains these variants:
     - `Story { player }`;
     - `StoryMission { player, story }`, where `story` is the Story
       controller's id that replaces the `IStoryMissionEvents*`. A mission's
       call into its story becomes a direct call,
       `story::on_mission_event(cx, story, ...)`;
     - `GenericMission { player, story, path }`, with the path copied
       within its bound. A path longer than the buffer gets a divergence row.
     - `RandomMission(RmDataKey)`, a key into Content's table of
       reference-counted mission data, which counts references as
       `FContentRefObject` does.

3. **The router is ported as it is (record 0006 point 8).**
   `content::router::Router` lives in `ContentState`:
   - the pending messages in delivery order;
   - the timer id;
   - the handler counter;
   - the handlers in registration order.

   GameSupport's constructor creates it, so the timer id is drawn from the
   counter at the same point as in the C++.
   - **Now.** For a delay of 0 or less, `router::route(cx, msg, delay,
     target, sender, scope)` sets the delivery options and finds the
     first handler with the receiver's id. It then calls that handler's
     owner synchronously, as a nested call with the same `&mut Cx`. An
     unknown receiver drops the message.
   - **Later.** The port computes the delivery time as
     `(game_time + f64::from(delay)) as f32`. The message goes in before
     the first later one, and `api::controller::set_timer` is called on
     SpaceSupport's id with `(timer_id, 0, delay)`.
   - **Firing.** SpaceSupport's `TimerExpired` case tests the music timer,
     then the faction-name timers, then the router. The router delivers
     and drops the head of its queue, whichever message armed the timer.
     An empty queue does nothing. The C++ reads an empty list's sentinel
     there, so this gets a divergence row if any path can reach it.
   - **Messages and handlers.** A routed message is `RmMessage`:
     neuron-core's `StateMachineMessage` header plus one payload variant
     for each of the ten classes. A handler is a `HandlerRef` that names
     its owner by keys (the mission's controller id, then a machine,
     trigger or offer-component key), so delivery looks the owner up again.
   - **State machines.** The machines keep neuron-core's `StateMachine`
     and `StateMachineGroup` bookkeeping. Their process loops need the
     context, because the random-mission handlers dispatch, so neuron-core
     gains a host-generic form. It follows `BusHost`: a `MachineHost`
     trait addresses a machine by key and handles an event, and
     `process_in` and `group_process_in` look the machine up again after
     every handler call. neuron-core keeps no game rules.

4. **Content's callbacks are handles (D710; `u32::MAX` reserved, D722).**
   - **The handles.** Content's callback objects get a `CallbackHandle(u32)`:
     the ship bases (NPC and player ships) as directive callbacks, and the
     NPC ships and solar NPCs as content callbacks. One allocator hands
     them out from 1, increasing, never reused. It never hands out 0,
     which means null, or `u32::MAX`, which is `LAUNCH_ACTION_CALLBACK`.
     It reaches the reserved value only after 2³² − 2 allocations, so a
     debug assertion guards it. It is separate from `GContentNextUniqueId`,
     whose values are observable.
   - **Submitting them.** `SetPersonalityParams` carries
     `CallbackArg::Set(handle)` for both kinds. game-sim's
     `set_content_callback` takes `Option<(handle, creation_type)>`.
     game-logic asks Content's object for its creation type when it builds
     the op, as the C++'s `set_content_callback` asks `get_creation_type`.
   - **Directive results.** After a ship's behaviour tick, at the point
     where D722 runs the launch action, game-logic reads
     `ManagerState::completed`. It calls each handle in the manager's
     slots in slot order (through `ServerHooks::directive_complete` into
     `content::callbacks`; amended with P3.6c). Content's handles and the
     reserved one share the slots under the one slot rule.
   - **Content callbacks inside the tick.** The content callback has ten
     slots. `get_creation_type` is asked when the callback is submitted
     (above). game-sim queues the seven callbacks that only report (follow,
     tether, lead, in sights, scan state, combat state, drift) as
     `ServerRequest::Content` entries. They
     go in the same ordered list as the dock, formation and lane requests,
     so their order relative to those calls is the C++'s; they reach
     Content after the tick (D622). The two callbacks that answer,
     `request_flee_destination` and `information_request`, are answered
     read-only. As built with P3.6c (amended by the owner on 2026-11-25),
     the world asks game-sim's hook `ServerHooks::content_answer` just
     before the behaviour tick of a ship whose manager has a content
     callback, and the flee takes that answer when it starts; nothing the
     answer reads changes within the tick. The hook takes `&mut World` but
     only reads it, because the hooks cannot be threaded into the manager.
     An answer is not purely a read in the C++: `FTargetGuide`'s
     `RequestFleeDestination` swaps the guide's targets (`SwapTargets`,
     `Guides.cpp:346-416`). That swap, the NPC ship's new active goto
     directive and `FleeRequest` are queued behind the answer and made
     after the tick. This removes D168 and D184, rewrites D194 and adds
     D852 for the moves out of the tick. Open question 3 offers the
     synchronous alternative.
   - **Shared directives.** The C++'s directive objects are reference
     counted and shared, and running one writes into it (`FollowDirectivesOf`
     writes the dock port into the leader's, `Execute` the default
     priority). The owner chose on 2026-11-25 to keep them shared: Content
     holds them in a counted arena (`content::directives`) and hands out an
     opaque `DirectiveRef`, so the writes reach every holder as in the C++
     and need no row. P3.6c uses it for the directives its guides, launches
     and flee answers create; P3.5c uses it for the missions' `[ObjList]`
     map.

5. **The static tables are typed data from `outdir/`, compared with the
   oracle (P3.4, Tier A).**
   - **Loading.** `content::tables` holds one typed structure for each
     table, loaded by a port of its C++ loader. The loaders read through
     neuron-core's `IniReader` over the `DataSource` the host gives
     (D690), in `LoadContentGameTables`' order. A file that does not open
     reads as empty, as the untested open does in the C++.
   - **Order and keys.** `std::map` becomes `BTreeMap` and `std::list` a
     `Vec`, in insertion order. `weighted_vector` keeps its order. Ids are
     `CreateID` values. The fields the reconstruction cannot name are
     named after their offset (verification.md, definition of done 4).
   - **Server state.** Loaders that read server state
     (`GetReputationGroup`, `GetBaseID`) take `&Cx`.
   - **System records.** `FSystemInfo`'s load (`LoadFromReader`, `LoadFull`,
     the trade lanes, the solar NPCs with their reputations) is a P3.4
     part, because the stations' factions come from it. The solar NPCs'
     `SubmitPersonalityState` waits for the NPCs of P3.6.
   - **Pilots.** The pilot personalities replace the oracle's personalities
     in the scenario driver. A data test compares each `[Pilot]` with
     `OracleSimPilotPersonality`, an export that already exists
     (`PilotPersonalities.cpp:1364-1371`).
   - **The random-mission tables.** Most random-mission tables are not
     loaded by `LoadContentGameTables`. A static object, the generator
     (`GRandomMissionGeneratorInstance`, `RandomMissionGen.cpp:1090`), loads
     them when Content.dll loads: its constructor (`:313-374`) reads
     `NpcRankToDiff.ini`, `Diff2Money.ini`, `RMLootInfo.ini`,
     `SolarFormations.ini`, `KillableSolars.ini`,
     `VignetteCriticalLoot.ini` and the vignette tree of
     `VignetteParams.ini`. Their lifetime is the DLL's, not GameSupport's,
     so the port keeps them in `Content::rm` (`RmData` behind an `Arc`),
     loaded once per load of the DLL before its first controller, and not
     in `ContentTables` (amended by the owner on 2026-11-25, with P3.7a).

6. **Mission scripts are enums (P3.5: Tier A for parsing and saves, Tier
   B for the run time).**
   - **The types.** `Condition` is an enum of 44 variants and `Action` an
     enum of 85. Each variant is a struct of its C++ class's fields. The
     two factories are tables in the C++ order with their listen ids, read
     with `is_value` (case-insensitive, first match). A second `Cnd_*`
     line replaces the first. A trigger is
     `Trigger { state, condition, actions, events, repeatable, id }` in a
     `BTreeMap`. The active list is record 0006 point 7's cursor walk over
     trigger ids, and the pending activations and the initially active
     triggers are `BTreeMap`s.
   - **Parsing.** It takes `&mut Cx`, because `Cnd_Timer` and the
     `[MsnRandEnc]` populators draw from the world's generator, and it
     calls the getters of Context. It runs in the mission's constructor
     after its heartbeat draws, so the draws come in the C++ order.
   - **The trigger pass.**
     - The condition is taken out for `matches` (point 1).
     - Each action is cloned from `(trigger id, index)` before it runs, and
       the next one is looked up again afterwards.
     - If the mission ended or its triggers were torn down during the
       action, the pass stops. The C++ goes on through freed actions; this
       is a divergence row under record 0006 point 7's held-pointer rule.
     - `Act_ChangeState` takes effect at the end of `handle_mission_message`,
       as in the C++.
     - A mission that destroyed itself ignores every message until its
       teardown (`MissionObject.cpp:129-142`).
   - **The mission object.** TU38 is the base of both script and random
     missions. It becomes a `MissionObject` struct embedded in
     `MissionScript` and `RandomMission`, and its virtual slots become a
     match on the mission's variant.
   - **Saves.** Content writes its sections into the `&mut dyn Write` that
     the P3.3 writer lends, under the same single-player tests as the C++,
     with `\n` that the save path translates as a text-mode `fprintf`
     would. `%f` becomes a new `neuron_core::msvcrt::format_f` beside
     `format_g`, on the same exact decimal expansion, with vectors in
     neuron-core's number suite. Loads read the lent `IniReader` and save
     and restore its state where the C++ does.
   - **The story shell.** StoryLauncher, Story and the per-mission
     overrides; the rank thresholds are a table.

7. **Population and NPCs are Tier B (P3.6).**
   - **What is ported.**
     - SpacePop and BasePop, the encounters, spawn placement and the
       random encounters;
     - the NPC ships, which live in a Content arena keyed by `NpcShipKey`,
       with their callbacks of point 4;
     - the guides, GCS, the dock queues, the formations, the combat
       reputation, and SpaceSupport's loot preparation with the
       `EquipDropLoot` and `ObjectDestroyed` out-parameters;
     - BaseSupport.
   - **How it is verified.**
     - Within the scenario envelopes of decision O20.
     - Against the dispatch trace.
     - Against `load_rand.jsonl`'s population draws, which the README P2.5
       row defers to P3.6 for Gate G3.
     - Gate G3's `dock_at_station` through the server needs SpaceSupport's
       dock queues, which come with this package.
   - **Loot preference.** The port compares as the reconstruction does,
     case-sensitively, so no NPC loots on the shipped data (owner,
     2026-11-24, question 4). If a check of the original binary shows
     otherwise, the reference gets an approved fix and the port follows.

8. **Random missions are state enums; an offer is rebuilt from its seed
   (P3.7: Tier A for offers, Tier B for the run time).**
   - **One generator.** Content's `srand` reseeds the world's CRT generator,
     the one the bus and the AI draw from, because the C++ reseeds the
     shared generator (T11: draw order kept where a seed rebuilds state).
   - **The tables and the generator.** The random-mission tables, the
     vignette tree and the generator are ported; `BuildOffer` is a
     function of the query, the tree path, the tables and the generator.
     The tables and the tree are `Content::rm` (point 5; amended by the
     owner on 2026-11-25, with P3.7a).
   - **Machines.** The runtime machines are state enums driven through
     point 3's `MachineHost`.
   - **State outside the seed.** The state outside the seed of Context
     (the three counters, the faction name reservations, `GRmDestSystems`
     and `GRmObjectiveZones`) is part of `ContentState`: the counters and
     the destination systems in `ContentState::rm`, the reservations in
     `ContentState::faction_name_uses`, the objective zones with
     `CreateMission` (P3.7c). The vignette cookies come with the runtime
     machines (P3.7b). The dump does not record the counters as input: it
     compares them as outputs, from a fresh load that runs the oracle's
     cases in the same order (the ship-group serial and the enemy group's
     reservations on each mission line, the counters in the ships' and
     solars' nicknames). Amended by the owner on 2026-11-25, with P3.7a.
   - **The rank.** It goes through `f64::ln`. It can differ from x87
     `FYL2X` only where `x + 0.5` lies within an ulp of an integer. The
     dump shows the rank, so a mismatch shows, and one found gets a T11
     row.

9. **Temporary rows and open facts, by package.**

   | Package | Row or fact | What happens |
   | --- | --- | --- |
   | P3.4 | F17 | Rechecked: the system records' solar NPCs give named solars with a `reputation` their faction on `SystemLoaded` (`NpcCreate.cpp:70-104`), before any player's creates. The dump of point 5 confirms it. F17 is then rewritten as known, with no change to the reference. The replication comparison then expects the affiliations |
   | P3.4 | D734 (route check) | `CheckTradelaneRoute` (`PlayerState.cpp:541-575`) comes with the player state. Rings answer 1 for closed routes, and the row narrows to the missing conversation |
   | P3.5 | D710, D722 | The allocator of point 4 meets the owner's condition. The rows stay as design rows, with their observable effect rewritten |
   | P3.5 | — | The mission sections behind `PlayerSave` and `SaveGame`, and `Act_PlayerCanTradelane` and the other lane actions, which set the route check's input |
   | P3.6 | D734 | Removed: the GCS "granted" conversation (`TradelaneRequests.cpp:51-60`) |
   | P3.6 | D168, D184 | Removed: the follow and guide events and the flee destination reach Content |
   | P3.6 | D194 | Rewritten: the directive results reach Content's handles after the tick |
   | P3.6 | — | Items left to Content by the P3.3 row: `LaunchQueuedShips`, the descriptions' `init` and `EnumerateCharacterPlacementIni`; and the population draws and the flee, loot and other behaviour scenarios that the P2.5 row defers to P3.6 |
   | — | D750 | Not Content's: P3.9 |
   | P3.6c | D760 | Removed: the AI's target, cruise, thruster and equipment changes reach the server as requests after the behaviour tick, and the replication comparison judges the four packets in its exact window (owner, 2026-11-25) |

10. **Increments.** Each increment below is one reviewable pull request,
    merged on green under O41 unless it carries new divergence rows or a
    change to a record.

    | PR | Content | Verifies |
    | --- | --- | --- |
    | P3.4a | `content` skeleton: `Content`, `Instance`, `CreateParams`, `instantiate` and `destroy_instance`, the counter with its five load-time ids, GameSupport's constructor and heartbeat, the game time | Controller order and ids, heartbeat phases against `load_rand.jsonl`'s `controller` phase, the counter's values against the timers in the dispatch trace |
    | P3.4b | `LoadContentGameTables`: voices, empathy, formations, pilots, NPC ship archetypes, factions and their goods, ship classes, mission ship properties, loot and phantom loot, GCS exclusions, toughness, rank difficulty, news | Tier A `content_*` dumps; the pilots against `OracleSimPilotPersonality` |
    | P3.4c | `mBases.ini`, the system paths and their `[SystemConnections]` writer, `specific_npc.ini`, encounter parameters, zones and patrol paths (as building blocks of the system record), the cargo helpers but `GenerateCargo` | Tier A dumps |
    | P3.4d | GameSupport's run time: player states with `[mPlayer]`, system records with `LoadFull` (assembling P3.4c's encounter parameters, zones and patrol paths) and the solar NPCs' reputations, base records, gate locks, `LoadGame` and `SaveGame` (calling P3.4c's base and system-path loaders); the space paths (`SpacePaths.cpp`) where the system records' trade lanes suffice, the rest with P3.6; SpaceSupport's `SystemLoaded`, `SystemUnloaded` and the system-record half of `SystemEnter` and `SystemExit`; SpacePop's solar-detection flag, `CollectSpacePopZones` and `IsSegmentClear`; `format_f` with unit tests (moved from P3.5b) | Tier A: system records after every system's load (F17); `[mPlayer]` and gate-lock round trips; D734 narrowed |
    | P3.5a1 | The parser: `Condition`, `Action`, triggers, the mission object's sections (`[MsnShip]`, `[MsnSolar]`, `[MsnFormation]`, `[MsnLoot]`, `[MsnRandEnc]`, `[Dialog]`, `[NPC]`, `[NNObjective]`, `[Mission]`), mission dialogs | Tier A mission script dumps of the 14 story missions (Gate G3) |
    | P3.5a2 | `[ObjList]` (23 directives and their AI ops) and `[MetaBehavior]` | The same dumps extended to both sections |
    | P3.5b | The mission, trigger, snapshot and `[StoryInfo]` sections (`format_f` itself came with P3.4d) | `format_f` vectors; save round trips; Rust saves loaded by the C++ build (Gate G3) |
    | P3.5c | The run time: the trigger engine, conditions, the 85 actions in groups (one pull request per group if large), object lists, nags, the NN path | Tier B mission trace (Verification) |
    | P3.5d | The story shell and `StoryMissions.cpp`'s overrides | Tier B story trace |
    | P3.6a | SpaceSupport's core: handler order, loot preparation, the players' formations, dock queues (with the dock-list half of `SystemEnter` and `SystemExit`); the trade lanes moved in | `dock_at_station` through the server; scenarios with SpaceSupport on both sides |
    | P3.6b | GCS and voices | D734 removed; a scenario without `comms off` |
    | P3.6c | NPC ships, callback handles, guides, the NPC name builder (moved from P3.4c), `GenerateCargo` (moved from P3.4c), the NPC formations (`PromoteFormationLeader`, `PickFormationLeader`, `BreakFormationAtDock`'s NPC part; moved from P3.6a), the combat reputation of TU56 and the players' hostile contacts (D845), the AI's target, cruise, thruster and equipment requests to the server (D760) | D168, D184, D760 removed; D194 rewritten; flee, follow and guide scenarios driven by Content |
    | P3.6d | SpacePop, BasePop, encounters, spawn placement, random encounters | Population draws in `load_rand.jsonl`; a populated scenario |
    | P3.6e | BaseSupport | Tier B base session trace |
    | P3.7a | Random-mission tables, vignette tree, generator | Tier A offers from a seed (Gate G3) |
    | P3.7b | Router, `MachineHost`, the DestroyMission controller, the runtime machines | Router and machine traces (Tier B) |
    | P3.7c | Vendor and bar offers, `[RandomMission]` saves and the restore from save; `CreateMission` (`DestroyMission.cpp:1041-1120`) with its zone search (`CollectZonesInRange`, `CollectZonesAlongTradeLanes`, `CollectZonesThroughJumps`, `PickObjectiveZone`), `PickEnemyGroup`, `IsRouteBlocked` (`:719-752`) and the route check of the picked zone (`:991`; the search, the check and `PickEnemyGroup` at `:776-1027`), `ReleaseRmDestSystems` (`RandomMissionDB.cpp:414-422`) and `PickRmMissionDifficulty` (`:216-292`) (added by the owner on 2026-11-25, with P3.7a) | Tier A rebuild from a saved seed (Gate G3) |

    The owner moved the NPC name builder, the space paths and
    `GenerateCargo` out of P3.4c on 2026-11-23: none of them is a loader,
    and each needs state a later package brings (the faction-name pool and
    reputation allocation, the system records' trade lanes and SpacePop's
    flags, the guide description and ship parameters).

    The owner split P3.5a on 2026-11-24: `[ObjList]` and `[MetaBehavior]`
    draw no `rand()` and no other section reads them, so P3.5a2 adds them
    without changing P3.5a1's output.

    The owner gave the combat reputation (TU56), which point 7 listed
    without an increment, and the NPC-only formation functions to P3.6c on
    2026-11-25 (with P3.6a).

    The owner gave D760 to P3.6c on 2026-11-25: the AI's target, cruise,
    thruster and equipment changes had no increment, and the replication
    comparison cannot judge the combat scenario's NPCs without them.

    The owner moved `format_f` from P3.5b, SpaceSupport's system-load cases
    and the system-record half of `SystemEnter` and `SystemExit` from P3.6a,
    and SpacePop's solar-detection flag, `CollectSpacePopZones` and
    `IsSegmentClear` from P3.6d into P3.4d on 2026-11-23, because the
    `[mPlayer]` files and the system records need them; P3.5b keeps the
    `format_f` oracle vectors.

    P3.4 comes first. P3.5a and P3.5b, then P3.6a to P3.6c, can run in
    parallel with each other, because they share only P3.4. P3.5c needs
    P3.6c for the ships its actions spawn. P3.7 comes last, because it
    builds on P3.5's mission object.

    The owner re-sequenced Phase 3 on 2026-11-25 (decision O48): P3.7a and
    P3.7c, which Gate G3 needs, come before P3.5c, P3.5d, P3.6b, P3.6d,
    P3.6e, P3.7b and the rest of P3.6c (guide types 2 to 6, the name
    builder, `GenerateCargo`). Where P3.7a or P3.7c needs P3.5c's run time,
    it takes the part it needs and the record says so.

    P3.7a took the part of P3.6c's name builder that an assassination
    target needs: the faction name reservations, `PickPilotName`,
    `PickFormationDesig` and the default `BuildNpcName`
    (`game-logic::content::npc_names`). P3.6c's follow-up keeps the rest
    of the name builder (amended by the owner on 2026-11-25, with P3.7a).

## Alternatives considered

- **A separate `game-content` crate.** AGENTS.md puts missions and
  encounters in GameLogic, and `pub::` and `Cx` live there. A separate
  crate would need either a generic context or a dependency cycle.
- **All state per controller, with no shared `ContentState`.** The C++
  state is process-wide and read across controllers: SpaceSupport reads
  the player states that GameSupport keeps, and missions read the system
  records. Splitting it would need borrows across instances.
- **Content as a third field of `Cx`** (`Cx { world, logic, content }`). It
  would allow split borrows. But it changes record 0006 point 2 and every
  signature, and the getters take `&Cx` anyway.
- **Static tables as `static` or `OnceLock` globals.** This goes against
  T10. Tests would share state, and a reload on a second start would be
  wrong.
- **Conditions and actions as trait objects** (`Box<dyn Condition>`). That
  mirrors the vtables. Enums give an exhaustive dump, save code and
  `Clone`, and the ledger already says enums.
- **Mission scripts kept as text and parsed on use.** This moves
  `Cnd_Timer`'s draws and the getters' calls away from the constructor.
- **Delayed router messages driven by Content's heartbeat instead of bus
  timers.** This changes when they are delivered and their order. Record
  0006 point 8 decided otherwise.
- **A separate generator for random missions.** The C++ reseeds the shared
  one, so every later server draw depends on it.
- **Content callbacks as trait objects inside game-sim.** game-sim would
  depend on Content. D710 and T10 decided on handles.
- **`Rc<RefCell<…>>` for NPC ships and mission data.** This is record
  0006's option D: a panic at run time where the borrow checker would
  prove the code.

## Consequences and open questions

- **game-logic** gains the `content` tree, `Logic::content` and the new
  `CreateParams` variants. Its delivery calls Content, and the trade-lane
  handlers move into SpaceSupport's switch.
- **neuron-core** gains `msvcrt::format_f` and the host-generic
  state-machine loops.
- **game-sim** gains:
  - `set_content_callback` with a handle;
  - `ServerRequest::Content`;
  - `ServerHooks::content_answer` in the behaviour tick;
  - directive results delivered to handles.

  The bare world keeps its behaviour, so the P2.x replays are unchanged.
- **The oracle.** Content.dll gains oracle exports and the harness gains
  session files (Verification). The dispatch trace owed by record 0006
  point 10 is a prerequisite of every Tier B claim here.
- **Divergence rows.** The rows of points 2, 3, 4, 6 and 8 come with the
  packages that port their sites.

Questions for the owner, each with a recommendation. The owner answered 1 to 3
and 5 to 10 as recommended on 2026-11-22; 4 stays open until P3.6:

1. **Module layout.** Should Content live in `game-logic::content::{tables,
   game_support, space_support, base_support, population, npc, gcs,
   mission, story, random_mission, router}`, or in the plan's top-level
   modules `game-logic::{tables, mission, story, population, npc,
   random_mission}`? *Recommended: under `content`.* `tables` at the top of
   game-logic would be ambiguous beside the server's own tables, and
   GameSupport, SpaceSupport and GCS have no home in the plan's list. The
   plan's Phase 3 table would be updated to match.
2. **Tables behind `Arc`.** Should the tables the C++ never changes after
   loading be an immutable `Arc<ContentTables>` that handlers clone, with
   everything else in `ContentState`? *Recommended: yes.* The other choice
   is to copy each table entry out before every call that dispatches.
3. **Content callbacks inside the behaviour tick.** Should the seven
   reporting callbacks be queued in order with the AI's server requests and
   delivered after the tick, with the two answering callbacks answered
   read-only during the tick (point 4)? Or should game-sim thread the
   server's hooks into the tick, with the ticking ship's manager taken
   out, so that every callback runs synchronously? *Recommended: the
   queue.* It extends D622, D194 and D722 instead of reopening them. If
   the scenario comparisons show `rand()` draws out of order inside a
   tick, the synchronous form is the fallback.
4. **NPC loot preference.** Does the original Content.dll compare
   `loot_preference` case-sensitively? The reconstruction does, so no NPC
   loots on the shipped data. *Recommended:* keep the reconstruction's
   behaviour and have the owner check the original binary from the
   installation's `EXE` tree, at TU45's loader (06f36f10-06f46940). If the
   original compares case-sensitively, nothing changes. If it upper-cases
   the value or compares without case, the reference gets an approved fix
   like O30 and the port follows. The repository holds no original
   binary to settle this. *Answered 2026-11-24: as recommended; the port
   keeps the reconstruction's case-sensitive test.*
5. **F17.** Should F17 be closed as known from the code, "Content gives
   named solars their faction on `SystemLoaded`", once P3.4's
   system-record dump shows it? *Recommended: yes, with no change to the
   reference.* The owner asked on 2026-11-21 to check before any such
   change.
6. **Depth of the mission dump.** Should the mission dump list every
   field of every condition and action (one dumper per class, 129 in
   Content.dll), or only the trigger structure (ids, states, class names,
   listen ids, action order)? *Recommended: every field.* Gate G3 says
   "mission script dumps equal", and a structure-only dump would not
   catch a misparsed value.
7. **The callback-handle allocator.** Should it be monotonic, never
   reused, skip 0 and `u32::MAX`, and be separate from
   `GContentNextUniqueId`? *Recommended: yes.*
8. **The state-machine host.** Should the context-passing machine loops go
   into neuron-core beside the ported Common classes, or stay in
   game-logic? *Recommended: neuron-core*, generic as `BusHost` is.
9. **Tier B evidence for the mission run time.** Should P3.5c and P3.5d be
   accepted on a new `Oracle.exe sim` option that creates a mission for a
   harness player? The option would be `mission <name> <session>`, over
   the `player` directive proposed on the replication branch. It would
   record `dispatch.jsonl` and each tick's trigger states in
   `mission.jsonl`, and be judged by dispatch order to the first difference
   and by position envelopes. *Recommended: yes*, starting with M01a's
   space part. The client-driven `single_player.txt` run needs P5.
10. **Order of the packages.** P3.4, then P3.5 and P3.6 interleaved as in
    point 10, then P3.7, each increment a pull request merged on green
    under O41. *Recommended as written.*

## Verification

| Package | CI or data job compares | New oracle work |
| --- | --- | --- |
| P3.4 | `content_<table>` dumps of every table after start-up, and of the system records after every system's `SystemLoaded`, against `crates/game-logic/tests/data_dump.rs` (data job). The pilots against `OracleSimPilotPersonality`. `[mPlayer]` and gate-lock round trips. Heartbeat phases against `load_rand.jsonl` | Content.dll export `OracleDumpContentTables`; session file `Oracle/content_tables.txt` (`dump content_tables`) on the multiplayer start-up, as `system_loads.txt` uses `OracleDumpSystemLoads` |
| P3.5 | Mission script dumps of the 14 story missions, each created on a fixed seed; the dump includes the CRT's draw count before and after (Gate G3). Save round trips and C++ loads of Rust saves (Gate G3). `format_f` vectors (CI). The mission trace (Tier B) | Export `OracleDumpMissionScripts` (all 129 class dumpers, in a new `Content/oracle_content_dump.cpp`); session file `Oracle/mission_scripts.txt`; the `mission` option of open question 9 and `mission.jsonl`; `format_f` cases in the number suite |
| P3.6 | The scenarios with SpaceSupport, GCS and the populators on both sides, within approved envelopes. The dispatch trace. The population draws of `load_rand.jsonl`. `dock_at_station` through the server (Gate G3) | `dispatch.jsonl` (record 0006 point 10, owed); scenarios for flee, loot, follow and guide under Content; a populated scenario |
| P3.7 | Offers built from fixed seeds over the data's bases and zones, and the rebuild of saved `[RandomMission]` sections from their seeds (Gate G3), field by field: the tree path, rank, texts, objectives and counters. The router and machine traces (Tier B) | Export `OracleDumpRmOffers` (query, seed and counters in; offer out); session file `Oracle/rm_offers.txt` |

Dumps made from `outdir/` are produced when the tests run and never
committed (AGENTS.md). Only `format_f`'s synthetic vectors are committed.
