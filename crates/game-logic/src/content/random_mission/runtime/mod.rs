//! The random missions' run time (work package P3.7b, record 0008 points 3
//! and 8): the `DestroyMission` controller (`Content/DestroyMission.cpp`
//! TU27 over `FRandomMissionObject`, TU47 in `RandomMissionDB.cpp`, and the
//! mission object of TU38), its runtime machines (`RandomMissionMachine.cpp`
//! and `RandomMissionVignette.cpp`, TU61) and their triggers
//! (`RandomMissionTriggers.cpp`, TU57).
//!
//! **The machines** are state enums in an arena of their controller's
//! instance ([`RandomMission::machines`]), keyed by their router handler id,
//! which the C++ hands out once and never again. The controller's own
//! state-machine group ([`RandomMission::sm`]) and each runtime machine are
//! groups: their members are the runtime machines, and the sub-machines and
//! triggers. Every loop runs through neuron-core's
//! [`MachineHost`](neuron_core::util::state_machine::MachineHost)
//! (record 0008 question 8): [`Host`] looks a machine up again after every
//! handler call, so a handler can dispatch, route a message at once (a
//! nested delivery), add or mark machines, and the walk stays on the live
//! list.
//!
//! **What reaches a machine.** The bus's messages go to
//! [`handle_message`] (`FMissionObject::HandleMessage`), which hands them
//! to the group as a controller event ([`message::ControllerEvent`]); the
//! router's deliveries go to [`receive_message`]
//! (`StateMachineMessageHandler::receive_message`).
//!
//! What waits for later packages (D1021 to D1024): the mission objectives
//! the players see (`FMissionPlayer`'s lists and their sends, P3.5c5; the
//! `rand()` draws of their path objectives are made where the C++ makes
//! them), the comm sequences' playback (the GCS conversation manager,
//! P3.6b), the ship groups' spawn (P3.5c2's NPC blocks) and the mission
//! object's body beyond what a random mission's players reach (D973).

pub mod message;
mod machine;
mod mission;
mod sub;
pub mod trace;

use std::collections::{BTreeMap, BTreeSet};
use std::io::Write;

use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::math::Vector;
use neuron_core::util::state_machine::{
    self as sm, EventType, MachineHost, SmEvent, StateMachine,
};
use neuron_server::controller::ControllerId;

use self::message::{Controller, ControllerEvent, RmMessage};
use super::RmDataKey;
use super::offer::LootSpawn;
use crate::content::Instance;
use crate::content::router::{self, HandlerOwner};
use crate::events::{ControllerMessage, NpcShipKey};
use crate::server::{CreateParams, Cx};

pub use machine::{ActiveObjective, ComponentKind, ObjectiveSource, Vignette};
pub use sub::{SpawnRecord, SubMachine, Trigger, TriggerKind};

/// An event offered to a random mission's machine.
pub type Event<'a> = SmEvent<'a, RmMessage, ControllerEvent>;

/// The random-mission globals of the run time (record 0008 point 8): the
/// vignette cookies and `GRmMultiplayerDelay`.
#[derive(Clone, Debug, PartialEq)]
pub struct RuntimeState {
    /// `GRmVignetteNextCookie` (`RandomMissionVignette.cpp:43`): the next
    /// cookie of the vignette machine's function-local statics.
    pub next_cookie: u32,
    /// The statics, each the counter's value the first time its code was
    /// reached ([`CookieSite`]); `None` until then.
    pub cookies: [Option<u32>; CookieSite::COUNT],
    /// `GRmMultiplayerDelay` (30): 45 once a runtime machine is built
    /// outside single player.
    pub multiplayer_delay: f32,
    /// `GRmMissionObjectCount`: the live random-mission objects.
    pub mission_objects: i32,
}

impl Default for RuntimeState {
    fn default() -> Self {
        Self {
            next_cookie: 0,
            cookies: [None; CookieSite::COUNT],
            multiplayer_delay: 30.0,
            mission_objects: 0,
        }
    }
}

