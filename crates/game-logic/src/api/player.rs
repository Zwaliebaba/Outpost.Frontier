//! `pub::Player` (`Server/server_pub.cpp:4202-5344`): the player database
//! as Content sees it, by session id ([`crate::player::PlayerData`]), with
//! the `IClient` messages the C++ sends as rpclocal marshals them.
//!
//! Most functions open with `IsValidPlayer` (the id is not 0, within the
//! slots, and its slot has a client, [`neuron_server::session::
//! SessionTable::is_valid`]) and answer -2 otherwise; the comment of each
//! says when it does not. Where the C++ writes an out-parameter before a
//! failure (`GetRep` -1, `GetRank` 0, the counters -1), the port answers
//! the failure alone. The client id of a message is what the C++ passes:
//! the session id for most, the slot's client id for the counters, the
//! cash and `MissionSaveA`.
//!
//! The 57 exports, in the C++ order:
//!
//! | Export | Line | Port |
//! | --- | --- | --- |
//! | `MarkObj` | 4206 | [`mark_obj`] |
//! | `CfgInterfaceNotification` | 4229 | [`cfg_interface_notification`] |
//! | `GetSystem` | 4252 | [`system`] |
//! | `GetShip` | 4262 | [`ship`] |
//! | `GetShipID` | 4272 | [`ship_id`] |
//! | `GetRep` | 4282 | [`rep`] |
//! | `GetBase` | 4293 | [`base`] |
//! | `GetLocation` | 4303 | [`location`] |
//! | `GetCharacter` | 4314 | [`character`] |
//! | `GetRelativeHealth` | 4322 | [`relative_health`] |
//! | `EnumerateCargo` | 4343 | [`enumerate_cargo`] |
//! | `GetRemainingHoldSize` | 4361 | [`remaining_hold_size`] |
//! | `AddCargo` | 4407 | [`add_cargo`] |
//! | `RemoveCargo` | 4461 | [`remove_cargo`] |
//! | `SetShipAndLoadout` | 4499 | [`set_ship_and_loadout`] |
//! | `GetRank` | 4519 | [`rank`] |
//! | `GetMoneyNeededToNextRank` | 4530 | [`money_needed_to_next_rank`] |
//! | `GetAssetValue` | 4541 | [`asset_value`] |
//! | `AdjustCash` | 4610 | [`adjust_cash`] |
//! | `InspectCash` | 4647 | [`inspect_cash`] |
//! | `SetInitialPos` | 4662 | [`set_initial_pos`] |
//! | `SetInitialOrnt` | 4674 | [`set_initial_orient`] |
//! | `GetNumKills` | 4686 | [`num_kills`] |
//! | `SetNumKills` | 4704 | [`set_num_kills`] |
//! | `GetNumMissionSuccesses` | 4717 | [`num_mission_successes`] |
//! | `SetNumMissionSuccesses` | 4729 | [`set_num_mission_successes`] |
//! | `GetNumMissionFailures` | 4742 | [`num_mission_failures`] |
//! | `SetNumMissionFailures` | 4753 | [`set_num_mission_failures`] |
//! | `DisplayMissionMessage` | 4776 | [`display_mission_message`] |
//! | `SetStoryCue` | 4805 | [`set_story_cue`] |
//! | `PopUpDialog` | 4822 | [`pop_up_dialog`] |
//! | `ReturnBestPath` | 4832 | [`return_best_path`] |
//! | `ReturnPlayerStats` | 4842 | [`return_player_stats`] |
//! | `SetCamera` | 4886 | [`set_camera`] |
//! | `RevertCamera` | 4922 | [`revert_camera`] |
//! | `LoadHint` | 4939 | [`load_hint`] |
//! | `GetGroupSize` | 4953 | [`group_size`] |
//! | `GetName` | 4974 | [`name`] |
//! | `IsGroupMember` | 4993 | [`is_group_member`] |
//! | `RemoveFromGroup` | 5007 | [`remove_from_group`] |
//! | `RewardGroup` | 5035 | [`reward_group`] |
//! | `SetMsnID` | 5056 | [`set_msn_id`] |
//! | `GetMsnID` | 5073 | [`msn_id`] |
//! | `SetRank` | 5091 | [`set_rank`] |
//! | `SetMoneyNeededToNextRank` | 5104 | [`set_money_needed_to_next_rank`] |
//! | `SetMonkey` | 5129 | [`set_monkey`] |
//! | `SetRobot` | 5137 | [`set_robot`] |
//! | `SetRellan` | 5148 | [`set_rellan`] |
//! | `SendNNMessage` | 5170 | [`send_nn_message`] |
//! | `ForceLand` | 5188 | [`force_land`] |
//! | `SetCostume` | 5198 | [`set_costume`] |
//! | `GetGender` | 5206 | [`gender`] |
//! | `GetBody` | 5214 | [`body`] |
//! | `GetGroupMembers` | 5221 | [`group_members`] |
//! | `SetMissionObjectives` | 5260 | [`set_mission_objectives`] |
//! | `ReplaceMissionObjective` | 5318 | [`replace_mission_objective`] |
//! | `SetMissionObjectiveState` | 5334 | [`set_mission_objective_state`] |
//!
//! The mission functions send what Content gives them; the missions
//! themselves (P3.6) keep no server state here, as in the C++.

