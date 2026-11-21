//! `pub::Reputation` (`Server/server_pub.cpp:5969-6319`): the vibes of
//! ships and players over the world's vibe store
//! ([`game_sim::data::vibe::Vibes`]), the faction feelings of the reputation
//! registry ([`game_sim::data::reputation::Reputation`]), the players' vibe
//! ids ([`crate::server::VibeIds`]) and the notices that tell the clients of
//! a change (`Server_NotifyRepChange`, `:5916`).
//!
//! The exports, in the C++ order (the ADR counts 16 names; with the
//! overloads of `GetReputation` and `SetReputation` there are 20):
//!
//! | Export | Line | Port |
//! | --- | --- | --- |
//! | `Alloc` | 5973 | [`alloc`] |
//! | `Free` | 5992 | [`free`] |
//! | `GetAffiliation` | 6006 | [`affiliation`] |
//! | `SetAffiliation` | 6017 | [`set_affiliation`] |
//! | `SetReputation(vibe, group, value)` | 6032 | [`set_reputation`] |
//! | `GetReputation(vibe, group)` | 6046 | [`reputation`] |
//! | `GetAttitude` | 6057 | [`attitude`] |
//! | `SetAttitude` | 6073 | [`set_attitude`] |
//! | `GetRank` | 6095 | [`rank`] |
//! | `SetRank` | 6114 | [`set_rank`] |
//! | `GetReputationGroup` | 6157 | [`reputation_group`] |
//! | `GetName` | 6190 | [`name`] |
//! | `GetReputation(group, group)` | 6202 | [`group_reputation`] |
//! | `SetReputation(group, group, value)` | 6213 | [`set_group_reputation`] |
//! | `GetGroupName` | 6232 | [`group_name`] |
//! | `GetShortGroupName` | 6243 | [`short_group_name`] |
//! | `GetGroupFeelingsTowards` | 6254 | [`group_feelings_towards`] |
//! | `GetReputation(name)` | 6267 | [`solar_reputation_by_name`] |
//! | `GetReputation(ID_String)` | 6287 | [`solar_reputation`] |
//! | `EnumerateGroups` | 6308 | [`enumerate_groups`] |
//!
//! Fact F17 holds here too: a system's solars have no faction at load, so
//! a solar's vibe answers no affiliation and the feelings of no group.

use game_sim::protocol::ToClient;
use game_sim::protocol::fmtstr::FmtStr;
use game_sim::protocol::to_client::{Relation, SetReputation};
use neuron_core::id::create_id;

use super::PubError;
use crate::server::Cx;

fn send(cx: &mut Cx<'_>, client: u32, message: SetReputation) {
    cx.logic
        .client
        .send(client, ToClient::Server(message.into()));
}

fn flatten(name: &FmtStr) -> Vec<u8> {
    name.flatten().unwrap_or_default()
}

/// The display name as rpclocal's marshaller carries it: cut at the first
/// NUL, with the terminator when it is not empty.
fn wire_display_name(name: &[u16]) -> Vec<u16> {
    let end = name.iter().position(|&u| u == 0).unwrap_or(name.len());
    if end == 0 {
        return Vec::new();
    }
    let mut out = name[..end].to_vec();
    out.push(0);
    out
}

/// `Server_NotifyRepChange` (`:5916`): one `SendServerSetReputation`.
///
/// - Single player: `Vibe::Get` of the vibe (its affiliation, relations,
///   names and display name; an unknown vibe leaves the affiliation 0 and
///   the rest empty) goes to every active player's client.
/// - Otherwise only to `session` itself, when it is a valid slot with a
///   client: the vibe's attitude towards the session's vibe, the
///   affiliation 0 (the stack residue the C++ port zeroes) and no names.
pub fn notify_rep_change(cx: &mut Cx<'_>, vibe: i32, session: u32) {
    if cx.world.config().single_player {
        let summary = cx.world.vibes().0.summary(vibe);
        let message = match summary {
            Some(s) => SetReputation {
                target: vibe as u32,
                affiliation: s.affiliation,
                display_name: wire_display_name(&s.display_name),
                names: (flatten(&s.name1), flatten(&s.name2)),
                relations: s
                    .relations
                    .iter()
                    .map(|&(group_id, feeling)| Relation { feeling, group_id })
                    .collect(),
                attitude: 0.0,
            },
            None => SetReputation {
                target: vibe as u32,
                affiliation: 0,
                display_name: Vec::new(),
                names: (flatten(&FmtStr::default()), flatten(&FmtStr::default())),
                relations: Vec::new(),
                attitude: 0.0,
            },
        };
        let mut after = None;
        while let Some(active) = cx.logic.players.next_active(after) {
            after = Some(active);
            let client = cx.logic.players.get(active).map_or(0, |p| p.client_id);
            send(cx, client, message.clone());
        }
        return;
    }
    let Some(player_vibe) = cx
        .logic
        .players
        .get(session)
        .filter(|p| p.client_id != 0)
        .map(|p| p.rep_vibe)
    else {
        return;
    };
    let (vibes, rep) = cx.world.vibes();
    let attitude = vibes.attitude_towards(vibe, player_vibe, rep).1;
    let message = SetReputation {
        target: vibe as u32,
        affiliation: 0,
        display_name: Vec::new(),
        names: (flatten(&FmtStr::default()), flatten(&FmtStr::default())),
        relations: Vec::new(),
        attitude,
    };
    send(cx, session, message);
}