/// The function-local statics of the vignette machine's slot 2 that hold a
/// cookie (`RandomMissionVignette.cpp`), in declaration order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CookieSite {
    /// `RmVignetteEntryCookies().LeaveArea`, made on the first call.
    LeaveArea,
    /// `.ReenterArea`.
    ReenterArea,
    /// `.Countdown`.
    Countdown,
    /// State 8's `CookieTargetFar`.
    TargetFar,
    /// State 8's `CookieTargetNear`.
    TargetNear,
    /// State 9's `CookieNextGroup`.
    TargetNextGroup,
    /// State 9's `CookieTargetDone`.
    TargetDone,
    /// State 10's `CookieMainShips`.
    MainShips,
    /// State 10's `CookieMainFar`.
    MainFar,
    /// State 10's `CookieMainNear`.
    MainNear,
    /// State 11's `CookieNextGroup`.
    MainNextGroup,
}

impl CookieSite {
    /// The number of sites.
    pub const COUNT: usize = 11;
}

/// The cookie of `site`, drawn from the counter (post-increment) the first
/// time.
pub(crate) fn cookie(cx: &mut Cx<'_>, site: CookieSite) -> u32 {
    let state = &mut cx.logic.content.state.rm.runtime;
    if let Some(c) = state.cookies[site as usize] {
        return c;
    }
    let c = state.next_cookie;
    state.next_cookie = state.next_cookie.wrapping_add(1);
    state.cookies[site as usize] = Some(c);
    c
}

/// One machine of a random mission: its bookkeeping and its kind.
#[derive(Clone, Debug, PartialEq)]
pub struct Machine {
    /// `StateMachine`.
    pub sm: StateMachine,
    /// What it is.
    pub kind: MachineKind,
}

/// The machine kinds.
#[derive(Clone, Debug, PartialEq)]
pub enum MachineKind {
    /// `FRmVignetteMachine`, the runtime machine of every offer type.
    Vignette(Box<Vignette>),
    /// `FRmSolarMachine`.
    Solars(SubMachine),
    /// `FRmShipMachine`.
    Ships(SubMachine),
    /// `FRmLootMachine`.
    Loot(SubMachine),
    /// A trigger (`FRmTrigger` and its five kinds).
    Trigger(Trigger),
}

impl MachineKind {
    /// The kind's name in the traces.
    #[must_use]
    pub fn name(&self) -> &'static str {
        match self {
            Self::Vignette(_) => "vignette",
            Self::Solars(_) => "solars",
            Self::Ships(_) => "ships",
            Self::Loot(_) => "loot",
            // The oracle's hook sits in `FRmTrigger::Slot2`, shared by
            // every kind.
            Self::Trigger(_) => "trigger",
        }
    }
}

