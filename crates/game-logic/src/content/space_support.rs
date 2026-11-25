//! The SpaceSupport controller (`Content/SpaceSupport.cpp`, TU53; record
//! 0008 point 2; work packages P3.4d and P3.6a).
//!
//! - **Constructor** (`FSpaceSupport::FSpaceSupport`, `:303-308`): the
//!   controller's id into `GSpaceSupportControllerId`, a 1 s heartbeat
//!   (`GSpaceSupportHeartbeat`; one `rand()`), then `InitGcs`, the comm
//!   system's handlers and intervals (P3.6b). It has no destructor.
//! - **Handler** (`FSpaceSupport::HandleMessage`, `:560-984`), four steps
//!   over every message, in the C++ order ([`handle_message`]):
//!   1. the comm system's handler set for 15 messages (P3.6b);
//!   2. the NPC ship events for `CommComplete`, `ObjectDestroyed` and
//!      `TimerExpired` (P3.6c);
//!   3. the main switch, every case in the C++ order. The port has the
//!      dock queues ([`super::dock_queues`]), the formations
//!      ([`super::formations`]), the trade lanes
//!      ([`super::tradelane_requests`]), the loot preparation
//!      ([`prepare_loot`]), the solar attitudes of a multiplayer launch
//!      ([`sync_solar_attitudes`]), the system records' parts (P3.4d) and
//!      the music, the NPC ships, their lives and hostile contacts, the
//!      combat reputation and the faction-name timers (P3.6c,
//!      [`super::npc_names::handle_faction_name_timer`]). The parts of
//!      later packages are named stubs: the comm system (P3.6b) and the
//!      message router (P3.7b);
//!   4. the tail: the comm system again on load and unload (P3.6b), the
//!      dock terminal states except on `JumpgateAct`
//!      ([`super::dock_queues::apply_terminal_states`]), and each guided
//!      ship forwarding the message to its guide (P3.6c).

use game_sim::data::equip_desc::EquipDesc;
use game_sim::data::vc6::vc6_sort;
use neuron_core::math::Vector;
use neuron_core::num::trunc_to_i32;
use neuron_server::controller::ControllerId;

use super::dock_queues::{self, DockEntry};
use super::ships;
use super::{Instance, formations, players, systems, tradelane_requests};
use crate::api;
use crate::api::audio::Tryptich;
use crate::events::{ControllerMessage, ObjectDestroyed};
use crate::server::Cx;

/// `GSpaceSupportHeartbeat` (`:54-55`): the heartbeat period in seconds.
pub const HEARTBEAT: f32 = 1.0;

/// `GSpaceSupportLootRadius` (`:165`): the radius of the players whose
/// worth scales a loot drop.
pub const LOOT_RADIUS: f32 = 5000.0;

/// `FSpaceSupport::FSpaceSupport(ControllerId)`.
pub fn construct(cx: &mut Cx<'_>, id: ControllerId) -> Instance {
    cx.logic.content.state.space_support = id;
    super::set_heartbeat(cx, id, HEARTBEAT);
    // `InitGcs` (`:310-347`): the comm system's handlers 0 to 2 and its
    // intervals, chances and categories are P3.6b's.
    Instance::SpaceSupport
}

/// Step 1 (`:566-588`): the comm system's handler set (`GGcsHandlers`)
/// hears `ObjectDamaged`, `Hail`, `Heartbeat`, `PhantomEnter`,
/// `PhantomExit`, `DeathFuseLit`, `ObjectDestroyed`, `TimerExpired`,
/// `CombatState`, `CombatDrift`, `NpcTurnedHostile`, `FleeRequest`,
/// `DamageReputation`, `FormationLeaderChanged` and `ShipInSights`.
/// P3.6b (D844).
fn gcs_handlers(_cx: &mut Cx<'_>, _message: &mut ControllerMessage<'_>) {}

/// Step 2 (`:590-592`): the NPC ship events (`GNpcShipEvents`) of
/// `CommComplete`, `ObjectDestroyed` and `TimerExpired`
/// ([`super::npc_ships::handle_events_message`]).
fn npc_ship_events(cx: &mut Cx<'_>, message: &mut ControllerMessage<'_>) {
    super::npc_ships::handle_events_message(cx, message);
}

