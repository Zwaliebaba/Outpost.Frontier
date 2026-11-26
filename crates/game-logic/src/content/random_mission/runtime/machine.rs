//! The runtime machine (`FRmRuntimeMachine` and `FRmVignetteMachine`,
//! `RandomMissionMachine.cpp:987-2272`, `RandomMissionVignette.cpp`): one
//! per offer of a `DestroyMission`, for the three machine types (destroy
//! solars 1, assassinate a ship 2 or a solar 3), all vignette machines.
//!
//! The machine is a group: its three sub-machines (the records of the
//! solars, ships and loot it spawned) and the triggers its states arm. Its
//! slot 2 is one function in the C++; the port keeps its pieces by state:
//! the "any state" messages ([`any_state`]), states 0 to 2 of the base
//! (`OnState0` to `OnState2`), then 3 start, 8 pre-battle, 9 target fight,
//! 10 approach, 11 main battle, 12 critical loot, 5 succeeded, 6 failed
//! and 7 done.
//!
//! **The objectives** ([`Vignette::active`], `ActiveObjectives`) are the
//! offer components' objectives (by component and map key) and the
//! countdown the machine makes. The machine keeps their keys, markers and
//! targets exactly; the mission objectives built from them for the
//! players (`CreateMissionObjective`, `FMissionPlayer`'s list and its
//! sends) are P3.5c5's (D1021). The refreshes draw `rand()` where the C++
//! draws it ([`refresh_objectives`]).

#![allow(clippy::too_many_lines, clippy::neg_cmp_op_on_partial_ord)]

use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::math::Vector;
use neuron_core::num::trunc_to_i32;
use neuron_core::util::state_machine::StateMachine;

use super::super::offer::{Conversation, Objective, Offer, ShipComponent};
use super::message::{Body, Controller, MessageType, RmMessage};
use super::sub::{self, Trigger, TriggerKind};
use super::{
    CookieSite, Event, Host, Machine, MachineKind, controller_of, cookie, is_enter, is_exit,
    message_of, player_position,
};
use crate::api;
use crate::content::game_support::time_distance;
use crate::content::{players, population};

/// `GRmVignetteOuterRadius`: 3500 + 6000.
pub const OUTER_RADIUS: f32 = 9500.0;

/// The offer component an objective belongs to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ComponentKind {
    /// `General` (`+0xb8`).
    General,
    /// `TargetShips` (`+0xec`).
    TargetShips,
    /// `Solars` (`+0x144`).
    Solars,
    /// `Ships` (`+0x190`).
    Ships,
}

/// The countdown objective (`FRmObjectiveType4`) the machine makes when
/// the players leave the combat area.
#[derive(Clone, Debug, PartialEq)]
pub struct Countdown {
    /// `StrId`: the general component's objective 9's.
    pub str_id: i32,
    /// `ObjectiveType` (0xda).
    pub objective_type: i32,
    /// `MarkerSystemId`.
    pub marker_system: u32,
    /// `MarkerValue`.
    pub marker_value: i32,
    /// `MarkerPosition`.
    pub marker_position: Vector,
    /// `Seconds`.
    pub seconds: i32,
}

/// Where an active objective lives.
#[derive(Clone, Debug, PartialEq)]
pub enum ObjectiveSource {
    /// An offer component's objective, by its map key.
    Offer(ComponentKind, i32),
    /// The machine's own countdown.
    Countdown(Countdown),
}

/// One entry of `ActiveObjectives`: its `Key` and the objective.
#[derive(Clone, Debug, PartialEq)]
pub struct ActiveObjective {
    /// `FRmObjective::Key`.
    pub key: i32,
    /// The objective.
    pub source: ObjectiveSource,
}

/// `FRmVignetteMachine` with its base `FRmRuntimeMachine` and its group.
#[derive(Clone, Debug, PartialEq)]
pub struct Vignette {
    /// The group's members (sub-machines and triggers), in list order.
    pub members: Vec<u32>,
    /// The members marked for removal.
    pub removals: Vec<u32>,
    /// `FRmMachineGroup::Machines`: triggers kept across state changes.
    pub machines: Vec<u32>,
    /// `FRmMachineGroup::StateMachines`: triggers removed on the next
    /// state change.
    pub state_machines: Vec<u32>,
    /// `Solars` (`+0x58`): the solar machine's handler id.
    pub solars: u32,
    /// `Ships` (`+0x5c`).
    pub ships: u32,
    /// `Loot` (`+0x60`).
    pub loot: u32,
    /// `bObjectivesChanged`.
    pub objectives_changed: bool,
    /// `ActiveObjectives`.
    pub active: Vec<ActiveObjective>,
    /// The offer: its place in the mission data's offers.
    pub offer: usize,
    /// `Position`: the centre of the mission area.
    pub position: Vector,
    /// `AreaRadius` (3500).
    pub area_radius: f32,
    /// `bDefensiveShipSpawnSent`.
    pub defensive_sent: bool,
    /// `bBigShipSpawnSent`.
    pub big_sent: bool,
    /// `bFriendlySpawnSent`.
    pub friendly_sent: bool,
    /// `bAreaReached`.
    pub area_reached: bool,
    /// `FailReason`.
    pub fail_reason: i32,
    /// `Field_0xa8` (0).
    pub field_0xa8: u32,
    /// `LeftAreaTime` (-1 while the players are in the area).
    pub left_area_time: f32,
}

/// The comm sequence triggers (`ERmCommTrigger`).
#[allow(missing_docs)]
pub mod comm {
    pub const IN_SPACE_IN_SYSTEM: i32 = 1;
    pub const IN_SPACE_NOT_IN_SYSTEM: i32 = 2;
    pub const PRE_BATTLE_SPAWN_RANGE: i32 = 3;
    pub const KILLED_ALL_GOING_TO_MAIN: i32 = 4;
    pub const NOT_KILLED_ALL_GOING_TO_MAIN: i32 = 5;
    pub const MAIN_BATTLE_SOLAR_SPAWN_RANGE: i32 = 6;
    pub const MAIN_BATTLE_SHIP_SPAWN_RANGE: i32 = 7;
    pub const FIRST_FRIENDLY_SPAWN: i32 = 8;
    pub const FIRST_DEFENSIVE_SHIP_SPAWN: i32 = 9;
    pub const FIRST_BIG_SHIP_SPAWN: i32 = 10;
    pub const LEFT_COMBAT_AREA: i32 = 11;
    pub const REENTERED_COMBAT_AREA: i32 = 12;
    pub const GLOBAL_SUCCESS: i32 = 13;
    pub const GLOBAL_FAIL_COMBAT_RANGE: i32 = 14;
    pub const GLOBAL_FAIL_DESTROYED_CRITICAL_LOOT: i32 = 15;
    pub const GLOBAL_FAIL_OTHER: i32 = 16;
}

// ---------------------------------------------------------------------------
// Access
// ---------------------------------------------------------------------------

fn vig<'a>(host: &'a Host<'_, '_>, key: u32) -> Option<&'a Vignette> {
    match &host.machine_ref(key)?.kind {
        MachineKind::Vignette(v) => Some(v),
        _ => None,
    }
}

fn vig_mut<'a>(host: &'a mut Host<'_, '_>, key: u32) -> Option<&'a mut Vignette> {
    match &mut host.machine_mut(key)?.kind {
        MachineKind::Vignette(v) => Some(v),
        _ => None,
    }
}

/// The machine's offer (`VignetteOffer`).
fn offer<'a>(host: &'a Host<'_, '_>, key: u32) -> Option<&'a Offer> {
    let index = vig(host, key)?.offer;
    let data = host.rm()?.data?;
    host.cx
        .logic
        .content
        .state
        .rm
        .missions
        .get(data)?
        .offers
        .get(index)
}

fn offer_mut<'a>(host: &'a mut Host<'_, '_>, key: u32) -> Option<&'a mut Offer> {
    let index = vig(host, key)?.offer;
    let data = host.rm()?.data?;
    host.cx
        .logic
        .content
        .state
        .rm
        .missions
        .get_mut(data)?
        .offers
        .get_mut(index)
}

/// `Offer->System->SystemId`.
fn offer_system(host: &Host<'_, '_>, key: u32) -> u32 {
    offer(host, key).map_or(0, |o| o.system)
}

/// The objective of `key` in `component`'s map (`RmComponentObjective`;
/// the C++ inserts a null entry for a missing key and calls through it, a
/// key no offer of the shipped data lacks, D1025).
fn component_objective(
    o: &mut Offer,
    component: ComponentKind,
    key: i32,
) -> Option<&mut Objective> {
    let map = match component {
        ComponentKind::General => &mut o.general.objectives,
        ComponentKind::TargetShips => &mut o.target_ships.component.objectives,
        ComponentKind::Solars => &mut o.solars.component.objectives,
        ComponentKind::Ships => &mut o.ships.component.objectives,
    };
    map.get_mut(&key)
}

/// The main battle's centre: the solars' when that component is active,
/// else the ships'.
fn main_center(o: &Offer) -> Vector {
    if o.solars.component.active {
        o.solars.position
    } else {
        o.ships.position
    }
}

fn ship_component(o: &Offer, target: bool) -> &ShipComponent {
    if target { &o.target_ships } else { &o.ships }
}

/// `RmHasUnspawnedGroup`: a group to spawn that is not spawned yet.
fn has_unspawned(o: &Offer, target: bool) -> bool {
    ship_component(o, target)
        .groups
        .iter()
        .any(|g| g.field_0x00 != 0 && g.field_0x01 == 0)
}

/// `RmIsAwayFromOrigin`: some coordinate's square above 1.
fn away_from_origin(at: Vector) -> bool {
    let (x, y, z) = (f64::from(at.x), f64::from(at.y), f64::from(at.z));
    !(x * x <= 1.0) || !(y * y <= 1.0) || !(z * z <= 1.0)
}

/// An active objective's `GetPosition` (slot 7).
fn objective_position(host: &Host<'_, '_>, key: u32, a: &ActiveObjective) -> Option<Vector> {
    match &a.source {
        ObjectiveSource::Countdown(c) => {
            away_from_origin(c.marker_position).then_some(c.marker_position)
        }
        ObjectiveSource::Offer(component, map_key) => {
            let o = offer(host, key)?;
            let map = match component {
                ComponentKind::General => &o.general.objectives,
                ComponentKind::TargetShips => &o.target_ships.component.objectives,
                ComponentKind::Solars => &o.solars.component.objectives,
                ComponentKind::Ships => &o.ships.component.objectives,
            };
            map.get(map_key).map(|ob| ob.marker_position)
        }
    }
}

/// An active objective's `GetSystemId` (slot 9).
fn objective_system(host: &Host<'_, '_>, key: u32, a: &ActiveObjective) -> Option<u32> {
    match &a.source {
        ObjectiveSource::Countdown(c) => Some(c.marker_system),
        ObjectiveSource::Offer(component, map_key) => {
            let o = offer(host, key)?;
            let map = match component {
                ComponentKind::General => &o.general.objectives,
                ComponentKind::TargetShips => &o.target_ships.component.objectives,
                ComponentKind::Solars => &o.solars.component.objectives,
                ComponentKind::Ships => &o.ships.component.objectives,
            };
            map.get(map_key).map(|ob| ob.marker_system)
        }
    }
}