/// A `DestroyMission` controller's own state: the mission object of TU38
/// (what a random mission uses of it), `FRandomMissionObject` (TU47) and
/// `FDestroyMission` (TU27), with its machines.
#[derive(Clone, Debug, PartialEq)]
pub struct RandomMission {
    /// The create parameter's mission data, `FRandomMissionObject::Data`
    /// and `FDestroyMission::MissionData` (two references); none for
    /// parameters of another form (D770).
    pub data: Option<RmDataKey>,
    /// TU38 `PlayerId`.
    pub player: u32,
    /// TU38 `Players`: the players with a mission record, by id.
    pub records: BTreeSet<u32>,
    /// TU38 `DeadPlayerIds`.
    pub dead_players: Vec<u32>,
    /// TU38 `Reward` (`+0x10`).
    pub reward: u32,
    /// TU38 `HeartbeatInterval`.
    pub heartbeat_interval: f32,
    /// TU38 `Field_0x80`: 7, then the first offer's type.
    pub kind: u8,
    /// TU38 `Field_0x50`: the mission's title.
    pub title: FmtStr,
    /// TU38 `Field_0x68`: the mission's description.
    pub description: FmtStr,
    /// TU38 `bPendingDestroy`.
    pub pending_destroy: bool,
    /// TU38 `bDestroyed`.
    pub destroyed: bool,
    /// TU47 `ReputationChanges`: `{player name, group}` in the multimap's
    /// order (by name, then insertion).
    pub reputation_changes: Vec<(Vec<u16>, u32)>,
    /// TU47 `PlayerStates`: the mission's players in key order.
    pub players: Vec<u32>,
    /// TU47 `PlayerStateMap`'s keys.
    pub player_map: BTreeSet<u32>,
    /// TU47 `PlayerCount`.
    pub player_count: i32,
    /// TU47 `PlayerNames`.
    pub player_names: Vec<Vec<u16>>,
    /// TU47 `ElapsedTime` (`+0x234`).
    pub elapsed_time: f32,
    /// TU47 `FailReason` (`+0x23c`).
    pub fail_reason: i32,
    /// The group's `MessageHandler` id.
    pub handler: u32,
    /// The group's own machine.
    pub sm: StateMachine,
    /// The group's members (the runtime machines), in list order.
    pub members: Vec<u32>,
    /// The members marked for removal (`DyingMachines`), in marking order.
    pub removals: Vec<u32>,
    /// TU27 `DestSystem`: the mission system's record, by id; 0 for none.
    pub dest_system: u32,
    /// TU27 `Field_0x25c`.
    pub field_0x25c: u8,
    /// TU27 `PendingMachines`: built, not started.
    pub pending_machines: Vec<u32>,
    /// TU27 `StartingMachines`: sent the start.
    pub starting_machines: Vec<u32>,
    /// TU27 `RunningMachines`: answered it.
    pub running_machines: Vec<u32>,
    /// Every machine of the mission, by handler id.
    pub machines: BTreeMap<u32, Machine>,
}

impl RandomMission {
    /// The instance before its constructor's body.
    fn new(data: Option<RmDataKey>) -> Self {
        Self {
            data,
            player: 0,
            records: BTreeSet::new(),
            dead_players: Vec::new(),
            reward: 0,
            heartbeat_interval: crate::content::mission::object::HEARTBEAT,
            kind: 7,
            title: FmtStr::new(0),
            description: FmtStr::new(0),
            pending_destroy: false,
            destroyed: false,
            reputation_changes: Vec::new(),
            players: Vec::new(),
            player_map: BTreeSet::new(),
            player_count: 0,
            player_names: Vec::new(),
            elapsed_time: 0.0,
            fail_reason: 0,
            handler: 0,
            sm: StateMachine::new(),
            members: Vec::new(),
            removals: Vec::new(),
            dest_system: 0,
            field_0x25c: 0,
            pending_machines: Vec::new(),
            starting_machines: Vec::new(),
            running_machines: Vec::new(),
            machines: BTreeMap::new(),
        }
    }
}

/// The instance of controller `id`.
pub(crate) fn rm<'a>(cx: &'a Cx<'_>, id: ControllerId) -> Option<&'a RandomMission> {
    match cx.logic.content.instances.get(&id) {
        Some(Instance::RandomMission(r)) => Some(r),
        _ => None,
    }
}

/// The instance of controller `id`, to change.
pub(crate) fn rm_mut<'a>(cx: &'a mut Cx<'_>, id: ControllerId) -> Option<&'a mut RandomMission> {
    match cx.logic.content.instances.get_mut(&id) {
        Some(Instance::RandomMission(r)) => Some(r),
        _ => None,
    }
}

/// The machines' host: the context, the mission whose machines it holds,
/// and the open save of a `PlayerSave` being handled.
pub(crate) struct Host<'h, 'c> {
    /// The context.
    pub cx: &'h mut Cx<'c>,
    /// The `DestroyMission` controller.
    pub mission: ControllerId,
    /// The save a `PlayerSave` writes to.
    pub save: Option<&'h mut dyn Write>,
}

impl Host<'_, '_> {
    /// The mission's instance.
    pub fn rm(&self) -> Option<&RandomMission> {
        rm(self.cx, self.mission)
    }

    /// The mission's instance, to change.
    pub fn rm_mut(&mut self) -> Option<&mut RandomMission> {
        rm_mut(self.cx, self.mission)
    }

