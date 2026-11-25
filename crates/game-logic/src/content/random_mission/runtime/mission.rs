//! The `DestroyMission` controller: `FDestroyMission`
//! (`DestroyMission.cpp:131-490`) over `FRandomMissionObject`
//! (`RandomMissionDB.cpp:1033-1806`) and the mission object of TU38
//! (`MissionObject.cpp`).
//!
//! - **Constructor** ([`construct`]): the mission object's heartbeat
//!   (`0.0333`, one `rand()`), the group's handler, then TU47's body (the
//!   description, a 1 s heartbeat, another `rand()`, the player and its
//!   group, the live count, a record and a placeholder for each player, two
//!   mission messages), then TU27's (the destination system, the first
//!   offer's type) and the group's enter event, which state 0 takes.
//! - **Destructor** ([`destruct`]): every running machine removed and the
//!   dying machines destroyed, then the group's own handler, then the two
//!   references on the mission data.
//! - **Slot 2** ([`slot2`]): states 0 (the brief: the title, the timeout
//!   after 1.1 s), 1 (the runtime machines built, the first started after
//!   0.01 s), 2 (running: a machine's failure or success), 3 (succeeded),
//!   4 (failed), and the "any state" controller messages and machine
//!   messages, `PlayerSave`'s `[RandomMission]` section among them.

#![allow(clippy::neg_cmp_op_on_partial_ord)]

use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::util::state_machine::{self as sm, EventType, MachineHost, SmEvent};
use neuron_server::controller::ControllerId;

use super::message::{Body, Controller, MessageType, RmMessage};
use super::{Event, Host, controller_of, is_enter, machine, message_of, rm, rm_mut, trace};
use crate::api;
use crate::api::audio::Tryptich;
use crate::content::combat_reputation::apply_faction_reputation_change;
use crate::content::mission::object::HEARTBEAT;
use crate::content::router::{self, HandlerOwner};
use crate::events::ControllerMessage;
use crate::server::Cx;

/// `GRmMusicFade` (0.25).
const MUSIC_FADE: f32 = 0.25;

/// `GetReasonIds` (slot 45): the text of a failure reason, 1 outside
/// 0 to 14.
#[must_use]
pub fn reason_ids(reason: i32) -> u32 {
    match reason {
        0 => 0x331d,
        1 => 0x331e,
        2 => 0x331f,
        3 => 0x3320,
        4 => 0x3321,
        5 => 0x3322,
        6 => 0x3328,
        7 => 0x3329,
        8 => 0x332a,
        9 => 0x3323,
        10 => 0x3324,
        11 | 13 => 0x3325,
        12 => 0x3326,
        14 => 0x3327,
        _ => 1,
    }
}

/// The constructors (`MissionObject.cpp:83-102`, `RandomMissionDB.cpp:1033-1060`,
/// `DestroyMission.cpp:131-139`) on the instance already in its slot.
pub(super) fn construct(cx: &mut Cx<'_>, id: ControllerId) {
    // TU38: the heartbeat.
    crate::content::set_heartbeat(cx, id, HEARTBEAT);
    // The group: its handler.
    let handler = router::register(cx, HandlerOwner::Mission(id));
    let data = rm(cx, id).and_then(|r| r.data);
    // TU47: one reference each for TU47 and TU27 (the controller's).
    if let Some(key) = data {
        cx.logic.content.state.rm.missions.add_ref(key);
        cx.logic.content.state.rm.missions.add_ref(key);
    }
    let (description, player, dest_sys, first_kind) = data
        .and_then(|k| cx.logic.content.state.rm.missions.get(k))
        .map_or((FmtStr::new(0), 0, 0, None), |d| {
            (
                d.description.clone(),
                d.player,
                d.dest_sys,
                d.offers.first().map(|o| o.kind),
            )
        });
    if let Some(r) = rm_mut(cx, id) {
        r.handler = handler;
        r.description = description.clone();
        r.heartbeat_interval = 1.0;
    }
    crate::content::set_heartbeat(cx, id, 1.0);
    set_player(cx, id, player);
    cx.logic.content.state.rm.runtime.mission_objects += 1;
    let players = rm(cx, id).map(|r| r.players.clone()).unwrap_or_default();
    {
        let mut host = Host {
            cx,
            mission: id,
            save: None,
        };
        for p in players {
            // The record's slot 2 flag cleared, its placeholder objective
            // sent (P3.5c5, D1021).
            get_or_create_record(&mut host, p);
        }
    }
    let _ = api::player::display_mission_message(cx, player, &FmtStr::new(0), 0xa, true);
    let _ = api::player::display_mission_message(cx, player, &description, 8, true);
    // TU27.
    let dest = if cx.logic.content.state.systems.contains_key(&dest_sys) {
        dest_sys
    } else {
        0
    };
    if let Some(r) = rm_mut(cx, id) {
        r.dest_system = dest;
        // The first offer's type, read without an empty test (D1027).
        if let Some(kind) = first_kind {
            r.kind = kind;
        }
    }
    let mut host = Host {
        cx,
        mission: id,
        save: None,
    };
    sm::group_process_in(&mut host, handler, &SmEvent::Plain(EventType::Tick));
}