use game_sim::data::equip_desc::{EquipDesc, SUB_OBJECT_ID_NONE};
use game_sim::protocol::ToClient;
use game_sim::protocol::common::{CommonMessage, PopUpDialog, RequestBestPath, RequestPlayerStats};
use game_sim::protocol::fmtstr::FmtStr;
use game_sim::protocol::to_client::{
    CfgInterfaceNotification, LoadHint, MarkObj, MissionObjective as WireObjective, MissionSaveA,
    ReplaceMissionObjective, RevertCamera, SendNnMessage, ServerMessage, SetCamera,
    SetMissionFailures, SetMissionMessage, SetMissionObjectiveState, SetMissionObjectives,
    SetMissionSuccesses, SetNumKills, SetStoryCue,
};
use neuron_core::math::{Matrix, Transform, Vector, matrix_to_quaternion};
use neuron_core::num::trunc_to_i32_via_i64;

use super::PubError;
use crate::client;
use crate::player::{self, CASH_CEILING, PlayerData};
use crate::server::Cx;

/// `CreateID("rellan")`: the costume `SetRellan` gives (decision O17).
pub const RELLAN_COSTUME: u32 = 0x960d_d804;

/// What `GetName` answers for a null active name.
const BAD_NAME_LITERAL: &str = "<bad name>";

/// `PlayerData_GetCharacterName`'s fallback (`>>>BAD   NAME<<<`).
const BAD_NAME: &str = ">>>BAD   NAME<<<";

/// One objective of `SetMissionObjectives` and `ReplaceMissionObjective`
/// (`pub::Player::MissionObjective`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct MissionObjective {
    /// `Type`: the objective's state; its low byte travels.
    pub kind: u32,
    /// `Description`.
    pub description: FmtStr,
}

/// One cargo line `EnumerateCargo` reports: the good, the count and the
/// item's id.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CargoLine {
    /// `Arch2Good` of the item's archetype (0 for none; not tested).
    pub good: u32,
    /// `Count`.
    pub count: i32,
    /// `Id`.
    pub id: u16,
}

fn send(cx: &mut Cx<'_>, client: u32, message: impl Into<ServerMessage>) {
    cx.logic
        .client
        .send(client, ToClient::Server(message.into()));
}

fn send_common(cx: &mut Cx<'_>, client: u32, message: impl Into<CommonMessage>) {
    cx.logic
        .client
        .send(client, ToClient::Common(message.into()));
}

/// `RpcFlattenFmtStr`.
fn flatten(text: &FmtStr) -> Vec<u8> {
    text.flatten().unwrap_or_default()
}

/// `IsValidPlayer`.
fn valid(cx: &Cx<'_>, player: u32) -> bool {
    cx.logic.players.is_valid(player)
}

/// The slot of a valid player (`IsValidPlayer` then `PlayerDB_GetPlayer`).
fn slot<'c>(cx: &'c Cx<'_>, player: u32) -> Result<&'c PlayerData, PubError> {
    if !valid(cx, player) {
        return Err(PubError::BAD_ID);
    }
    cx.logic.players.get(player).ok_or(PubError::BAD_ID)
}

/// The slot of a valid player, mutably.
fn slot_mut<'c>(cx: &'c mut Cx<'_>, player: u32) -> Result<&'c mut PlayerData, PubError> {
    if !valid(cx, player) {
        return Err(PubError::BAD_ID);
    }
    cx.logic.players.get_mut(player).ok_or(PubError::BAD_ID)
}

/// The group a slot names (`group_ptr`), when it exists.
fn group_of(cx: &Cx<'_>, player: u32) -> Option<u32> {
    cx.logic
        .players
        .get(player)
        .and_then(|p| p.group)
        .filter(|&g| cx.logic.groups.get(g).is_some())
}

/// The members of a group in id order (`m_Members`).
fn members(cx: &Cx<'_>, group: u32) -> Vec<u32> {
    cx.logic
        .groups
        .get(group)
        .map(|g| g.members.iter().copied().collect())
        .unwrap_or_default()
}