/// `Alloc`: a vibe id from the players' map (`Server_VibeID_Alloc`, -1 when
/// it is full), a vibe record for it (`Vibe::Alloc`), its names
/// (`Vibe::SetName`, no display name) and the notice to the session-0
/// clients ([`notify_rep_change`]). Returns the id.
///
/// # Errors
///
/// `Vibe::Alloc`'s result (-1) when the map is full; the id is -1 then and
/// no name is set or notice sent.
pub fn alloc(cx: &mut Cx<'_>, name: &FmtStr, nickname: &FmtStr) -> Result<i32, PubError> {
    let vibe = cx.logic.vibe_ids.alloc();
    let (vibes, _) = cx.world.vibes();
    let result = vibes.alloc(vibe);
    if result != 0 {
        return Err(PubError(result));
    }
    vibes.set_name(vibe, name, nickname, None);
    notify_rep_change(cx, vibe, 0);
    Ok(vibe)
}

/// `Free`: `Vibe::Free` (only a dynamic record goes), then the id's bit in
/// the players' map is cleared, whatever the first answered. The C++ throws
/// `out_of_range` for an id outside 0 to 4095; the port leaves the map as it
/// is (D670).
///
/// # Errors
///
/// `Vibe::Free`'s result: -2 when no dynamic record has the id.
pub fn free(cx: &mut Cx<'_>, vibe: i32) -> Result<(), PubError> {
    let result = cx.world.vibes().0.free(vibe);
    cx.logic.vibe_ids.free(vibe);
    if result == 0 {
        Ok(())
    } else {
        Err(PubError(result))
    }
}

/// `GetAffiliation`: the vibe's faction (`Vibe::GetAffiliation` with
/// players excluded: a player's vibe answers `0xffffffff`).
///
/// # Errors
///
/// -2 for an unknown vibe (the C++ writes `0xffffffff` then).
pub fn affiliation(cx: &mut Cx<'_>, vibe: i32) -> Result<u32, PubError> {
    match cx.world.vibes().0.affiliation(vibe, true) {
        (0, affiliation) => Ok(affiliation),
        _ => Err(PubError::BAD_ID),
    }
}

/// `SetAffiliation`: `Vibe::SetAffiliation(vibe, faction, false)`, then
/// the notice.
///
/// # Errors
///
/// -2 for an unknown vibe or faction.
pub fn set_affiliation(cx: &mut Cx<'_>, vibe: i32, faction: u32) -> Result<(), PubError> {
    let (vibes, rep) = cx.world.vibes();
    if vibes.set_affiliation(vibe, faction, false, rep) != 0 {
        return Err(PubError::BAD_ID);
    }
    notify_rep_change(cx, vibe, 0);
    Ok(())
}

/// `SetReputation(vibe, group, value)`: the vibe's feeling towards the
/// group becomes `value` (`Vibe::SetGroupFeelingsTowards`), then the
/// notice.
///
/// # Errors
///
/// -2 for an unknown vibe or a faction lookup that fails.
pub fn set_reputation(cx: &mut Cx<'_>, vibe: i32, group: u32, value: f32) -> Result<(), PubError> {
    let (vibes, rep) = cx.world.vibes();
    if vibes.set_group_feelings_towards(vibe, group, value, rep) != 0 {
        return Err(PubError::BAD_ID);
    }
    notify_rep_change(cx, vibe, 0);
    Ok(())
}

/// `GetReputation(vibe, group)`: the vibe's feeling towards the group
/// (`Vibe::GetGroupFeelingsTowards`).
///
/// # Errors
///
/// -2 for an unknown vibe or a faction lookup that fails (the value is 0).
pub fn reputation(cx: &mut Cx<'_>, vibe: i32, group: u32) -> Result<f32, PubError> {
    let (vibes, rep) = cx.world.vibes();
    match vibes.group_feelings_towards(vibe, group, rep) {
        (0, value) => Ok(value),
        _ => Err(PubError::BAD_ID),
    }
}

