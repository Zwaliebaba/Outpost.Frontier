//! Content.dll in game-logic (work package P3.4, decision record 0008,
//! decision O47): the 23 controller kinds of `CreateInstance`
//! (`Content/Content.cpp:94-135`), their process-wide state and their
//! handlers.
//!
//! Content's state is one [`Content`] value in [`Logic`](crate::server::Logic)
//! (record 0006 point 3, record 0008 point 1), in three parts:
//!
//! - [`ContentTables`], what the C++ never changes after loading, built by
//!   GameSupport's constructor and dropped by its destructor, behind an
//!   [`Arc`] that a handler clones to keep a table across a call;
//! - [`ContentState`], the live globals: the unique-id counter with its
//!   five load-time ids ([`counter`]), the game time, the message router
//!   ([`router`]) and the controller ids the C++ keeps as globals;
//! - the instances, each controller's own state by controller id
//!   ([`Instance`]).
//!
//! The bus reaches Content through three calls of the server's
//! [`BusHost`](neuron_server::controller::BusHost):
//!
//! - [`instantiate`]: `CreateInstance`'s factory and the kind's
//!   constructor;
//! - [`destroy_instance`]: `DestroyInstance`, the kind's destructor and,
//!   when the last Content controller goes, the DLL's unload;
//! - [`deliver`]: `IDLL::HandleMessage`, the kind's handler.
//!
//! **The borrow rules** (record 0008 point 1). A borrow of
//! `cx.logic.content` ends before any call that takes `&mut Cx`; what the
//! C++ holds by pointer is held by key and looked up again. An item may be
//! taken out of its slot only for a call that cannot dispatch
//! ([`with_instance_taken`], [`NoDispatch`]): in debug builds the bus
//! counts its deliveries, and the take asserts that the count is
//! unchanged when the item goes back.
//!
//! **What P3.4a ports.** The constructors' and destructors' effects on the
//! bus and on the counter (every heartbeat with its `rand()` draw, the
//! controller-id globals, the router's timer id), GameSupport's heartbeat
//! and the game time, and SpaceSupport's trade-lane case. The tables
//! (P3.4b, [`tables`]), the other loaders (P3.4c: [`bases`],
//! [`system_paths`], [`npc_desc`] with StoryLauncher's specific NPCs,
//! [`encounter_params`], [`zones`], [`patrol_paths`] and [`cargo`]),
//! GameSupport's run time (P3.4d: the player states of [`players`], the
//! system records of [`systems`] with their solar NPCs ([`solar_npcs`]),
//! the gate locks of [`gate_locks`], `LoadGame` and `SaveGame`, the space
//! paths of [`space_paths`]), the missions' constructors and parser (P3.5a,
//! [`mission`]), the rest of the missions and the story (P3.5), SpaceSupport's
//! other steps, the populators and BaseSupport (P3.6) and the random
//! missions (P3.7) come with their packages. P3.6a brings SpaceSupport's
//! core ([`space_support`]): its handler's order, the dock queues
//! ([`dock_queues`]), the formations ([`formations`]) over Content's ships
//! ([`ships`]), the loot preparation, and the trade-lane requests
//! ([`tradelane_requests`]) moved in from `crate::tradelane`.

pub mod base_support;
/// `missions\mBases.ini` (TU17, TU22; P3.4c).
pub mod bases;
/// The faction, archetype and cargo helpers (TU44; P3.4c).
pub mod cargo;
pub mod counter;
/// The dock queues (TU53 `DockQueues.cpp`; P3.6a).
pub mod dock_queues;
/// The encounter parameters (TU29; P3.4c).
pub mod encounter_params;
/// The formation helpers (TU53 `DockQueues.cpp`; P3.6a).
pub mod formations;
pub mod game_support;
/// The gate locks (TU53; P3.4d).
pub mod gate_locks;
pub mod mission;
/// The NPC character descriptions and the specific NPCs (TU42; P3.4c).
pub mod npc_desc;
/// The patrol paths (TU45; P3.4c).
pub mod patrol_paths;
/// The player states (TU46; P3.4d).
pub mod players;
pub mod population;
pub mod random_mission;
pub mod router;
/// Content's ships, the ship base of TU50 (P3.6a).
pub mod ships;
/// The solar NPCs (TU24; P3.4d).
pub mod solar_npcs;
/// The space paths (TU53 `SpacePaths.cpp`; P3.4d).
pub mod space_paths;
pub mod space_support;
pub mod story;
/// The system paths and their `[SystemConnections]` writer (TU41; P3.4c).
pub mod system_paths;
/// The system records (TU58; P3.4d).
pub mod systems;
/// The static tables of `LoadContentGameTables` (`Content/Encounters.cpp:497-591`;
/// P3.4b).
pub mod tables;
/// The trade-lane requests (TU55; P3.3, moved into Content with P3.6a).
pub mod tradelane_requests;
/// The zones of a system record (TU64, TU30; P3.4c).
pub mod zones;