/// The destructors (`DestroyMission.cpp:146-151`,
/// `RandomMissionDB.cpp:1066-1069`, `MissionObject.cpp:107-128`): every
/// running machine removed, the dying ones destroyed; the group's members
/// and its handler; the live count; the two references on the data.
pub(super) fn destruct(cx: &mut Cx<'_>, id: ControllerId) {
    let Some((handler, running, data)) = rm(cx, id).map(|r| (r.handler, r.running_machines.clone(), r.data))
    else {
        return;
    };
    {
        let mut host = Host {
            cx,
            mission: id,
            save: None,
        };
        for machine in running {
            host.mark(handler, machine);
        }
        host.remove_dying(handler);
        // `~StateMachineGroup`: the members left are deleted front to back,
        // then the group's own handler goes.
        let members = host.rm().map(|r| r.members.clone()).unwrap_or_default();
        for member in members {
            host.destroy_machine(member);
        }
        if let Some(r) = host.rm_mut() {
            r.members.clear();
        }
        router::unregister(host.cx, handler);
    }
    cx.logic.content.state.rm.runtime.mission_objects -= 1;
    if let Some(key) = data {
        super::super::mission::release(cx, key);
        super::super::mission::release(cx, key);
    }
}

/// `FRandomMissionObject::SetPlayer` (slot 18, `:1085-1112`): the map and
/// the list emptied, the mission object's player set; then every member of
/// the player's group mapped, listed and named.
fn set_player(cx: &mut Cx<'_>, id: ControllerId, player: u32) {
    if let Some(r) = rm_mut(cx, id) {
        r.player_map.clear();
        r.players.clear();
        r.player = player;
    }
    if player == 0 || !cx.logic.content.state.players.contains_key(&player) {
        // `PlayerState` is null: the C++ reads the group through it only
        // when the player has a state (D1027).
        return;
    }
    let members = api::player::group_members(cx, player).unwrap_or_default();
    for member in members {
        let name = api::player::name(cx, member).unwrap_or_default();
        let Some(r) = rm_mut(cx, id) else { return };
        r.player_map.insert(member);
        r.players.push(member);
        add_player_name(r, name);
    }
}

/// `AddPlayerName` (`:1115-1124`): a new name counts while fewer than 8
/// are listed.
fn add_player_name(r: &mut super::RandomMission, name: Vec<u16>) {
    if r.player_names.contains(&name) {
        return;
    }
    if r.player_names.len() < 8 {
        r.player_count += 1;
    }
    r.player_names.push(name);
}

/// `GetOrCreatePlayer` (slot 2 of TU47 over TU38's, `MissionObject.cpp:1233-1252`):
/// the player's mission record, made for a player with a state (in a
/// system, outside single player). False when there is none.
pub(super) fn get_or_create_record(host: &mut Host<'_, '_>, player: u32) -> bool {
    if host.rm().is_some_and(|r| r.records.contains(&player)) {
        return true;
    }
    let Some(state) = host.cx.logic.content.state.players.get(&player) else {
        return false;
    };
    if !api::misc::single_player(host.cx) && state.system.is_none() {
        return false;
    }
    if let Some(r) = host.rm_mut() {
        r.records.insert(player);
    }
    true
}

/// `DecrementPlayerCount` (slot 32).
pub(super) fn decrement_player_count(host: &mut Host<'_, '_>) -> i32 {
    let Some(r) = host.rm_mut() else { return 0 };
    r.player_count -= 1;
    r.player_count
}