/// `FRmRuntimeMachine::GetSystemId` (slot 8, `:1023-1037`): the system of
/// the first objective that has one, else the offer's.
pub(super) fn system_id(host: &Host<'_, '_>, key: u32) -> u32 {
    let Some(v) = vig(host, key) else { return 0 };
    v.active
        .iter()
        .find_map(|a| objective_system(host, key, a))
        .unwrap_or_else(|| offer_system(host, key))
}

/// `FRmRuntimeMachine::CanSave` (slot 12): false in states 5, 6 and 7.
pub(super) fn can_save(host: &Host<'_, '_>, key: u32) -> bool {
    !matches!(host.state_of(key), 5..=7)
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

/// `CreateRmRuntimeMachine` and the vignette machine's constructor
/// (`:2251-2259`, `:987-1009`, `:2139-2145`): the machine registers, then
/// its solar, ship and loot machines, each added to its group and ticked.
/// `None`, and nothing made, for a machine type outside 1 to 3.
pub(super) fn build(host: &mut Host<'_, '_>, offer_index: usize) -> Option<u32> {
    let machine_type = {
        let data = host.rm()?.data?;
        host.cx
            .logic
            .content
            .state
            .rm
            .missions
            .get(data)?
            .offers
            .get(offer_index)?
            .machine_type
    };
    if !(1..=3).contains(&machine_type) {
        return None;
    }
    let key = host.register();
    if !api::misc::single_player(host.cx) {
        host.cx.logic.content.state.rm.runtime.multiplayer_delay = 45.0;
    }
    let vignette = Vignette {
        members: Vec::new(),
        removals: Vec::new(),
        machines: Vec::new(),
        state_machines: Vec::new(),
        solars: 0,
        ships: 0,
        loot: 0,
        objectives_changed: false,
        active: Vec::new(),
        offer: offer_index,
        position: Vector::ZERO,
        area_radius: 3500.0,
        defensive_sent: false,
        big_sent: false,
        friendly_sent: false,
        area_reached: false,
        fail_reason: 0,
        field_0xa8: 0,
        left_area_time: -1.0,
    };
    if let Some(r) = host.rm_mut() {
        r.machines.insert(
            key,
            Machine {
                sm: StateMachine::new(),
                kind: MachineKind::Vignette(Box::new(vignette)),
            },
        );
    }
    let solars = host.register();
    host.add_member(key, solars, sub::sub_machine(MachineKind::Solars, key));
    let ships = host.register();
    host.add_member(key, ships, sub::sub_machine(MachineKind::Ships, key));
    let loot = host.register();
    host.add_member(key, loot, sub::sub_machine(MachineKind::Loot, key));
    if let Some(v) = vig_mut(host, key) {
        v.solars = solars;
        v.ships = ships;
        v.loot = loot;
    }
    Some(key)
}

/// A trigger of this machine made and registered.
fn new_trigger(
    host: &mut Host<'_, '_>,
    cookie: u32,
    owner: u32,
    kind: TriggerKind,
) -> (u32, Machine) {
    let id = host.register();
    (
        id,
        Machine {
            sm: StateMachine::new(),
            kind: MachineKind::Trigger(Trigger::new(cookie, owner, kind)),
        },
    )
}

/// `FRmMachineGroup::AddStateMachine`: added and ticked, then listed to go
/// on the next state change.
fn add_state_machine(host: &mut Host<'_, '_>, key: u32, cookie: u32, kind: TriggerKind) -> u32 {
    let (id, machine) = new_trigger(host, cookie, key, kind);
    host.add_member(key, id, machine);
    if let Some(v) = vig_mut(host, key) {
        v.state_machines.push(id);
    }
    id
}

/// `FRmMachineGroup::AddMachine`: added and ticked, then listed to stay.
fn add_machine(host: &mut Host<'_, '_>, key: u32, cookie: u32, kind: TriggerKind) -> u32 {
    let (id, machine) = new_trigger(host, cookie, key, kind);
    host.add_member(key, id, machine);
    if let Some(v) = vig_mut(host, key) {
        v.machines.push(id);
    }
    id
}

/// `FRmMachineGroup::RemoveMachine`: marked, then out of `StateMachines`
/// or, when it is not there, out of `Machines`.
fn remove_machine(host: &mut Host<'_, '_>, key: u32, id: u32) {
    host.mark(key, id);
    let Some(v) = vig_mut(host, key) else { return };
    let before = v.state_machines.len();
    v.state_machines.retain(|&m| m != id);
    if v.state_machines.len() == before {
        v.machines.retain(|&m| m != id);
    }
}

/// A proximity trigger's kind.
fn proximity(system: u32, center: Vector, radius: f32) -> TriggerKind {
    TriggerKind::Proximity {
        center,
        radius_sq: TriggerKind::square(radius),
        system,
    }
}

/// A leave trigger's kind.
fn leave(system: u32, center: Vector, radius: f32) -> TriggerKind {
    TriggerKind::Leave {
        center,
        radius_sq: TriggerKind::square(radius),
        system,
    }
}

/// `AreaRadius + d` (or `- d`), stored as a float.
fn area_plus(host: &Host<'_, '_>, key: u32, d: f64) -> f32 {
    let r = vig(host, key).map_or(3500.0, |v| v.area_radius);
    (f64::from(r) + d) as f32
}

// ---------------------------------------------------------------------------
// Objectives
// ---------------------------------------------------------------------------

/// `FindObjective`.
fn find_objective(host: &Host<'_, '_>, key: u32, objective: i32) -> Option<usize> {
    vig(host, key)?
        .active
        .iter()
        .position(|a| a.key == objective)
}

fn has_objective(host: &Host<'_, '_>, key: u32, objective: i32) -> bool {
    find_objective(host, key, objective).is_some()
}

/// `AddObjective` (`:2014-2023`).
fn add_objective(host: &mut Host<'_, '_>, key: u32, objective: i32, source: ObjectiveSource) {
    let Some(v) = vig_mut(host, key) else { return };
    v.active.push(ActiveObjective {
        key: objective,
        source,
    });
    v.objectives_changed = true;
    refresh_objectives(host, key);
}

/// `InsertObjective` before `at` (`:2049-2061`).
fn insert_objective(
    host: &mut Host<'_, '_>,
    key: u32,
    objective: i32,
    source: ObjectiveSource,
    at: usize,
) {
    let Some(v) = vig_mut(host, key) else { return };
    let at = at.min(v.active.len());
    v.active.insert(
        at,
        ActiveObjective {
            key: objective,
            source,
        },
    );
    v.objectives_changed = true;
    refresh_objectives(host, key);
}

/// `RebuildObjective` (`:2064-2078`): out and in again at its place.
fn rebuild_objective(host: &mut Host<'_, '_>, key: u32, objective: i32) -> bool {
    let Some(at) = find_objective(host, key, objective) else {
        return false;
    };
    let Some(v) = vig_mut(host, key) else {
        return false;
    };
    let entry = v.active.remove(at);
    insert_objective(host, key, entry.key, entry.source, at);
    true
}

/// `RemoveObjective` (`:2081-2094`).
fn remove_objective(host: &mut Host<'_, '_>, key: u32, objective: i32) -> Option<ActiveObjective> {
    let at = find_objective(host, key, objective)?;
    let v = vig_mut(host, key)?;
    let entry = v.active.remove(at);
    v.objectives_changed = true;
    refresh_objectives(host, key);
    Some(entry)
}

/// `FRmObjectiveType2::SetMarker` on a component objective.
fn set_marker(
    host: &mut Host<'_, '_>,
    key: u32,
    component: ComponentKind,
    objective: i32,
    system: u32,
    at: Vector,
) {
    if let Some(o) = offer_mut(host, key)
        && let Some(ob) = component_objective(o, component, objective)
    {
        ob.marker_system = system;
        ob.marker_value = 0x44d;
        ob.marker_position = at;
    }
}

/// `FRmObjectiveType3::SetTargets` on a component objective (`:213-238`):
/// the marker, the old targets unmarked for the mission players, the new
/// ones marked.
fn set_targets(
    host: &mut Host<'_, '_>,
    key: u32,
    component: ComponentKind,
    objective: i32,
    ids: Vec<u32>,
    system: u32,
    at: Vector,
) {
    let players = host.players();
    let old = {
        let Some(o) = offer_mut(host, key) else {
            return;
        };
        let Some(ob) = component_objective(o, component, objective) else {
            return;
        };
        ob.marker_position = at;
        ob.marker_system = system;
        ob.marker_value = 0;
        std::mem::replace(&mut ob.targets, ids.clone())
    };
    for id in old {
        for &p in &players {
            let _ = api::player::mark_obj(host.cx, p, id, 0);
        }
    }
    for &id in &ids {
        for &p in &players {
            let _ = api::player::mark_obj(host.cx, p, id, 1);
        }
    }
}

/// The objects of `indices` in sub-machine `machine` that are alive and
/// have a bit of `mask`.
fn alive_flagged(host: &Host<'_, '_>, machine: u32, indices: &[usize], mask: u32) -> Vec<u32> {
    let records = sub::records(host, machine);
    indices
        .iter()
        .filter_map(|&i| records.get(i))
        .filter(|r| r.flags & mask != 0 && r.state == 2)
        .map(|r| r.object)
        .collect()
}

/// `SetTargetRecords` with the records of `indices`.
#[allow(clippy::too_many_arguments)]
fn set_target_records(
    host: &mut Host<'_, '_>,
    key: u32,
    component: ComponentKind,
    objective: i32,
    machine: u32,
    indices: &[usize],
    system: u32,
    at: Vector,
) {
    let ids = alive_flagged(host, machine, indices, 2);
    set_targets(host, key, component, objective, ids, system, at);
}

/// `RefreshObjectives` (slot 16, `:1286-1298`): the players in this
/// machine's system are brought up to date; then the change flag clears.
pub(super) fn refresh_objectives(host: &mut Host<'_, '_>, key: u32) {
    for player in host.players() {
        let system = host
            .cx
            .logic
            .content
            .state
            .players
            .get(&player)
            .and_then(|s| s.system)
            .unwrap_or(0);
        if system == system_id(host, key) {
            update_player_objectives(host, key, player);
        }
    }
    if let Some(v) = vig_mut(host, key) {
        v.objectives_changed = false;
    }
}

/// `UpdatePlayerObjectives` (slot 18, `:1314-1406`) as far as it changes
/// the server: the player's mission record, then, for a player in this
/// machine's system, the path objectives' `rand()` unless the ship travels
/// a lane. The objective list itself and its send are P3.5c5's (D1021).
fn update_player_objectives(host: &mut Host<'_, '_>, key: u32, player: u32) {
    if !super::mission::get_or_create_record(host, player) {
        return;
    }
    let Some(state) = host.cx.logic.content.state.players.get(&player) else {
        return;
    };
    if state.system.unwrap_or(0) != system_id(host, key) {
        return;
    }
    let on_lane = state.ship.as_ref().is_some_and(|s| s.lane_entry != 0);
    if !on_lane {
        update_path_objectives(host, key, player);
    }
}

/// `UpdatePathObjectives` (slot 19, `:1409-1475`): with a first objective
/// that has a position and a player away from the origin, the path's text
/// id is drawn (`rand() * 3 >> 15` over the machine type's three).
fn update_path_objectives(host: &mut Host<'_, '_>, key: u32, player: u32) {
    let Some(first) = vig(host, key).and_then(|v| v.active.first().cloned()) else {
        return;
    };
    if objective_position(host, key, &first).is_none() {
        return;
    }
    let at = player_position(host.cx, player);
    if !away_from_origin(at) {
        return;
    }
    let machine_type = offer(host, key).map_or(0, |o| o.machine_type);
    if (1..=3).contains(&machine_type) {
        let _ = host.cx.world.rng().rand();
    }
}

/// `SendPlaceholderObjectives` (slot 17): every mission player's record
/// (its placeholder objective is P3.5c5's).
fn send_placeholder_objectives(host: &mut Host<'_, '_>) {
    for player in host.players() {
        super::mission::get_or_create_record(host, player);
    }
}

// ---------------------------------------------------------------------------
// Conversations
// ---------------------------------------------------------------------------

/// `FRmOfferComponent::SendConversations` of the general component
/// (`:361-376`): each conversation of the trigger, prepared, goes to the
/// machine after its delay in a copy of its own.
fn send_conversations(host: &mut Host<'_, '_>, key: u32, trigger: i32, player: Option<u32>) {
    let Some((sender, list)) = offer(host, key).map(|o| {
        let list: Vec<(usize, f32)> = o
            .general
            .conversations
            .iter()
            .enumerate()
            .filter(|(_, c)| c.trigger == trigger)
            .map(|(i, c)| (i, c.delay))
            .collect();
        (o.general.handler, list)
    }) else {
        return;
    };
    for (index, delay) in list {
        let prepared = {
            let Some(o) = offer(host, key) else { return };
            let Some(c) = o.general.conversations.get(index) else {
                continue;
            };
            let mut c = c.clone();
            prepare_conversation(host, key, &mut c, player);
            c
        };
        if let Some(o) = offer_mut(host, key)
            && let Some(c) = o.general.conversations.get_mut(index)
        {
            // `MarkReady` is on the offer's own conversation.
            c.ready = true;
        }
        host.send_delayed(
            sender,
            RmMessage::new(
                MessageType::Conversation,
                Body::Conversation(Box::new(prepared)),
            ),
            delay,
            key,
        );
    }
}

/// `PrepareConversation` (`:383-411`): `MarkReady`; each line's speaker
/// and listeners are resolved by the machine (their playback is the comm
/// system's, P3.6b, D1022).
fn prepare_conversation(
    _host: &mut Host<'_, '_>,
    _key: u32,
    conversation: &mut Conversation,
    _player: Option<u32>,
) {
    conversation.ready = true;
}

/// `SendSystemConversations` (`:432-447`): a mapped player with a ship
/// hears `IN_SPACE_IN_SYSTEM` in this machine's system, else
/// `IN_SPACE_NOT_IN_SYSTEM`.
fn send_system_conversations(host: &mut Host<'_, '_>, key: u32, player: u32, system: u32) {
    if !host.is_mapped(player) {
        return;
    }
    let has_ship = host
        .cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .is_some_and(|s| s.ship.is_some());
    if !has_ship {
        return;
    }
    let trigger = if system == system_id(host, key) {
        comm::IN_SPACE_IN_SYSTEM
    } else {
        comm::IN_SPACE_NOT_IN_SYSTEM
    };
    send_conversations(host, key, trigger, Some(player));
}

/// The player whose ship is `ship` (`FindContentPlayerStateByShip`).
fn player_by_ship(host: &Host<'_, '_>, ship: u32) -> Option<u32> {
    players::find_by_ship(&host.cx.logic.content.state.players, ship)
}

// ---------------------------------------------------------------------------
// Slot 2
// ---------------------------------------------------------------------------

/// A trigger-fired message of `site`'s cookie: `(sender, last, activator)`.
fn fired(
    host: &mut Host<'_, '_>,
    key: u32,
    event: &Event<'_>,
    site: CookieSite,
) -> Option<(u32, bool, u32)> {
    let m = message_of(event)?;
    let current = host.state_of(key);
    if !m.is(MessageType::TriggerFired, current) {
        return None;
    }
    let Body::TriggerFired {
        cookie: c,
        last,
        activator,
    } = m.body
    else {
        return None;
    };
    (c == cookie(host.cx, site)).then_some((m.header.sender, last, activator))
}

/// The routed message of `kind` for the current state.
fn routed<'a>(
    host: &Host<'_, '_>,
    key: u32,
    event: &'a Event<'_>,
    kind: MessageType,
) -> Option<&'a RmMessage> {
    let m = message_of(event)?;
    m.is(kind, host.state_of(key)).then_some(m)
}

