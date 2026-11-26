//! A random mission's players' records and their objectives (work package
//! P3.5c5, D1021): TU38's records (`FMissionPlayer`, `MissionObject.cpp:
//! 1052-1290`) as `FRandomMissionObject` keeps them
//! (`RandomMissionDB.cpp:1036-1449`), the objectives it adds itself (the
//! docked marker, the path to the mission's system and the player count),
//! and the sends, over the script missions' objective run time
//! ([`crate::content::mission::objectives`]).
//!
//! The C++'s comparisons are kept as written: an x87 `FCOMP` branch is
//! false for a NaN on either side.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use neuron_core::math::Vector;

use super::message::Controller;
use super::{Host, player_position};
use crate::api;
use crate::content::mission::object::MissionPlayer;
use crate::content::mission::objective::Objective;
use crate::content::mission::objectives::{self, Header};
use crate::content::mission::runtime::ID_MASK;
use crate::content::space_paths;

/// `GRmPathReachSq` (`RandomMissionDB.cpp:988`): a path objective within
/// 1000 is reached.
const PATH_REACH_SQ: f64 = 1_000_000.0;

/// The docked objective's ids (`0x3319`).
pub(super) const DOCKED_IDS: i32 = 0x3319;

/// `RmHasPosition` / `RmIsAwayFromOrigin`: some coordinate's square above 1.
pub(super) fn has_position(at: Vector) -> bool {
    let (x, y, z) = (f64::from(at.x), f64::from(at.y), f64::from(at.z));
    !(x * x <= 1.0) || !(y * y <= 1.0) || !(z * z <= 1.0)
}

/// `RmRandomBelow(range)` (`RandomMissionDB.cpp:1004-1008`): one `rand()`.
fn random_below(host: &mut Host<'_, '_>, range: i32) -> i32 {
    let product = host.cx.world.rng().rand().wrapping_mul(range);
    (product + ((product >> 31) & 0x7fff)) >> 15
}

/// The mission's part of a record's send: its controller and player, its
/// title, type and description.
pub(super) fn header(host: &Host<'_, '_>) -> Option<Header> {
    let r = host.rm()?;
    Some(Header {
        controller: host.mission.0,
        player: r.player,
        title: r.title.clone(),
        kind: r.kind,
        description: r.description.clone(),
    })
}

/// `Mission->PlayerState` for an objective the mission makes: its player,
/// when the player has a state.
pub(super) fn owner(host: &Host<'_, '_>) -> Option<u32> {
    let player = host.rm()?.player;
    host.cx
        .logic
        .content
        .state
        .players
        .contains_key(&player)
        .then_some(player)
}

/// Runs `f` on record `player`, taken out of the mission for the call.
pub(super) fn with_record<R>(
    host: &mut Host<'_, '_>,
    player: u32,
    f: impl FnOnce(&mut Host<'_, '_>, &mut MissionPlayer) -> R,
) -> Option<R> {
    let mut record = host.rm_mut()?.records.remove(&player)?;
    let result = f(host, &mut record);
    if let Some(r) = host.rm_mut() {
        r.records.insert(player, record);
    }
    Some(result)
}

/// `FMissionPlayer::SendObjectives` of record `player`. A random mission's
/// own objectives have no labels.
pub(super) fn send(host: &mut Host<'_, '_>, player: u32) {
    let Some(header) = header(host) else { return };
    let restart = with_record(host, player, |host, record| {
        objectives::send_objectives(host.cx, record, &header, &|_, _| 0)
    });
    if restart == Some(true)
        && let Some(r) = host.rm_mut()
    {
        r.objective_timer = 0.0;
    }
}

/// `FMissionObject::UpdateObjectiveTimer(message)` (`MissionObject.cpp:
/// 1339-1377`) of a random mission, the mission object's tail while it is
/// its player's live mission: the player's `BaseExit` stops Content's
/// shared timer and `SpaceEnter` restarts it; a heartbeat while it runs
/// adds the frame time, and at 180 s every record is sent again as it is.
pub(super) fn update_timer(host: &mut Host<'_, '_>, message: Controller) {
    let Some(player) = host.rm().map(|r| r.player) else {
        return;
    };
    match message {
        Controller::BaseExit(p) => {
            if p == player && player & ID_MASK != 0 {
                host.cx.logic.content.state.objective_timer_running = false;
            }
        }
        Controller::SpaceEnter { player: p, .. } => {
            if p == player && player & ID_MASK != 0 {
                host.cx.logic.content.state.objective_timer_running = true;
            }
        }
        Controller::Heartbeat(dt) => {
            if !host.cx.logic.content.state.objective_timer_running {
                return;
            }
            let Some(r) = host.rm_mut() else { return };
            let sum = f64::from(dt) + f64::from(r.objective_timer);
            r.objective_timer = sum as f32;
            if !(sum >= objectives::TIMER_PERIOD) {
                return;
            }
            let keys: Vec<u32> = r.records.keys().copied().collect();
            for p in keys {
                let Some(record) = host.rm_mut().and_then(|r| r.records.get_mut(&p)) else {
                    continue;
                };
                record.set_resend(true);
                send(host, p);
                if let Some(record) = host.rm_mut().and_then(|r| r.records.get_mut(&p)) {
                    record.set_resend(false);
                }
            }
        }
        _ => {}
    }
}