use std::collections::BTreeMap;
use std::sync::Arc;

use game_sim::data::DataSource;
use neuron_server::controller::ControllerId;

use crate::events::ControllerMessage;
use crate::server::{ControllerKind, CreateParams, Cx};

pub use tables::ContentTables;

/// Content's live globals (record 0008 point 1): the namespace-scope
/// state of Content.dll that its handlers change.
#[derive(Clone, Debug, PartialEq)]
pub struct ContentState {
    /// `GContentNextUniqueId` (`Content/Content.cpp:14`).
    pub ids: counter::UniqueIds,
    /// The five ids the DLL's dynamic initialisers take when it loads.
    pub load_ids: counter::LoadTimeIds,
    /// `GGameSupportElapsed` (`Content/GameSupport.cpp:35`): the time
    /// GameSupport's heartbeats have added up, in seconds
    /// ([`game_support::game_time`]).
    pub game_time: f64,
    /// `GMessageRouter` (`Content/ContentMessageRouter.cpp:10`): made by
    /// GameSupport's constructor, dropped by its destructor.
    pub router: Option<router::Router>,
    /// `GSpaceSupportControllerId` (`Content/SpaceSupport.cpp:47`), set by
    /// SpaceSupport's constructor; 0 until then. The router and Content's
    /// timers are set on it.
    pub space_support: ControllerId,
    /// `GSpacePopControllerId` (`Content/Populators.cpp:53`), set by
    /// SpacePop's constructor; 0 until then.
    pub space_pop: ControllerId,
    /// `GBasePopInstance` (`Content/Populators.cpp:794`): the BasePop that
    /// was created last; never cleared (D771, D772).
    pub base_pop: Option<ControllerId>,
    /// `GStoryMissionController` (`Content/Story.cpp:31`): the campaign
    /// mission Story created last (P3.5); 0 until then.
    pub story_mission: ControllerId,
    /// `GSpecificNpcs` (`Content/NpcShips.cpp:26`): StoryLauncher's
    /// constructor reads them, its destructor empties them; nothing changes
    /// them in between ([`npc_desc`]).
    pub specific_npcs: Arc<npc_desc::SpecificNpcs>,
    /// `GContentBases` (`Content/BaseDB.cpp:38`): rebuilt on `LoadGame`
    /// (P3.4d), a base's rooms loaded by BaseSupport (P3.6e), emptied by
    /// GameSupport's destructor ([`bases`]).
    pub bases: bases::ContentBases,
    /// `GSystemPathsAll`, `GSystemPathsGate` and `GSystemPathsHole`
    /// (`Content/SystemConnections.cpp:20-22`): rebuilt on `LoadGame`
    /// (P3.4d) ([`system_paths`]).
    pub system_paths: system_paths::SystemPaths,
    /// `GContentPlayerStates` (`Content/PlayerState.cpp:37`): made on
    /// `PlayerLoad`, dropped on `PlayerLogout` ([`players`]).
    pub players: players::PlayerStates,
    /// `GSystemInfos` (`Content/SystemInfo.cpp:32`): rebuilt on `LoadGame`,
    /// loaded on `SystemLoaded`, emptied by GameSupport's destructor
    /// ([`systems`]).
    pub systems: systems::SystemInfos,
    /// `GContentGateLocks` and `GContentNpcGateLocks`
    /// (`Content/SpaceSupport.cpp:70`, `:1184`; [`gate_locks`]).
    pub gate_locks: gate_locks::GateLocks,
    /// `GFp7SystemId` (`Content/PlayerState.cpp:40`): the system
    /// `OnSystemChange` does not count as visited, looked up on every call
    /// (a miss keeps the last value).
    pub relay_seven_system: u32,
    /// `GSpacePopSolarDetection` (`Content/Populators.cpp:47`, true),
    /// stored in single player only ([`population::set_solar_detection`]).
    pub solar_detection: bool,
    /// `GMissionDialogs` (`Content/MissionDialogs.cpp:17`): the `[Dialog]`
    /// sections of the missions loaded, by `CreateID` of their nickname
    /// (first id wins); a script mission's end empties it (P3.5c), its
    /// destructor does not ([`mission::dialog`]).
    pub mission_dialogs: BTreeMap<u32, mission::dialog::MissionDialog>,
    /// The six clamps the missions save and restore (P3.5b;
    /// [`mission::save::Clamps`]): `GSpacePopAttackClamp`,
    /// `GSpacePopEncounterEnabled` (`Populators.cpp:44-56`),
    /// `GMissionScanClamp` (`MissionScript.cpp:43`),
    /// `G_bContentGcsClampDisabled` (`Content.cpp:19`),
    /// `GSpacePopHostileClamp` and `GSpacePopPopulationEnabled`.
    pub clamps: mission::save::Clamps,
    /// `GStoryDebugFlag` (`Story.cpp:28`): `[StoryInfo]`'s `debug`, shared by
    /// every Story (P3.5b).
    pub story_debug: u8,
    /// The dock lists, the terminal states and the launch queues of TU53
    /// (`Content/DockQueues.cpp:35-39`; P3.6a, [`dock_queues`]).
    pub dock_queues: dock_queues::DockQueues,
    /// `GLastFormationId` (`Content/GameSupport.cpp:770`): the last
    /// formation id handed out (P3.6a, [`formations::allocate_id`]).
    pub last_formation_id: u32,
}