/// `FDestroyMission::Slot2` (`DestroyMission.cpp:257-481`).
pub(super) fn slot2(host: &mut Host<'_, '_>, event: &Event<'_>, state: i32) -> bool {
    let Some(me) = host.rm().map(|r| r.handler) else {
        return false;
    };
    let current = host.state_of(me);
    if state < 0 {
        if let Some(c) = controller_of(event) {
            match c.message {
                Controller::Heartbeat(_) => {
                    refresh_all_objectives(host);
                    random_mission_object(host, event);
                }
                Controller::ObjectDestroyed { .. } => {
                    if host.rm().is_some_and(|r| r.running_machines.is_empty()) {
                        random_mission_object(host, event);
                    }
                }
                Controller::GroupMemberLeft(player) => {
                    let mut i = 0;
                    while let Some(&target) = host.rm().and_then(|r| r.running_machines.get(i)) {
                        host.send_immediate(
                            me,
                            RmMessage::new(MessageType::PlayerLeft, Body::PlayerLeft { player }),
                            target,
                        );
                        i += 1;
                    }
                    random_mission_object(host, event);
                }
                Controller::PlayerSave(client) => {
                    player_save(host, client);
                    random_mission_object(host, event);
                }
                // `DockRequest` and `Msg0x25` scan the players for a ship
                // without reading the result; 0x27 and the rest go to TU47.
                _ => {
                    random_mission_object(host, event);
                }
            }
            return true;
        }
        let Some(m) = message_of(event) else {
            return false;
        };
        if m.is(MessageType::MachineStarted, current) {
            let sender = m.header.sender;
            if host.rm().is_some_and(|r| r.running_machines.is_empty()) {
                host.set_state(me, 2);
            }
            if let Some(r) = host.rm_mut() {
                erase_first(&mut r.starting_machines, sender);
                append_unique(&mut r.running_machines, sender);
            }
            return true;
        }
        if m.is(MessageType::RefreshObjectives, current) {
            if let Some(r) = host.rm_mut() {
                r.field_0x25c = 1;
            }
            refresh_all_objectives(host);
            return true;
        }
        return false;
    }
    match state {
        0 => {
            if is_enter(event) {
                // The title (`0x32fa` + the first offer's type) and the
                // mission data's description.
                let description = host
                    .rm()
                    .and_then(|r| r.data)
                    .and_then(|k| host.cx.logic.content.state.rm.missions.get(k))
                    .map(|d| (d.description.clone(), d.offers.first().map_or(0, |o| o.kind)));
                if let Some(r) = host.rm_mut() {
                    let (text, kind) = description.unwrap_or((FmtStr::new(0), 0));
                    r.title = FmtStr::new(0x32fa + u32::from(kind));
                    r.description = text;
                }
                host.send_to_me_self(me, RmMessage::plain(MessageType::Timeout), 1.1);
                return true;
            }
            if message_of(event).is_some_and(|m| m.is(MessageType::Timeout, current)) {
                host.set_state(me, 1);
                return true;
            }
            false
        }
        1 => {
            if !is_enter(event) {
                return false;
            }
            create_runtime_machines(host);
            start_next(host, me);
            true
        }
        3 => {
            if !is_enter(event) {
                return false;
            }
            on_mission_succeed(host);
            true
        }
        4 => {
            if !is_enter(event) {
                return false;
            }
            let reason = host.rm().map_or(0, |r| r.fail_reason);
            fail_mission(host, reason);
            true
        }
        2 => {
            let Some(m) = message_of(event) else {
                return false;
            };
            let sender = m.header.sender;
            if m.is(MessageType::MachineFailed, current) {
                if let Some(r) = host.rm_mut() {
                    erase_first(&mut r.running_machines, sender);
                }
                host.mark(me, sender);
                let reason = match m.body {
                    Body::MachineFailed { reason } => reason,
                    _ => 0,
                };
                if let Some(r) = host.rm_mut() {
                    r.fail_reason = reason;
                }
                host.set_state(me, 4);
                return true;
            }
            if !m.is(MessageType::MachineSucceeded, current) {
                return false;
            }
            if let Some(r) = host.rm_mut() {
                erase_first(&mut r.running_machines, sender);
            }
            host.mark(me, sender);
            let busy = host
                .rm()
                .is_some_and(|r| !r.running_machines.is_empty() || !r.starting_machines.is_empty());
            if busy {
                return true;
            }
            if host.rm().is_some_and(|r| r.pending_machines.is_empty()) {
                host.set_state(me, 3);
                return true;
            }
            start_next(host, me);
            true
        }
        _ => false,
    }
}