/// `SendPlaceholderObjective` of record `player`.
pub(super) fn send_placeholder(host: &mut Host<'_, '_>, player: u32) {
    let Some(header) = header(host) else { return };
    let owner = owner(host);
    with_record(host, player, |host, record| {
        objectives::send_placeholder(host.cx, record, &header, owner)
    });
}

/// `RemovePlayer(FindPlayer(player))`: the record's emptied objectives
/// sent, the record gone.
pub(super) fn remove(host: &mut Host<'_, '_>, player: u32) {
    let Some(record) = host.rm_mut().and_then(|r| r.records.get_mut(&player)) else {
        return;
    };
    record.objectives.clear();
    send(host, player);
    if let Some(r) = host.rm_mut() {
        r.records.remove(&player);
    }
}

/// `ClearObjectives`: every record's objectives deleted and the empty list
/// sent.
pub(super) fn clear_all(host: &mut Host<'_, '_>) {
    let keys: Vec<u32> = host
        .rm()
        .map(|r| r.records.keys().copied().collect())
        .unwrap_or_default();
    for player in keys {
        if let Some(record) = host.rm_mut().and_then(|r| r.records.get_mut(&player)) {
            record.objectives.clear();
        }
        send(host, player);
    }
}

/// `FRandomMissionObject::UpdateDockedObjective` (slot 39,
/// `RandomMissionDB.cpp:1338-1366`): a player without a ship gets a nav
/// marker at his position (ids `0x3319`, state 0x10) at the front, once;
/// with a ship it goes. Whether the list changed.
pub(super) fn update_docked(host: &mut Host<'_, '_>, player: u32) -> bool {
    let has_ship = host
        .cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .is_some_and(|s| s.ship.is_some());
    let front_docked = host
        .rm()
        .and_then(|r| r.records.get(&player))
        .and_then(|rec| rec.objectives.first())
        .is_some_and(|o| o.ids == DOCKED_IDS);
    if has_ship {
        if front_docked && let Some(rec) = host.rm_mut().and_then(|r| r.records.get_mut(&player)) {
            rec.objectives.remove(0);
            return true;
        }
        return false;
    }
    if front_docked {
        return false;
    }
    let Some(system) = host
        .cx
        .logic
        .content
        .state
        .players
        .get(&player)
        .map(|s| s.system.unwrap_or(0))
    else {
        return false;
    };
    let at = player_position(host.cx, player);
    if !has_position(at) {
        return false;
    }
    let mut o = Objective::of(2, owner(host));
    o.hidden = false;
    o.ids = DOCKED_IDS;
    o.state = 0x10;
    o.marker = [system, 0, at.x.to_bits(), at.y.to_bits(), at.z.to_bits()];
    let Some(rec) = host.rm_mut().and_then(|r| r.records.get_mut(&player)) else {
        return false;
    };
    rec.objectives.insert(0, o);
    true
}

/// `FRandomMissionObject::UpdatePlayerCountObjective` (slot 31,
/// `RandomMissionDB.cpp:1171-1201`): with more than one player name, the
/// first type 8 objective shows the count less one (flags 0x50 when it
/// changes; none while the count is 0 or less), or a new one (state 0x58,
/// ids `0x331a`) is added. Whether the list changed, or `changed` as it was.
pub(super) fn update_player_count(host: &mut Host<'_, '_>, player: u32, changed: bool) -> bool {
    let Some(r) = host.rm() else { return changed };
    if r.player_names.len() == 1 {
        return changed;
    }
    let (count, header_player) = (r.player_count, r.player);
    let owner = owner(host);
    let Some(mut list) = host
        .rm_mut()
        .and_then(|r| r.records.get_mut(&player))
        .map(|rec| std::mem::take(&mut rec.objectives))
    else {
        return changed;
    };
    let result = if let Some(o) = list.iter_mut().find(|o| o.kind == 8) {
        if count > 0 && o.value != (count - 1) as u32 {
            o.value = (count - 1) as u32;
            o.state |= 0x50;
            objectives::build_description(host.cx, o, header_player, &|_, _| 0);
            true
        } else {
            changed
        }
    } else {
        let mut o = Objective::of(8, owner);
        o.state = 0x58;
        o.hidden = false;
        o.ids = 0x331a;
        o.value = (count - 1) as u32;
        objectives::build_description(host.cx, &mut o, header_player, &|_, _| 0);
        list.push(o);
        true
    };
    if let Some(rec) = host.rm_mut().and_then(|r| r.records.get_mut(&player)) {
        rec.objectives = list;
    }
    result
}