/// The tail's guided ships (`GSpaceSupportGuidedShips`, `:970-983`): each
/// forwards the message to its guide (`ForwardToGuide`), then the emptied
/// entries go. The walk reads each entry again, so an entry a guide
/// unregisters during it is skipped.
fn guided_ships(cx: &mut Cx<'_>, message: &mut ControllerMessage<'_>) {
    let mut at = 0;
    while at < cx.logic.content.state.guided_ships.len() {
        if let Some(ship) = cx.logic.content.state.guided_ships[at] {
            super::guides::forward_to_guide(cx, ship, message);
        }
        at += 1;
    }
    cx.logic.content.state.guided_ships.retain(Option::is_some);
}

/// `RegisterGuidedShip(Ship)` (`SpacePaths.cpp:446-449`).
pub fn register_guided_ship(cx: &mut Cx<'_>, ship: crate::events::NpcShipKey) {
    cx.logic.content.state.guided_ships.push(Some(ship));
}

/// `UnregisterGuidedShip(Ship)` (`:452-463`): its first entry emptied.
pub fn unregister_guided_ship(cx: &mut Cx<'_>, ship: crate::events::NpcShipKey) {
    if let Some(e) = cx
        .logic
        .content
        .state
        .guided_ships
        .iter_mut()
        .find(|e| **e == Some(ship))
    {
        *e = None;
    }
}

/// The comm system's type 2 event (`FGcsDockGrantedEvent` to
/// `GGcsConversations.QueueConversation`) of a dock answer: P3.6b (D844).
/// The C++ fills the event's terminal type from the terminal (or the
/// ship's classes) and queues it; nothing is queued until the comm system
/// comes.
pub(crate) fn gcs_dock_granted_event(
    _cx: &mut Cx<'_>,
    _entry: &DockEntry,
    _index: i32,
    _response: i32,
    _waited_in_queue: bool,
) {
}

