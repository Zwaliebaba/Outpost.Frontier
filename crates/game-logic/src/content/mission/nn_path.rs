//! The NN path's run time (`FMissionNnPath`, `Content/MissionObject.cpp:
//! 4286-4597`; work package P3.5c5): `Act_NNPath`'s route to a point or a
//! labelled object, laid out as type 5 objectives through the player's
//! system's trade lanes and sent in place of the mission's objectives.
//!
//! - [`handle_message`] (`HandleMessage`, `:4311-4419`), from the script
//!   mission's tail while the path is active and the mission is its
//!   player's live mission or the player has none: the first heartbeat
//!   starts its 1 s timer; the timer, a launch, entering the path's system
//!   and the player's mission change rebuild and send the path (and start
//!   the next timer); a lane (`TradelaneEnter`) sends it without its first
//!   objective, docked state 4 until `TradelaneExit` or `BaseExit`;
//!   leaving a system drops it.
//! - [`rebuild`] (`Rebuild`, `:4461-4525`): the destination (the target's
//!   position in the path's system, else the next jump towards it), the
//!   legs from the player's position ([`space_paths::build_nn_path`] to
//!   within the target's radius and 200), and the objectives
//!   ([`objectives::build_path_objectives`]).
//! - [`send_objectives`] (`SendObjectives`, `:4530-4575`).
//! - [`reset`] (`Reset`, `:4578-4597`).
//!
//! Each timer takes the next of Content's unique ids (`GContentNextUniqueId`)
//! and is set on the mission's controller.

use neuron_core::math::Vector;
use neuron_server::controller::ControllerId;

use super::objectives::{self, script_header};
use super::runtime::{ID_MASK, label_object_id, script, script_mut};
use super::save::NnPath;
use crate::api;
use crate::api::player::MissionObjective;
use crate::content::space_paths;
use crate::events::ControllerMessage;
use crate::server::Cx;

/// `Reset` (`MissionObject.cpp:4578-4597`): inactive, the objectives and
/// legs gone, the target, label and radius cleared, both ids 1, the
/// position, system, state, arrival and timer 0.
pub fn reset(path: &mut NnPath) {
    *path = NnPath::default();
}

/// The `StartNnPathTimer` block (`MissionObject.cpp:4294-4304`): the next
/// unique id, a 1 s timer on the mission.
fn start_timer(cx: &mut Cx<'_>, id: ControllerId) {
    let timer = cx.logic.content.state.ids.take();
    if let Some(s) = script_mut(cx, id) {
        s.nn_path.timer_id = timer;
    }
    let _ = api::controller::set_timer(cx, id, timer, 0, 1.0);
}

/// `GetNextHopConnectionObject(from, to, kind)` over Content's system
/// paths.
pub fn next_hop(cx: &mut Cx<'_>, from: u32, to: u32, kind: i32) -> u32 {
    let paths = std::mem::take(&mut cx.logic.content.state.system_paths);
    let take = crate::content::NoDispatch::begin(&cx.logic.content);
    let hop = crate::content::system_paths::next_hop_connection_object(cx, &paths, from, to, kind);
    take.end(&cx.logic.content);
    cx.logic.content.state.system_paths = paths;
    hop
}