impl ContentState {
    /// The state of a freshly loaded Content.dll: the counter at 1, then
    /// the dynamic initialisers that take its five load-time ids
    /// ([`counter::LoadTimeIds::take`]).
    #[must_use]
    pub fn load() -> Self {
        let mut ids = counter::UniqueIds::default();
        let load_ids = counter::LoadTimeIds::take(&mut ids);
        Self {
            ids,
            load_ids,
            game_time: 0.0,
            router: None,
            space_support: ControllerId::BROADCAST,
            space_pop: ControllerId::BROADCAST,
            base_pop: None,
            story_mission: ControllerId::BROADCAST,
            specific_npcs: Arc::default(),
            bases: bases::ContentBases::new(),
            system_paths: system_paths::SystemPaths::default(),
            players: players::PlayerStates::new(),
            systems: systems::SystemInfos::new(),
            gate_locks: gate_locks::GateLocks::default(),
            relay_seven_system: 0,
            solar_detection: true,
            mission_dialogs: BTreeMap::new(),
            clamps: mission::save::Clamps::default(),
            story_debug: 0,
            dock_queues: dock_queues::DockQueues::default(),
            last_formation_id: 0,
        }
    }
}

impl Default for ContentState {
    fn default() -> Self {
        Self::load()
    }
}

/// One controller's own state (record 0008 point 2). The support
/// controllers are facades over the globals and carry nothing.
#[derive(Clone, Debug, PartialEq)]
pub enum Instance {
    /// `FGameSupport` ([`game_support`]).
    GameSupport,
    /// `FSpaceSupport` ([`space_support`]).
    SpaceSupport,
    /// `FBaseSupport` ([`base_support`]).
    BaseSupport,
    /// `FSpacePop` ([`population`]).
    SpacePop,
    /// `FBasePop` ([`population::BasePop`]).
    BasePop(population::BasePop),
    /// `FStoryLauncher` ([`story`]).
    StoryLauncher,
    /// `FStory` ([`story::Story`]).
    Story(story::Story),
    /// The script missions: `Mission_01a` to `Mission_13` and
    /// `GenericMission` ([`mission::MissionScript`]).
    Mission(Box<mission::MissionScript>),
    /// `DestroyMission` ([`random_mission::RandomMission`]).
    RandomMission(random_mission::RandomMission),
}