/// `if (Fired->bFinal) RemoveMachine(Fired->Sender)`.
fn retire(host: &mut Host<'_, '_>, key: u32, sender: u32, last: bool) {
    if last {
        remove_machine(host, key, sender);
    }
}

/// `FRmVignetteMachine::Slot2` (`RandomMissionVignette.cpp:504-545`).
pub(super) fn slot2(host: &mut Host<'_, '_>, key: u32, event: &Event<'_>, state: i32) -> bool {
    // The three entry statics, on the first call.
    cookie(host.cx, CookieSite::LeaveArea);
    cookie(host.cx, CookieSite::ReenterArea);
    cookie(host.cx, CookieSite::Countdown);
    match state {
        s if s < 0 => any_state(host, key, event),
        0 => {
            if is_enter(event) {
                host.set_state(key, 1);
            }
            false
        }
        1 => {
            let current = host.state_of(key);
            if message_of(event).is_some_and(|m| m.is(MessageType::StartMachine, current)) {
                host.set_state(key, 2);
            }
            false
        }
        2 => {
            if is_enter(event) {
                host.set_state(key, 3);
            }
            false
        }
        3 => start(host, key, event),
        8 => pre_battle(host, key, event),
        9 => target_battle(host, key, event),
        10 => approach_main(host, key, event),
        11 => main_battle(host, key, event),
        12 => critical_loot(host, key, event),
        5 => succeeded(host, key, event),
        6 => failed(host, key, event),
        7 => done(host, key, event),
        _ => false,
    }
}

/// The messages of every state (`HandleAnyState`, `:548-830`).
fn any_state(host: &mut Host<'_, '_>, key: u32, event: &Event<'_>) -> bool {
    if let Some(c) = controller_of(event) {
        match c.message {
            Controller::Heartbeat(_) => {
                refresh_objectives(host, key);
                return true;
            }
            Controller::SpaceEnter { system, player }
            | Controller::SystemEnter { system, player } => {
                send_system_conversations(host, key, player, system);
                return true;
            }
            Controller::GroupMemberJoined(player) => {
                if host.is_mapped(player) {
                    on_player_joined(host, key, player);
                }
                return true;
            }
            Controller::PlayerLoad(player) => {
                if host.is_mapped(player) {
                    set_player_attitudes(host, key, player);
                }
                return true;
            }
            Controller::Docked { ship, .. } => {
                if !api::misc::single_player(host.cx)
                    || !vig(host, key).is_some_and(|v| v.area_reached)
                {
                    return true;
                }
                let Some(player) = player_by_ship(host, ship) else {
                    return true;
                };
                if !host.is_mapped(player) {
                    return true;
                }
                if let Some(v) = vig_mut(host, key) {
                    v.fail_reason = 13;
                }
                host.set_state(key, 6);
                return true;
            }
            Controller::ObjectDestroyed {
                player, position, ..
            } => {
                if !host.is_mapped(player) {
                    return true;
                }
                let center = vig(host, key).map_or(Vector::ZERO, |v| v.position);
                let d = sub_vectors(position, center);
                let dsq = (f64::from(d.x) * f64::from(d.x) + f64::from(d.z) * f64::from(d.z))
                    + f64::from(d.y) * f64::from(d.y);
                if !(dsq < 100_000_000.0) {
                    return true;
                }
                if super::mission::decrement_player_count(host) > 0 {
                    return true;
                }
                send_conversations(host, key, comm::GLOBAL_FAIL_OTHER, Some(player));
                let reason = if api::misc::single_player(host.cx) {
                    2
                } else {
                    3
                };
                if let Some(v) = vig_mut(host, key) {
                    v.fail_reason = reason;
                }
                host.set_state(key, 6);
                return true;
            }
            _ => return false,
        }
    }
    let Some(m) = message_of(event) else {
        return false;
    };
    let current = host.state_of(key);
    if m.is(MessageType::Conversation, current) {
        // Valid conversations go to the comm system's manager (P3.6b,
        // D1022).
        return true;
    }
    if m.is(MessageType::PlayerLeft, current) {
        if let Body::PlayerLeft { player } = m.body
            && host.is_mapped(player)
        {
            unmark_objects(host, key, &[player]);
        }
        return true;
    }
    if m.is(MessageType::CriticalLootSpawned, current) {
        let Body::Loot { object, .. } = m.body else {
            return true;
        };
        critical_loot_spawned(host, key, object);
        return true;
    }
    if let Some((sender, last, activator)) = fired(host, key, event, CookieSite::LeaveArea) {
        retire(host, key, sender, last);
        let now = host.cx.logic.content.state.game_time as f32;
        if let Some(v) = vig_mut(host, key) {
            v.left_area_time = now;
        }
        let by = player_by_ship(host, activator);
        send_conversations(host, key, comm::LEFT_COMBAT_AREA, by);
        let delay = host.cx.logic.content.state.rm.runtime.multiplayer_delay;
        let system = offer_system(host, key);
        let position = vig(host, key).map_or(Vector::ZERO, |v| v.position);
        let str_id = offer_mut(host, key)
            .and_then(|o| {
                component_objective(o, ComponentKind::General, 9).map(|ob| ob.text.str_id)
            })
            .unwrap_or(0);
        let mut countdown = Countdown {
            str_id,
            objective_type: 0xda,
            marker_system: 0,
            marker_value: 0,
            marker_position: Vector::ZERO,
            seconds: 0,
        };
        set_countdown(
            &mut countdown,
            trunc_to_i32(f64::from(delay)),
            system,
            position,
        );
        insert_objective(host, key, 9, ObjectiveSource::Countdown(countdown), 0);
        let reenter = cookie(host.cx, CookieSite::ReenterArea);
        let radius = area_plus(host, key, -250.0);
        add_machine(host, key, reenter, proximity(system, position, radius));
        let countdown = cookie(host.cx, CookieSite::Countdown);
        add_machine(host, key, countdown, TriggerKind::Timer { delay });
        host.send_to_me_broadcast(key, RmMessage::plain(MessageType::CountdownTick), 5.0);
        return true;
    }
    if let Some((sender, last, activator)) = fired(host, key, event, CookieSite::ReenterArea) {
        retire(host, key, sender, last);
        let by = player_by_ship(host, activator);
        send_conversations(host, key, comm::REENTERED_COMBAT_AREA, by);
        send_placeholder_objectives(host);
        remove_objective(host, key, 9);
        let leave_cookie = cookie(host.cx, CookieSite::LeaveArea);
        let system = offer_system(host, key);
        let position = vig(host, key).map_or(Vector::ZERO, |v| v.position);
        let radius = area_plus(host, key, 250.0);
        add_state_machine(host, key, leave_cookie, leave(system, position, radius));
        if let Some(v) = vig_mut(host, key) {
            v.left_area_time = -1.0;
        }
        return true;
    }
    if let Some((sender, last, activator)) = fired(host, key, event, CookieSite::Countdown) {
        retire(host, key, sender, last);
        let left = vig(host, key).map_or(-1.0, |v| v.left_area_time);
        if left == -1.0 {
            return true;
        }
        let now = host.cx.logic.content.state.game_time;
        let elapsed = time_distance(f64::from(left), now);
        let delay = host.cx.logic.content.state.rm.runtime.multiplayer_delay;
        let limit = f64::from(delay) - 1.0;
        if limit > elapsed || limit.is_nan() || elapsed.is_nan() {
            return true;
        }
        let by = player_by_ship(host, activator);
        send_conversations(host, key, comm::GLOBAL_FAIL_COMBAT_RANGE, by);
        let reason = if api::misc::single_player(host.cx) {
            0xb
        } else {
            0xc
        };
        if let Some(v) = vig_mut(host, key) {
            v.fail_reason = reason;
            v.left_area_time = -1.0;
        }
        host.set_state(key, 6);
        return true;
    }
    if m.is(MessageType::CountdownTick, current) {
        let left = vig(host, key).map_or(-1.0, |v| v.left_area_time);
        if left == -1.0 {
            return true;
        }
        let now = host.cx.logic.content.state.game_time;
        let elapsed = trunc_to_i32(time_distance(f64::from(left), now));
        // The C++ reads the countdown without an end test: the tick runs
        // only while it is active.
        let Some(ActiveObjective {
            source: ObjectiveSource::Countdown(mut countdown),
            ..
        }) = remove_objective(host, key, 9)
        else {
            return true;
        };
        let delay = host.cx.logic.content.state.rm.runtime.multiplayer_delay;
        let remaining = f64::from(delay) - f64::from(elapsed);
        if !(remaining > 0.0) {
            return true;
        }
        let system = offer_system(host, key);
        let position = vig(host, key).map_or(Vector::ZERO, |v| v.position);
        set_countdown(&mut countdown, trunc_to_i32(remaining), system, position);
        insert_objective(host, key, 9, ObjectiveSource::Countdown(countdown), 0);
        host.send_to_me_broadcast(key, RmMessage::plain(MessageType::CountdownTick), 5.0);
        return true;
    }
    if m.is(MessageType::ShipDestroyed, current) {
        if let Body::Ship { object, .. } = m.body {
            let ships = vig(host, key).map_or(0, |v| v.ships);
            let record = sub::records(host, ships)
                .iter()
                .find(|r| object != 0 && r.object == object)
                .cloned();
            if let Some(record) = record {
                spawn_record_loot(host, key, &record);
            }
        }
        return true;
    }
    false
}