/// `FMissionNnPath::HandleMessage(message)` (`MissionObject.cpp:4311-4419`)
/// of script mission `id`'s path. Returns the path's state.
pub fn handle_message(cx: &mut Cx<'_>, id: ControllerId, message: &ControllerMessage<'_>) -> i32 {
    use ControllerMessage as M;
    let Some(s) = script(cx, id) else { return 0 };
    let (active, state, owner) = (s.nn_path.active, s.nn_path.state, s.object.player);
    if !active {
        return state;
    }
    let owner_set = owner & ID_MASK != 0;
    let player_ship = cx
        .logic
        .content
        .state
        .players
        .get(&owner)
        .map(crate::content::players::PlayerState::ship_id);
    let set_state = |cx: &mut Cx<'_>, value: i32| {
        if let Some(s) = script_mut(cx, id) {
            s.nn_path.state = value;
        }
    };
    let mut launched = false;
    match *message {
        M::BaseExit { player, .. } => {
            if state == 3 && owner == player && owner_set {
                set_state(cx, 4);
            }
            return path_state(cx, id);
        }
        M::TradelaneEnter { ship, .. } => {
            let Some(own) = player_ship else {
                return state;
            };
            if state != 3 || ship != own || ship & ID_MASK == 0 {
                return state;
            }
            set_state(cx, 4);
            send_objectives(cx, id, true, false);
            return path_state(cx, id);
        }
        M::TradelaneExit { ship, .. } => {
            let Some(own) = player_ship else {
                return state;
            };
            if state != 4 || ship != own || ship & ID_MASK == 0 {
                return state;
            }
            start_timer(cx, id);
            set_state(cx, 3);
            return 3;
        }
        M::Heartbeat { .. } => {
            if state != 1 {
                return state;
            }
            start_timer(cx, id);
            set_state(cx, 2);
            return 2;
        }
        M::SpaceEnter { player, .. } => {
            if owner != player || !owner_set {
                return state;
            }
            if state == 4 {
                start_timer(cx, id);
                set_state(cx, 3);
            }
            let system = cx
                .logic
                .content
                .state
                .players
                .get(&owner)
                .and_then(|p| p.system)
                .unwrap_or(0);
            if let Some(s) = script(cx, id)
                && system == s.nn_path.system_id
            {
                let target = label_object_id(cx, s, &s.nn_path.label);
                if let Some(s) = script_mut(cx, id) {
                    s.nn_path.target_object_id = target;
                }
            }
            launched = true;
        }
        M::SpaceExit { player, .. } => {
            if owner == player
                && owner_set
                && let Some(s) = script_mut(cx, id)
            {
                s.nn_path.target_object_id = 0;
            }
            return state;
        }
        M::SystemExit { .. } => {
            if let Some(s) = script_mut(cx, id) {
                s.nn_path.objectives.clear();
                s.nn_path.legs.clear();
            }
            send_objectives(cx, id, false, false);
            return path_state(cx, id);
        }
        M::SystemEnter { system, player, .. } => {
            if player != owner || player_ship.is_none() {
                return state;
            }
            let Some(s) = script(cx, id) else {
                return state;
            };
            if system != s.nn_path.system_id {
                if let Some(s) = script_mut(cx, id) {
                    s.nn_path.target_object_id = 0;
                }
                return state;
            }
            if !s.nn_path.label.is_empty() {
                let target = label_object_id(cx, s, &s.nn_path.label);
                if let Some(s) = script_mut(cx, id) {
                    s.nn_path.target_object_id = target;
                }
            }
            launched = true;
        }
        M::TimerExpired { param1, .. } => {
            let timer = script(cx, id).map_or(0, |s| s.nn_path.timer_id);
            if param1 != timer || state == 4 {
                return state;
            }
        }
        M::PlayerMissionChanged { mission, player } => {
            if player != owner {
                return state;
            }
            if mission != id && mission.0 & ID_MASK != 0 {
                return state;
            }
        }
        _ => return state,
    }
    rebuild(cx, id);
    send_objectives(cx, id, false, launched);
    start_timer(cx, id);
    path_state(cx, id)
}

fn path_state(cx: &Cx<'_>, id: ControllerId) -> i32 {
    script(cx, id).map_or(0, |s| s.nn_path.state)
}