/// `FRandomMissionObject::UpdatePathObjectives` (slot 40,
/// `RandomMissionDB.cpp:1376-1434`), towards the mission's system
/// (`target`, slot 8; nothing for 0). The player needs a state, a position
/// and a system record outside the mission's system. On a lane, once per
/// trip, a first path objective within 1000 goes. Otherwise the path to
/// the next jump of the route (the mission data's jump preference) is laid
/// out ([`objectives::build_path_objectives`]: ids `0x330e` plus
/// `RmRandomBelow(5)`, 1 and `0x3318`; types 0x5a, 9 and 0x1a). Whether
/// the list changed.
pub(super) fn update_mission_path(
    host: &mut Host<'_, '_>,
    player: u32,
    target: u32,
    jump_pref: i32,
) -> bool {
    if target == 0 {
        return false;
    }
    if !host.cx.logic.content.state.players.contains_key(&player) {
        return false;
    }
    let at = player_position(host.cx, player);
    if !has_position(at) {
        return false;
    }
    let Some(state) = host.cx.logic.content.state.players.get(&player) else {
        return false;
    };
    let Some(system) = state.system else {
        return false;
    };
    if system == target || !host.cx.logic.content.state.systems.contains_key(&system) {
        return false;
    }
    if state.ship.as_ref().is_some_and(|s| s.lane_entry != 0) {
        let Some(rec) = host.rm_mut().and_then(|r| r.records.get_mut(&player)) else {
            return false;
        };
        if rec.path_checked {
            return false;
        }
        rec.path_checked = true;
        let Some(front) = rec.objectives.first() else {
            return false;
        };
        if front.kind != 5 {
            return false;
        }
        let leg = front.marker;
        let now = player_position(host.cx, player);
        let delta = |n: f32, m: u32| (f64::from(n) - f64::from(f32::from_bits(m))) as f32;
        let (dx, dy, dz) = (
            delta(now.x, leg[2]),
            delta(now.y, leg[3]),
            delta(now.z, leg[4]),
        );
        let distance_sq = f64::from(dy) * f64::from(dy)
            + f64::from(dz) * f64::from(dz)
            + f64::from(dx) * f64::from(dx);
        if !(distance_sq <= PATH_REACH_SQ) {
            return false;
        }
        if let Some(rec) = host.rm_mut().and_then(|r| r.records.get_mut(&player)) {
            rec.objectives.remove(0);
        }
        return true;
    }
    if let Some(rec) = host.rm_mut().and_then(|r| r.records.get_mut(&player)) {
        rec.path_checked = false;
    }
    let jump = crate::content::mission::nn_path::next_hop(host.cx, system, target, jump_pref);
    let jump_at = crate::content::system_paths::connection_position(host.cx, system, jump)
        .unwrap_or(Vector::ZERO);
    let legs = host
        .cx
        .logic
        .content
        .state
        .systems
        .get(&system)
        .map(|record| space_paths::build_nn_path(record, at, jump_at, 0.0).1)
        .unwrap_or_default();
    let first_ids = 0x330e + random_below(host, 5);
    let owner = owner(host);
    let Some(mut list) = host
        .rm_mut()
        .and_then(|r| r.records.get_mut(&player))
        .map(|rec| std::mem::take(&mut rec.objectives))
    else {
        return false;
    };
    let changed = objectives::build_path_objectives(
        host.cx,
        system,
        &legs,
        &mut list,
        [first_ids, 1, 0x3318],
        jump,
        [0x5a, 9, 0x1a],
        owner,
    );
    if let Some(rec) = host.rm_mut().and_then(|r| r.records.get_mut(&player)) {
        rec.objectives = list;
    }
    changed
}

/// The `rand()` draws of a machine's path objectives (`UpdatePathObjectives`,
/// `RandomMissionMachine.cpp:1428-1435`): one for machine types 1 to 3.
pub(super) fn machine_path_ids(host: &mut Host<'_, '_>, machine_type: i32) -> i32 {
    let base = match machine_type {
        1 => 0x3307,
        2 => 0x3304,
        3 => 0x330a,
        _ => return 0,
    };
    let draw = host.cx.world.rng().rand();
    (draw.wrapping_mul(3) >> 15) + base
}

/// The first object `ScanObjects` finds at `at` (0x20 at most, radius 0,
/// mask `0x600de0`); 0 when it finds none (the C++ reads its buffer's
/// uninitialised first word then; the port's buffer is 0).
pub(super) fn scan_first(cx: &mut crate::server::Cx<'_>, system: u32, at: Vector) -> u32 {
    api::system::scan_objects(cx, system, 0x20, at, 0.0, 0x60_0de0)
        .ok()
        .and_then(|found| found.first().copied())
        .unwrap_or(0)
}
