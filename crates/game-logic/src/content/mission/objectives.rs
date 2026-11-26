//! The objectives' run time (TU38, `Content/MissionObject.cpp`; work
//! package P3.5c5): the objectives' descriptions (`BuildDescription`,
//! `:516-778`), a player record's send (`FMissionPlayer::SendObjectives`,
//! `:1087-1198`) and placeholder (`SendPlaceholderObjective`,
//! `:1212-1227`), the path objectives (`BuildPathObjectives`, `:203-299`),
//! and the mission object's objective calls over its records
//! (`RemovePlayer`, `UpdateObjectiveTimer`, `SetObjectiveVisible`,
//! `SetObjectiveState`, `ClearObjectives`, `HideAllObjectives`,
//! `RenumberAllObjectives` and `RenumberObjectives`, `:1265-1484`) for a
//! script mission.
//!
//! The functions over one record ([`send_objectives`],
//! [`send_placeholder`], [`build_path_objectives`]) take the mission's part
//! as a [`Header`] and serve the random missions too
//! (`content::random_mission::runtime`). The script mission's calls take
//! the record out of the mission object while it is sent and put it back
//! after; nothing the send reads or calls looks at the records.
//!
//! A record's send goes to the client as `SETMISSIONOBJECTIVES`
//! (`pub::Player::SetMissionObjectives`, [`api::player`]); the state of one
//! objective as `SETMISSIONOBJECTIVESTATE`.
//!
//! The C++'s comparisons are kept as written: an x87 `FCOMP` branch is
//! false for a NaN on either side.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::math::Vector;
use neuron_server::controller::ControllerId;

use super::object::MissionPlayer;
use super::objective::Objective;
use super::runtime::{ID_MASK, label_object_id, script, script_mut};
use crate::api;
use crate::api::player::MissionObjective;
use crate::content::space_paths::NnPathLeg;
use crate::events::ControllerMessage;
use crate::server::Cx;

/// `Field_0x80` of a script mission: the constructor's 7, which nothing
/// of a script mission changes.
pub const SCRIPT_KIND: u8 = 7;

/// The objective timer's period, in seconds (`MissionObject.cpp:1339`).
pub const TIMER_PERIOD: f64 = 180.0;

/// What a record's send takes of its mission: the mission object's
/// controller and player, and the title, type and description that
/// travel with the objectives (`Field_0x50`, `Field_0x80`, `Field_0x68`).
#[derive(Clone, Debug, PartialEq)]
pub struct Header {
    /// `ControllerId`.
    pub controller: u32,
    /// `PlayerId`.
    pub player: u32,
    /// `Field_0x50`.
    pub title: FmtStr,
    /// `Field_0x80`.
    pub kind: u8,
    /// `Field_0x68`.
    pub description: FmtStr,
}

/// The header of script mission `id`; `None` without it.
#[must_use]
pub fn script_header(cx: &Cx<'_>, id: ControllerId) -> Option<Header> {
    let s = script(cx, id)?;
    Some(Header {
        controller: id.0,
        player: s.object.player,
        title: FmtStr::new(s.object.title),
        kind: SCRIPT_KIND,
        description: FmtStr::new(s.object.offer),
    })
}