/// The next pending machine started: it moves to the starting list and
/// `GRmMsgStartMachine` goes to it after 0.01 s.
fn start_next(host: &mut Host<'_, '_>, me: u32) {
    let Some(next) = host.rm().and_then(|r| r.pending_machines.first().copied()) else {
        return;
    };
    if let Some(r) = host.rm_mut() {
        append_unique(&mut r.starting_machines, next);
        erase_first(&mut r.pending_machines, next);
    }
    host.send_delayed(me, RmMessage::plain(MessageType::StartMachine), 0.01, next);
}

/// `DmEraseFirst`.
fn erase_first(ids: &mut Vec<u32>, id: u32) {
    if let Some(at) = ids.iter().position(|&i| i == id) {
        ids.remove(at);
    }
}

/// `DmAppendUnique`.
fn append_unique(ids: &mut Vec<u32>, id: u32) {
    if !ids.contains(&id) {
        ids.push(id);
    }
}

/// `CreateRuntimeMachines` (`:229-249`): a machine per offer, added to the
/// group, its id appended to the pending list. A type outside 1 to 3
/// builds none, and the C++ reads its id through null (D1027).
fn create_runtime_machines(host: &mut Host<'_, '_>) {
    let Some((me, count)) = host.rm().map(|r| {
        let count = r
            .data
            .and_then(|k| host.cx.logic.content.state.rm.missions.get(k))
            .map_or(0, |d| d.offers.len());
        (r.handler, count)
    }) else {
        return;
    };
    for index in 0..count {
        let Some(key) = machine::build(host, index) else {
            continue;
        };
        if let Some(r) = host.rm_mut() {
            r.members.push(key);
        }
        sm::start_in(host, key);
        if let Some(r) = host.rm_mut() {
            append_unique(&mut r.pending_machines, key);
        }
    }
    let _ = me;
}

/// The single-player save (`:399-414`): the mission's player, in single
/// player, gets the `[RandomMission]` section unless the first running
/// machine cannot save.
fn player_save(host: &mut Host<'_, '_>, client: u32) {
    let Some((player, data, first)) = host
        .rm()
        .map(|r| (r.player, r.data, r.running_machines.first().copied()))
    else {
        return;
    };
    if !api::misc::single_player(host.cx) || client != player {
        return;
    }
    if let Some(first) = first
        && !machine::can_save(host, first)
    {
        return;
    }
    let Some(data) = data.and_then(|k| host.cx.logic.content.state.rm.missions.get(k)).cloned() else {
        return;
    };
    if let Some(file) = host.save.as_deref_mut() {
        let _ = super::super::mission::write_section(&data, file);
    }
}

/// `FRandomMissionObject::Slot2` (`RandomMissionDB.cpp:1615-1720`), called
/// with the "any state": only with players and controller events, each
/// answered.
fn random_mission_object(host: &mut Host<'_, '_>, event: &Event<'_>) -> bool {
    if host.rm().is_none_or(|r| r.players.is_empty()) {
        return false;
    }
    let Some(c) = controller_of(event) else {
        return false;
    };
    let flag_time = flag_time(host);
    match c.message {
        Controller::SystemEnter { system, player } => {
            if host.rm().is_some_and(|r| r.elapsed_time <= flag_time) {
                update_player_system_flags(host, player, system);
            }
        }
        Controller::SpaceEnter { system, player } => {
            if host.is_mapped(player) {
                // The record's objectives cleared (P3.5c5), then refreshed.
                get_or_create_record(host, player);
                refresh_objectives(host, player);
            }
            if host.rm().is_some_and(|r| r.elapsed_time <= flag_time) {
                update_player_system_flags(host, player, system);
            }
        }
        Controller::Heartbeat(dt) => {
            let single = api::misc::single_player(host.cx);
            let player = host.rm().map_or(0, |r| r.player);
            let has_ship = host
                .cx
                .logic
                .content
                .state
                .players
                .get(&player)
                .is_some_and(|s| s.ship.is_some());
            let count = (single && has_ship) || !single;
            let elapsed = host.rm().map_or(-1.0, |r| r.elapsed_time);
            if count && elapsed != -1.0 {
                if !(elapsed >= flag_time) {
                    if let Some(r) = host.rm_mut() {
                        r.elapsed_time = (f64::from(elapsed) + f64::from(dt)) as f32;
                    }
                } else {
                    if let Some(r) = host.rm_mut() {
                        r.elapsed_time = -1.0;
                    }
                    for p in host.players() {
                        if let Some(s) = host.cx.logic.content.state.players.get_mut(&p) {
                            s.flags &= 0xffff_fffb;
                        }
                    }
                }
            }
            mission_object(host, c.message);
        }
        Controller::AbortMission(player) => on_player_left_group(host, player),
        Controller::ObjectDestroyed { player, .. } => {
            if host.is_mapped(player) && decrement_player_count(host) <= 0 {
                let reason = if api::misc::single_player(host.cx) { 2 } else { 3 };
                fail_mission(host, reason);
            }
        }
        Controller::FormationLeaderChanged => {}
        Controller::PlayerLoad(player) => {
            if host.is_mapped(player) {
                rejoin_player(host, player);
            }
        }
        Controller::GroupMemberJoined(player) => add_group_member(host, player),
        Controller::PlayerLogout(player) => {
            if host.is_mapped(player) {
                remove_mission_player(host, player, 0xe);
            }
            mission_object(host, c.message);
        }
        Controller::GroupMemberLeft(player) => {
            if host.is_mapped(player) {
                remove_mission_player(host, player, 1);
            }
            mission_object(host, c.message);
        }
        other => mission_object(host, other),
    }
    true
}