/// `MarkObj`: the object (`Server_LookupGameObject`, destroyed or not)
/// records the player's mark (`mark_for_player`: 1 adds, 0 removes), and
/// `MARKOBJ` (the object, the mark's low byte) goes to the player.
///
/// # Errors
///
/// -2 for an invalid player; -3 for an unknown object.
pub fn mark_obj(cx: &mut Cx<'_>, player: u32, object: u32, mark: i32) -> Result<(), PubError> {
    if !valid(cx, player) {
        return Err(PubError::BAD_ID);
    }
    let h = cx.world.find(object).ok_or(PubError::NOT_FOUND)?;
    if let Some(entry) = cx.logic.objects.entry(cx.world, h) {
        entry.mark_for_player(player, mark);
    }
    let message = MarkObj {
        object_id: object,
        mark_type: mark as u8,
    };
    send(cx, player, message);
    Ok(())
}

/// `CfgInterfaceNotification`: `CFGINTERFACENOTIFICATION` (the type, the
/// parameter, the flag as 0 or 1) to the player.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn cfg_interface_notification(
    cx: &mut Cx<'_>,
    player: u32,
    notif_type: u32,
    enable: bool,
    param: i32,
) -> Result<(), PubError> {
    if !valid(cx, player) {
        return Err(PubError::BAD_ID);
    }
    let message = CfgInterfaceNotification {
        notif_type,
        enable: u8::from(enable),
        param: param as u32,
    };
    send(cx, player, message);
    Ok(())
}

/// `GetSystem`: the player's system.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn system(cx: &Cx<'_>, player: u32) -> Result<u32, PubError> {
    slot(cx, player).map(|p| p.system_id)
}

/// `GetShip`: the player's ship object (0 when docked).
///
/// # Errors
///
/// -2 for an invalid player.
pub fn ship(cx: &Cx<'_>, player: u32) -> Result<u32, PubError> {
    slot(cx, player).map(|p| p.ship_object_id)
}

/// `GetShipID`: the player's ship archetype.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn ship_id(cx: &Cx<'_>, player: u32) -> Result<u32, PubError> {
    slot(cx, player).map(|p| p.ship_archetype)
}

/// `GetRep`: the player's vibe.
///
/// # Errors
///
/// -2 for an invalid player (the C++ writes -1 first).
pub fn rep(cx: &Cx<'_>, player: u32) -> Result<i32, PubError> {
    slot(cx, player).map(|p| p.rep_vibe)
}

/// `GetBase`: the base the player is in (0 in space).
///
/// # Errors
///
/// -2 for an invalid player.
pub fn base(cx: &Cx<'_>, player: u32) -> Result<u32, PubError> {
    slot(cx, player).map(|p| p.current_base)
}

/// `GetLocation`: the room the player is in.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn location(cx: &Cx<'_>, player: u32) -> Result<u32, PubError> {
    slot(cx, player).map(|p| p.current_location)
}

/// `GetCharacter`: the slot's `+0x3fc`.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn character(cx: &Cx<'_>, player: u32) -> Result<u32, PubError> {
    slot(cx, player).map(|p| p.field_3fc)
}

/// `GetRelativeHealth`: the hull's health.
///
/// # Errors
///
/// -2 for an invalid player (the C++ writes -1 first).
pub fn relative_health(cx: &Cx<'_>, player: u32) -> Result<f32, PubError> {
    slot(cx, player).map(|p| p.relative_health)
}

/// `EnumerateCargo`: the player's items that are not mounted, in list
/// order, each with `Arch2Good` of its archetype (not tested), its count
/// and its id. The C++ calls the enumerator for each; the port answers
/// the lines, so the caller may call back into the server between them.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn enumerate_cargo(cx: &Cx<'_>, player: u32) -> Result<Vec<CargoLine>, PubError> {
    let p = slot(cx, player)?;
    let goods = &cx.world.data().goods;
    Ok(p.equipment
        .iter()
        .filter(|e| e.equipped == 0)
        .map(|e| CargoLine {
            good: goods.arch_to_good(e.arch_id),
            count: e.count,
            id: e.id,
        })
        .collect())
}

/// `EquipDescList::get_cargo_space_occupied`: each internal line's count
/// times its archetype's volume added to a float total, stored as a float
/// after each line. A line of an unknown archetype counts as volume 0
/// (D116).
pub(crate) fn cargo_space_occupied(cx: &Cx<'_>, items: &[EquipDesc]) -> f32 {
    let data = cx.world.data();
    let strings = cx.world.strings();
    let mut total = 0.0f32;
    for item in items {
        let volume = data
            .archetypes
            .equipment
            .get(&item.arch_id)
            .map(|a| a.volume);
        total = (item.cargo_space_occupied(strings, volume) + f64::from(total)) as f32;
    }
    total
}