/// `SubtractVectors`: each difference stored as a float.
fn sub_vectors(a: Vector, b: Vector) -> Vector {
    Vector::new(
        (f64::from(a.x) - f64::from(b.x)) as f32,
        (f64::from(a.y) - f64::from(b.y)) as f32,
        (f64::from(a.z) - f64::from(b.z)) as f32,
    )
}

/// `FRmObjectiveType4::SetCountdown`: the marker only for a position away
/// from the origin; the seconds always.
fn set_countdown(c: &mut Countdown, seconds: i32, system: u32, at: Vector) {
    if away_from_origin(at) {
        c.marker_system = system;
        c.marker_value = 0x44d;
        c.marker_position = at;
    }
    c.seconds = seconds;
}

/// The "any state" handler's critical loot spawn (`:651-692`).
fn critical_loot_spawned(host: &mut Host<'_, '_>, key: u32, object: u32) {
    let loot = vig(host, key).map_or(0, |v| v.loot);
    let Some(record) = sub::records(host, loot)
        .iter()
        .find(|r| object != 0 && r.object == object)
        .cloned()
    else {
        return;
    };
    if record.loot_kind != 2 {
        return;
    }
    clear_ship_targets(host, key);
    clear_solar_targets(host, key);
    host.set_state(key, 12);
    let objective = match record.mode {
        1 => 0xa,
        2 | 3 => 0xb,
        _ => return,
    };
    let at = api::space_obj::location(host.cx, record.object).map_or(Vector::ZERO, |(p, _)| p);
    let system = api::space_obj::system(host.cx, record.object).unwrap_or(0);
    if let Some(i) = find_objective(host, key, objective) {
        let critical = sub_query_loot(host, loot, record.mode, 2, 0, 2, 0);
        let (component, map_key) = match vig(host, key).map(|v| v.active[i].source.clone()) {
            Some(ObjectiveSource::Offer(c, k)) => (c, k),
            _ => return,
        };
        set_target_records(host, key, component, map_key, loot, &critical, system, at);
        rebuild_objective(host, key, objective);
    } else {
        let index = sub::records(host, loot)
            .iter()
            .position(|r| r.object == record.object);
        let indices: Vec<usize> = index.into_iter().collect();
        set_target_records(
            host,
            key,
            ComponentKind::General,
            objective,
            loot,
            &indices,
            system,
            at,
        );
        add_objective(
            host,
            key,
            objective,
            ObjectiveSource::Offer(ComponentKind::General, objective),
        );
    }
}

fn sub_query_loot(
    host: &Host<'_, '_>,
    loot: u32,
    mode: i32,
    kind: i32,
    parent: u32,
    state: i32,
    holder: u32,
) -> Vec<usize> {
    match host.machine_ref(loot).map(|m| &m.kind) {
        Some(MachineKind::Loot(s)) => s.query_loot(mode, kind, parent, state, holder),
        _ => Vec::new(),
    }
}

fn sub_query_ships(
    host: &Host<'_, '_>,
    ships: u32,
    kind: i32,
    state: i32,
    mask: u32,
) -> Vec<usize> {
    match host.machine_ref(ships).map(|m| &m.kind) {
        Some(MachineKind::Ships(s)) => s.query_ships(kind, state, mask),
        _ => Vec::new(),
    }
}

fn sub_query_solars(
    host: &Host<'_, '_>,
    solars: u32,
    kind: i32,
    state: i32,
    mask: u32,
) -> Vec<usize> {
    match host.machine_ref(solars).map(|m| &m.kind) {
        Some(MachineKind::Solars(s)) => s.query_solars(kind, state, mask),
        _ => Vec::new(),
    }
}

/// State 3 (`HandleStart`, `:834-866`).
fn start(host: &mut Host<'_, '_>, key: u32, event: &Event<'_>) -> bool {
    if !is_enter(event) {
        return false;
    }
    let mission = host.rm().map_or(0, |r| r.handler);
    host.send_immediate(key, RmMessage::plain(MessageType::MachineStarted), mission);
    let Some((target_active, target_pos, center, system)) = offer(host, key).map(|o| {
        (
            o.target_ships.component.active,
            o.target_ships.position,
            main_center(o),
            o.system,
        )
    }) else {
        return true;
    };
    if target_active {
        set_marker(host, key, ComponentKind::General, 1, system, target_pos);
        add_objective(
            host,
            key,
            1,
            ObjectiveSource::Offer(ComponentKind::General, 1),
        );
        host.set_state(key, 8);
    } else {
        set_marker(host, key, ComponentKind::General, 2, system, center);
        add_objective(
            host,
            key,
            2,
            ObjectiveSource::Offer(ComponentKind::General, 2),
        );
        host.set_state(key, 10);
    }
    if let Some(v) = vig_mut(host, key) {
        v.area_radius = 3500.0;
    }
    for player in host.players() {
        let Some(s) = host.cx.logic.content.state.players.get(&player) else {
            continue;
        };
        if s.ship.is_none() {
            continue;
        }
        let system = s.system.unwrap_or(0);
        send_system_conversations(host, key, player, system);
    }
    true
}

/// State 8 (`HandlePreBattle`, `:869-916`).
fn pre_battle(host: &mut Host<'_, '_>, key: u32, event: &Event<'_>) -> bool {
    let far = cookie(host.cx, CookieSite::TargetFar);
    let near = cookie(host.cx, CookieSite::TargetNear);
    let (system, target) =
        offer(host, key).map_or((0, Vector::ZERO), |o| (o.system, o.target_ships.position));
    if is_enter(event) {
        if let Some(v) = vig_mut(host, key) {
            v.position = target;
        }
        add_state_machine(host, key, near, proximity(system, target, 2500.0));
        add_state_machine(host, key, far, proximity(system, target, 10000.0));
        return true;
    }
    if let Some((sender, last, _)) = fired(host, key, event, CookieSite::TargetFar) {
        retire(host, key, sender, last);
        population::add_exclusion_sphere(host.cx, [target.x, target.y, target.z, 10000.0]);
        if let Some(v) = vig_mut(host, key) {
            v.area_reached = true;
        }
        return true;
    }
    if let Some((sender, last, activator)) = fired(host, key, event, CookieSite::TargetNear) {
        retire(host, key, sender, last);
        let by = player_by_ship(host, activator);
        send_conversations(host, key, comm::PRE_BATTLE_SPAWN_RANGE, by);
        remove_objective(host, key, 1);
        host.set_state(key, 9);
        return true;
    }
    false
}