/// The result mapping `GetAttitude` and `SetAttitude` share: -1 becomes -2,
/// -2 becomes -3, the others stay.
fn attitude_code(result: i32) -> PubError {
    match result {
        -1 => PubError::BAD_ID,
        -2 => PubError::NOT_FOUND,
        other => PubError(other),
    }
}

/// `GetAttitude`: `from`'s attitude towards `to` (`Vibe::AttitudeTowards`).
///
/// # Errors
///
/// -3 when either vibe is unknown (`AttitudeTowards` answers -2 for `from`,
/// mapped to -3, and -3 for `to`). The C++ port's answer; Server.dll
/// returned the out pointer for `from`.
pub fn attitude(cx: &mut Cx<'_>, from: i32, to: i32) -> Result<f32, PubError> {
    let (vibes, rep) = cx.world.vibes();
    match vibes.attitude_towards(from, to, rep) {
        (0, value) => Ok(value),
        (result, _) => Err(attitude_code(result)),
    }
}

/// `SetAttitude`: `from`'s attitude towards `to` becomes `attitude`
/// (`Vibe::AdjustAttitude`). Outside single player, when `to` belongs to a
/// client (`Vibe::GetClientID`), that session gets the notice of `from`.
///
/// # Errors
///
/// -3 when either vibe is unknown (as [`attitude`]).
pub fn set_attitude(cx: &mut Cx<'_>, from: i32, to: i32, attitude: f32) -> Result<(), PubError> {
    let (vibes, rep) = cx.world.vibes();
    let result = vibes.adjust_attitude(from, to, attitude, rep);
    if result != 0 {
        return Err(attitude_code(result));
    }
    if !cx.world.config().single_player {
        let client = cx.world.vibes().0.client_id(to);
        if client != 0 {
            notify_rep_change(cx, from, client);
        }
    }
    Ok(())
}

/// `GetRank`: the vibe's rank as a float (converted unsigned).
///
/// # Errors
///
/// -2 for an unknown vibe (the C++ writes 0 then).
pub fn rank(cx: &mut Cx<'_>, vibe: i32) -> Result<f32, PubError> {
    match cx.world.vibes().0.rank(vibe) {
        (0, rank) => Ok(rank as f32),
        _ => Err(PubError::BAD_ID),
    }
}

/// `SetRank`: the vibe's rank becomes `ftol(rank)`. Outside single player,
/// when the vibe belongs to a client, the old rank is read first, from the
/// vibe whose key is the client id (the C++ passes the client id to
/// `Vibe::GetRank`), and when the rank changed every active player hears
/// the client's new rank (`SendServerMiscObjUpdateRank`). Without that
/// read the old rank is `0xffffffff`.
///
/// # Errors
///
/// -2 for an unknown vibe.
pub fn set_rank(cx: &mut Cx<'_>, vibe: i32, rank: f32) -> Result<(), PubError> {
    let mut client = 0;
    let mut old_rank = u32::MAX;
    if !cx.world.config().single_player {
        client = cx.world.vibes().0.client_id(vibe);
        if client & 0x3fff_ffff != 0 {
            let (result, value) = cx.world.vibes().0.rank(client as i32);
            if result == 0 {
                old_rank = value;
            }
        }
    }
    let new_rank = neuron_core::num::trunc_to_i32(f64::from(rank)) as u32;
    if cx.world.vibes().0.set_rank(vibe, new_rank) != 0 {
        return Err(PubError::BAD_ID);
    }
    if client & 0x3fff_ffff != 0 && new_rank != old_rank {
        let mut after = None;
        while let Some(active) = cx.logic.players.next_active(after) {
            after = Some(active);
            let to = cx.logic.players.get(active).map_or(0, |p| p.client_id);
            crate::client::misc_obj_update_rank(cx, to, client, new_rank as i32);
        }
    }
    Ok(())
}

/// `GetReputationGroup`: the faction of a nickname (cut to 15 bytes,
/// `Reputation::get_id`).
///
/// # Errors
///
/// -2 for an empty or unknown nickname (the C++ writes `0xffffffff`).
pub fn reputation_group(cx: &Cx<'_>, name: &[u8]) -> Result<u32, PubError> {
    match cx.world.reputation().get_id(name) {
        game_sim::data::reputation::MISSING_GROUP => Err(PubError::BAD_ID),
        id => Ok(id),
    }
}

/// `GetName`: the vibe's two names (`Vibe::GetName`).
///
/// # Errors
///
/// -2 for an unknown vibe.
pub fn name(cx: &mut Cx<'_>, vibe: i32) -> Result<(FmtStr, FmtStr), PubError> {
    cx.world
        .vibes()
        .0
        .name(vibe)
        .map(|(a, b, _)| (a, b))
        .ok_or(PubError::BAD_ID)
}