/// `GetRemainingHoldSize`: `pub::GetCargoHoldSize` of the ship (0 when it
/// fails), unsigned, less the space the player's cargo takes.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn remaining_hold_size(cx: &Cx<'_>, player: u32) -> Result<f32, PubError> {
    let p = slot(cx, player)?;
    let hold = super::misc::cargo_hold_size(cx, p.ship_archetype).unwrap_or(0);
    let occupied = cargo_space_occupied(cx, &p.equipment);
    Ok((f64::from(hold) - f64::from(occupied)) as f32)
}

/// `AddCargo`: units of a good for the player. In space they join the
/// ship's hold and the record together (`StarSystem::add_cargo`,
/// [`crate::cargo::add_cargo`]; `mission` keeps them apart and marks the
/// stack); docked they join the record's cargo bay
/// (`PlayerData_AddEquipment` with `mission` as `no_combine`, notified).
///
/// # Errors
///
/// -2 for an invalid player, or when the player's ship is not found; -4
/// for a health below 0, above 1 or NaN.
pub fn add_cargo(
    cx: &mut Cx<'_>,
    player: u32,
    good: u32,
    quantity: u32,
    health: f32,
    mission: bool,
) -> Result<(), PubError> {
    let p = slot(cx, player)?;
    // `FCOMP` with the unordered case taken as a failure: NaN is refused.
    if !(0.0..=1.0).contains(&health) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let ship = p.ship_object_id;
    if ship != 0 {
        let h = cx.world.find(ship).ok_or(PubError::BAD_ID)?;
        let arch = cx.world.data().goods.good_to_arch(good);
        crate::cargo::add_cargo(cx, h, arch, quantity, health, mission);
        return Ok(());
    }
    let arch = cx.world.data().goods.good_to_arch(good);
    let bay = cx.world.strings().cargo_bay();
    player::add_equipment(
        cx,
        player,
        arch,
        bay,
        quantity as i32,
        health,
        0,
        mission,
        true,
    );
    Ok(())
}

/// `RemoveCargo`: units of one item leave the player: in space from the
/// ship's stack and the record together (`StarSystem::remove_cargo`,
/// [`crate::cargo::remove_cargo`]), docked from the record
/// (`PlayerData_RemoveEquipment`, notified).
///
/// # Errors
///
/// -2 for an invalid player, or when the player's ship is not found.
pub fn remove_cargo(
    cx: &mut Cx<'_>,
    player: u32,
    item: u16,
    quantity: u32,
) -> Result<(), PubError> {
    let ship = slot(cx, player)?.ship_object_id;
    if ship != 0 {
        let h = cx.world.find(ship).ok_or(PubError::BAD_ID)?;
        crate::cargo::remove_cargo(cx, h, item, quantity as i32);
        return Ok(());
    }
    player::remove_equipment(cx, player, item, quantity as i32, true);
    Ok(())
}

/// `SetShipAndLoadout`: the ship (`PlayerData_SetShipArch`), then the items
/// as the whole equipment, each with its id cleared so the record numbers
/// it afresh (`PlayerData_LoadEquipment`).
///
/// # Errors
///
/// -2 for an invalid player.
pub fn set_ship_and_loadout(
    cx: &mut Cx<'_>,
    player: u32,
    ship: u32,
    items: &[EquipDesc],
) -> Result<(), PubError> {
    slot(cx, player)?;
    player::set_ship_arch(cx, player, ship);
    let fresh: Vec<EquipDesc> = items
        .iter()
        .map(|item| {
            let mut copy = *item;
            copy.id = SUB_OBJECT_ID_NONE;
            copy
        })
        .collect();
    player::load_equipment(cx, player, &fresh);
    Ok(())
}

/// `GetRank`: the player's rank.
///
/// # Errors
///
/// -2 for an invalid player (the C++ writes 0 first).
pub fn rank(cx: &Cx<'_>, player: u32) -> Result<i32, PubError> {
    slot(cx, player).map(|p| p.rank_level as i32)
}

/// `GetMoneyNeededToNextRank`.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn money_needed_to_next_rank(cx: &Cx<'_>, player: u32) -> Result<i32, PubError> {
    slot(cx, player).map(|p| p.money_needed_to_next_rank)
}