/// The mission data's `FlagTime`.
fn flag_time(host: &Host<'_, '_>) -> f32 {
    host.rm()
        .and_then(|r| r.data)
        .and_then(|k| host.cx.logic.content.state.rm.missions.get(k))
        .map_or(360.0, |d| d.flag_time)
}

/// `FMissionObject::HandleMissionMessage` (`MissionObject.cpp:1496-1925`)
/// for what a random mission reaches of it: the players leaving and
/// joining, the dead players. Its saves, loads, respawns, jumpers and
/// random encounters are D1024's.
fn mission_object(host: &mut Host<'_, '_>, message: Controller) {
    let player = host.rm().map_or(0, |r| r.player);
    if !host.cx.logic.content.state.players.contains_key(&player) {
        return;
    }
    match message {
        Controller::PlayerLogout(gone) | Controller::GroupMemberLeft(gone) => {
            if let Some(r) = host.rm_mut() {
                r.records.remove(&gone);
                erase_first(&mut r.dead_players, gone);
            }
            if player == gone {
                // `Slot15` is false: the first remaining record's player.
                let next = host.rm().and_then(|r| r.records.iter().next().copied()).unwrap_or(0);
                set_player(host.cx, host.mission, next);
            }
            let now = host.rm().map_or(0, |r| r.player);
            if !host.cx.logic.content.state.players.contains_key(&now)
                && let Some(r) = host.rm_mut()
            {
                r.pending_destroy = true;
            }
        }
        Controller::GroupMemberJoined(member) => {
            if api::player::is_group_member(host.cx, player, member) {
                join_mission(host, member, player);
            }
        }
        Controller::Docked { ship, .. } => {
            if let Some(dead) = crate::content::players::find_by_ship(&host.cx.logic.content.state.players, ship)
                && let Some(r) = host.rm_mut()
            {
                r.dead_players.push(dead);
            }
        }
        Controller::SpaceEnter { player: p, .. } => {
            if let Some(r) = host.rm_mut() {
                erase_first(&mut r.dead_players, p);
            }
        }
        _ => {}
    }
}

/// `FContentPlayerState::JoinMission` (`PlayerState.cpp:799-811`): the
/// live mission, `PlayerMissionChanged` to every controller, the mission
/// id.
fn join_mission(host: &mut Host<'_, '_>, player: u32, sub: u32) {
    let mission = host.mission;
    let Some(state) = host.cx.logic.content.state.players.get_mut(&player) else {
        return;
    };
    state.live_mission = mission.0;
    let mut message = ControllerMessage::PlayerMissionChanged { mission, player };
    let _ = host.cx.dispatch(ControllerId::BROADCAST, &mut message);
    let sub = if sub == 0 { player } else { sub };
    api::player::set_msn_id(host.cx, player, mission.0, sub, false, 0);
}

/// `EndLiveMission(_, VnpcState, bGroup)` (`PlayerState.cpp:718-743`).
fn end_live_mission(host: &mut Host<'_, '_>, player: u32, vnpc_state: u32, group: bool) {
    if let Some(state) = host.cx.logic.content.state.players.get_mut(&player) {
        for v in &mut state.vnpcs {
            if v[3] == 1 {
                v[3] = vnpc_state;
            }
        }
    }
    let members = if group {
        api::player::group_members(host.cx, player).unwrap_or_default()
    } else {
        vec![player]
    };
    for member in members {
        let Some(state) = host.cx.logic.content.state.players.get_mut(&member) else {
            continue;
        };
        state.live_mission = 0;
        let mut message = ControllerMessage::PlayerMissionChanged {
            mission: ControllerId(0),
            player: member,
        };
        let _ = host.cx.dispatch(ControllerId::BROADCAST, &mut message);
        api::player::set_msn_id(host.cx, member, 0, member, false, 0);
    }
}