/// State 9 (`HandleTargetBattle`, `:919-1101`).
fn target_battle(host: &mut Host<'_, '_>, key: u32, event: &Event<'_>) -> bool {
    let next_group = cookie(host.cx, CookieSite::TargetNextGroup);
    let target_done = cookie(host.cx, CookieSite::TargetDone);
    let leave_cookie = cookie(host.cx, CookieSite::LeaveArea);
    if is_enter(event) {
        let Some((system, target, groups, mode, value)) = offer(host, key).map(|o| {
            (
                o.system,
                o.target_ships.position,
                o.target_ships
                    .groups
                    .iter()
                    .map(|g| (g.serial, g.field_0x04, g.field_0x08))
                    .collect::<Vec<_>>(),
                o.target_ships.field_0x50,
                o.target_ships.field_0x54,
            )
        }) else {
            return true;
        };
        let radius = area_plus(host, key, 250.0);
        add_state_machine(host, key, leave_cookie, leave(system, target, radius));
        if let Some(&(first, _, _)) = groups.first() {
            spawn_group_with_triggers(host, key, first, true, next_group);
            for &(serial, field_0x04, field_0x08) in &groups[1..] {
                if field_0x04 != 1 {
                    continue;
                }
                host.send_to_me_self(
                    key,
                    RmMessage::new(MessageType::Cookie, Body::Cookie { cookie: serial }),
                    field_0x08 as f32,
                );
            }
        }
        let ships = vig(host, key).map_or(0, |v| v.ships);
        match mode {
            1 => {
                add_state_machine(
                    host,
                    key,
                    target_done,
                    TriggerKind::Timer {
                        delay: value as f32,
                    },
                );
            }
            2 => {
                add_state_machine(
                    host,
                    key,
                    target_done,
                    TriggerKind::ShipCount {
                        ships,
                        threshold: value,
                    },
                );
            }
            3 => {
                add_state_machine(
                    host,
                    key,
                    target_done,
                    TriggerKind::ShipHealth {
                        ships,
                        threshold: value,
                        kind: 2,
                    },
                );
            }
            _ => {}
        }
        return true;
    }
    if let Some((sender, last, _)) = fired(host, key, event, CookieSite::TargetDone) {
        retire(host, key, sender, last);
        if offer(host, key).is_some_and(|o| has_unspawned(o, true)) {
            return true;
        }
        let ships = vig(host, key).map_or(0, |v| v.ships);
        let none_alive = sub_query_ships(host, ships, 0, 2, 0).is_empty();
        if none_alive {
            if !offer(host, key).is_some_and(|o| has_unspawned(o, false)) {
                host.set_state(key, 5);
                return true;
            }
            send_conversations(host, key, comm::KILLED_ALL_GOING_TO_MAIN, None);
        } else {
            send_conversations(host, key, comm::NOT_KILLED_ALL_GOING_TO_MAIN, None);
        }
        let (system, center) =
            offer(host, key).map_or((0, Vector::ZERO), |o| (o.system, main_center(o)));
        set_marker(host, key, ComponentKind::TargetShips, 3, system, center);
        insert_objective(
            host,
            key,
            2,
            ObjectiveSource::Offer(ComponentKind::TargetShips, 3),
            0,
        );
        host.set_state(key, 10);
        return true;
    }
    if let Some((sender, last, activator)) = fired(host, key, event, CookieSite::TargetNextGroup) {
        retire(host, key, sender, last);
        if !offer(host, key).is_some_and(|o| has_unspawned(o, true))
            && ![7, 4, 6, 5].iter().any(|&k| has_objective(host, key, k))
        {
            return true;
        }
        spawn_group_with_triggers(host, key, activator, true, next_group);
        return true;
    }
    if let Some(m) = routed(host, key, event, MessageType::Cookie) {
        if let Body::Cookie { cookie: serial } = m.body {
            spawn_group_with_triggers(host, key, serial, true, next_group);
        }
        return true;
    }
    if routed(host, key, event, MessageType::GroupDestroyed).is_some() {
        return true;
    }
    if let Some(m) = routed(host, key, event, MessageType::FlaggedShipsLost) {
        let object = match m.body {
            Body::Ship { object, .. } => object,
            _ => 0,
        };
        on_flagged_ships_lost(host, key, object);
        if offer(host, key).is_some_and(|o| has_unspawned(o, true)) {
            return true;
        }
        let len = vig(host, key).map_or(0, |v| v.active.len());
        if len != 0 && (len != 1 || !has_objective(host, key, 9)) {
            return true;
        }
        if len == 1 {
            remove_objective(host, key, 9);
            if let Some(v) = vig_mut(host, key) {
                v.left_area_time = -1.0;
            }
        }
        if !offer(host, key).is_some_and(|o| has_unspawned(o, false)) {
            host.set_state(key, 5);
            return true;
        }
        send_conversations(host, key, comm::KILLED_ALL_GOING_TO_MAIN, None);
        add_objective(
            host,
            key,
            2,
            ObjectiveSource::Offer(ComponentKind::TargetShips, 3),
        );
        host.set_state(key, 10);
        return true;
    }
    if is_exit(event) {
        let pending = host.machine_ref(key).map_or(0, |m| m.sm.pending_state());
        if pending != 10 {
            return true;
        }
        if let Some(v) = vig_mut(host, key) {
            v.area_radius = OUTER_RADIUS;
        }
        // Every alive ship leaves its formation for the main battle (its
        // directives come with the ships' spawn, D1023).
        return true;
    }
    false
}

/// State 10 (`HandleApproachMain`, `:1104-1273`).
fn approach_main(host: &mut Host<'_, '_>, key: u32, event: &Event<'_>) -> bool {
    let main_ships = cookie(host.cx, CookieSite::MainShips);
    let main_far = cookie(host.cx, CookieSite::MainFar);
    let main_near = cookie(host.cx, CookieSite::MainNear);
    let leave_cookie = cookie(host.cx, CookieSite::LeaveArea);
    let (system, center, target_active) = offer(host, key).map_or((0, Vector::ZERO, false), |o| {
        (o.system, main_center(o), o.target_ships.component.active)
    });
    if is_enter(event) {
        let old = vig(host, key).map_or(Vector::ZERO, |v| v.position);
        let count = vig(host, key).map_or(0, |v| v.active.len());
        for i in 0..count {
            let Some(a) = vig(host, key).and_then(|v| v.active.get(i).cloned()) else {
                break;
            };
            let Some(at) = objective_position(host, key, &a) else {
                continue;
            };
            let close = |p: f32, q: f32| {
                let d = f64::from(p) - f64::from(q);
                d * d <= 1.0
            };
            if !(close(at.x, old.x) && close(at.y, old.y) && close(at.z, old.z)) {
                continue;
            }
            match a.source {
                ObjectiveSource::Countdown(_) => {
                    if let Some(v) = vig_mut(host, key)
                        && let Some(ObjectiveSource::Countdown(c)) =
                            v.active.get_mut(i).map(|a| &mut a.source)
                    {
                        c.marker_position = center;
                    }
                }
                ObjectiveSource::Offer(component, map_key) => {
                    if let Some(o) = offer_mut(host, key)
                        && let Some(ob) = component_objective(o, component, map_key)
                    {
                        ob.marker_position = center;
                    }
                }
            }
        }
        if let Some(v) = vig_mut(host, key) {
            v.position = center;
        }
        if target_active {
            let radius = area_plus(host, key, 250.0);
            add_state_machine(host, key, leave_cookie, leave(system, center, radius));
        }
        add_state_machine(host, key, main_ships, proximity(system, center, 7500.0));
        add_state_machine(host, key, main_far, proximity(system, center, 10000.0));
        return true;
    }
    if let Some(m) = routed(host, key, event, MessageType::FlaggedShipsLost) {
        let object = match m.body {
            Body::Ship { object, .. } => object,
            _ => 0,
        };
        on_flagged_ships_lost(host, key, object);
        if offer(host, key).is_some_and(|o| has_unspawned(o, false)) {
            return true;
        }
        if [7, 4, 6, 5, 0xa, 0xb, 0xc, 0xd]
            .iter()
            .any(|&k| has_objective(host, key, k))
        {
            return true;
        }
        host.set_state(key, 5);
        return true;
    }
    if let Some(m) = routed(host, key, event, MessageType::SolarDestroyed) {
        let object = match m.body {
            Body::Solar { object } => object,
            _ => 0,
        };
        let solars = vig(host, key).map_or(0, |v| v.solars);
        let Some(solar_type) = sub::records(host, solars)
            .iter()
            .find(|r| object != 0 && r.object == object)
            .map(|r| r.solar_type)
        else {
            return true;
        };
        if !sub_query_solars(host, solars, solar_type, 2, 2).is_empty() {
            return true;
        }
        if solar_type == 0 {
            remove_objective(host, key, 4);
        } else if solar_type == 1 {
            remove_objective(host, key, 5);
        }
        if !sub_query_solars(host, solars, -1, 2, 2).is_empty() {
            return true;
        }
        if vig(host, key).is_some_and(|v| v.active.len() == 1) && has_objective(host, key, 8) {
            remove_objective(host, key, 8);
        }
        if offer(host, key).is_some_and(|o| has_unspawned(o, false)) {
            return true;
        }
        if [0xa, 0xb, 0xc, 0xd, 7, 4, 6, 5]
            .iter()
            .any(|&k| has_objective(host, key, k))
        {
            return true;
        }
        if has_objective(host, key, 9) {
            remove_objective(host, key, 9);
            if let Some(v) = vig_mut(host, key) {
                v.left_area_time = -1.0;
            }
        }
        host.set_state(key, 5);
        return true;
    }
    if let Some((sender, last, activator)) = fired(host, key, event, CookieSite::MainShips) {
        retire(host, key, sender, last);
        let by = player_by_ship(host, activator);
        send_conversations(host, key, comm::MAIN_BATTLE_SOLAR_SPAWN_RANGE, by);
        let count = offer(host, key).map_or(0, |o| o.solars.solars.len());
        for i in 0..count {
            spawn_solar(host, key, i);
        }
        add_state_machine(host, key, main_near, proximity(system, center, 2500.0));
        return true;
    }
    if let Some((sender, last, _)) = fired(host, key, event, CookieSite::MainFar) {
        retire(host, key, sender, last);
        population::add_exclusion_sphere(host.cx, [center.x, center.y, center.z, 10000.0]);
        if let Some(v) = vig_mut(host, key) {
            v.area_reached = true;
        }
        return true;
    }
    if let Some((sender, last, activator)) = fired(host, key, event, CookieSite::MainNear) {
        retire(host, key, sender, last);
        let by = player_by_ship(host, activator);
        send_conversations(host, key, comm::MAIN_BATTLE_SHIP_SPAWN_RANGE, by);
        host.set_state(key, 11);
        return true;
    }
    if is_exit(event) {
        remove_objective(host, key, 2);
        return true;
    }
    false
}

/// State 11's common end: with no objective or only the countdown, the
/// countdown goes and state 5.
fn main_tail(host: &mut Host<'_, '_>, key: u32) -> bool {
    let len = vig(host, key).map_or(0, |v| v.active.len());
    if len != 0 && (len != 1 || !has_objective(host, key, 9)) {
        return true;
    }
    if len == 1 {
        remove_objective(host, key, 9);
        if let Some(v) = vig_mut(host, key) {
            v.left_area_time = -1.0;
        }
    }
    host.set_state(key, 5);
    true
}

/// State 11 (`HandleMainBattle`, `:1276-1403`).
fn main_battle(host: &mut Host<'_, '_>, key: u32, event: &Event<'_>) -> bool {
    let next_group = cookie(host.cx, CookieSite::MainNextGroup);
    let leave_cookie = cookie(host.cx, CookieSite::LeaveArea);
    if is_enter(event) {
        if let Some(v) = vig_mut(host, key) {
            v.area_radius = 3500.0;
        }
        let (system, center, groups) =
            offer(host, key).map_or((0, Vector::ZERO, Vec::new()), |o| {
                (
                    o.system,
                    main_center(o),
                    o.ships
                        .groups
                        .iter()
                        .map(|g| (g.serial, g.field_0x04, g.field_0x08))
                        .collect::<Vec<_>>(),
                )
            });
        let position = vig(host, key).map_or(Vector::ZERO, |v| v.position);
        let solars = vig(host, key).map_or(0, |v| v.solars);
        let others = sub_query_solars(host, solars, 1, 2, 2);
        if !others.is_empty() {
            set_target_records(
                host,
                key,
                ComponentKind::Solars,
                5,
                solars,
                &others,
                system,
                position,
            );
            add_objective(
                host,
                key,
                5,
                ObjectiveSource::Offer(ComponentKind::Solars, 5),
            );
        }
        let big = sub_query_solars(host, solars, 0, 2, 2);
        if !big.is_empty() {
            set_target_records(
                host,
                key,
                ComponentKind::Solars,
                4,
                solars,
                &big,
                system,
                position,
            );
            add_objective(
                host,
                key,
                4,
                ObjectiveSource::Offer(ComponentKind::Solars, 4),
            );
        }
        if let Some(&(first, first_mode, _)) = groups.first() {
            if first_mode != 1 {
                spawn_group_with_triggers(host, key, first, false, next_group);
            }
            for &(serial, field_0x04, field_0x08) in &groups {
                if field_0x04 != 1 {
                    continue;
                }
                host.send_to_me_self(
                    key,
                    RmMessage::new(MessageType::Cookie, Body::Cookie { cookie: serial }),
                    field_0x08 as f32,
                );
            }
        }
        let radius = area_plus(host, key, 250.0);
        add_state_machine(host, key, leave_cookie, leave(system, center, radius));
        return true;
    }
    if let Some((sender, last, activator)) = fired(host, key, event, CookieSite::MainNextGroup) {
        retire(host, key, sender, last);
        if !offer(host, key).is_some_and(|o| has_unspawned(o, false))
            && !has_target_objectives(host, key)
        {
            return true;
        }
        spawn_group_with_triggers(host, key, activator, false, next_group);
        return true;
    }
    if let Some(m) = routed(host, key, event, MessageType::Cookie) {
        if let Body::Cookie { cookie: serial } = m.body {
            spawn_group_with_triggers(host, key, serial, false, next_group);
        }
        return true;
    }
    if let Some(m) = routed(host, key, event, MessageType::FlaggedShipsLost) {
        let object = match m.body {
            Body::Ship { object, .. } => object,
            _ => 0,
        };
        on_flagged_ships_lost(host, key, object);
        if offer(host, key).is_some_and(|o| has_unspawned(o, false)) {
            return true;
        }
        return main_tail(host, key);
    }
    if let Some(m) = routed(host, key, event, MessageType::SolarDestroyed) {
        let object = match m.body {
            Body::Solar { object } => object,
            _ => 0,
        };
        let solars = vig(host, key).map_or(0, |v| v.solars);
        let Some(solar_type) = sub::records(host, solars)
            .iter()
            .find(|r| object != 0 && r.object == object)
            .map(|r| r.solar_type)
        else {
            return true;
        };
        if !sub_query_solars(host, solars, solar_type, 2, 2).is_empty() {
            return true;
        }
        if solar_type == 0 {
            remove_objective(host, key, 4);
        } else if solar_type == 1 {
            remove_objective(host, key, 5);
        }
        if vig(host, key).is_some_and(|v| v.active.len() == 1) && has_objective(host, key, 8) {
            remove_objective(host, key, 8);
        }
        if offer(host, key).is_some_and(|o| has_unspawned(o, false)) {
            return true;
        }
        return main_tail(host, key);
    }
    false
}