/// Content's state in the server (record 0008 point 1).
#[derive(Clone, Debug)]
pub struct Content {
    /// The tables the C++ never changes after loading.
    pub tables: Arc<ContentTables>,
    /// The live globals.
    pub state: ContentState,
    /// Each controller's own state, by controller id.
    pub instances: BTreeMap<ControllerId, Instance>,
    /// The bus's deliveries so far (debug builds): a take of an item out
    /// of its slot asserts that this does not change before it is put back.
    #[cfg(debug_assertions)]
    deliveries: u64,
}

impl Default for Content {
    fn default() -> Self {
        Self::new()
    }
}

impl Content {
    /// Content.dll as it loads: no tables, the load-time state, no
    /// controllers.
    #[must_use]
    pub fn new() -> Self {
        Self {
            tables: Arc::new(ContentTables::default()),
            state: ContentState::load(),
            instances: BTreeMap::new(),
            #[cfg(debug_assertions)]
            deliveries: 0,
        }
    }

    /// The instance of controller `id`.
    #[must_use]
    pub fn instance(&self, id: ControllerId) -> Option<&Instance> {
        self.instances.get(&id)
    }

    /// The instance of controller `id`, to change between call-outs.
    pub fn instance_mut(&mut self, id: ControllerId) -> Option<&mut Instance> {
        self.instances.get_mut(&id)
    }

    /// Counts one delivery of the bus (debug builds; nothing in a release
    /// build). [`Cx`]'s `deliver` calls it before every handler.
    pub fn count_delivery(&mut self) {
        #[cfg(debug_assertions)]
        {
            self.deliveries += 1;
        }
    }

    /// The deliveries counted so far; 0 in a release build.
    #[must_use]
    pub fn deliveries(&self) -> u64 {
        #[cfg(debug_assertions)]
        {
            self.deliveries
        }
        #[cfg(not(debug_assertions))]
        {
            0
        }
    }

    /// Content.dll's unload (`FreeLibrary` once its last controller is torn
    /// down): the next load starts from the static initialisers again, so
    /// the tables, the globals and the counter are those of a fresh load.
    /// The delivery count is the bus's and stays.
    fn unload(&mut self) {
        self.tables = Arc::new(ContentTables::default());
        self.state = ContentState::load();
        self.instances.clear();
    }
}

/// A take of a Content item out of its slot for a call that cannot
/// dispatch (record 0008 point 1): made before the take, checked when the
/// item goes back. In a debug build [`NoDispatch::end`] asserts that the
/// bus delivered nothing in between.
#[derive(Clone, Copy, Debug)]
#[must_use = "a take is checked when its item goes back"]
pub struct NoDispatch {
    deliveries: u64,
}

impl NoDispatch {
    /// Starts a take.
    pub fn begin(content: &Content) -> Self {
        Self {
            deliveries: content.deliveries(),
        }
    }

    /// Ends a take: the bus must not have delivered a message since
    /// [`NoDispatch::begin`].
    pub fn end(self, content: &Content) {
        debug_assert_eq!(
            content.deliveries(),
            self.deliveries,
            "a message was delivered while a Content item was out of its slot"
        );
    }
}