/// `GetAssetValue`: 0.8 (a float) of the price of the ship's good
/// (`find_by_ship_arch`), then for each item of a known good (an archetype
/// whose low 30 bits are 0 is skipped): a combinable good's count times its
/// price, any other's `ftol(health * price)` times its count, times 0.3 (a
/// float); the total is stored as a float after each term.
///
/// # Errors
///
/// -2 for an invalid player, a ship archetype whose low 30 bits are 0, or
/// a ship no good sells (the C++ writes 0 first).
pub fn asset_value(cx: &Cx<'_>, player: u32) -> Result<f32, PubError> {
    let p = slot(cx, player)?;
    if p.ship_archetype & 0x3fff_ffff == 0 {
        return Err(PubError::BAD_ID);
    }
    let goods = &cx.world.data().goods;
    let ship_good = goods
        .find_by_ship_arch(p.ship_archetype)
        .ok_or(PubError::BAD_ID)?;
    let mut value = (f64::from(0.8f32) * f64::from(ship_good.price)) as f32;
    for item in &p.equipment {
        if item.arch_id & 0x3fff_ffff == 0 {
            continue;
        }
        let Some(good) = goods.find_by_archetype(item.arch_id) else {
            continue;
        };
        let term = if good.combinable {
            f64::from(item.count) * f64::from(good.price)
        } else {
            let unit = trunc_to_i32_via_i64(f64::from(item.health) * f64::from(good.price));
            f64::from(unit.wrapping_mul(item.count)) * f64::from(0.3f32)
        };
        value = (f64::from(value) + term) as f32;
    }
    Ok(value)
}

/// `AdjustCash`: `PlayerData::AdjustCash(amount, true)` inlined: the earned
/// total grows; on a multiplayer server before a character is selected
/// nothing else happens; otherwise the clamped cash, `SETCASH` to the
/// slot's client and `PlayerWorthChanged` ([`player::adjust_cash`]).
///
/// # Errors
///
/// -2 for an invalid player.
pub fn adjust_cash(cx: &mut Cx<'_>, player: u32, amount: i32) -> Result<(), PubError> {
    slot(cx, player)?;
    player::adjust_cash(cx, player, amount, true);
    Ok(())
}

/// `InspectCash`: the cash, clamped to 0 ..= 999 999 999 (not stored).
///
/// # Errors
///
/// -2 for an invalid player (the C++ writes 0 first).
pub fn inspect_cash(cx: &Cx<'_>, player: u32) -> Result<i32, PubError> {
    slot(cx, player).map(|p| p.cash.clamp(0, CASH_CEILING))
}

/// `SetInitialPos`: where the ship starts in space.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn set_initial_pos(cx: &mut Cx<'_>, player: u32, pos: Vector) -> Result<(), PubError> {
    slot_mut(cx, player)?.initial_pos = [pos.x, pos.y, pos.z];
    Ok(())
}

/// `SetInitialOrnt`: the ship's starting orientation, the nine floats.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn set_initial_orient(cx: &mut Cx<'_>, player: u32, orient: &Matrix) -> Result<(), PubError> {
    slot_mut(cx, player)?.initial_orient = orient.m;
    Ok(())
}

/// `GetNumKills`.
///
/// # Errors
///
/// -2 for an invalid player (the C++ writes -1).
pub fn num_kills(cx: &Cx<'_>, player: u32) -> Result<i32, PubError> {
    slot(cx, player).map(|p| p.num_kills)
}

/// `SetNumKills`: stored, and `SETNUMKILLS` to the slot's client.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn set_num_kills(cx: &mut Cx<'_>, player: u32, kills: i32) -> Result<(), PubError> {
    let p = slot_mut(cx, player)?;
    p.num_kills = kills;
    let client = p.client_id;
    send(
        cx,
        client,
        SetNumKills {
            value: kills as u32,
        },
    );
    Ok(())
}

/// `GetNumMissionSuccesses`.
///
/// # Errors
///
/// -2 for an invalid player (the C++ writes -1).
pub fn num_mission_successes(cx: &Cx<'_>, player: u32) -> Result<i32, PubError> {
    slot(cx, player).map(|p| p.num_mission_successes)
}

/// `SetNumMissionSuccesses`: stored, and `SETMISSIONSUCCESSES` to the
/// slot's client.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn set_num_mission_successes(cx: &mut Cx<'_>, player: u32, count: i32) -> Result<(), PubError> {
    let p = slot_mut(cx, player)?;
    p.num_mission_successes = count;
    let client = p.client_id;
    send(
        cx,
        client,
        SetMissionSuccesses {
            value: count as u32,
        },
    );
    Ok(())
}

/// `GetNumMissionFailures`.
///
/// # Errors
///
/// -2 for an invalid player (the C++ writes -1).
pub fn num_mission_failures(cx: &Cx<'_>, player: u32) -> Result<i32, PubError> {
    slot(cx, player).map(|p| p.num_mission_failures)
}

/// `SetNumMissionFailures`: stored, and `SETMISSIONFAILURES` to the slot's
/// client.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn set_num_mission_failures(cx: &mut Cx<'_>, player: u32, count: i32) -> Result<(), PubError> {
    let p = slot_mut(cx, player)?;
    p.num_mission_failures = count;
    let client = p.client_id;
    send(
        cx,
        client,
        SetMissionFailures {
            value: count as u32,
        },
    );
    Ok(())
}