/// The drop of a held critical loot next to its holder's ship.
fn drop_held(host: &mut Host<'_, '_>, key: u32, object: u32, holder: u32) {
    let ship = host
        .cx
        .logic
        .content
        .state
        .players
        .get(&holder)
        .map_or(0, players::PlayerState::ship_id);
    let loot = vig(host, key).map_or(0, |v| v.loot);
    host.send_immediate(
        key,
        RmMessage::new(
            MessageType::LootDrop,
            Body::Loot {
                object,
                value: ship,
            },
        ),
        loot,
    );
}

/// State 12 (`HandleCriticalLoot`, `:1406-1633`).
fn critical_loot(host: &mut Host<'_, '_>, key: u32, event: &Event<'_>) -> bool {
    if is_enter(event) {
        return true;
    }
    if matches!(
        controller_of(event).map(|c| c.message),
        Some(Controller::Docked { .. })
    ) {
        return true;
    }
    let loot = vig(host, key).map_or(0, |v| v.loot);
    if let Some(m) = routed(host, key, event, MessageType::PlayerLeft) {
        let Body::PlayerLeft { player } = m.body else {
            return false;
        };
        if !host.is_mapped(player) {
            return false;
        }
        let has_ship = host
            .cx
            .logic
            .content
            .state
            .players
            .get(&player)
            .is_some_and(|s| s.ship.is_some());
        if !has_ship {
            return false;
        }
        let held = sub_query_loot(host, loot, 0, 2, 0, 4, player);
        if let Some(&i) = held.first()
            && let Some(r) = sub::records(host, loot).get(i).cloned()
        {
            drop_held(host, key, r.object, r.holder);
        }
        return false;
    }
    let record_of = |host: &Host<'_, '_>, object: u32| {
        sub::records(host, loot)
            .iter()
            .find(|r| object != 0 && r.object == object)
            .cloned()
    };
    if let Some(m) = routed(host, key, event, MessageType::LootPickedUp) {
        let Body::Loot {
            object,
            value: ship,
        } = m.body
        else {
            return true;
        };
        let Some(record) = record_of(host, object) else {
            return true;
        };
        match record.mode {
            1 => {
                drop_held(host, key, record.object, record.holder);
                return true;
            }
            2 | 3 => {}
            _ => return true,
        }
        let picker = player_by_ship(host, ship);
        if !picker.is_some_and(|p| host.is_mapped(p)) {
            drop_held(host, key, record.object, record.holder);
            return true;
        }
        remove_objective(host, key, 0xb);
        if record.mode == 2 {
            host.set_state(key, 5);
            return true;
        }
        // The dock of the offer base (`FContentBase::FindDockObject` near
        // the holder's ship).
        let base = offer(host, key).map_or(0, |o| o.offer_base);
        let dock = super::super::mission::find_dock_object_id(host.cx, base);
        let (where_, _) = api::space_obj::location(host.cx, dock)
            .unwrap_or((Vector::ZERO, neuron_core::math::Matrix::IDENTITY));
        let base_system = api::misc::base_system(host.cx, base).unwrap_or(0);
        set_targets(
            host,
            key,
            ComponentKind::General,
            0xc,
            vec![dock],
            base_system,
            where_,
        );
        add_objective(
            host,
            key,
            0xc,
            ObjectiveSource::Offer(ComponentKind::General, 0xc),
        );
        return true;
    }
    if let Some(m) = routed(host, key, event, MessageType::LootRespawned) {
        let Body::Loot { object, .. } = m.body else {
            return true;
        };
        let Some(record) = record_of(host, object) else {
            return true;
        };
        let at = api::space_obj::location(host.cx, record.object).map_or(Vector::ZERO, |(p, _)| p);
        let system = api::space_obj::system(host.cx, record.object).unwrap_or(0);
        if record.mode == 3 {
            remove_objective(host, key, 0xc);
            remove_objective(host, key, 0xb);
            let index: Vec<usize> = sub::records(host, loot)
                .iter()
                .position(|r| r.object == record.object)
                .into_iter()
                .collect();
            set_target_records(
                host,
                key,
                ComponentKind::General,
                0xb,
                loot,
                &index,
                system,
                at,
            );
            add_objective(
                host,
                key,
                0xb,
                ObjectiveSource::Offer(ComponentKind::General, 0xb),
            );
            return true;
        }
        if record.mode != 1 && record.mode != 2 {
            return true;
        }
        let objective = if record.mode == 1 { 0xa } else { 0xb };
        let critical = sub_query_loot(host, loot, record.mode, 2, 0, 2, 0);
        let Some(a) = find_objective(host, key, objective)
            .and_then(|i| vig(host, key).map(|v| v.active[i].clone()))
        else {
            return true;
        };
        if let ObjectiveSource::Offer(component, map_key) = a.source {
            set_target_records(host, key, component, map_key, loot, &critical, system, at);
        }
        rebuild_objective(host, key, a.key);
        return true;
    }
    if let Some(m) = routed(host, key, event, MessageType::LootDestroyedByPlayer) {
        let Body::Loot {
            object,
            value: ship,
        } = m.body
        else {
            return true;
        };
        let Some(record) = record_of(host, object) else {
            return true;
        };
        match record.mode {
            1 => {
                remove_objective(host, key, 0xa);
                host.set_state(key, 5);
            }
            2 | 3 => {
                remove_objective(host, key, 0xb);
                let by = player_by_ship(host, ship);
                send_conversations(host, key, comm::GLOBAL_FAIL_DESTROYED_CRITICAL_LOOT, by);
                if let Some(v) = vig_mut(host, key) {
                    v.fail_reason = if record.mode == 2 { 4 } else { 5 };
                }
                host.set_state(key, 6);
            }
            _ => {}
        }
        return true;
    }
    if let Some(m) = routed(host, key, event, MessageType::LootDestroyed) {
        let Body::Loot { object, .. } = m.body else {
            return true;
        };
        let mode = record_of(host, object).map_or(0, |r| r.mode);
        if let Some(v) = vig_mut(host, key) {
            match mode {
                1 => v.fail_reason = 6,
                2 => v.fail_reason = 7,
                3 => v.fail_reason = 8,
                _ => {}
            }
        }
        host.set_state(key, 6);
        return true;
    }
    if let Some(Controller::ObjectDestroyed { player, .. }) =
        controller_of(event).map(|c| c.message)
    {
        if !host.is_mapped(player) {
            return false;
        }
        let all = sub_query_loot(host, loot, 0, 2, 0, 0, 0);
        if let Some(&i) = all.first()
            && let Some(r) = sub::records(host, loot).get(i)
            && r.state == 4
            && r.holder == player
        {
            if let Some(v) = vig_mut(host, key) {
                v.fail_reason = 9;
            }
            host.set_state(key, 6);
        }
        return false;
    }
    if let Some(m) = routed(host, key, event, MessageType::LootHolderDocked) {
        let Body::LootPlayer { object, base, .. } = m.body else {
            return true;
        };
        let Some(record) = record_of(host, object) else {
            return true;
        };
        if record.mode != 3 {
            return true;
        }
        if offer(host, key).is_some_and(|o| o.offer_base == base) {
            remove_objective(host, key, 0xc);
            host.set_state(key, 5);
            return true;
        }
        if let Some(v) = vig_mut(host, key) {
            v.fail_reason = 10;
        }
        host.set_state(key, 6);
        return true;
    }
    false
}

/// State 5 (`HandleSucceeded`, `:1637-1664`).
fn succeeded(host: &mut Host<'_, '_>, key: u32, event: &Event<'_>) -> bool {
    if is_enter(event) {
        send_conversations(host, key, comm::GLOBAL_SUCCESS, None);
        host.send_to_me_self(key, RmMessage::plain(MessageType::Timeout), 5.0);
        return true;
    }
    if routed(host, key, event, MessageType::Timeout).is_some() {
        on_succeeded(host, key);
        let mission = host.rm().map_or(0, |r| r.handler);
        host.send_delayed(
            key,
            RmMessage::plain(MessageType::MachineSucceeded),
            0.001,
            mission,
        );
        host.set_state(key, 7);
        return true;
    }
    if let Some((sender, last, _)) = fired(host, key, event, CookieSite::ReenterArea) {
        retire(host, key, sender, last);
        return true;
    }
    false
}

/// State 6 (`HandleFailed`, `:1668-1693`).
fn failed(host: &mut Host<'_, '_>, key: u32, event: &Event<'_>) -> bool {
    if is_enter(event) {
        host.send_to_me_self(key, RmMessage::plain(MessageType::Timeout), 5.0);
        return true;
    }
    if routed(host, key, event, MessageType::Timeout).is_some() {
        let reason = vig(host, key).map_or(0, |v| v.fail_reason);
        let mission = host.rm().map_or(0, |r| r.handler);
        host.send_delayed(
            key,
            RmMessage::new(MessageType::MachineFailed, Body::MachineFailed { reason }),
            0.001,
            mission,
        );
        host.set_state(key, 7);
        return true;
    }
    if let Some((sender, last, _)) = fired(host, key, event, CookieSite::ReenterArea) {
        retire(host, key, sender, last);
        return true;
    }
    false
}

/// State 7 (`HandleDone`, `:1696-1713`).
fn done(host: &mut Host<'_, '_>, key: u32, event: &Event<'_>) -> bool {
    if is_enter(event) {
        shutdown(host, key);
        return true;
    }
    if let Some((sender, last, _)) = fired(host, key, event, CookieSite::ReenterArea) {
        retire(host, key, sender, last);
        return true;
    }
    false
}

// ---------------------------------------------------------------------------
// Helpers of the states
// ---------------------------------------------------------------------------