/// Takes controller `id`'s instance out of its slot, runs `call`, which
/// must not dispatch, and puts the instance back (record 0008 point 1).
/// `None`, without a call, when the controller has no instance.
pub fn with_instance_taken<R>(
    cx: &mut Cx<'_>,
    id: ControllerId,
    call: impl FnOnce(&mut Cx<'_>, &mut Instance) -> R,
) -> Option<R> {
    let mut instance = cx.logic.content.instances.remove(&id)?;
    let take = NoDispatch::begin(&cx.logic.content);
    let result = call(cx, &mut instance);
    take.end(&cx.logic.content);
    cx.logic.content.instances.insert(id, instance);
    Some(result)
}

/// `CreateInstance(ControllerId, Name, Parms)` (`Content/Content.cpp:123-134`):
/// the factory's kind for `name` (`_stricmp`, first match), its
/// constructor run, its instance stored under `id`; `None` for a name the
/// factory does not know (a null interface), with nothing done.
pub fn instantiate(
    cx: &mut Cx<'_>,
    id: ControllerId,
    name: &[u8],
    params: &CreateParams,
) -> Option<ControllerKind> {
    let kind = ControllerKind::from_instance_name(name)?;
    let instance = match kind {
        ControllerKind::GameSupport => game_support::construct(cx, id),
        ControllerKind::SpaceSupport => space_support::construct(cx, id),
        ControllerKind::BaseSupport => base_support::construct(cx, id),
        ControllerKind::SpacePop => population::construct_space_pop(cx, id),
        ControllerKind::BasePop => population::construct_base_pop(cx, id),
        ControllerKind::StoryLauncher => story::construct_launcher(cx, id),
        ControllerKind::Story => story::construct_story(cx, id, params),
        ControllerKind::Mission01a
        | ControllerKind::Mission01b
        | ControllerKind::Mission(_)
        | ControllerKind::GenericMission => mission::construct(cx, id, kind, params),
        ControllerKind::DestroyMission => random_mission::construct(cx, id, params),
    };
    cx.logic.content.instances.insert(id, instance);
    Some(kind)
}

/// `DestroyInstance(Instance)` (`Content/Content.cpp:137-141`), called by
/// the bus's teardown after the node has left its list: the kind's
/// destructor, then the instance is dropped. When no Content controller
/// is left, Content.dll unloads (the bus's `FreeLibrary` drops its last
/// reference), and the next create finds a freshly loaded DLL.
pub fn destroy_instance(cx: &mut Cx<'_>, id: ControllerId, kind: ControllerKind) {
    match kind {
        ControllerKind::GameSupport => game_support::destruct(cx, id),
        ControllerKind::BaseSupport => base_support::destruct(cx, id),
        ControllerKind::StoryLauncher => story::destruct_launcher(cx, id),
        // No destructor body: SpaceSupport, SpacePop and BasePop have none,
        // Story's is the default one, and the missions' come with P3.5 and
        // P3.7.
        ControllerKind::SpaceSupport
        | ControllerKind::SpacePop
        | ControllerKind::BasePop
        | ControllerKind::Story
        | ControllerKind::Mission01a
        | ControllerKind::Mission01b
        | ControllerKind::Mission(_)
        | ControllerKind::GenericMission
        | ControllerKind::DestroyMission => {}
    }
    let content = &mut cx.logic.content;
    content.instances.remove(&id);
    if content.instances.is_empty() {
        content.unload();
    }
}

/// `IDLL::HandleMessage` of controller `id` of `kind`: the kind's handler.
/// The kinds without a ported handler ignore the message for now.
pub fn deliver(
    cx: &mut Cx<'_>,
    id: ControllerId,
    kind: ControllerKind,
    message: &mut ControllerMessage<'_>,
) {
    match kind {
        ControllerKind::GameSupport => game_support::handle_message(cx, id, message),
        ControllerKind::SpaceSupport => space_support::handle_message(cx, id, message),
        ControllerKind::BaseSupport
        | ControllerKind::SpacePop
        | ControllerKind::BasePop
        | ControllerKind::StoryLauncher
        | ControllerKind::Story
        | ControllerKind::Mission01a
        | ControllerKind::Mission01b
        | ControllerKind::Mission(_)
        | ControllerKind::GenericMission
        | ControllerKind::DestroyMission => {}
    }
}

/// A data source that has no files: a world without a data root.
struct NoFiles;

impl DataSource for NoFiles {
    fn read(&self, _path: &[u8]) -> Option<Vec<u8>> {
        None
    }
}

/// The world's data root for a loader that changes the context while it
/// reads (`OpenContentDataIni`, `pub::BuildSystemReader`); a source without
/// files when the world has none.
pub(crate) fn data_source(cx: &Cx<'_>) -> Box<dyn DataSource> {
    match cx.world.data_host() {
        Some(host) => Box::new(host),
        None => Box::new(NoFiles),
    }
}

/// `pub::Controller::SetHeartbeatInterval(id, interval)` from a
/// constructor. The id is the controller being made, which the bus has
/// listed already, so the call cannot fail; it draws one `rand()`.
fn set_heartbeat(cx: &mut Cx<'_>, id: ControllerId, interval: f32) {
    let _ = crate::api::controller::set_heartbeat_interval(cx, id, interval);
}