/// `GetReputation(group, group)`: one faction's feeling towards another
/// (`Reputation::get_feelings_towards`: 1 towards itself, 0 without an
/// entry).
///
/// # Errors
///
/// -4 when either id is `0xffffffff`; -2 for an unknown faction.
pub fn group_reputation(cx: &Cx<'_>, from: u32, to: u32) -> Result<f32, PubError> {
    if from == u32::MAX || to == u32::MAX {
        return Err(PubError::BAD_ARGUMENT);
    }
    match cx.world.reputation().feelings_towards(from, to) {
        (0, value) => Ok(value.unwrap_or(0.0)),
        _ => Err(PubError::BAD_ID),
    }
}

/// `SetReputation(group, group, value)`: `affect_relation` by the
/// difference between `value` and the current feeling (a float), then the
/// notice for the vibe whose key is `from`'s id (the C++ passes the group
/// id as a vibe).
///
/// # Errors
///
/// -4 when either id is `0xffffffff`; -2 for an unknown faction.
pub fn set_group_reputation(
    cx: &mut Cx<'_>,
    from: u32,
    to: u32,
    value: f32,
) -> Result<(), PubError> {
    if from == u32::MAX || to == u32::MAX {
        return Err(PubError::BAD_ARGUMENT);
    }
    let current = match cx.world.reputation().feelings_towards(from, to) {
        (0, current) => current.unwrap_or(0.0),
        _ => return Err(PubError::BAD_ID),
    };
    let delta = (f64::from(value) - f64::from(current)) as f32;
    cx.world.reputation_mut().affect_relation(from, to, delta);
    notify_rep_change(cx, from as i32, 0);
    Ok(())
}

/// `GetGroupName`: the faction's name id (`Reputation::get_name`, 0 for an
/// unknown faction). `get_name` never answers `0xffffffff`, so the C++'s
/// -2 arm is never taken.
///
/// # Errors
///
/// -2 when the name id is `0xffffffff`.
pub fn group_name(cx: &Cx<'_>, group: u32) -> Result<u32, PubError> {
    match cx.world.reputation().get_name(group) {
        u32::MAX => Err(PubError::BAD_ID),
        id => Ok(id),
    }
}

/// `GetShortGroupName`: as [`group_name`] with the short name.
///
/// # Errors
///
/// -2 when the name id is `0xffffffff`.
pub fn short_group_name(cx: &Cx<'_>, group: u32) -> Result<u32, PubError> {
    match cx.world.reputation().get_short_name(group) {
        u32::MAX => Err(PubError::BAD_ID),
        id => Ok(id),
    }
}

/// `GetGroupFeelingsTowards`: as [`reputation`], but every result other
/// than -1 is a success, with the value the lookup left (0 for an unknown
/// vibe or faction). The vibe store never answers -1.
///
/// # Errors
///
/// -2 when the lookup answers -1.
pub fn group_feelings_towards(cx: &mut Cx<'_>, vibe: i32, group: u32) -> Result<f32, PubError> {
    let (vibes, rep) = cx.world.vibes();
    match vibes.group_feelings_towards(vibe, group, rep) {
        (-1, _) => Err(PubError::BAD_ID),
        (_, value) => Ok(value),
    }
}

/// `GetReputation(name)`: the vibe of a solar by nickname
/// (`SolarReps::FindSolarRep(CreateID(name))`).
///
/// # Errors
///
/// -2 when no vibe has the id (the C++ writes -1).
pub fn solar_reputation_by_name(cx: &mut Cx<'_>, name: &[u8]) -> Result<i32, PubError> {
    solar_reputation(cx, create_id(name))
}

/// `GetReputation(ID_String)`: `SolarReps::FindSolarRep`, the id itself
/// when a vibe has it (`Vibe::Verify`).
///
/// # Errors
///
/// -2 when no vibe has the id, or the id is -1 (the C++ writes -1).
pub fn solar_reputation(cx: &mut Cx<'_>, id: u32) -> Result<i32, PubError> {
    let key = id as i32;
    let rep = if cx.world.vibes().0.verify(key) {
        key
    } else {
        -1
    };
    if rep == -1 {
        Err(PubError::BAD_ID)
    } else {
        Ok(rep)
    }
}

/// `EnumerateGroups`: every faction id in id order
/// (`Reputation::enumerate`) until the callback answers false. Always 0.
pub fn enumerate_groups(cx: &Cx<'_>, mut callback: impl FnMut(u32) -> bool) -> i32 {
    for (id, _) in cx.world.reputation().groups() {
        if !callback(id) {
            break;
        }
    }
    0
}