/// A label's object for a type 4 objective (`GetLabelObjectId` of the
/// objective's mission).
pub type LabelObject<'f> = &'f dyn Fn(&Cx<'_>, &[u8]) -> u32;

/// `GetLabelObjectId` of script mission `id`'s object.
fn script_label(id: ControllerId) -> impl Fn(&Cx<'_>, &[u8]) -> u32 {
    move |cx, label| script(cx, id).map_or(0, |s| label_object_id(cx, s, label))
}

/// `BuildDescription` (slot 3) of every class (`MissionObject.cpp:516-778`):
/// the description made again from the objective's values. Always true.
///
/// - 1: the base; 2: the nav marker; 3: the mad lib's own string;
/// - 4: the reputation of the label's object (-1 without one, or when
///   `GetRep` fails), then the nav marker;
/// - 5 and 6: the system, the reputation instance when the mission's
///   player has a state, the objective's player state a ship and the
///   instance is not -1, then the nav marker;
/// - 7: a copy of its text; 8: its number.
pub fn build_description(
    cx: &Cx<'_>,
    o: &mut Objective,
    mission_player: u32,
    label_object: LabelObject<'_>,
) -> bool {
    let ids = o.ids as u32;
    let mut text = FmtStr::new(ids);
    match o.kind {
        1 => text.append_base(o.base_id),
        2 => text.append_nav_marker(o.marker),
        3 => text.append_string(ids),
        4 => {
            let object = label_object(cx, &o.label);
            let rep = if object & ID_MASK != 0 {
                api::space_obj::rep(cx, object).unwrap_or(-1)
            } else {
                -1
            };
            text.append_rep_instance(rep);
            text.append_nav_marker(o.marker);
        }
        5 | 6 => {
            text.append_system(o.system_id);
            let players = &cx.logic.content.state.players;
            // The objective's cached state; a null one has no ship.
            let ship = o
                .owner
                .and_then(|p| players.get(&p))
                .is_some_and(|p| p.ship.is_some());
            if players.contains_key(&mission_player) && ship && o.rep_instance != -1 {
                text.append_rep_instance(o.rep_instance);
            }
            text.append_nav_marker(o.marker);
        }
        7 => text = o.text.clone(),
        8 => text.append_int(o.value),
        _ => text = FmtStr::new(0),
    }
    o.description = text;
    true
}

/// `FMissionPlayer::SendObjectives` (`MissionObject.cpp:1087-1198`).
///
/// Nothing without the mission's player's state, or when the record's
/// player is on another live mission (a record whose player has no state
/// sends nothing; the C++ reads the freed state, D1043). Otherwise the
/// mission's objective timer restarts (returned true) and the visible
/// objectives are described and sent, in order, each given its place
/// (a hidden one -1). Unless the record repeats them ([`MissionPlayer::
/// resend`]), an objective with flag 0x10 is compared with the clones
/// sent before (`IsSame`): a known one loses the first of 0x100, else
/// 0x20 and 0x40 or else 0x10; a new one loses 0x40 and is cloned into
/// the sent list. An objective of ids 1 then becomes state 9 (keeping
/// 0x80). No visible objective sends an empty list. (Every class's
/// `BuildDescription` succeeds, so the C++'s write past its array when one
/// fails cannot happen.)
pub fn send_objectives(
    cx: &mut Cx<'_>,
    record: &mut MissionPlayer,
    header: &Header,
    label_object: LabelObject<'_>,
) -> bool {
    let players = &cx.logic.content.state.players;
    if !players.contains_key(&header.player) {
        return false;
    }
    let Some(live) = players.get(&record.player).map(|p| p.live_mission) else {
        return false;
    };
    if live != header.controller && live != 0 {
        return false;
    }
    let mut count = 0;
    for o in &mut record.objectives {
        if !o.hidden && build_description(cx, o, header.player, label_object) {
            count += 1;
        }
    }
    let mut sent = Vec::with_capacity(count);
    if count != 0 {
        for o in &mut record.objectives {
            if o.hidden {
                o.index = -1;
                continue;
            }
            if !record.resend && o.state & 0x10 != 0 {
                if record.sent.iter().any(|k| k.is_same(o)) {
                    if o.state & 0x100 != 0 {
                        o.state &= !0x100;
                    } else {
                        if o.state & 0x20 != 0 {
                            o.state &= !0x20;
                        }
                        if o.state & 0x40 != 0 {
                            o.state &= !0x40;
                        } else {
                            o.state &= !0x10;
                        }
                    }
                } else {
                    o.state &= !0x40;
                    record.sent.push(o.clone());
                }
            }
            if o.ids == 1 {
                o.state = (o.state & 0x80) | 9;
            }
            o.index = sent.len() as i32;
            sent.push(MissionObjective {
                kind: o.state,
                description: o.description.clone(),
            });
        }
    }
    let _ = api::player::set_mission_objectives(
        cx,
        record.player,
        header.controller,
        &sent,
        &header.title,
        header.kind,
        &header.description,
    );
    true
}

/// `SendPlaceholderObjective` (`MissionObject.cpp:1212-1227`): the record's
/// objectives set aside, a list of one type 7 objective of state 0x89 (an
/// empty text) sent in their place, then the objectives put back as they
/// were. Returns whether the timer restarts.
pub fn send_placeholder(
    cx: &mut Cx<'_>,
    record: &mut MissionPlayer,
    header: &Header,
    owner: Option<u32>,
) -> bool {
    let backup = std::mem::take(&mut record.objectives);
    let mut placeholder = Objective::of(7, owner);
    placeholder.state = 0x89;
    record.objectives.push(placeholder);
    let restart = send_objectives(cx, record, header, &|_, _| 0);
    record.objectives = backup;
    restart
}

/// `BuildPathObjectives(System, Legs, List, FirstIds, LegIds, TargetIds,
/// Target, FirstType, LegType, TargetType, Where)` (slot 11,
/// `MissionObject.cpp:203-299`) in system `system`.
///
/// Without legs the run of type 5 objectives from the first one goes
/// (true), or nothing changes when there is none (false). With legs, a
/// first type 5 objective at the first leg's point (each axis within 1,
/// the squared difference in `f64`) leaves the list as it is (false);
/// otherwise its run goes and each leg is laid out at the front, in order:
/// a lane (kind 0) as two objectives, its start (the first leg's ids and
/// type, a later leg's the leg's) and its end (the leg's), each with its
/// ring's reputation; free flight (kind 1) as one with the target's ids,
/// type and reputation (none without a target); another kind as a default
/// objective. Every objective's marker is the leg's point in `system`
/// (value 1) and its system the target's connection's destination, else
/// `system`. True.
///
/// The objectives always go before the list's first. The C++'s `Where`
/// (a random mission machine's, for a player without a ship: the list's
/// first node, read before the run goes) is that node too, unless the
/// node was in the run, which leaves the C++ inserting before a deleted
/// node (D1044).
#[allow(clippy::too_many_arguments)]
pub fn build_path_objectives(
    cx: &mut Cx<'_>,
    system: u32,
    legs: &[NnPathLeg],
    list: &mut Vec<Objective>,
    ids: [i32; 3],
    target: u32,
    types: [u32; 3],
    owner: Option<u32>,
) -> bool {
    let [first_ids, leg_ids, target_ids] = ids;
    let [first_type, leg_type, target_type] = types;
    let first = list.iter().position(|o| o.kind == 5);
    let run_end = |list: &[Objective], from: usize| {
        from + list[from..].iter().take_while(|o| o.kind == 5).count()
    };
    let Some(first_leg) = legs.first() else {
        let Some(from) = first else { return false };
        let end = run_end(list, from);
        list.drain(from..end);
        return true;
    };
    if let Some(from) = first {
        let point = if first_leg.kind == 1 {
            first_leg.point_b
        } else {
            first_leg.point_a
        };
        let marker = &list[from].marker;
        let close = |p: f32, m: u32| {
            let delta = f64::from(p) - f64::from(f32::from_bits(m));
            delta * delta <= 1.0
        };
        if close(point.x, marker[2]) && close(point.y, marker[3]) && close(point.z, marker[4]) {
            return false;
        }
        let end = run_end(list, from);
        list.drain(from..end);
    }
    let goto = if target == 0 {
        system
    } else {
        crate::content::system_paths::connection_goto_system(cx, system, target).unwrap_or(system)
    };
    let fill = |o: &mut Objective, rep: i32, point: Vector| {
        o.rep_instance = rep;
        o.system_id = goto;
        o.marker = [
            system,
            1,
            point.x.to_bits(),
            point.y.to_bits(),
            point.z.to_bits(),
        ];
    };
    let mut at = 0;
    for (n, leg) in legs.iter().enumerate() {
        let mut o = Objective::of(5, owner);
        match leg.kind {
            0 => {
                if n == 0 {
                    o.state = first_type;
                    o.ids = first_ids;
                } else {
                    o.state = leg_type;
                    o.ids = leg_ids;
                }
                let rep_a = api::space_obj::rep(cx, leg.object_a).unwrap_or(-1);
                fill(&mut o, rep_a, leg.point_a);
                let rep_b = api::space_obj::rep(cx, leg.object_b).unwrap_or(-1);
                list.insert(at, o);
                at += 1;
                let mut b = Objective::of(5, owner);
                b.state = leg_type;
                b.ids = leg_ids;
                fill(&mut b, rep_b, leg.point_b);
                list.insert(at, b);
                at += 1;
                continue;
            }
            1 => {
                let rep = if target == 0 {
                    -1
                } else {
                    api::space_obj::rep(cx, target).unwrap_or(-1)
                };
                o.ids = target_ids;
                o.state = target_type;
                fill(&mut o, rep, leg.point_b);
            }
            _ => {}
        }
        list.insert(at, o);
        at += 1;
    }
    true
}

/// Runs `f` on record `player` of script mission `id`, taken out of the
/// mission object for the call; `None` without the record.
fn with_record<R>(
    cx: &mut Cx<'_>,
    id: ControllerId,
    player: u32,
    f: impl FnOnce(&mut Cx<'_>, &mut MissionPlayer) -> R,
) -> Option<R> {
    let mut record = script_mut(cx, id)?.object.players.remove(&player)?;
    let result = f(cx, &mut record);
    if let Some(s) = script_mut(cx, id) {
        s.object.players.insert(player, record);
    }
    Some(result)
}

/// `FMissionPlayer::SendObjectives` of record `player` of script mission
/// `id`, the mission's timer restarted when the send passes its gates.
pub fn send(cx: &mut Cx<'_>, id: ControllerId, player: u32) {
    let Some(header) = script_header(cx, id) else {
        return;
    };
    let label = script_label(id);
    let restart = with_record(cx, id, player, |cx, record| {
        send_objectives(cx, record, &header, &label)
    });
    if restart == Some(true)
        && let Some(s) = script_mut(cx, id)
    {
        s.object.objective_timer = 0.0;
    }
}

/// The record keys of script mission `id`, in key order.
fn record_keys(cx: &Cx<'_>, id: ControllerId) -> Vec<u32> {
    script(cx, id).map_or_else(Vec::new, |s| s.object.players.keys().copied().collect())
}

/// `RemovePlayer(FindPlayer(player))` (`MissionObject.cpp:1265-1279`): the
/// record's objectives deleted, the empty list sent, the record gone.
pub fn remove_player(cx: &mut Cx<'_>, id: ControllerId, player: u32) {
    let Some(s) = script_mut(cx, id) else { return };
    let Some(record) = s.object.players.get_mut(&player) else {
        return;
    };
    record.objectives.clear();
    send(cx, id, player);
    if let Some(s) = script_mut(cx, id) {
        s.object.players.remove(&player);
    }
}

/// `ClearObjectives` (`MissionObject.cpp:1429-1438`): every record's
/// objectives deleted and the empty list sent.
pub fn clear_objectives(cx: &mut Cx<'_>, id: ControllerId) {
    for player in record_keys(cx, id) {
        if let Some(record) = script_mut(cx, id).and_then(|s| s.object.players.get_mut(&player)) {
            record.objectives.clear();
        }
        send(cx, id, player);
    }
}

/// The restore's `ClearObjectives` (`MissionScript.cpp:1958-1970`), made
/// after the restore read the objectives that followed it: every record
/// sends the empty list it sent then, its objectives set aside for the
/// send.
pub fn send_empty_all(cx: &mut Cx<'_>, id: ControllerId) {
    for player in record_keys(cx, id) {
        let Some(record) = script_mut(cx, id).and_then(|s| s.object.players.get_mut(&player))
        else {
            continue;
        };
        let kept = std::mem::take(&mut record.objectives);
        send(cx, id, player);
        if let Some(record) = script_mut(cx, id).and_then(|s| s.object.players.get_mut(&player)) {
            record.objectives = kept;
        }
    }
}

/// `HideAllObjectives` (`MissionObject.cpp:1441-1453`).
pub fn hide_all(cx: &mut Cx<'_>, id: ControllerId) {
    if let Some(s) = script_mut(cx, id) {
        for record in s.object.players.values_mut() {
            for o in &mut record.objectives {
                o.hidden = true;
            }
        }
    }
}

/// `SetObjectiveVisible(&nickname, visible)` (`MissionObject.cpp:1380-1400`):
/// each record's first objective of the nickname shown or hidden; whether
/// any had one.
pub fn set_visible(cx: &mut Cx<'_>, id: ControllerId, nickname: u32, visible: bool) -> bool {
    let Some(s) = script_mut(cx, id) else {
        return false;
    };
    let mut found = false;
    for record in s.object.players.values_mut() {
        if let Some(o) = record
            .objectives
            .iter_mut()
            .find(|o| o.nickname == nickname)
        {
            o.hidden = !visible;
            found = true;
        }
    }
    found
}

/// `SetObjectiveState(&nickname, state)` (`MissionObject.cpp:1403-1426`):
/// every objective of the nickname in every record takes the state; one
/// with a place (an index above -1) has it sent.
pub fn set_state(cx: &mut Cx<'_>, id: ControllerId, nickname: u32, state: u32) {
    let mut sends = Vec::new();
    if let Some(s) = script_mut(cx, id) {
        for record in s.object.players.values_mut() {
            for o in &mut record.objectives {
                if o.nickname != nickname {
                    continue;
                }
                o.state = state;
                if o.index > -1 {
                    sends.push((record.player, o.index));
                }
            }
        }
    }
    for (player, index) in sends {
        let _ = api::player::set_mission_objective_state(cx, player, id.0, index, state);
    }
}

/// `RenumberObjectives(player)` (`MissionObject.cpp:1464-1484`): the
/// visible objectives numbered from 0 (each after the first made state
/// 10), the hidden ones -1, then the record sent. (The C++ does not test
/// the record; every caller passes a record's key.)
pub fn renumber(cx: &mut Cx<'_>, id: ControllerId, player: u32) {
    let Some(record) = script_mut(cx, id).and_then(|s| s.object.players.get_mut(&player)) else {
        return;
    };
    let mut next = 0;
    for o in &mut record.objectives {
        if o.hidden {
            o.index = -1;
            continue;
        }
        if next != 0 {
            o.state = 10;
        }
        o.index = next;
        next += 1;
    }
    send(cx, id, player);
}

/// `RenumberAllObjectives` (`MissionObject.cpp:1456-1461`): every record,
/// in key order.
pub fn renumber_all(cx: &mut Cx<'_>, id: ControllerId) {
    for player in record_keys(cx, id) {
        renumber(cx, id, player);
    }
}

/// Every record of script mission `id` sent again as it is (the timer's
/// and `PlayerMissionChanged`'s resend: `SetField25(true)`, the send,
/// `SetField25(false)`).
fn resend(cx: &mut Cx<'_>, id: ControllerId, player: u32) {
    if let Some(record) = script_mut(cx, id).and_then(|s| s.object.players.get_mut(&player)) {
        record.set_resend(true);
    } else {
        return;
    }
    send(cx, id, player);
    if let Some(record) = script_mut(cx, id).and_then(|s| s.object.players.get_mut(&player)) {
        record.set_resend(false);
    }
}

/// `FMissionObject::UpdateObjectiveTimer(message)` (`MissionObject.cpp:
/// 1339-1377`), the mission object's tail while the mission is its
/// player's live mission: the player's `BaseExit` stops the shared timer
/// and `SpaceEnter` restarts it; a heartbeat while it runs adds the frame
/// time (the sum in `f64`, stored rounded, compared unrounded), and at
/// 180 s every record is sent again as it is.
pub fn update_timer(cx: &mut Cx<'_>, id: ControllerId, message: &ControllerMessage<'_>) {
    let Some(player) = script(cx, id).map(|s| s.object.player) else {
        return;
    };
    match *message {
        ControllerMessage::BaseExit { player: p, .. } => {
            if p == player && player & ID_MASK != 0 {
                cx.logic.content.state.objective_timer_running = false;
            }
        }
        ControllerMessage::SpaceEnter { player: p, .. } => {
            if p == player && player & ID_MASK != 0 {
                cx.logic.content.state.objective_timer_running = true;
            }
        }
        ControllerMessage::Heartbeat { elapsed } => {
            if !cx.logic.content.state.objective_timer_running {
                return;
            }
            let Some(s) = script_mut(cx, id) else { return };
            let sum = f64::from(elapsed) + f64::from(s.object.objective_timer);
            s.object.objective_timer = sum as f32;
            if !(sum >= TIMER_PERIOD) {
                return;
            }
            for p in record_keys(cx, id) {
                resend(cx, id, p);
            }
        }
        _ => {}
    }
}

/// `OnMessage100b`'s resend (`MissionScript.cpp:1716-1728`): the mission
/// player's first objective sent again with flags 0x30 or-ed into its
/// state for the send, the record repeating every objective; its state
/// then back as it was. A record without objectives sends nothing (the
/// C++ reads through null, D1043).
pub fn resend_first(cx: &mut Cx<'_>, id: ControllerId) {
    let Some(player) = script(cx, id).map(|s| s.object.player) else {
        return;
    };
    let Some(record) = script_mut(cx, id).and_then(|s| s.object.players.get_mut(&player)) else {
        return;
    };
    let Some(first) = record.objectives.first_mut() else {
        return;
    };
    let saved = first.state;
    first.state = saved | 0x30;
    resend(cx, id, player);
    if let Some(first) = script_mut(cx, id)
        .and_then(|s| s.object.players.get_mut(&player))
        .and_then(|r| r.objectives.first_mut())
    {
        first.state = saved;
    }
}