/// The comm system's "granted" event of a lane ring
/// (`Content/TradelaneRequests.cpp:51-60`): P3.6b (D734).
pub(crate) fn gcs_lane_granted_event(_cx: &mut Cx<'_>, _system: u32, _ring: u32, _ship: u32) {}

/// `FSpaceSupport::HandleMessage`: the four steps in the C++ order.
pub fn handle_message(cx: &mut Cx<'_>, _id: ControllerId, message: &mut ControllerMessage<'_>) {
    gcs_handlers(cx, message);
    if matches!(
        message,
        ControllerMessage::CommComplete { .. }
            | ControllerMessage::ObjectDestroyed(_)
            | ControllerMessage::TimerExpired { .. }
    ) {
        npc_ship_events(cx, message);
    }
    main_switch(cx, message);
    // The tail (`:965-983`).
    if matches!(
        message,
        ControllerMessage::SystemLoaded { .. } | ControllerMessage::SystemUnloaded { .. }
    ) {
        gcs_handlers(cx, message);
    }
    if !matches!(message, ControllerMessage::JumpgateAct { .. }) {
        dock_queues::apply_terminal_states(cx);
    }
    guided_ships(cx, message);
}

/// The main switch (`:594-962`), its cases in the C++ order.
#[allow(clippy::too_many_lines)]
fn main_switch(cx: &mut Cx<'_>, message: &mut ControllerMessage<'_>) {
    match message {
        ControllerMessage::CommComplete { .. } => {
            // The system record's pending comms (`SendPendingComms`) and
            // the conversations (`GGcsConversations.OnMessage`): P3.6b.
        }
        ControllerMessage::ObjectDamaged(event) => {
            let event = *event;
            super::combat_reputation::on_damage_reputation(cx, &event);
            // FindSystemInfo is not null-tested (D843).
            if let Some(ship) = ships::find_in_system(cx, event.system, event.id) {
                ships::add_damage(
                    &mut cx.logic.content.state,
                    ship,
                    event.inflictor,
                    event.damage,
                );
            }
        }
        ControllerMessage::DockCancel { ship, target, .. } => {
            let (ship, target) = (*ship, *target);
            dock_queues::cancel_ship_docking(cx, ship, target, true);
        }
        ControllerMessage::DockComplete { ship, system, .. } => {
            let (ship, system) = (*ship, *system);
            if !cx.logic.content.state.systems.contains_key(&system) {
                return;
            }
            formations::break_formation_at_dock(cx, system, ship, true, false);
            dock_queues::set_dock_equipment(cx, ship, false, false, None);
            let Some(key) = cx.logic.content.state.dock_queues.docking.find_ship(ship) else {
                return;
            };
            dock_queues::finish_docking(cx, key);
            dock_queues::update_dock_queues(cx);
        }
        ControllerMessage::DockRequest {
            ship,
            target,
            system,
            flag,
        } => {
            let (ship, target, system, flag) = (*ship, *target, *system, *flag);
            if let Some(key) = cx.logic.content.state.dock_queues.docking.find_ship(ship) {
                dock_queues::finish_docking(cx, key);
            }
            dock_queues::handle_dock_request(cx, ship, target, system, flag);
        }
        ControllerMessage::EquipDropLoot(event) => {
            let state = &cx.logic.content.state;
            if players::find_by_ship(&state.players, event.inflictor).is_none() {
                event.desc.count = 0;
                return;
            }
            let mut items = vec![event.desc];
            prepare_loot(cx, event.system, event.object, &mut items, false);
            event.desc = items[0];
        }
        ControllerMessage::TradelaneRequest {
            ship,
            ring,
            end_ring,
            system,
        } => {
            let (ship, ring, end_ring, system) = (*ship, *ring, *end_ring, *system);
            tradelane_requests::handle_request(cx, ship, ring, end_ring, system);
        }
        ControllerMessage::TradelaneRelease { ship, ring, .. } => {
            let (ship, ring) = (*ship, *ring);
            tradelane_requests::handle_release(cx, ship, ring);
        }
        ControllerMessage::FormationRequest {
            ship,
            leader,
            system,
        } => {
            let (ship, leader, system) = (*ship, *leader, *system);
            let result = formations::handle_formation_request(cx, ship, leader, system);
            if result == 0 {
                // FindShip is not null-tested; a joined ship is found.
                let key = ships::find_in_system(cx, system, ship);
                if let Some(key) = key {
                    formations::reset_formation_state(cx, key);
                }
                let _ = api::space_obj::formation_response(cx, ship, 0);
                if let Some(key) = key {
                    formations::follow_leader_into_dock(cx, key);
                }
            } else {
                let _ = api::space_obj::formation_response(cx, ship, result);
            }
        }
        ControllerMessage::FormationLeave { ship, system, .. } => {
            let (ship, system) = (*ship, *system);
            // FindSystemInfo and the second FindShip are not null-tested
            // (D843).
            if let Some(key) = ships::find_in_system(cx, system, ship) {
                let formation = ships::formation_id(&cx.logic.content.state, key);
                if formation != u32::MAX {
                    formations::cancel_docking_behind_leader(cx, key, formation);
                    ships::set_formation(&mut cx.logic.content.state, key, u32::MAX, 0, None);
                }
            }
            if let Some(key) = ships::find_in_system(cx, system, ship) {
                formations::reset_formation_state(cx, key);
            }
            if !api::misc::single_player(cx) {
                return;
            }
            let state = &mut cx.logic.content.state;
            if let Some(player) = players::find_by_ship(&state.players, ship)
                && let Some(p) = state.players.get_mut(&player)
                && p.value_e4 == 10
            {
                p.value_e4 = 0;
            }
        }
        ControllerMessage::Heartbeat { elapsed } => {
            let elapsed = *elapsed;
            heartbeat(cx, elapsed);
        }
        ControllerMessage::Msg0x25 { ship, system, .. } => {
            // An NPC ship of the record's space objects left the system: it
            // is destroyed (`DESTROY_NORMAL`).
            let (ship, system) = (*ship, *system);
            if ships::find_space_obj(&cx.logic.content.state, system, ship).is_some() {
                let _ = api::space_obj::destroy(cx, ship, 0);
            }
        }
        ControllerMessage::Msg0x27 { ship, land, .. } => {
            let (ship, land) = (*ship, *land);
            if land {
                dock_queues::cancel_ship_docking(cx, ship, 0, false);
                return;
            }
            let q = &mut cx.logic.content.state.dock_queues;
            let Some(key) = q.docking.find_ship(ship) else {
                return;
            };
            let Some(entry) = q.docking.get_mut(key) else {
                return;
            };
            entry.inside = true;
            let (target, index) = (entry.target, entry.index);
            q.mark_terminal(target, index, false);
            dock_queues::update_dock_queues(cx);
        }
        ControllerMessage::LaunchComplete { ship, .. } => {
            let ship = *ship;
            if let Some(key) = cx.logic.content.state.dock_queues.docking.find_ship(ship) {
                dock_queues::finish_docking(cx, key);
            }
        }
        ControllerMessage::PlayerManeuver {
            player,
            object,
            value,
        } => {
            if let Some(p) = cx.logic.content.state.players.get_mut(player) {
                p.value_e4 = *value;
                p.value_e8 = *object;
            }
        }
        ControllerMessage::FreeTerminalRequest { player, target, .. } => {
            let (player, target) = (*player, *target);
            // The ship arch's failed lookup leaves the C++'s local
            // uninitialised (D841: 0, no properties).
            let arch = api::player::ship_id(cx, player).unwrap_or(0);
            let Some(properties) = cx
                .logic
                .content
                .tables
                .ship_mission_properties
                .get(&arch)
                .cloned()
            else {
                return;
            };
            let mut classes: Vec<u32> = Vec::new();
            for p in properties {
                if !classes.contains(&p) {
                    classes.push(p);
                }
            }
            let mut index = if api::misc::single_player(cx) { 0 } else { -1 };
            if dock_queues::choose_dock_terminal(cx, &classes, target, None, &mut index)
                == dock_queues::RESPONSE_PROCEED
            {
                let _ = api::misc::report_free_terminal(message, target, index);
            }
        }
        ControllerMessage::SpaceEnter {
            system,
            player,
            launch_object,
            launch_kind,
        } => {
            let (system, player, target, index) = (*system, *player, *launch_object, *launch_kind);
            space_enter(cx, system, player, target, index);
        }
        ControllerMessage::SpaceExit { system, .. } => {
            let system = *system;
            erase_launch_queues_unless_occupied(cx, system);
            // `OnPlayerChanged` (`Content/SystemInfo.cpp:428-433`), whose
            // player argument is not read.
            if cx.logic.content.state.systems.contains_key(&system) && api::misc::single_player(cx)
            {
                systems::destroy_objects_if_unoccupied(cx, system);
            }
        }
        ControllerMessage::ObjectDestroyed(event) => object_destroyed(cx, event),
        ControllerMessage::SystemExit {
            system,
            destination,
            player,
        } => {
            let (system, destination, player) = (*system, *destination, *player);
            erase_launch_queues_unless_occupied(cx, system);
            if !cx.logic.content.state.systems.contains_key(&system) {
                return;
            }
            // A player without a state is dereferenced by the C++ (D801).
            let Some(state) = cx.logic.content.state.players.get(&player) else {
                return;
            };
            if let Some(ship) = state.ship.as_ref().map(|s| s.ship) {
                let docking = &cx.logic.content.state.dock_queues.docking;
                if let Some(key) = docking.find_ship(ship)
                    && docking.get(key).is_some_and(|e| e.system != destination)
                {
                    dock_queues::finish_docking(cx, key);
                }
            }
            if let Some(record) = cx.logic.content.state.systems.get_mut(&system) {
                record.remove_player(player);
            }
            if api::misc::single_player(cx) {
                systems::destroy_objects_if_unoccupied(cx, system);
            }
        }
        ControllerMessage::SystemEnter {
            system,
            player,
            arrival,
            ..
        } => {
            let (system, player, arrival) = (*system, *player, *arrival);
            system_enter(cx, system, player, arrival);
        }
        ControllerMessage::SystemLoaded { system } => systems::load_full(cx, *system),
        ControllerMessage::SystemUnloaded { system } => {
            systems::unload_full(cx, *system);
            cx.logic.content.state.dock_queues.applied.clear();
        }
        ControllerMessage::TimerExpired { param1, param2 } => {
            let (param1, param2) = (*param1, *param2);
            if param1 == cx.logic.content.state.load_ids.music_timer {
                let _ = api::audio::cancel_music(cx, param2);
            } else {
                // The faction-name queues, then the message router
                // (`GMessageRouter->OnTimerExpired`, P3.7b), which sets no
                // timer yet.
                super::npc_names::handle_faction_name_timer(cx, param1, param2);
            }
        }
        ControllerMessage::DamageReputation {
            provoked,
            attacker,
            victim,
            system,
        } => {
            let (provoked, attacker, victim, system) = (*provoked, *attacker, *victim, *system);
            if !provoked || !cx.logic.content.state.systems.contains_key(&system) {
                return;
            }
            let (Some(ship), Some(other)) = (
                ships::find_in_system(cx, system, attacker),
                ships::find_in_system(cx, system, victim),
            ) else {
                return;
            };
            let state = &cx.logic.content.state;
            if players::find_by_ship(&state.players, attacker).is_none() {
                return;
            }
            if ships::formation_id(state, ship) == ships::formation_id(state, other) {
                formations::remove_from_formation(cx, ship);
            }
        }
        ControllerMessage::NpcShipCreated { ship } => {
            let ship = *ship;
            formations::reset_formation_state(cx, ships::ShipKey::Npc(ship));
        }
        _ => {}
    }
}

/// `IsContentSystemOccupied` (`Content/PlayerState.cpp:955-959`) as the
/// `SpaceExit` and `SystemExit` cases test it: a system id whose low 30
/// bits are 0, or a record without players, drops every launch queue. A
/// missing record, which the C++ dereferences, counts as empty (D843).
fn erase_launch_queues_unless_occupied(cx: &mut Cx<'_>, system: u32) {
    let occupied = system & 0x3fff_ffff != 0
        && cx
            .logic
            .content
            .state
            .systems
            .get(&system)
            .is_some_and(|r| !r.players.is_empty());
    if !occupied {
        dock_queues::erase_all_launch_queues(cx);
    }
}

/// The heartbeat case (`:715-746`): every system record's NPC ship lives
/// (with the heartbeat's period), every record's flagged objects away from
/// the players, the dock queues, the launch queues, the combat reputation,
/// each player's ship's hostile contacts (with the elapsed time) and the
/// comm system's conversations (P3.6b).
fn heartbeat(cx: &mut Cx<'_>, elapsed: f32) {
    let ids: Vec<u32> = cx.logic.content.state.systems.keys().copied().collect();
    for &system in &ids {
        systems::update_npc_ship_lives(cx, system, HEARTBEAT);
    }
    for &system in &ids {
        systems::destroy_flagged_objects_away_from_players(cx, system);
    }
    dock_queues::update_dock_queues(cx);
    let _ = dock_queues::launch_queued_ships(cx);
    super::combat_reputation::on_heartbeat_reputation(cx);
    let players: Vec<u32> = cx
        .logic
        .content
        .state
        .players
        .iter()
        .filter(|(_, p)| p.ship.is_some())
        .map(|(&id, _)| id)
        .collect();
    for player in players {
        super::combat_reputation::update_hostile_contacts(cx, player, elapsed);
    }
}

/// The `SpaceEnter` case (`:820-861`): a multiplayer launch first syncs the
/// solar attitudes; a launch from a base or a docking ring (type
/// `0x600de0`) puts the player's ship in the docking list as launched. The
/// dock hail's silence and the launch conversation are P3.6b's (D844).
fn space_enter(cx: &mut Cx<'_>, system: u32, player: u32, target: u32, index: i32) {
    if !api::misc::single_player(cx) {
        sync_solar_attitudes(cx, system, player);
    }
    let Ok(target_type) = api::space_obj::object_type(cx, target) else {
        return;
    };
    if target_type & 0x60_0de0 == 0 {
        return;
    }
    let ship = api::player::ship(cx, player).unwrap_or(0);
    dock_queues::push_launched(cx, system, ship, target, index);
}

/// The `SystemEnter` case (`:877-912`): the player joins the record's
/// players and an entering player's ship is no scan target and calm
/// (P3.4d); a ship arriving through an airlock (type `0x400`) enters the
/// docking list as launched at terminal 0.
fn system_enter(cx: &mut Cx<'_>, system: u32, player: u32, arrival: u32) {
    let state = &mut cx.logic.content.state;
    if let Some(record) = state.systems.get_mut(&system) {
        // A player without a state is dereferenced by the C++ (D801).
        if let Some(player_state) = state.players.get_mut(&player) {
            record.add_player(player);
            if let Some(ship) = player_state.ship.as_mut() {
                ship.scan_target = false;
                // `ResetHostileState`.
                ship.in_combat = false;
                ship.calm_since = f64::from(-1.0_f32);
                ship.hostile_contact_count = 0;
            }
        }
    }
    if arrival == 0 {
        return;
    }
    let Ok(arrival_type) = api::space_obj::object_type(cx, arrival) else {
        return;
    };
    if arrival_type & 0x400 == 0 {
        return;
    }
    let ship = api::player::ship(cx, player).unwrap_or(0);
    dock_queues::push_launched(cx, system, ship, arrival, 0);
}

/// `FSpaceSupport::SyncSolarAttitudes(Msg)` (`:107-162`): every object of
/// the system (up to 1000, type `0xfdff`) whose solar reputation and
/// affiliation are known takes the launching player's feeling towards its
/// affiliation as its attitude towards him, when the two differ by more
/// than 0.01.
pub fn sync_solar_attitudes(cx: &mut Cx<'_>, system: u32, player: u32) {
    let ship = api::player::ship(cx, player).unwrap_or(0);
    if ship == 0 {
        return;
    }
    let objects = api::system::scan_objects(cx, system, 0x3e8, Vector::ZERO, f32::MAX, 0xfdff)
        .unwrap_or_default();
    let Ok(player_rep) = api::space_obj::rep(cx, ship) else {
        return;
    };
    if player_rep == -1 || objects.is_empty() {
        return;
    }
    for object in objects {
        let Ok(solar_rep) = api::space_obj::solar_rep(cx, object) else {
            continue;
        };
        let Ok(affiliation) = api::reputation::affiliation(cx, solar_rep) else {
            continue;
        };
        let attitude = api::reputation::attitude(cx, solar_rep, player_rep).unwrap_or(0.0);
        let feeling = api::reputation::reputation(cx, player_rep, affiliation).unwrap_or(0.0);
        if (f64::from(attitude) - f64::from(feeling)).abs() > 0.01 {
            let _ = api::reputation::set_attitude(cx, solar_rep, player_rep, feeling);
        }
    }
}

/// `rand() / 32768` as the C++ widens it: `double(rand()) * double(1.0f /
/// 32768.0f)`.
fn unit_rand(cx: &mut Cx<'_>) -> f64 {
    f64::from(cx.world.rng().rand()) * f64::from(1.0_f32 / 32768.0)
}

/// `FSpaceSupport::PrepareLoot(System, Ship, &Items, bAddPhantomLoot)`
/// (`:353-500`): the items sorted by archetype (VC6 `std::sort`) with each
/// run of one archetype folded into its first item; then, for a ship
/// Content knows in the system, each item's count scaled by the worth of
/// the players within 5000 of the ship and its loot properties (the good's
/// own, else the default commodity or equipment record), with a random
/// health; with `add_phantom_loot`, an NPC ship not of affiliation
/// `0x62e8` adds the phantom loot of its rank.
pub fn prepare_loot(
    cx: &mut Cx<'_>,
    system: u32,
    ship: u32,
    items: &mut Vec<EquipDesc>,
    add_phantom_loot: bool,
) {
    vc6_sort(items, |a, b| a.arch_id < b.arch_id);
    let mut at = 0;
    while at < items.len() {
        let next = at + 1;
        while next < items.len() && items[next].arch_id == items[at].arch_id {
            items[at].count = items[at].count.wrapping_add(items[next].count);
            items.remove(next);
        }
        at = next;
    }
    // The record is not null-tested by either caller (D843).
    let Some(key) = ships::find_in_system(cx, system, ship) else {
        return;
    };
    let position = ships::position(cx, key);
    let near = systems::find_players_near(cx, system, position, LOOT_RADIUS);
    let mut worth = 0.0_f32;
    for player in near {
        let cash = api::player::inspect_cash(cx, player).unwrap_or(0);
        let asset = api::player::asset_value(cx, player).unwrap_or(0.0);
        let player_worth = f64::from(cash) + f64::from(asset);
        worth = (player_worth + f64::from(worth)) as f32;
    }
    if !items.is_empty() {
        let worth_value = trunc_to_i32(f64::from(worth));
        // The out-parameters keep their value when a call does not write
        // them: the good starts as the ship's id, the commodity flag as the
        // low byte of the sum's bits.
        let mut good = ship;
        let mut commodity = worth.to_bits() & 0xff != 0;
        let tables = cx.logic.content.tables.clone();
        let loot = &tables.loot_props;
        for item in items.iter_mut() {
            if let Ok(g) = api::misc::translate_arch_to_good(cx, item.arch_id) {
                good = g;
            }
            if let Ok(c) = api::misc::is_commodity(cx, good) {
                commodity = c;
            }
            let props = super::tables::loot::find_loot_props(loot, good).or_else(|| {
                let default = if commodity {
                    b"default_commodity".as_slice()
                } else {
                    b"default_equipment".as_slice()
                };
                super::tables::loot::find_loot_props(loot, neuron_core::id::create_id(default))
            });
            // No default record, which the C++ dereferences: the item is
            // left as it is (D843).
            let Some(props) = props.copied() else {
                continue;
            };
            let d = props.drop_properties;
            let count = item.count;
            let mut new_count = 0;
            if worth_value >= d[0] {
                let excess = worth_value.wrapping_sub(d[0]);
                let mut scaled = 0;
                if excess > 0 {
                    scaled = count;
                    if d[1] != 0 {
                        let mut capped = excess.wrapping_div(d[1]);
                        if capped >= count {
                            capped = count;
                        }
                        scaled = trunc_to_i32((f64::from(capped) + 0.5).floor());
                    }
                }
                let mut bounded = d[3];
                if d[2].wrapping_add(scaled) < d[3] {
                    bounded = scaled.wrapping_add(d[2]);
                }
                if bounded >= count {
                    bounded = count;
                }
                let expected = (f64::from(bounded) * f64::from(props.drop_chance)) as f32;
                new_count = trunc_to_i32(f64::from(expected).floor());
                let fraction = (f64::from(expected) - f64::from(expected).floor()) as f32;
                if unit_rand(cx) < f64::from(fraction) {
                    new_count = new_count.wrapping_add(1);
                }
                if new_count >= d[4] {
                    new_count = d[4];
                }
            }
            let status = (unit_rand(cx) * f64::from(0.3_f32) + 0.5) as f32;
            item.health = status;
            item.count = new_count;
        }
    }
    if add_phantom_loot && ships::is_npc(key) && ships::affiliation(cx, key) != 0x62e8 {
        let rank = ships::reputation_rank(cx, key);
        let tables = cx.logic.content.tables.clone();
        for phantom in super::tables::loot::collect_phantom_loot(&tables.phantom_loot, rank) {
            let draw = unit_rand(cx);
            if draw.partial_cmp(&f64::from(phantom.percent_chance))
                != Some(std::cmp::Ordering::Less)
            {
                continue;
            }
            let mut desc = EquipDesc::new(cx.world.strings());
            desc.arch_id = phantom.nickname;
            let range = phantom
                .num_to_drop_max
                .wrapping_sub(phantom.num_to_drop_min)
                .wrapping_add(1);
            let draw = cx.world.rng().rand();
            desc.count = (draw.wrapping_mul(range) / 32768).wrapping_add(phantom.num_to_drop_min);
            items.push(desc);
        }
    }
}

/// `FSpaceSupport::OnObjectDestroyed` (`:986-1081`), the `ObjectDestroyed`
/// case: the players targeting the object (`+0xe8`) forget it; a Content
/// ship leaves its lane, and when killed drops the loot it earned (a
/// player's top damage: prepared with phantom loot; anyone else's: none),
/// counts for its killer, and an NPC ship hands its formation on (P3.6c);
/// a killed object a player owned plays `music_death` for him; then the
/// object leaves the launch queues, the gate locks, the system record, the
/// dock lists and the terminal states.
fn object_destroyed(cx: &mut Cx<'_>, event: &mut ObjectDestroyed) {
    let object = event.id;
    for state in cx.logic.content.state.players.values_mut() {
        if state.value_e8 == object {
            state.value_e8 = 0;
        }
    }
    let has_record = cx.logic.content.state.systems.contains_key(&event.system);
    let ship = ships::find_in_system(cx, event.system, object);
    if let Some(key) = ship {
        let mut guide_kept = false;
        let lane = ships::base(&cx.logic.content.state, key)
            .filter(|s| s.lane_entry != 0)
            .map(|s| s.lane_entry);
        if let Some(entry_ring) = lane {
            let time = super::game_support::game_time(cx);
            if let Some(record) = cx.logic.content.state.systems.get_mut(&event.system) {
                record.remove_trade_lane_ship(object, entry_ring, time);
            }
        }
        if event.kind == 1 {
            let (top_player, top_any) = ships::top_damagers(&cx.logic.content.state, key);
            if top_player != 0 && top_player == top_any {
                let ship_id = ships::id(&cx.logic.content.state, key);
                let mut dropped = std::mem::take(&mut event.dropped_equip);
                prepare_loot(cx, event.system, ship_id, &mut dropped, true);
                event.dropped_equip = dropped;
            } else {
                event.dropped_equip.clear();
            }
            let ship_id = ships::id(&cx.logic.content.state, key);
            let killer = if top_player != 0 {
                top_player
            } else {
                event.killer
            };
            super::combat_reputation::apply_kill_reputation(cx, ship_id, killer);
            if let ships::ShipKey::Npc(npc) = key {
                guide_kept = super::formations::promote_formation_leader(cx, event.system, npc);
            }
            let state = &cx.logic.content.state;
            let archetype = ships::base(state, key).map_or(0, |s| s.archetype);
            if let Some(killer) = players::find_by_ship(&state.players, event.killer)
                && let Some(k) = cx.logic.content.state.players.get_mut(&killer)
            {
                k.count_ship_type_killed(archetype);
            }
        }
        // An NPC ship's guide is told it lost its ship (`SetShip(null)`)
        // unless a new leader took it.
        if let ships::ShipKey::Npc(npc) = key
            && !guide_kept
            && let Some(g) = cx
                .logic
                .content
                .state
                .npc_ships
                .get(&npc)
                .and_then(|s| s.guide)
        {
            super::guides::set_ship(cx, g, None);
        }
    }
    if event.owner_player != 0 && event.kind == 1 {
        let track = Tryptich {
            space: 0,
            danger: 0,
            battle: 0,
            override_music: api::misc::nickname_id(b"music_death"),
            flag: 1,
            fade: -1.0,
        };
        let _ = api::audio::set_music(cx, event.owner_player, &track);
    }
    let single_player = api::misc::single_player(cx);
    dock_queues::erase_launch_queue_of(cx, object);
    let state = &mut cx.logic.content.state;
    state.gate_locks.remove_npc(object);
    state.gate_locks.unregister(0, object, single_player);
    if has_record {
        systems::remove_space_obj(cx, event.system, object, true);
        systems::remove_mission_object(cx, event.system, object);
    }
    dock_queues::cancel_ship_docking(cx, object, 0, false);
    let q = &mut cx.logic.content.state.dock_queues;
    dock_queues::DockQueues::erase_first_terminal_of(&mut q.terminals, object);
    dock_queues::DockQueues::erase_first_terminal_of(&mut q.applied, object);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_loot_folds_runs_of_one_archetype() {
        let mut world = game_sim::object::World::new(
            std::sync::Arc::new(game_sim::data::config::GameData::new()),
            game_sim::object::world::WorldConfig::default(),
        );
        let mut logic =
            crate::server::Logic::new(Box::new(crate::server::RecordingSink::default()), 1, true);
        let mut cx = Cx {
            world: &mut world,
            logic: &mut logic,
        };
        let mut items = Vec::new();
        for (arch, count) in [(5, 1), (3, 2), (5, 4), (3, 1), (9, 1)] {
            let mut d = EquipDesc::new(cx.world.strings());
            d.arch_id = arch;
            d.count = count;
            items.push(d);
        }
        let draws = cx.world.rng().clone();
        prepare_loot(&mut cx, 1, 77, &mut items, true);
        let folded: Vec<(u32, i32)> = items.iter().map(|d| (d.arch_id, d.count)).collect();
        assert_eq!(folded, [(3, 3), (5, 5), (9, 1)]);
        // No system record knows the ship: no draw.
        assert_eq!(*cx.world.rng(), draws);
    }
}