/// `RefreshAllObjectives` (slot 37).
fn refresh_all_objectives(host: &mut Host<'_, '_>) {
    for player in host.players() {
        refresh_objectives(host, player);
    }
}

/// `RefreshObjectives` (slot 38, `:1300-1316`): the record, then the path
/// objectives towards the mission's system, which draw `rand()` where the
/// C++ draws it; the docked and player-count objectives and the send are
/// P3.5c5's (D1021).
fn refresh_objectives(host: &mut Host<'_, '_>, player: u32) {
    if !get_or_create_record(host, player) {
        return;
    }
    update_path_objectives(host, player);
}

/// `Slot08`: the first running machine's system, else the destination's.
fn slot08(host: &Host<'_, '_>) -> u32 {
    let Some(r) = host.rm() else { return 0 };
    match r.running_machines.first() {
        Some(&m) => machine::system_id(host, m),
        None => r.dest_system,
    }
}

/// `FRandomMissionObject::UpdatePathObjectives` (slot 40, `:1367-1434`):
/// for a player with a position away from the origin in another system
/// than the mission's, not travelling a lane, the path's first text id is
/// drawn (`RmRandomBelow(5)`).
fn update_path_objectives(host: &mut Host<'_, '_>, player: u32) {
    let target = slot08(host);
    if target == 0 {
        return;
    }
    if !host.cx.logic.content.state.players.contains_key(&player) {
        return;
    }
    let at = super::player_position(host.cx, player);
    let axis = |v: f32| f64::from(v) * f64::from(v) <= 1.0;
    if axis(at.x) && axis(at.y) && axis(at.z) {
        return;
    }
    let Some(state) = host.cx.logic.content.state.players.get(&player) else {
        return;
    };
    let Some(system) = state.system else { return };
    if system == target {
        return;
    }
    if state.ship.as_ref().is_some_and(|s| s.lane_entry != 0) {
        return;
    }
    let _ = host.cx.world.rng().rand();
}

/// `UpdatePlayerSystemFlags` (`:1505-1531`).
fn update_player_system_flags(host: &mut Host<'_, '_>, player: u32, system: u32) {
    if !host.is_mapped(player) {
        return;
    }
    let (dest, base) = host
        .rm()
        .and_then(|r| r.data)
        .and_then(|k| host.cx.logic.content.state.rm.missions.get(k))
        .map_or((0, 0), |d| (d.dest_sys, d.offer_base));
    let offer_system = api::misc::base_system(host.cx, base).unwrap_or(system);
    let Some(s) = host.cx.logic.content.state.players.get_mut(&player) else {
        return;
    };
    if s.ship.is_none() {
        return;
    }
    if system == dest {
        s.flags |= 0xc;
    } else if system == offer_system {
        s.flags |= 4;
    } else {
        s.flags &= 0xffff_fff3;
    }
}

/// `RejoinPlayer` (`:1534-1554`).
fn rejoin_player(host: &mut Host<'_, '_>, player: u32) {
    join_mission(host, player, player);
    if let Some(r) = host.rm_mut() {
        r.player_map.insert(player);
        r.players = r.player_map.iter().copied().collect();
        // An existing record takes the state and is dropped (`RemovePlayer`).
        r.records.remove(&player);
    }
}

/// `OnPlayerLeftGroup` (`:1557-1571`): a player in a group leaves it;
/// alone, the mission is aborted.
fn on_player_left_group(host: &mut Host<'_, '_>, player: u32) {
    if !host.is_mapped(player) {
        return;
    }
    let members = api::player::group_members(host.cx, player).unwrap_or_default();
    if members.len() > 1 {
        let _ = api::player::remove_from_group(host.cx, player);
    } else {
        fail_mission(host, 1);
    }
}

/// `AddGroupMember` (`:1574-1612`).
fn add_group_member(host: &mut Host<'_, '_>, player: u32) {
    let Some(first) = host.rm().and_then(|r| {
        if r.player_map.contains(&player) {
            return None;
        }
        r.players.first().copied()
    }) else {
        return;
    };
    if !api::player::is_group_member(host.cx, first, player) {
        return;
    }
    let name = api::player::name(host.cx, player).unwrap_or_default();
    if let Some(r) = host.rm_mut() {
        r.player_map.insert(player);
        r.players.push(player);
        add_player_name(r, name);
    }
    join_mission(host, player, 0);
    get_or_create_record(host, player);
    let description = host.rm().map(|r| r.description.clone()).unwrap_or_default();
    let _ = api::player::display_mission_message(host.cx, player, &FmtStr::new(0), 0xa, false);
    let _ = api::player::display_mission_message(host.cx, player, &description, 8, false);
}