/// `DisplayMissionMessage`: type 0 skips the next autosave; then
/// `SET_MISSION_MESSAGE` (the text flattened, the type) to every member of
/// the player's group when `broadcast` and the player has one
/// (`CPlayerGroup::SetMissionMessage`), else to the player.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn display_mission_message(
    cx: &mut Cx<'_>,
    player: u32,
    text: &FmtStr,
    kind: u32,
    broadcast: bool,
) -> Result<(), PubError> {
    let p = slot_mut(cx, player)?;
    if kind == 0 {
        p.skip_next_autosave = true;
    }
    let message = SetMissionMessage {
        kind,
        text: flatten(text),
    };
    match group_of(cx, player).filter(|_| broadcast) {
        Some(group) => {
            for member in members(cx, group) {
                send(cx, member, message.clone());
            }
        }
        None => send(cx, player, message),
    }
    Ok(())
}

/// `SetStoryCue`: `SET_STORY_CUE` to the player; nothing is stored.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn set_story_cue(cx: &mut Cx<'_>, player: u32, cue: u32) -> Result<(), PubError> {
    if !valid(cx, player) {
        return Err(PubError::BAD_ID);
    }
    send(cx, player, SetStoryCue { value: cue });
    Ok(())
}

/// `PopUpDialog`: `POP_UP_DIALOG` (the title and the text flattened, the
/// buttons) to the id as given; the player is not checked. Always 0.
pub fn pop_up_dialog(cx: &mut Cx<'_>, player: u32, title: &FmtStr, text: &FmtStr, buttons: u32) {
    let message = PopUpDialog {
        data_a: flatten(title),
        data_b: flatten(text),
        field_10: buttons,
    };
    send_common(cx, player, message);
}

/// `ReturnBestPath`: `REQUEST_BEST_PATH` with the bytes to the id as
/// given; not checked. Always 0.
pub fn return_best_path(cx: &mut Cx<'_>, player: u32, data: &[u8]) {
    send_common(
        cx,
        player,
        RequestBestPath {
            data: data.to_vec(),
        },
    );
}

/// `ReturnPlayerStats`: `REQUEST_PLAYER_STATS` with the bytes to the id as
/// given; not checked. Always 0.
pub fn return_player_stats(cx: &mut Cx<'_>, player: u32, data: &[u8]) {
    send_common(
        cx,
        player,
        RequestPlayerStats {
            data: data.to_vec(),
        },
    );
}

/// `SetCamera`: `SET_CAMERA` (the translation, the rotation as a
/// quaternion through the math engine's `MatrixToQuaternion`, the field of
/// view and the aspect) to the player.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn set_camera(
    cx: &mut Cx<'_>,
    player: u32,
    transform: &Transform,
    fov: f32,
    aspect: f32,
) -> Result<(), PubError> {
    if !valid(cx, player) {
        return Err(PubError::BAD_ID);
    }
    let q = matrix_to_quaternion(&transform.rotation);
    let t = transform.translation;
    let message = SetCamera {
        position: [t.x, t.y, t.z],
        orientation: [q.w, q.x, q.y, q.z],
        fov,
        aspect,
    };
    send(cx, player, message);
    Ok(())
}

/// `RevertCamera`: `REVERT_CAMERA` to the player.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn revert_camera(cx: &mut Cx<'_>, player: u32) -> Result<(), PubError> {
    if !valid(cx, player) {
        return Err(PubError::BAD_ID);
    }
    send(cx, player, RevertCamera {});
    Ok(())
}

/// `LoadHint`: `LOADHINT` (the hint's kind and the bytes its `Serialize`
/// gives, which the caller passes) to the id as given; not checked. Always
/// 0.
pub fn load_hint(cx: &mut Cx<'_>, player: u32, kind: u32, data: &[u8]) {
    let message = LoadHint {
        kind,
        data: data.to_vec(),
    };
    send(cx, player, message);
}

/// `GetGroupSize`: the members of a valid player's group, 1 without one
/// (and for an invalid player). Always 0.
#[must_use]
pub fn group_size(cx: &Cx<'_>, player: u32) -> u32 {
    if !valid(cx, player) {
        return 1;
    }
    group_of(cx, player).map_or(1, |g| members(cx, g).len() as u32)
}