/// `ClearShipTargets` (`:286-301`).
fn clear_ship_targets(host: &mut Host<'_, '_>, key: u32) {
    remove_objective(host, key, 6);
    remove_objective(host, key, 7);
    let ships = vig(host, key).map_or(0, |v| v.ships);
    clear_targets(host, ships, sub_query_ships(host, ships, 0, 2, 2));
}

/// `ClearSolarTargets` (`:304-319`).
fn clear_solar_targets(host: &mut Host<'_, '_>, key: u32) {
    remove_objective(host, key, 4);
    remove_objective(host, key, 5);
    let solars = vig(host, key).map_or(0, |v| v.solars);
    clear_targets(host, solars, sub_query_solars(host, solars, -1, 2, 2));
}

/// The flagged records lose their flags and their marks.
fn clear_targets(host: &mut Host<'_, '_>, machine: u32, indices: Vec<usize>) {
    for i in indices {
        let object = {
            let Some(s) = sub::sub_mut(host, machine) else {
                return;
            };
            let Some(r) = s.records.get_mut(i) else {
                continue;
            };
            r.flags = 0;
            r.object
        };
        for p in host.players() {
            let _ = api::player::mark_obj(host.cx, p, object, 0);
        }
    }
}

/// `HasTargetObjectives`: one of 7, 4, 6, 5 is active.
fn has_target_objectives(host: &Host<'_, '_>, key: u32) -> bool {
    [7, 4, 6, 5].iter().any(|&k| has_objective(host, key, k))
}

/// `OnFlaggedShipsLost` (`:403-418`): the last flagged ship of its kind
/// takes objective 7 (big) or 6 (defensive) with it.
fn on_flagged_ships_lost(host: &mut Host<'_, '_>, key: u32, object: u32) {
    let ships = vig(host, key).map_or(0, |v| v.ships);
    let Some(kind) = sub::records(host, ships)
        .iter()
        .find(|r| object != 0 && r.object == object)
        .map(|r| r.ship_kind)
    else {
        return;
    };
    if kind == 2 && sub_query_ships(host, ships, 2, 2, 2).is_empty() {
        remove_objective(host, key, 7);
    } else if kind == 1 && sub_query_ships(host, ships, 1, 2, 2).is_empty() {
        remove_objective(host, key, 6);
    }
}

/// `SpawnGroupWithTriggers(Serial, Component, Cookie)` (`:325-400`).
fn spawn_group_with_triggers(
    host: &mut Host<'_, '_>,
    key: u32,
    serial: u32,
    target: bool,
    group_cookie: u32,
) {
    let Some((index, spawned, clears)) = offer(host, key).and_then(|o| {
        let groups = &ship_component(o, target).groups;
        groups
            .iter()
            .position(|g| g.serial == serial)
            .map(|i| (i, groups[i].field_0x01 != 0, groups[i].field_0x02 != 0))
    }) else {
        return;
    };
    if spawned {
        return;
    }
    if clears {
        clear_ship_targets(host, key);
    }
    spawn_ship_group(host, key, target, index);
    let ships = vig(host, key).map_or(0, |v| v.ships);
    let members = match host.machine_ref(ships).map(|m| &m.kind) {
        Some(MachineKind::Ships(s)) => s.query_group(serial),
        _ => Vec::new(),
    };
    mark_group_targets(host, key, &members, target);
    let friendly =
        offer(host, key).is_some_and(|o| ship_component(o, target).groups[index].field_0x40 > 0.0);
    if friendly {
        if !vig(host, key).is_some_and(|v| v.friendly_sent) {
            send_conversations(host, key, comm::FIRST_FRIENDLY_SPAWN, None);
            if let Some(v) = vig_mut(host, key) {
                v.friendly_sent = true;
            }
        }
    } else if !vig(host, key).is_some_and(|v| v.big_sent && v.defensive_sent) {
        let records = sub::records(host, ships);
        let defensive = members
            .iter()
            .any(|&i| records.get(i).is_some_and(|r| r.ship_kind == 1));
        let big = members
            .iter()
            .any(|&i| records.get(i).is_some_and(|r| r.ship_kind == 2));
        let (big_sent, defensive_sent) =
            vig(host, key).map_or((true, true), |v| (v.big_sent, v.defensive_sent));
        if !big_sent && big {
            send_conversations(host, key, comm::FIRST_BIG_SHIP_SPAWN, None);
            if let Some(v) = vig_mut(host, key) {
                v.big_sent = true;
            }
        } else if !defensive_sent && defensive {
            send_conversations(host, key, comm::FIRST_DEFENSIVE_SHIP_SPAWN, None);
            if let Some(v) = vig_mut(host, key) {
                v.defensive_sent = true;
            }
        }
    }
    let Some((next_serial, next_mode, next_value)) = offer(host, key).and_then(|o| {
        ship_component(o, target).groups[index + 1..]
            .iter()
            .find(|g| g.field_0x01 == 0)
            .map(|g| (g.serial, g.field_0x04, g.field_0x08))
    }) else {
        return;
    };
    let all_dead = add_state_machine(
        host,
        key,
        group_cookie,
        TriggerKind::ShipCount {
            ships,
            threshold: 0,
        },
    );
    set_activator(host, all_dead, next_serial);
    if next_mode == 2 {
        host.send_to_me_self(
            key,
            RmMessage::new(
                MessageType::Cookie,
                Body::Cookie {
                    cookie: next_serial,
                },
            ),
            next_value as f32,
        );
    } else if next_mode == 3 {
        let few = add_state_machine(
            host,
            key,
            group_cookie,
            TriggerKind::ShipCount {
                ships,
                threshold: next_value,
            },
        );
        set_activator(host, few, next_serial);
    }
}

/// `SetActivator` on a trigger after it was added.
fn set_activator(host: &mut Host<'_, '_>, trigger: u32, activator: u32) {
    if let Some(Machine {
        kind: MachineKind::Trigger(t),
        ..
    }) = host.machine_mut(trigger)
    {
        t.activator = activator;
    }
}

/// `MarkGroupTargets(Members, Map)` (`:452-500`): the first flagged member
/// makes the target objective of its kind (6 defensive, 7 otherwise); a
/// hostile group without one gets the component's objective 8. The C++
/// reads the first member without an empty test; a group whose spawn made
/// no record has none here (D1023).
fn mark_group_targets(host: &mut Host<'_, '_>, key: u32, members: &[usize], target: bool) {
    let component = if target {
        ComponentKind::TargetShips
    } else {
        ComponentKind::Ships
    };
    let ships = vig(host, key).map_or(0, |v| v.ships);
    let records = sub::records(host, ships).to_vec();
    let mut marked = false;
    if let Some(flagged) = members
        .iter()
        .filter_map(|&i| records.get(i))
        .find(|r| r.flags & 2 != 0)
    {
        let objective = if flagged.ship_kind != 1 { 7 } else { 6 };
        let system = offer_system(host, key);
        let position = vig(host, key).map_or(Vector::ZERO, |v| v.position);
        if let Some(i) = find_objective(host, key, objective) {
            let same = sub_query_ships(host, ships, flagged.ship_kind, 2, 2);
            let a = vig(host, key).map(|v| v.active[i].clone());
            if let Some(ActiveObjective {
                key: k,
                source: ObjectiveSource::Offer(c, map_key),
            }) = a
            {
                set_target_records(host, key, c, map_key, ships, &same, system, position);
                rebuild_objective(host, key, k);
            }
        } else {
            set_target_records(
                host, key, component, objective, ships, members, system, position,
            );
            add_objective(
                host,
                key,
                objective,
                ObjectiveSource::Offer(component, objective),
            );
        }
        remove_objective(host, key, 8);
        marked = true;
    }
    let Some(first) = members.first().and_then(|&i| records.get(i)) else {
        return;
    };
    if !(first.attitude < 0.0) {
        return;
    }
    let has_8 = offer(host, key).is_some_and(|o| {
        ship_component(o, target)
            .component
            .objectives
            .contains_key(&8)
    });
    if !has_8 || marked || [8, 6, 7].iter().any(|&k| has_objective(host, key, k)) {
        return;
    }
    add_objective(host, key, 8, ObjectiveSource::Offer(component, 8));
}

/// `SpawnShipGroup` (slot 24, `:1724-1904`): the group is marked spawned.
/// Its ships, their NPC blocks, reputations, formation and records come
/// with the mission ships' spawn of P3.5c2 (D1023).
fn spawn_ship_group(host: &mut Host<'_, '_>, key: u32, target: bool, index: usize) {
    if let Some(o) = offer_mut(host, key) {
        let component = if target {
            &mut o.target_ships
        } else {
            &mut o.ships
        };
        if let Some(g) = component.groups.get_mut(index) {
            g.field_0x01 = 1;
        }
    }
}

/// `SpawnSolar` (slot 23, `:1574-1680`): the solar's reputation named by
/// its killable solar's string, its record, the solar made in the offer's
/// system with the `NOTHING` graph and its pilot, invincible by its mode
/// flag, its attitude towards the mission players; the record kept and the
/// solar listed as a mission object of its system.
fn spawn_solar(host: &mut Host<'_, '_>, key: u32, index: usize) -> bool {
    let Some((system_of_offer, solar)) =
        offer(host, key).map(|o| (o.system, o.solars.solars.get(index).cloned()))
    else {
        return false;
    };
    if system_of_offer == 0 {
        return false;
    }
    let Some(solar) = solar else { return false };
    let system = if host
        .cx
        .logic
        .content
        .state
        .systems
        .contains_key(&solar.dest_sys)
    {
        solar.dest_sys
    } else {
        0
    };
    let name = FmtStr::new((solar.arch_string_id.wrapping_add(0x507d0)) as u32);
    let vibe = api::reputation::alloc(host.cx, &name, &FmtStr::new(0)).unwrap_or(-1);
    let _ = api::reputation::set_affiliation(host.cx, vibe, solar.faction);
    let mut info =
        api::space_obj::SolarInfo::new(system, solar.archetype, &solar.nickname, solar.position);
    info.orientation = solar.orientation;
    info.hit_pts = solar.hitpoints;
    info.loadout = solar.loadout;
    info.vibe = vibe;
    let Ok(object) = api::space_obj::create_solar(host.cx, &info) else {
        return false;
    };
    let mut op = api::ai::SetPersonalityParams {
        state_graph: host
            .cx
            .world
            .data()
            .state_graphs
            .find(b"NOTHING", game_sim::ai::state_graph::GraphType::Leader),
        content_callback: api::ai::CallbackArg::Null,
        directive_callback: api::ai::CallbackArg::Null,
        ..api::ai::SetPersonalityParams::default()
    };
    if solar.has_pilot != 0 {
        op.set_personality = true;
        let tables = std::sync::Arc::clone(&host.cx.logic.content.tables);
        tables.pilots.personality(solar.pilot, &mut op.personality);
    }
    let _ = api::ai::submit_state(
        host.cx,
        object,
        &api::ai::BaseOp::new(api::ai::Op::SetPersonalityParams(Box::new(op))),
    );
    if solar.mode_flag & 1 != 0 {
        let _ = api::space_obj::set_invincible(
            host.cx,
            object,
            true,
            solar.field_0x4e0 != 0,
            solar.invincible_duration,
        );
    }
    if solar.field_0x4c8 != -2.0 {
        for p in host.players() {
            let rep = host
                .cx
                .logic
                .content
                .state
                .players
                .get(&p)
                .map_or(-1, |s| s.reputation);
            let _ = api::reputation::set_attitude(host.cx, vibe, rep, solar.field_0x4c8);
        }
    }
    let record = sub::SpawnRecord {
        name: solar.nickname.clone(),
        state: 2,
        loot: solar.loot.clone(),
        flags: solar.mode_flag as u32,
        attitude: solar.field_0x4c8,
        solar_type: solar.solar_type,
        reputation: vibe,
        counted: true,
        object,
        ..sub::SpawnRecord::default()
    };
    let solars = vig(host, key).map_or(0, |v| v.solars);
    if let Some(s) = sub::sub_mut(host, solars) {
        s.records.push(record);
    }
    if let Some(r) = host.cx.logic.content.state.systems.get_mut(&system) {
        r.mission_objects.push(object);
    }
    true
}

