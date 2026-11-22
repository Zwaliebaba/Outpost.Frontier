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
//! (P3.4b, [`tables`]), the other loaders (P3.4c), GameSupport's run time
//! (P3.4d), the mission and story constructors (P3.5), SpaceSupport's
//! other steps, the populators and BaseSupport (P3.6) and the random
//! missions (P3.7) come with their packages.

pub mod base_support;
pub mod counter;
pub mod game_support;
pub mod mission;
pub mod population;
pub mod random_mission;
pub mod router;
pub mod space_support;
pub mod story;
/// The static tables of `LoadContentGameTables` (`Content/Encounters.cpp:497-591`;
/// P3.4b).
pub mod tables;

use std::collections::BTreeMap;
use std::sync::Arc;

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
    Mission(mission::MissionScript),
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

/// `pub::Controller::SetHeartbeatInterval(id, interval)` from a
/// constructor. The id is the controller being made, which the bus has
/// listed already, so the call cannot fail; it draws one `rand()`.
fn set_heartbeat(cx: &mut Cx<'_>, id: ControllerId, interval: f32) {
    let _ = crate::api::controller::set_heartbeat_interval(cx, id, interval);
}