/// `GetName`: the active character's name (`PlayerDB::
/// GetActiveCharacterName`): on a multiplayer server before a character is
/// selected none, which answers `<bad name>`; otherwise the vibe's display
/// name, or `>>>BAD   NAME<<<` for a vibe without one. `None` (the string
/// left as it was) for an invalid player. Always 0.
#[must_use]
pub fn name(cx: &mut Cx<'_>, player: u32) -> Option<Vec<u16>> {
    let p = slot(cx, player).ok()?;
    if cx.logic.multiplayer_server && !p.selected_character.is_valid() {
        return Some(BAD_NAME_LITERAL.encode_utf16().collect());
    }
    let vibe = p.rep_vibe;
    let display = cx.world.vibes().0.name(vibe).map(|(_, _, display)| display);
    Some(match display {
        Some(units) => {
            let end = units.iter().position(|&u| u == 0).unwrap_or(units.len());
            units[..end].to_vec()
        }
        None => BAD_NAME.encode_utf16().collect(),
    })
}

/// `IsGroupMember`: true for the same id; otherwise whether the first
/// player (when valid) has a group the second is a member of. Always 0.
#[must_use]
pub fn is_group_member(cx: &Cx<'_>, first: u32, second: u32) -> bool {
    if first == second {
        return true;
    }
    if !valid(cx, first) {
        return false;
    }
    group_of(cx, first).is_some_and(|g| {
        cx.logic
            .groups
            .get(g)
            .is_some_and(|g| g.members.contains(&second))
    })
}

/// `RemoveFromGroup`: the player leaves its group (`CPlayerGroup::
/// DelMember`, [`crate::group::del_member`]).
///
/// # Errors
///
/// -3 for an invalid player or one without a group.
pub fn remove_from_group(cx: &mut Cx<'_>, player: u32) -> Result<(), PubError> {
    if !valid(cx, player) {
        return Err(PubError::NOT_FOUND);
    }
    let group = group_of(cx, player).ok_or(PubError::NOT_FOUND)?;
    crate::group::del_member(cx, group, player);
    Ok(())
}

/// `RewardGroup`: the members of the player's group take the amount
/// (`CPlayerGroup::RewardMembers`); a valid player without a group takes
/// it alone (`PlayerData::AdjustCash(amount, true)`). The C++ reads the
/// group of any id, valid or not, from the slot array without a bound; an
/// id outside the array has no group here (D695). Always 0.
pub fn reward_group(cx: &mut Cx<'_>, player: u32, amount: i32) {
    if let Some(group) = group_of(cx, player) {
        crate::group::reward_members(cx, group, amount);
        return;
    }
    if valid(cx, player) {
        player::adjust_cash(cx, player, amount, true);
    }
}

/// `SetMsnID`: the slot's group mission and who set it; with `save`,
/// `MISSIONSAVEA` (the parameter) to the slot's client; then
/// `MISCOBJUPDATE`'s mission form to the player. Nothing for an invalid
/// player. Always 0.
pub fn set_msn_id(cx: &mut Cx<'_>, player: u32, mission: u32, set_by: u32, save: bool, param: u32) {
    let Ok(p) = slot_mut(cx, player) else {
        return;
    };
    p.group_mission_id = mission;
    p.group_mission_set_by = set_by;
    let client_id = p.client_id;
    if save {
        send(cx, client_id, MissionSaveA { value: param });
    }
    client::misc_obj_update_mission(cx, player, mission, set_by);
}

/// `GetMsnID`: the slot's group mission; `None` (left as it was) for an
/// invalid player. Always 0.
#[must_use]
pub fn msn_id(cx: &Cx<'_>, player: u32) -> Option<u32> {
    slot(cx, player).ok().map(|p| p.group_mission_id)
}

/// `SetRank`: stored (unsigned), and `MISCOBJUPDATE`'s rank form (the
/// player twice: as the client and as the player) to the player.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn set_rank(cx: &mut Cx<'_>, player: u32, rank: i32) -> Result<(), PubError> {
    slot_mut(cx, player)?.rank_level = rank as u32;
    client::misc_obj_update_rank(cx, player, player, rank);
    Ok(())
}

/// `SetMoneyNeededToNextRank`: stored.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn set_money_needed_to_next_rank(
    cx: &mut Cx<'_>,
    player: u32,
    money: i32,
) -> Result<(), PubError> {
    slot_mut(cx, player)?.money_needed_to_next_rank = money;
    Ok(())
}

/// `SetMonkey`: the base costume's first part becomes `0x88db0044`; the
/// rest stays. Nothing is sent.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn set_monkey(cx: &mut Cx<'_>, player: u32) -> Result<(), PubError> {
    slot_mut(cx, player)?.costume.part_ids[0] = 0x88db_0044;
    Ok(())
}

/// `SetRobot`: the base costume's parts become 0, `0x8dffc147`,
/// `0x9dc4c883` and `0x93582d4f`. Nothing is sent.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn set_robot(cx: &mut Cx<'_>, player: u32) -> Result<(), PubError> {
    slot_mut(cx, player)?.costume.part_ids = [0, 0x8dff_c147, 0x9dc4_c883, 0x9358_2d4f];
    Ok(())
}