/// The music of a mission's end (`RmMissionMusic`).
fn music(nickname: &[u8]) -> Tryptich {
    Tryptich {
        space: 0,
        danger: 0,
        battle: 0,
        override_music: api::misc::nickname_id(nickname),
        flag: 1,
        fade: MUSIC_FADE,
    }
}

/// The offer group's event rate.
fn event_rate(host: &Host<'_, '_>, event: usize) -> (u32, f32) {
    let group = host
        .rm()
        .and_then(|r| r.data)
        .and_then(|k| host.cx.logic.content.state.rm.missions.get(k))
        .map_or(u32::MAX, |d| d.offer_group as u32);
    (group, host.cx.logic.content.tables.faction_event_rate(group, event))
}

/// `ChangePlayerReputation` (slot 41, `:1439-1455`): once per player name
/// and group unless forced.
fn change_player_reputation(host: &mut Host<'_, '_>, player: u32, group: u32, delta: f32, force: bool, empathy: bool) {
    let name = api::player::name(host.cx, player).unwrap_or_default();
    if !force && has_reputation_entry(host, &name, group) {
        return;
    }
    if let Some(r) = host.rm_mut() {
        // The multimap's order: by name, then insertion.
        let at = r
            .reputation_changes
            .iter()
            .position(|(n, _)| *n > name)
            .unwrap_or(r.reputation_changes.len());
        r.reputation_changes.insert(at, (name, group));
    }
    let vibe = api::player::rep(host.cx, player).unwrap_or(0);
    apply_faction_reputation_change(host.cx, vibe, delta, group, empathy);
}

/// `HasReputationEntry` (`:1486-1502`): from the name's first entry to the
/// end of the multimap, any entry of the group.
fn has_reputation_entry(host: &Host<'_, '_>, name: &[u16], group: u32) -> bool {
    let Some(r) = host.rm() else { return false };
    let Some(start) = r.reputation_changes.iter().position(|(n, _)| n.as_slice() >= name) else {
        return false;
    };
    if r.reputation_changes[start].0.as_slice() != name {
        return false;
    }
    r.reputation_changes[start..].iter().any(|&(_, g)| g == group)
}

/// `ChangeGroupReputation` (slot 43): slot 41 for every mapped player.
fn change_group_reputation(host: &mut Host<'_, '_>, group: u32, delta: f32, empathy: bool) {
    let players: Vec<u32> = host.rm().map(|r| r.player_map.iter().copied().collect()).unwrap_or_default();
    for p in players {
        change_player_reputation(host, p, group, delta, false, empathy);
    }
}

/// `ChangeGroupReputationAlways` (slot 44): with empathy, always.
fn change_group_reputation_always(host: &mut Host<'_, '_>, group: u32, delta: f32) {
    let players: Vec<u32> = host.rm().map(|r| r.player_map.iter().copied().collect()).unwrap_or_default();
    for p in players {
        let vibe = api::player::rep(host.cx, p).unwrap_or(0);
        apply_faction_reputation_change(host.cx, vibe, delta, group, true);
    }
}

/// A count of a random mission's end by type.
fn count(map: &mut std::collections::BTreeMap<u8, i32>, kind: u8) {
    *map.entry(kind).or_insert(0) += 1;
}

/// `FDestroyMission::FailMission` (slot 35, `:198-209`) over TU47's
/// (`RandomMissionDB.cpp:1230-1258`): the reason's text to the first
/// player; every player's flags cleared, the end counted, the failure
/// music; the offer group's reputation; the records' objectives cleared;
/// `EndMission`; the player's live mission ended.
pub(super) fn fail_mission(host: &mut Host<'_, '_>, reason: i32) {
    trace::mission(host.cx, "fail", i64::from(reason));
    let text = FmtStr::new(reason_ids(reason));
    if let Some(first) = host.players().first().copied() {
        let _ = api::player::display_mission_message(host.cx, first, &text, 2, true);
    }
    let failure = music(b"music_failure");
    let kind = host.rm().map_or(0, |r| r.kind);
    for p in host.players() {
        let Some(s) = host.cx.logic.content.state.players.get_mut(&p) else {
            continue;
        };
        s.flags = 0;
        if reason == 1 {
            count(&mut s.stats.random_missions_aborted, kind);
        } else {
            count(&mut s.stats.random_missions_failed, kind);
        }
        if s.ship.is_some() {
            api::audio::set_music(host.cx, p, &failure);
        }
    }
    let (group, rate) = event_rate(host, if reason == 1 { 3 } else { 2 });
    change_group_reputation(host, group, rate, reason != 1);
    end_mission(host);
    let player = host.rm().map_or(0, |r| r.player);
    let live = host
        .cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .is_some_and(|s| s.live_mission == host.mission.0);
    if live {
        end_live_mission(host, player, 3, true);
    }
}