    /// Machine `key` of the mission.
    pub fn machine_ref(&self, key: u32) -> Option<&Machine> {
        self.rm()?.machines.get(&key)
    }

    /// Machine `key` of the mission, to change.
    pub fn machine_mut(&mut self, key: u32) -> Option<&mut Machine> {
        self.rm_mut()?.machines.get_mut(&key)
    }

    /// The state of machine `key` (the mission's group included); -1 when
    /// it is gone.
    pub fn state_of(&self, key: u32) -> i32 {
        let Some(r) = self.rm() else { return -1 };
        if key == r.handler {
            return r.sm.state();
        }
        r.machines.get(&key).map_or(-1, |m| m.sm.state())
    }

    /// `set_state` on machine `key`.
    pub fn set_state(&mut self, key: u32, state: i32) {
        let Some(r) = self.rm_mut() else { return };
        if key == r.handler {
            r.sm.set_state(state);
        } else if let Some(m) = r.machines.get_mut(&key) {
            m.sm.set_state(state);
        }
    }

    /// `RouteMessage` through the router.
    pub fn route(&mut self, message: RmMessage, delay: f32, target: u32, sender: u32, scope: i32) {
        router::route(self.cx, message, delay, target, sender, scope);
    }

    /// `MessageHandler::send_immediate_message` from `sender`.
    pub fn send_immediate(&mut self, sender: u32, message: RmMessage, target: u32) {
        self.route(message, 0.0, target, sender, -1);
    }

    /// `MessageHandler::send_delayed_message` from `sender`.
    pub fn send_delayed(&mut self, sender: u32, message: RmMessage, delay: f32, target: u32) {
        self.route(message, delay, target, sender, -1);
    }

    /// `StateMachineMessageHandler::send_delayed_message_to_me` with
    /// `SMMS_Self` (the machine's current state as the delivery state).
    pub fn send_to_me_self(&mut self, key: u32, message: RmMessage, delay: f32) {
        let state = self.state_of(key);
        self.route(message, delay, key, key, state);
    }

    /// The same with `SMMS_Broadcast` (any state).
    pub fn send_to_me_broadcast(&mut self, key: u32, message: RmMessage, delay: f32) {
        self.route(message, delay, key, key, -1);
    }

    /// `remove_state_machine(Id)` on group `group`: the member is marked
    /// once; an id that is not a member of the group marks nothing.
    pub fn mark(&mut self, group: u32, id: u32) {
        let Some(r) = self.rm_mut() else { return };
        if group == r.handler {
            if r.members.contains(&id) && !r.removals.contains(&id) {
                r.removals.push(id);
            }
            return;
        }
        if let Some(Machine {
            kind: MachineKind::Vignette(v),
            ..
        }) = r.machines.get_mut(&group)
            && v.members.contains(&id)
            && !v.removals.contains(&id)
        {
            v.removals.push(id);
        }
    }

    /// `add_state_machine` on group `group`: `machine` registers (the
    /// caller made its handler id `key`), joins the list and is ticked.
    pub fn add_member(&mut self, group: u32, key: u32, machine: Machine) {
        let Some(r) = self.rm_mut() else { return };
        r.machines.insert(key, machine);
        if group == r.handler {
            r.members.push(key);
        } else if let Some(Machine {
            kind: MachineKind::Vignette(v),
            ..
        }) = r.machines.get_mut(&group)
        {
            v.members.push(key);
        }
        sm::start_in(self, key);
    }

    /// A new handler id owned by this mission's machines.
    pub fn register(&mut self) -> u32 {
        router::register(self.cx, HandlerOwner::Machine(self.mission))
    }

    /// The mission's players (`GetPlayerStates`), as ids.
    pub fn players(&self) -> Vec<u32> {
        self.rm().map(|r| r.players.clone()).unwrap_or_default()
    }

    /// Whether player `player` is in the mission's map.
    pub fn is_mapped(&self, player: u32) -> bool {
        self.rm().is_some_and(|r| r.player_map.contains(&player))
    }