/// `FMissionNnPath::Rebuild` (`MissionObject.cpp:4461-4525`): nothing
/// without the player's state or system record. In the path's system the
/// destination is the path's position, which a player with a ship takes
/// from the labelled target (resolved once, its radius read then;
/// `GetLocation` of a target of 0 fails and keeps the position); in
/// another system it is the next jump towards it (a gate, else any
/// connection; the origin when there is none). The legs then go from the
/// player's position to within the target's radius and 200; one leg or
/// none in the path's system shows the target's ids and, the first time,
/// arrives (state 5). The objectives are laid out with ids (travel, ids,
/// ids) and types (8, 9, 8).
pub fn rebuild(cx: &mut Cx<'_>, id: ControllerId) {
    let Some(s) = script(cx, id) else { return };
    let player = s.object.player;
    let Some(state) = cx.logic.content.state.players.get(&player) else {
        return;
    };
    let has_ship = state.ship.is_some();
    let Some(system) = state.system else { return };
    if !cx.logic.content.state.systems.contains_key(&system) {
        return;
    }
    let path_system = s.nn_path.system_id;
    let mut target = 0;
    let mut destination = Vector::ZERO;
    if system == path_system {
        if has_ship && !s.nn_path.label.is_empty() {
            let mut target_id = s.nn_path.target_object_id;
            let mut radius = s.nn_path.target_radius;
            if target_id & ID_MASK == 0 {
                target_id = label_object_id(cx, s, &s.nn_path.label);
                if target_id & ID_MASK != 0
                    && let Ok((r, _)) = api::space_obj::radius(cx, target_id)
                {
                    radius = r;
                }
            }
            target = target_id;
            let position = api::space_obj::location(cx, target).map(|(p, _)| p).ok();
            if let Some(s) = script_mut(cx, id) {
                s.nn_path.target_object_id = target_id;
                s.nn_path.target_radius = radius;
                if let Some(p) = position {
                    s.nn_path.position = p;
                }
            }
        }
        destination = script(cx, id).map_or(Vector::ZERO, |s| s.nn_path.position);
    } else {
        target = next_hop(cx, system, path_system, 1);
        if target & ID_MASK == 0 {
            target = next_hop(cx, system, path_system, 0);
        }
        if let Some(p) = crate::content::system_paths::connection_position(cx, system, target) {
            destination = p;
        }
    }
    let Some(s) = script_mut(cx, id) else { return };
    s.nn_path.objectives.clear();
    s.nn_path.legs.clear();
    // `TargetRadius + 200.0f`, a float sum.
    let range = (f64::from(s.nn_path.target_radius) + 200.0) as f32;
    let from = crate::content::random_mission::runtime::player_position(cx, player);
    let legs = cx
        .logic
        .content
        .state
        .systems
        .get(&system)
        .map(|record| space_paths::build_nn_path(record, from, destination, range).1)
        .unwrap_or_default();
    let Some(s) = script_mut(cx, id) else { return };
    let path = &mut s.nn_path;
    let travel = path.travel_ids;
    let mut ids = travel;
    if legs.len() <= 1 && system == path.system_id {
        ids = path.target_ids;
        if !path.arrived {
            path.arrived = true;
            path.state = 5;
        }
    }
    let mut list = std::mem::take(&mut path.objectives);
    path.legs.clone_from(&legs);
    let owner = cx
        .logic
        .content
        .state
        .players
        .contains_key(&player)
        .then_some(player);
    objectives::build_path_objectives(
        cx,
        system,
        &legs,
        &mut list,
        [travel, ids, ids],
        target,
        [8, 9, 8],
        owner,
    );
    if let Some(s) = script_mut(cx, id) {
        s.nn_path.objectives = list;
    }
}

/// `FMissionNnPath::SendObjectives(bSkipFirst, bLaunched)` (`MissionObject.
/// cpp:4530-4575`): the path's objectives (its first left out with
/// `skip_first`; none to send sends nothing), each described, the first
/// one sent state 0x3a while the path starts or arrives (state 2 or 5),
/// 0x1a after a launch (either makes the state 3), else 0xa, the later
/// ones 9; sent to the mission's player with the mission's title, type
/// and description, without the records' gates.
pub fn send_objectives(cx: &mut Cx<'_>, id: ControllerId, skip_first: bool, launched: bool) {
    let Some(header) = script_header(cx, id) else {
        return;
    };
    let Some(s) = script_mut(cx, id) else { return };
    let count = s.nn_path.objectives.len() as i64 - i64::from(skip_first);
    if count <= 0 {
        return;
    }
    let mut list = std::mem::take(&mut s.nn_path.objectives);
    let mut state = s.nn_path.state;
    let label =
        move |cx: &Cx<'_>, l: &[u8]| script(cx, id).map_or(0, |s| label_object_id(cx, s, l));
    let mut sent = Vec::with_capacity(list.len());
    for (n, o) in list.iter_mut().enumerate() {
        if skip_first && n == 0 {
            continue;
        }
        if !objectives::build_description(cx, o, header.player, &label) {
            continue;
        }
        let later = !sent.is_empty();
        o.state = if state == 2 || state == 5 {
            state = 3;
            if later { 9 } else { 0x3a }
        } else if launched {
            state = 3;
            if later { 9 } else { 0x1a }
        } else if later {
            9
        } else {
            0xa
        };
        sent.push(MissionObjective {
            kind: o.state,
            description: o.description.clone(),
        });
    }
    if let Some(s) = script_mut(cx, id) {
        s.nn_path.objectives = list;
        s.nn_path.state = state;
    }
    let _ = api::player::set_mission_objectives(
        cx,
        header.player,
        header.controller,
        &sent,
        &header.title,
        header.kind,
        &header.description,
    );
}