/// `FDestroyMission::RemoveMissionPlayer` (slot 36, `:213-219`) over
/// TU47's (`:1261-1289`).
fn remove_mission_player(host: &mut Host<'_, '_>, player: u32, reason: i32) {
    trace::mission(host.cx, "remove_player", i64::from(player));
    let _ = api::player::display_mission_message(host.cx, player, &FmtStr::new(reason_ids(reason)), 2, false);
    let (group, rate) = event_rate(host, if reason == 1 { 3 } else { 2 });
    change_player_reputation(host, player, group, rate, false, true);
    if let Some(r) = host.rm_mut() {
        r.records.remove(&player);
        r.player_map.remove(&player);
        r.players = r.player_map.iter().copied().collect();
    }
    let next = host.rm().and_then(|r| r.player_map.iter().next().copied()).unwrap_or(0);
    if let Some(r) = host.rm_mut() {
        r.player = next;
    }
    let kind = host.rm().map_or(0, |r| r.kind);
    let Some(s) = host.cx.logic.content.state.players.get_mut(&player) else {
        // The C++ reads the missing state (D1027).
        return;
    };
    s.flags = 0;
    if reason == 1 {
        count(&mut s.stats.random_missions_aborted, kind);
        end_live_mission(host, player, 4, false);
        return;
    }
    if reason == 0xe {
        return;
    }
    count(&mut s.stats.random_missions_failed, kind);
    end_live_mission(host, player, 3, false);
}

/// `FDestroyMission::EndMission` (slot 22, `:179-184`): every running
/// machine's `Shutdown`, then the mission object's `EndMission`
/// (`MissionObject.cpp:1292-1322`): the records removed, the texts
/// cleared, the teardown asked for.
fn end_mission(host: &mut Host<'_, '_>) {
    trace::mission(host.cx, "end", 0);
    let running = host.rm().map(|r| r.running_machines.clone()).unwrap_or_default();
    for m in running {
        machine::shutdown(host, m);
    }
    if let Some(r) = host.rm_mut() {
        r.records.clear();
        r.title = FmtStr::new(0);
        r.description = FmtStr::new(0);
        r.pending_destroy = true;
    }
}

/// `FDestroyMission::OnMissionSucceed` (slot 23, `:187-191`) over TU47's
/// (`RandomMissionDB.cpp:1139-1153`) and TU38's
/// (`MissionObject.cpp:1932-1945`): the reward, the completion counted with
/// the victory music, the offer group's reputation always; the reward
/// split over the group, the records' objectives cleared, `EndMission`,
/// the group's live mission ended.
fn on_mission_succeed(host: &mut Host<'_, '_>) {
    let reward = host
        .rm()
        .and_then(|r| r.data)
        .and_then(|k| host.cx.logic.content.state.rm.missions.get(k))
        .map_or(0, |d| d.reward as u32);
    trace::mission(host.cx, "succeed", i64::from(reward));
    if let Some(r) = host.rm_mut() {
        r.reward = reward;
    }
    let victory = music(b"music_victory");
    let kind = host.rm().map_or(0, |r| r.kind);
    for p in host.players() {
        let Some(s) = host.cx.logic.content.state.players.get_mut(&p) else {
            continue;
        };
        count(&mut s.stats.random_missions_completed, kind);
        if s.ship.is_some() {
            api::audio::set_music(host.cx, p, &victory);
        }
    }
    let (group, rate) = event_rate(host, 1);
    change_group_reputation_always(host, group, rate);
    let player = host.rm().map_or(0, |r| r.player);
    if !host.cx.logic.content.state.players.contains_key(&player) {
        end_mission(host);
        return;
    }
    let size = api::player::group_size(host.cx, player).max(1);
    api::player::reward_group(host.cx, player, (reward / size) as i32);
    end_mission(host);
    end_live_mission(host, player, 2, true);
}