    /// A machine and its members destroyed: the members front to back
    /// (`~StateMachineGroup`), then the machine's own handler.
    pub fn destroy_machine(&mut self, key: u32) {
        let Some(machine) = self.rm_mut().and_then(|r| r.machines.remove(&key)) else {
            return;
        };
        if let MachineKind::Vignette(v) = machine.kind {
            for member in v.members {
                self.destroy_machine(member);
            }
        }
        router::unregister(self.cx, key);
    }
}

impl MachineHost for Host<'_, '_> {
    type Key = u32;
    type Message = RmMessage;
    type Controller = ControllerEvent;

    fn machine(&mut self, key: u32) -> Option<&mut StateMachine> {
        let r = self.rm_mut()?;
        if key == r.handler {
            return Some(&mut r.sm);
        }
        r.machines.get_mut(&key).map(|m| &mut m.sm)
    }

    fn handle(&mut self, key: u32, event: &Event<'_>, state: i32) -> bool {
        let Some(r) = self.rm() else { return false };
        if key == r.handler {
            trace::slot2(self.cx, key, "mission", event.event_type(), state);
            return mission::slot2(self, event, state);
        }
        let Some(m) = r.machines.get(&key) else {
            return false;
        };
        let name = m.kind.name();
        let kind = match &m.kind {
            MachineKind::Vignette(_) => 0,
            MachineKind::Solars(_) => 1,
            MachineKind::Ships(_) => 2,
            MachineKind::Loot(_) => 3,
            MachineKind::Trigger(_) => 4,
        };
        trace::slot2(self.cx, key, name, event.event_type(), state);
        match kind {
            0 => machine::slot2(self, key, event, state),
            1 => sub::solar_slot2(self, key, event, state),
            2 => sub::ship_slot2(self, key, event, state),
            3 => sub::loot_slot2(self, key, event, state),
            _ => sub::trigger_slot2(self, key, event, state),
        }
    }

    fn state_changing(&mut self, key: u32) {
        // `FRmMachineGroup::handle_state_change`: every `StateMachines` id
        // removed (marked) front first.
        let ids = match self.machine_mut(key) {
            Some(Machine {
                kind: MachineKind::Vignette(v),
                ..
            }) => std::mem::take(&mut v.state_machines),
            _ => return,
        };
        for id in ids {
            self.mark(key, id);
        }
    }

    fn is_group(&self, key: u32) -> bool {
        let Some(r) = self.rm() else { return false };
        key == r.handler
            || matches!(
                r.machines.get(&key),
                Some(Machine {
                    kind: MachineKind::Vignette(_),
                    ..
                })
            )
    }

    fn member(&self, group: u32, index: usize) -> Option<u32> {
        let r = self.rm()?;
        if group == r.handler {
            return r.members.get(index).copied();
        }
        match r.machines.get(&group) {
            Some(Machine {
                kind: MachineKind::Vignette(v),
                ..
            }) => v.members.get(index).copied(),
            _ => None,
        }
    }

    fn member_index(&self, group: u32, member: u32) -> Option<usize> {
        let r = self.rm()?;
        if group == r.handler {
            return r.members.iter().position(|&m| m == member);
        }
        match r.machines.get(&group) {
            Some(Machine {
                kind: MachineKind::Vignette(v),
                ..
            }) => v.members.iter().position(|&m| m == member),
            _ => None,
        }
    }

    fn remove_dying(&mut self, group: u32) {
        let Some(r) = self.rm_mut() else { return };
        let marked = if group == r.handler {
            std::mem::take(&mut r.removals)
        } else {
            match r.machines.get_mut(&group) {
                Some(Machine {
                    kind: MachineKind::Vignette(v),
                    ..
                }) => std::mem::take(&mut v.removals),
                _ => return,
            }
        };
        for id in marked {
            let Some(r) = self.rm_mut() else { return };
            let list = if group == r.handler {
                &mut r.members
            } else {
                match r.machines.get_mut(&group) {
                    Some(Machine {
                        kind: MachineKind::Vignette(v),
                        ..
                    }) => &mut v.members,
                    _ => return,
                }
            };
            if let Some(at) = list.iter().position(|&m| m == id) {
                list.remove(at);
                self.destroy_machine(id);
            }
        }
    }
}