/// `SetRellan`: the base costume becomes the costume `rellan`
/// (`CostumeDescriptions::get_costume`; a missing one is reported and
/// gives no parts). Nothing is sent.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn set_rellan(cx: &mut Cx<'_>, player: u32) -> Result<(), PubError> {
    slot(cx, player)?;
    let data = cx.world.data_arc();
    let costume = data
        .costumes
        .get_costume(RELLAN_COSTUME, &mut cx.logic.reports);
    slot_mut(cx, player)?.costume = costume;
    Ok(())
}

/// `SendNNMessage`: `SENDNNMESSAGE` (the message) to the player.
///
/// # Errors
///
/// -4 for message 0 and for an invalid player alike.
pub fn send_nn_message(cx: &mut Cx<'_>, player: u32, message: u32) -> Result<(), PubError> {
    if message == 0 || !valid(cx, player) {
        return Err(PubError::BAD_ARGUMENT);
    }
    send(cx, player, SendNnMessage { value: message });
    Ok(())
}

/// `ForceLand`: `IServerImpl::Dock(base, player)` ([`crate::dock::
/// force_land`]), which checks the player itself. Always 0.
pub fn force_land(cx: &mut Cx<'_>, player: u32, base: u32) {
    crate::dock::force_land(cx, base, player);
}

/// `SetCostume`: a shipped no-op. Always 0.
pub fn set_costume(_cx: &mut Cx<'_>, _player: u32, _costume: i32) {}

/// `GetGender`: 1, for any id. Always 0.
#[must_use]
pub fn gender(_cx: &Cx<'_>, _player: u32) -> i32 {
    1
}

/// `GetBody`: 0, for any id. Always 0.
#[must_use]
pub fn body(_cx: &Cx<'_>, _player: u32) -> u32 {
    0
}

/// `GetGroupMembers`: the members of the player's group in id order
/// (`CPlayerGroup::StoreMemberList`), or the player alone.
///
/// # Errors
///
/// -2 for an invalid player (the C++ clears the vector first).
pub fn group_members(cx: &Cx<'_>, player: u32) -> Result<Vec<u32>, PubError> {
    slot(cx, player)?;
    Ok(match group_of(cx, player) {
        Some(group) => members(cx, group),
        None => vec![player],
    })
}

/// `SetMissionObjectives`: `SETMISSIONOBJECTIVES` (the mission, the type
/// byte as the state, the title and the description flattened, and each
/// objective's state byte and description) to the player. Nothing is
/// stored.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn set_mission_objectives(
    cx: &mut Cx<'_>,
    player: u32,
    mission: u32,
    objectives: &[MissionObjective],
    title: &FmtStr,
    kind: u8,
    description: &FmtStr,
) -> Result<(), PubError> {
    if !valid(cx, player) {
        return Err(PubError::BAD_ID);
    }
    let message = SetMissionObjectives {
        field_00: mission,
        state: u32::from(kind),
        text_a: flatten(title),
        text_b: flatten(description),
        objectives: objectives
            .iter()
            .map(|o| WireObjective {
                state: o.kind as u8,
                text: flatten(&o.description),
            })
            .collect(),
    };
    send(cx, player, message);
    Ok(())
}

/// `ReplaceMissionObjective`: `REPLACEMISSIONOBJECTIVE` (the mission, the
/// index's low byte as the state, the description flattened) to the
/// player; the objective's type does not travel. Nothing is stored.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn replace_mission_objective(
    cx: &mut Cx<'_>,
    player: u32,
    mission: u32,
    index: u32,
    objective: &MissionObjective,
) -> Result<(), PubError> {
    if !valid(cx, player) {
        return Err(PubError::BAD_ID);
    }
    let message = ReplaceMissionObjective {
        field_00: mission,
        state: index & 0xFF,
        text: flatten(&objective.description),
    };
    send(cx, player, message);
    Ok(())
}

/// `SetMissionObjectiveState`: `SETMISSIONOBJECTIVESTATE` (the mission,
/// the index's low byte, the state's low word) to the player.
///
/// # Errors
///
/// -2 for an invalid player.
pub fn set_mission_objective_state(
    cx: &mut Cx<'_>,
    player: u32,
    mission: u32,
    index: i32,
    state: u32,
) -> Result<(), PubError> {
    if !valid(cx, player) {
        return Err(PubError::BAD_ID);
    }
    let message = SetMissionObjectiveState {
        field_00: mission,
        field_04: u32::from(index as u8),
        field_08: state as u16,
    };
    send(cx, player, message);
    Ok(())
}