/// `SpawnLoot` (slot 25, `:1910-1980`): only a critical loot gets a record
/// and a message; a loot placed beside its parent is made only when the
/// parent exists.
fn spawn_loot(
    host: &mut Host<'_, '_>,
    key: u32,
    loot: &super::super::offer::LootSpawn,
    parent: u32,
) {
    let system = offer_system(host, key);
    if system == 0 {
        return;
    }
    let critical_name = loot.critical.and_then(|i| {
        let rm = std::sync::Arc::clone(&host.cx.logic.content.rm);
        rm.tables.critical_loot.get(i).map(|c| c.name)
    });
    let mut info = api::space_obj::LootInfo::new(system, loot.archetype, loot.units, Vector::ZERO);
    info.owner = parent;
    info.hit_pts_scale = loot.hit_pts_scale;
    info.can_ai_tractor = loot.can_ai_tractor != 0;
    if loot.kind == 2 {
        info.mission = true;
    }
    if let Some(name) = critical_name {
        info.name_id = (name as u32).wrapping_add(0x50bb8);
    }
    let mut object = 0;
    if loot.rel_pos != 0 {
        if parent & 0x3fff_ffff != 0 {
            let at = api::space_obj::location(host.cx, parent).map_or(Vector::ZERO, |(p, _)| p);
            let (linear, angular) =
                api::space_obj::motion(host.cx, parent).unwrap_or((Vector::ZERO, Vector::ZERO));
            info.position = Vector::new(
                (f64::from(at.x) + f64::from(loot.rel_pos_offset.x)) as f32,
                (f64::from(at.y) + f64::from(loot.rel_pos_offset.y)) as f32,
                (f64::from(at.z) + f64::from(loot.rel_pos_offset.z)) as f32,
            );
            info.velocity = linear;
            info.angular_velocity = angular;
            object = api::space_obj::create_loot(host.cx, &info).unwrap_or(0);
        }
    } else {
        object = api::space_obj::create_loot(host.cx, &info).unwrap_or(0);
    }
    if loot.kind != 2 {
        return;
    }
    let record = sub::SpawnRecord {
        object,
        parent,
        units: loot.units as i32,
        mode: loot.field_0x100,
        loot_kind: loot.kind,
        holder: 0,
        good: 0,
        flags: 2,
        ..sub::SpawnRecord::default()
    };
    if loot.field_0x100 != 1 {
        let _ = api::space_obj::set_invincible2(host.cx, object, false, true, 1.0);
    }
    let machine = vig(host, key).map_or(0, |v| v.loot);
    if let Some(s) = sub::sub_mut(host, machine) {
        s.records.push(record);
    }
    host.send_immediate(
        machine,
        RmMessage::new(
            MessageType::CriticalLootSpawned,
            Body::Loot { object, value: 0 },
        ),
        key,
    );
}

/// `SpawnRecordLoot` (slot 26): every loot of the record, parented to it.
fn spawn_record_loot(host: &mut Host<'_, '_>, key: u32, record: &sub::SpawnRecord) {
    for loot in &record.loot {
        spawn_loot(host, key, loot, record.object);
    }
}

/// `SetPlayerAttitudes` (slot 21, `:1527-1551`): every alive ship's and
/// solar's attitude (unless -2) towards the player.
fn set_player_attitudes(host: &mut Host<'_, '_>, key: u32, player: u32) {
    let rep = host
        .cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .map_or(-1, |s| s.reputation);
    let (ships, solars) = vig(host, key).map_or((0, 0), |v| (v.ships, v.solars));
    let ship_records: Vec<_> = sub_query_ships(host, ships, 0, 2, 0)
        .into_iter()
        .filter_map(|i| sub::records(host, ships).get(i).cloned())
        .collect();
    for r in ship_records {
        if r.attitude == -2.0 {
            continue;
        }
        let ship_rep = r.npc_ship.map_or(-1, |k| {
            crate::content::ships::reputation(
                &host.cx.logic.content.state,
                crate::content::ships::ShipKey::Npc(k),
            )
        });
        let _ = api::reputation::set_attitude(host.cx, ship_rep, rep, r.attitude);
    }
    let solar_records: Vec<_> = sub_query_solars(host, solars, -1, 2, 0)
        .into_iter()
        .filter_map(|i| sub::records(host, solars).get(i).cloned())
        .collect();
    for r in solar_records {
        if r.attitude == -2.0 {
            continue;
        }
        let _ = api::reputation::set_attitude(host.cx, r.reputation, rep, r.attitude);
    }
}

/// `OnPlayerJoined` (slot 20, `:1479-1499`): the player's attitudes, each
/// active target objective set again for the mission players and rebuilt,
/// then the refresh.
fn on_player_joined(host: &mut Host<'_, '_>, key: u32, player: u32) {
    set_player_attitudes(host, key, player);
    let count = vig(host, key).map_or(0, |v| v.active.len());
    for i in 0..count {
        let Some(a) = vig(host, key).and_then(|v| v.active.get(i).cloned()) else {
            break;
        };
        let ObjectiveSource::Offer(component, map_key) = a.source else {
            continue;
        };
        let Some(ob) =
            offer_mut(host, key).and_then(|o| component_objective(o, component, map_key).cloned())
        else {
            continue;
        };
        if ob.kind != 3 {
            continue;
        }
        set_targets(
            host,
            key,
            component,
            map_key,
            ob.targets.clone(),
            ob.marker_system,
            ob.marker_position,
        );
        rebuild_objective(host, key, a.key);
    }
    refresh_objectives(host, key);
}

/// `UnmarkObjects` (slot 28, `:2208-2247`): every alive ship, solar and
/// loot unmarked for the players; each held critical loot leaves its
/// holder's cargo.
fn unmark_objects(host: &mut Host<'_, '_>, key: u32, players: &[u32]) {
    let Some((ships, solars, loot)) = vig(host, key).map(|v| (v.ships, v.solars, v.loot)) else {
        return;
    };
    let mut objects = Vec::new();
    for i in sub_query_ships(host, ships, 0, 2, 0) {
        if let Some(r) = sub::records(host, ships).get(i) {
            objects.push(r.object);
        }
    }
    for i in sub_query_solars(host, solars, -1, 2, 0) {
        if let Some(r) = sub::records(host, solars).get(i) {
            objects.push(r.object);
        }
    }
    for i in sub_query_loot(host, loot, 0, 0, 0, 2, 0) {
        if let Some(r) = sub::records(host, loot).get(i) {
            objects.push(r.object);
        }
    }
    for object in objects {
        for &p in players {
            let _ = api::player::mark_obj(host.cx, p, object, 0);
        }
    }
    let held: Vec<(u32, u16)> = sub_query_loot(host, loot, 0, 0, 0, 4, 0)
        .into_iter()
        .filter_map(|i| {
            sub::records(host, loot)
                .get(i)
                .map(|r| (r.holder, r.cargo_slot))
        })
        .collect();
    for (holder, slot) in held {
        let _ = api::player::remove_cargo(host.cx, holder, slot, 1);
    }
}

/// `FRmVignetteMachine::Shutdown` (slot 7, `:2147-2196`): the population's
/// exclusion spheres, the alive solars flagged in the offer's system, the
/// alive loot (a critical one destroyed, any other made vulnerable), then
/// every object unmarked for the mission players. The ships' zone
/// behaviour and guides come with their spawn (D1023).
pub(super) fn shutdown(host: &mut Host<'_, '_>, key: u32) {
    let Some((target_active, target, center, system)) = offer(host, key).map(|o| {
        (
            o.target_ships.component.active,
            o.target_ships.position,
            main_center(o),
            o.system,
        )
    }) else {
        return;
    };
    if target_active {
        population::remove_exclusion_sphere(host.cx, [target.x, target.y, target.z, 10000.0]);
    }
    population::remove_exclusion_sphere(host.cx, [center.x, center.y, center.z, 10000.0]);
    let Some((solars, loot)) = vig(host, key).map(|v| (v.solars, v.loot)) else {
        return;
    };
    let alive_solars: Vec<u32> = sub_query_solars(host, solars, -1, 2, 0)
        .into_iter()
        .filter_map(|i| sub::records(host, solars).get(i).map(|r| r.object))
        .collect();
    if let Some(record) = host.cx.logic.content.state.systems.get_mut(&system) {
        for object in alive_solars {
            if !record.flagged_objects.contains(&object) {
                record.flagged_objects.push(object);
            }
        }
    }
    let alive_loot: Vec<(u32, i32)> = sub_query_loot(host, loot, 0, 0, 0, 2, 0)
        .into_iter()
        .filter_map(|i| {
            sub::records(host, loot)
                .get(i)
                .map(|r| (r.object, r.loot_kind))
        })
        .collect();
    for (object, kind) in alive_loot {
        if kind == 2 {
            let _ = api::space_obj::destroy(host.cx, object, 0);
        } else {
            let _ = api::space_obj::set_invincible(host.cx, object, false, true, 0.0);
        }
    }
    let players = host.players();
    unmark_objects(host, key, &players);
}

/// `FRmVignetteMachine::OnSucceeded` (slot 27, `:2199-2232`): each player
/// with a ship within 10000 of the area gets flags 4 and 120 seconds, the
/// others flags 0; the reward text goes to the first player.
fn on_succeeded(host: &mut Host<'_, '_>, key: u32) {
    let text = {
        let Some(o) = offer(host, key).cloned() else {
            return;
        };
        let mut text = FmtStr::new(o.reward_text.str_id as u32);
        for &param in &o.reward_text.params {
            super::super::offer::text_param(host.cx, &o, param, &mut text);
        }
        text
    };
    let position = vig(host, key).map_or(Vector::ZERO, |v| v.position);
    let players = host.players();
    for &p in &players {
        let has_ship = host
            .cx
            .logic
            .content
            .state
            .players
            .get(&p)
            .is_some_and(|s| s.ship.is_some());
        let near = has_ship && {
            let at = player_position(host.cx, p);
            let dx = f64::from(at.x) - f64::from(position.x);
            let dy = f64::from(at.y) - f64::from(position.y);
            let dz = f64::from(at.z) - f64::from(position.z);
            (dx * dx + dz * dz) + dy * dy < 10000.0 * 10000.0
        };
        if let Some(s) = host.cx.logic.content.state.players.get_mut(&p) {
            if near {
                s.flags = 4;
                s.countdown = 120.0;
            } else {
                s.flags = 0;
            }
        }
    }
    if let Some(&first) = players.first() {
        let _ = api::player::display_mission_message(host.cx, first, &text, 3, true);
    }
}