/// `CreateDestroyMission`: the instance and its constructors
/// ([`mission::construct`]).
pub fn construct(cx: &mut Cx<'_>, id: ControllerId, params: &CreateParams) -> Instance {
    let data = match *params {
        CreateParams::RandomMission(key) => Some(key),
        _ => None,
    };
    // The instance goes into its slot first: the constructor's group
    // process can route a message to the mission at once.
    cx.logic
        .content
        .instances
        .insert(id, Instance::RandomMission(Box::new(RandomMission::new(data))));
    mission::construct(cx, id);
    cx.logic
        .content
        .instances
        .remove(&id)
        .unwrap_or_else(|| Instance::RandomMission(Box::new(RandomMission::new(data))))
}

/// `~FDestroyMission` and its bases ([`mission::destruct`]).
pub fn destruct(cx: &mut Cx<'_>, id: ControllerId) {
    mission::destruct(cx, id);
}

/// `FMissionObject::HandleMessage` (`MissionObject.cpp:129-142`) of a
/// `DestroyMission`: a destroyed mission ignores every message; one that
/// asks for its teardown, before or during the message, has it asked of
/// the bus.
pub fn handle_message(cx: &mut Cx<'_>, id: ControllerId, message: &mut ControllerMessage<'_>) {
    let Some(r) = rm(cx, id) else { return };
    if r.destroyed {
        return;
    }
    if !r.pending_destroy {
        handle_mission_message(cx, id, message);
        if !rm(cx, id).is_some_and(|r| r.pending_destroy) {
            return;
        }
    }
    crate::api::controller::destroy(cx, id);
    if let Some(r) = rm_mut(cx, id) {
        r.destroyed = true;
    }
}

/// `FRandomMissionObject::HandleMissionMessage` (`RandomMissionDB.cpp:1127-1136`):
/// the message as a controller event through the group's slot 1.
fn handle_mission_message(cx: &mut Cx<'_>, id: ControllerId, message: &mut ControllerMessage<'_>) {
    let event = controller_event(message);
    let Some(group) = rm(cx, id).map(|r| r.handler) else {
        return;
    };
    let save: Option<&mut dyn Write> = match message {
        ControllerMessage::PlayerSave { file, .. } => Some(&mut **file),
        _ => None,
    };
    let mut host = Host {
        cx,
        mission: id,
        save,
    };
    sm::dispatch_in(
        &mut host,
        group,
        &SmEvent::Controller {
            id: event.id as i32,
            data: &event,
        },
    );
}

/// `StateMachineMessageHandler::receive_message`: a routed message as a
/// user event through the owner's slot 1.
pub fn receive_message(cx: &mut Cx<'_>, owner: HandlerOwner, message: &RmMessage) {
    let (mission, key) = match owner {
        HandlerOwner::Component => return,
        HandlerOwner::Mission(m) => match rm(cx, m) {
            Some(r) => (m, r.handler),
            None => return,
        },
        HandlerOwner::Machine(m) => (m, message.header.receiver),
    };
    let mut host = Host {
        cx,
        mission,
        save: None,
    };
    if sm::MachineHost::machine(&mut host, key).is_none() {
        return;
    }
    sm::dispatch_in(&mut host, key, &SmEvent::Message(message));
}

/// The controller event the machines read of a message.
fn controller_event(message: &ControllerMessage<'_>) -> ControllerEvent {
    use ControllerMessage as M;
    let id = message.id();
    let message = match message {
        M::Heartbeat { elapsed } => Controller::Heartbeat(*elapsed),
        M::ObjectDestroyed(d) => Controller::ObjectDestroyed {
            object: d.id,
            killer: d.killer,
            player: d.owner_player,
            kind: d.kind,
            position: d.position,
        },
        M::ObjectDamaged(d) => Controller::ObjectDamaged {
            object: d.id,
            relative_damage: d.relative_damage,
        },
        M::LootAcquired {
            loot,
            ship,
            sub_object,
            good,
            count,
        } => Controller::LootAcquired {
            loot: *loot,
            ship: *ship,
            slot: *sub_object,
            good: *good,
            count: *count,
        },
        M::Msg0x27 { ship, base, .. } => Controller::Docked {
            ship: *ship,
            base: *base,
        },
        M::SystemUnloaded { system } => Controller::SystemUnloaded(*system),
        M::SpaceEnter { system, player, .. } => Controller::SpaceEnter {
            system: *system,
            player: *player,
        },
        M::SystemEnter { system, player, .. } => Controller::SystemEnter {
            system: *system,
            player: *player,
        },
        M::GroupMemberJoined { player } => Controller::GroupMemberJoined(*player),
        M::GroupMemberLeft { player } => Controller::GroupMemberLeft(*player),
        M::PlayerLoad { player, .. } => Controller::PlayerLoad(*player),
        M::PlayerSave { client, .. } => Controller::PlayerSave(*client),
        M::PlayerLogout { player } => Controller::PlayerLogout(*player),
        M::AbortMission { player, .. } => Controller::AbortMission(*player),
        M::DockRequest { .. } => Controller::DockRequest,
        M::Msg0x25 { .. } => Controller::ShipJumped,
        M::FormationLeaderChanged { .. } => Controller::FormationLeaderChanged,
        _ => Controller::Other,
    };
    ControllerEvent { id, message }
}

/// The controller event of `event`, if it is one.
pub(crate) fn controller_of<'a>(event: &'a Event<'_>) -> Option<&'a ControllerEvent> {
    match event {
        SmEvent::Controller { data, .. } => Some(data),
        _ => None,
    }
}

/// The routed message of `event`, if it is one.
pub(crate) fn message_of<'a>(event: &'a Event<'_>) -> Option<&'a RmMessage> {
    match event {
        SmEvent::Message(m) => Some(m),
        _ => None,
    }
}

/// The C++'s enter test (`type < 4 && type == 1`).
pub(crate) fn is_enter(event: &Event<'_>) -> bool {
    event.event_type() == EventType::Tick
}

/// The C++'s exit test (`type < 4 && type == 2`).
pub(crate) fn is_exit(event: &Event<'_>) -> bool {
    event.event_type() == EventType::Message
}

/// A player's position (`FContentPlayerState::GetPosition`,
/// `PlayerState.cpp:186-204`): its ship's, else the position of its base in
/// its system (`pub::GF::GetBasePosition`), else the origin.
pub(crate) fn player_position(cx: &mut Cx<'_>, player: u32) -> Vector {
    let Some(state) = cx.logic.content.state.players.get(&player) else {
        return Vector::ZERO;
    };
    if let Some(ship) = &state.ship {
        let ship = ship.ship;
        return crate::api::space_obj::location(cx, ship).map_or(Vector::ZERO, |(p, _)| p);
    }
    let (system, base) = (state.system.unwrap_or(0), state.base.unwrap_or(0));
    let source = crate::content::data_source(cx);
    crate::api::gf::base_position(cx, &*source, system, base).unwrap_or(Vector::ZERO)
}

/// The oracle session's `FRandomMissionObject::OracleJoinPlayer`: player
/// `player` mapped, listed and named as `SetPlayer` does for each member of
/// a group, which a player without a server slot does not have. Only the
/// Tier B session calls it.
pub fn join_player(cx: &mut Cx<'_>, id: ControllerId, player: u32) {
    let name = crate::api::player::name(cx, player).unwrap_or_default();
    let Some(r) = rm_mut(cx, id) else { return };
    r.player_map.insert(player);
    r.players.push(player);
    if !r.player_names.contains(&name) {
        if r.player_names.len() < 8 {
            r.player_count += 1;
        }
        r.player_names.push(name);
    }
}

/// A spawn record's loot list (for the records' constructors).
pub(crate) type Loot = Vec<LootSpawn>;

/// An NPC ship of a record.
pub(crate) type NpcShip = Option<NpcShipKey>;
