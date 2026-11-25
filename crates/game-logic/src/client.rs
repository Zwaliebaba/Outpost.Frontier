//! The server's `IClient` calls as typed messages (work package P3.2): each
//! function builds the packet rpclocal's sender builds from the slot's
//! arguments (`rpclocal/rpc_client_proxy.cpp`, `rpc_payload_ch2.cpp`,
//! `rpc_containers.cpp`) and hands it to the context's client sink, which
//! queues it (record 0002). More slots arrive with the work that calls them.

use game_sim::data::equip_desc::EquipDesc;
use game_sim::data::string_cache::StringCache;
use game_sim::protocol::ToClient;
use game_sim::protocol::common::{
    PlayerTrade, RequestGroupPositions, RequestRankLevel, SetInterfaceState, SetMissionLog,
    SetVisitedState, SetWeaponGroup,
};
use game_sim::protocol::fmtstr::FmtStr;
use game_sim::protocol::records::{CollisionGroup, EquipRecord};
use game_sim::protocol::to_client::{
    Directive, Launch, MiscObjUpdate, PlayerList, RequestCreateShipResp, RequestReturned,
    ScanNotify, SetAddItem, SetCash, SetCollisionGroups, SetDirective, SetEquipment, SetHullStatus,
    SetRemoveItem, SetShipArch,
};

use crate::character_file::CollisionGroupDesc;
use crate::server::Cx;

fn send(
    cx: &mut Cx<'_>,
    client: u32,
    message: impl Into<game_sim::protocol::to_client::ServerMessage>,
) {
    cx.logic
        .client
        .send(client, ToClient::Server(message.into()));
}

/// A hardpoint as the record or packet carries it: the name with its
/// terminator.
fn hardpoint_bytes(desc: &EquipDesc, strings: &StringCache) -> Vec<u8> {
    let mut bytes = strings.get(desc.hardpoint).to_vec();
    bytes.push(0);
    bytes
}

/// `SendServerSetAddItem` (slot +0x068, ch2 0x2e): the item's archetype,
/// id, count, health and flags, the add result, and the hardpoint when the
/// item is not in the internal cargo bay and its name is not empty.
pub fn set_add_item(cx: &mut Cx<'_>, client: u32, add_result: i32, desc: &EquipDesc) {
    let strings = cx.world.strings();
    let hardpoint = if desc.is_internal(strings) || strings.get(desc.hardpoint).is_empty() {
        Vec::new()
    } else {
        hardpoint_bytes(desc, strings)
    };
    let message = SetAddItem {
        arch_id: desc.arch_id,
        id: u32::from(desc.id),
        count: desc.count,
        health: desc.health,
        equipped: desc.equipped,
        mission: u8::from(desc.mission),
        field_0c: add_result as u32,
        hardpoint,
    };
    send(cx, client, message);
}

/// `SendServerSetRemoveItem` (slot +0x06c, ch2 0x2f): the sender permutes
/// its arguments, the id first, then the count, then the result.
pub fn set_remove_item(cx: &mut Cx<'_>, client: u32, remove_result: i32, id: u16, count: i32) {
    let message = SetRemoveItem {
        field_00: u32::from(id),
        field_04: count as u32,
        field_08: remove_result as u32,
    };
    send(cx, client, message);
}

/// `SendServerSetCash` (slot +0x134, ch2 0x30).
pub fn set_cash(cx: &mut Cx<'_>, client: u32, cash: i32) {
    send(cx, client, SetCash { value: cash as u32 });
}

/// `SendServerSetHullStatus` (slot +0x058, ch2 0x49).
pub fn set_hull_status(cx: &mut Cx<'_>, client: u32, status: f32) {
    send(cx, client, SetHullStatus { value: status });
}

/// `SendServerSetShipArch` (slot +0x054, ch2 0x23).
pub fn set_ship_arch(cx: &mut Cx<'_>, client: u32, ship: u32) {
    send(cx, client, SetShipArch { value: ship });
}

/// `SendServerSetCollisionGroups` (slot +0x05c, ch2 0x48).
pub fn set_collision_groups(cx: &mut Cx<'_>, client: u32, groups: &[CollisionGroupDesc]) {
    let message = SetCollisionGroups {
        groups: groups
            .iter()
            .map(|g| CollisionGroup {
                id: g.id,
                health: g.health,
            })
            .collect(),
    };
    send(cx, client, message);
}

/// `SendServerSetEquipment` (slot +0x060, ch2 0x24): every item as a
/// record; the hardpoint (with its terminator, even when the name is empty)
/// travels unless the item is internal (`RpcEquipHandleFromEquipDescVector`).
pub fn set_equipment(cx: &mut Cx<'_>, client: u32, items: &[EquipDesc]) {
    let strings = cx.world.strings();
    let records = items
        .iter()
        .map(|item| EquipRecord {
            count: item.count,
            health: item.health,
            arch_id: item.arch_id,
            id: item.id,
            equipped: item.equipped,
            field_0f: 0,
            hardpoint: if item.is_internal(strings) {
                Vec::new()
            } else {
                hardpoint_bytes(item, strings)
            },
        })
        .collect();
    send(cx, client, SetEquipment { records });
}

/// `SendServerRemoteObjectCargoUpdate` (slot +0x154, ch2 0x3d): another
/// ship's cargo, with the flag byte; only the items not equipped travel
/// (`RpcEquipHandleFromVector` with `bAll` clear), each hardpoint as in
/// [`set_equipment`].
pub fn remote_object_cargo_update(
    cx: &mut Cx<'_>,
    client: u32,
    object: u32,
    items: &[EquipDesc],
    flag: u8,
) {
    let strings = cx.world.strings();
    let records = items
        .iter()
        .filter(|item| item.equipped == 0)
        .map(|item| EquipRecord {
            count: item.count,
            health: item.health,
            arch_id: item.arch_id,
            id: item.id,
            equipped: item.equipped,
            field_0f: 0,
            hardpoint: if item.is_internal(strings) {
                Vec::new()
            } else {
                hardpoint_bytes(item, strings)
            },
        })
        .collect();
    let message = game_sim::protocol::to_client::RemoteObjectCargoUpdate {
        object_id: object,
        flag,
        records,
    };
    send(cx, client, message);
}

fn send_common(
    cx: &mut Cx<'_>,
    client: u32,
    message: impl Into<game_sim::protocol::common::CommonMessage>,
) {
    cx.logic
        .client
        .send(client, ToClient::Common(message.into()));
}

/// The player-list kinds (`kPlayerListKind*`).
const PLAYER_LIST_CLEAR: u32 = 0;
const PLAYER_LIST_ADD: u32 = 1;
const PLAYER_LIST_REMOVE: u32 = 2;

/// `SendServerPlayerListClear` (slot +0x1c0, ch2 0x52).
pub fn player_list_clear(cx: &mut Cx<'_>, client: u32) {
    let message = PlayerList {
        kind: PLAYER_LIST_CLEAR,
        ..PlayerList::default()
    };
    send(cx, client, message);
}

/// `SendServerPlayerListAdd` (slot +0x1b8, ch2 0x52): the name travels
/// with its terminator.
pub fn player_list_add(cx: &mut Cx<'_>, client: u32, name: &[u16], player: u32, present: bool) {
    let mut units = name.to_vec();
    units.push(0);
    let message = PlayerList {
        kind: PLAYER_LIST_ADD,
        player_id: player,
        field_2c: u8::from(present),
        name: units,
    };
    send(cx, client, message);
}

/// `SendServerPlayerListRemove` (slot +0x1bc, ch2 0x52).
pub fn player_list_remove(cx: &mut Cx<'_>, client: u32, player: u32) {
    let message = PlayerList {
        kind: PLAYER_LIST_REMOVE,
        player_id: player,
        ..PlayerList::default()
    };
    send(cx, client, message);
}

/// `SendServerMiscObjUpdateRank` (slot +0x1d4, ch2 0x54): the player and
/// the rank's low word.
pub fn misc_obj_update_rank(cx: &mut Cx<'_>, client: u32, player: u32, rank: i32) {
    let message = MiscObjUpdate {
        flags: 0x04 | 0x40,
        field_24: player,
        field_2c: rank as u16,
        ..MiscObjUpdate::default()
    };
    send(cx, client, message);
}

/// `SendServerMiscObjUpdatePlayerGroup` (slot +0x1d8, ch2 0x54).
pub fn misc_obj_update_player_group(cx: &mut Cx<'_>, client: u32, player: u32, group: u32) {
    let message = MiscObjUpdate {
        flags: 0x04 | 0x01,
        field_24: player,
        field_34: group,
        ..MiscObjUpdate::default()
    };
    send(cx, client, message);
}

/// `SendServerMiscObjUpdateSystem` (slot +0x1dc, ch2 0x54).
pub fn misc_obj_update_system(cx: &mut Cx<'_>, client: u32, player: u32, system: u32) {
    let message = MiscObjUpdate {
        flags: 0x04 | 0x80,
        field_24: player,
        field_30: system,
        ..MiscObjUpdate::default()
    };
    send(cx, client, message);
}

/// `PlayerData_SendVisitedState` through `SendCommonSetVisitedState` (ch1
/// 0x13): the count, then each visit's id and flags, five bytes each.
pub fn set_visited_state(
    cx: &mut Cx<'_>,
    client: u32,
    visits: &std::collections::BTreeMap<u32, u8>,
) {
    let mut data = Vec::with_capacity(4 + 5 * visits.len());
    data.extend_from_slice(&(visits.len() as u32).to_le_bytes());
    for (&id, &flags) in visits {
        data.extend_from_slice(&id.to_le_bytes());
        data.push(flags);
    }
    send_common(cx, client, SetVisitedState { data });
}

/// `SendCommonSetWeaponGroup` (ch1 0x11): the text up to its first NUL
/// (`strlen`).
pub fn set_weapon_group(cx: &mut Cx<'_>, client: u32, text: &[u8]) {
    let end = text.iter().position(|&b| b == 0).unwrap_or(text.len());
    send_common(
        cx,
        client,
        SetWeaponGroup {
            data: text[..end].to_vec(),
        },
    );
}

/// `PlayerData_SendMissionLog` through `SendCommonSetMissionLog` (ch1
/// 0x19): the words 1 and the entry count, then each entry flattened. An
/// entry that does not flatten is left out.
pub fn set_mission_log(cx: &mut Cx<'_>, client: u32, log: &[FmtStr]) {
    let mut data = Vec::new();
    data.extend_from_slice(&1u32.to_le_bytes());
    data.extend_from_slice(&(log.len() as u32).to_le_bytes());
    for entry in log {
        if let Ok(bytes) = entry.flatten() {
            data.extend_from_slice(&bytes);
        }
    }
    send_common(cx, client, SetMissionLog { data });
}

/// `SendCommonSetInterfaceState` (ch1 0x1c): the state as four bytes.
pub fn set_interface_state(cx: &mut Cx<'_>, client: u32, state: u32) {
    send_common(
        cx,
        client,
        SetInterfaceState {
            data: state.to_le_bytes().to_vec(),
        },
    );
}

/// `SendServerMiscObjUpdateMission` (slot +0x1c8, ch2 0x54): the group's
/// mission and who set it.
pub fn misc_obj_update_mission(cx: &mut Cx<'_>, client: u32, mission: u32, set_by: u32) {
    let message = MiscObjUpdate {
        flags: 0x02,
        field_38: mission,
        field_3c: set_by,
        ..MiscObjUpdate::default()
    };
    send(cx, client, message);
}

/// `SendServerSetMissionObjectives` (ch2 0x35) with no objectives: the
/// mission and state 0 and two empty format strings.
pub fn clear_mission_objectives(cx: &mut Cx<'_>, client: u32) {
    let empty = FmtStr::default().flatten().unwrap_or_default();
    let message = game_sim::protocol::to_client::SetMissionObjectives {
        field_00: 0,
        state: 0,
        text_a: empty.clone(),
        text_b: empty,
        objectives: Vec::new(),
    };
    send(cx, client, message);
}

/// `SendServerRequestReturned` (slot +0x14c, ch2 0x44): the object, and the
/// request type, response and third argument as bytes.
pub fn request_returned(
    cx: &mut Cx<'_>,
    client: u32,
    object: u32,
    kind: i32,
    response: i32,
    arg: i32,
) {
    let message = RequestReturned {
        object_id: object,
        request_type: kind as u8,
        response: response as u8,
        field_26: arg as u8,
    };
    send(cx, client, message);
}

/// `SendServerRequestCreateShipResp` (slot +0x0f4, ch2 0x27): the success
/// byte and the ship.
pub fn request_create_ship_resp(cx: &mut Cx<'_>, client: u32, success: bool, ship: u32) {
    let message = RequestCreateShipResp {
        field_00: u8::from(success),
        field_04: ship,
    };
    send(cx, client, message);
}

/// `SendServerLaunch` (slot +0x0f0, ch2 0x07): the ship, the launch object
/// and point, the position and the orientation.
pub fn launch(
    cx: &mut Cx<'_>,
    client: u32,
    ship: u32,
    object: u32,
    kind: i32,
    position: neuron_core::math::Vector,
    orientation: neuron_core::math::Quaternion,
) {
    let message = Launch {
        field_00: ship,
        field_04: object,
        field_08: kind as u32,
        position: [position.x, position.y, position.z],
        orientation: [orientation.w, orientation.x, orientation.y, orientation.z],
    };
    send(cx, client, message);
}

/// `SendServerMiscObjUpdateObjectGroup` (slot +0x1c4, ch2 0x54, mask 0x09):
/// the object and its group.
pub fn misc_obj_update_object_group(cx: &mut Cx<'_>, client: u32, object: u32, group: u32) {
    let message = MiscObjUpdate {
        flags: 0x08 | 0x01,
        field_28: object,
        field_34: group,
        ..MiscObjUpdate::default()
    };
    send(cx, client, message);
}

/// `SendServerMiscObjUpdateAffiliation` (slot +0x1d0, ch2 0x54, mask 0x28):
/// the object and its affiliation.
pub fn misc_obj_update_affiliation(cx: &mut Cx<'_>, client: u32, object: u32, affiliation: u32) {
    let message = MiscObjUpdate {
        flags: 0x08 | 0x20,
        field_28: object,
        field_54: affiliation,
        ..MiscObjUpdate::default()
    };
    send(cx, client, message);
}

/// `SendServerObjectCargoUpdate` (slot +0x158, ch2 0x55): the object and
/// one stack (its id, archetype, the count's low word, health and the raw
/// mission byte).
pub fn object_cargo_update(cx: &mut Cx<'_>, client: u32, object: u32, desc: &EquipDesc) {
    let message = game_sim::protocol::to_client::ObjectCargoUpdate {
        object_id: object,
        arch_id: desc.arch_id,
        equip_id: desc.id,
        count: desc.count as u16,
        status: desc.health,
        mission: u8::from(desc.mission),
    };
    send(cx, client, message);
}

/// `SendServerSetDirective` (slot +0x190, ch2 0x4e): the directive record
/// as the server built it, sent to the client by reference.
pub fn set_directive(cx: &mut Cx<'_>, client: u32, directive: Directive) {
    send(cx, client, SetDirective { directive });
}

/// `SendServerScanNotify` (slot +0x1b4, ch2 0x39): the scanner and the
/// good scanned for.
pub fn scan_notify(cx: &mut Cx<'_>, client: u32, scanner: u32, good: u32) {
    let message = ScanNotify {
        field_00: scanner,
        field_04: good,
    };
    send(cx, client, message);
}

/// `SendServerMiscObjUpdateFormation` (slot +0x1e0, ch2 0x54, mask 0x100):
/// the formation's members. The marshaller raises the flag once per
/// member, so an empty list goes out with no flag and no list.
pub fn misc_obj_update_formation(cx: &mut Cx<'_>, client: u32, ids: &[u32]) {
    let message = MiscObjUpdate {
        flags: if ids.is_empty() { 0 } else { 0x100 },
        ids: ids.to_vec(),
        ..MiscObjUpdate::default()
    };
    send(cx, client, message);
}

/// `SendCommonRequestGroupPositions` (slot +0x180, ch1 0x17): the block.
pub fn request_group_positions(cx: &mut Cx<'_>, client: u32, data: Vec<u8>) {
    send_common(cx, client, RequestGroupPositions { data });
}

/// `SendCommonRequestRankLevel` (slot +0x18c, ch1 0x1a): the block.
pub fn request_rank_level(cx: &mut Cx<'_>, client: u32, data: Vec<u8>) {
    send_common(cx, client, RequestRankLevel { data });
}

/// One `FLPACKET_COMMON_PLAYER_TRADE` (ch1 0x12) of the server's.
fn player_trade(cx: &mut Cx<'_>, client: u32, trade: PlayerTrade) {
    send_common(cx, client, trade);
}

/// `SendCommonPlayerTradeInitiateTrade` (slot +0x194, action 0x01): the
/// partner's ship as the target.
pub fn player_trade_initiate(cx: &mut Cx<'_>, client: u32, ship: u32) {
    let trade = PlayerTrade {
        action: 0x01,
        target_player: ship,
        ..PlayerTrade::default()
    };
    player_trade(cx, client, trade);
}

/// `SendCommonPlayerTradeTerminateTrade` (slot +0x198, action 0x02): the
/// reason, stored to the packet's byte.
pub fn player_trade_terminate(cx: &mut Cx<'_>, client: u32, reason: i32) {
    let [reason, ..] = reason.to_le_bytes();
    let trade = PlayerTrade {
        action: 0x02,
        reason: i8::from_le_bytes([reason]),
        ..PlayerTrade::default()
    };
    player_trade(cx, client, trade);
}

/// `SendCommonPlayerTradeAcceptTrade` (slot +0x19c, action 0x40): the
/// ship of the player that accepted, and whether it did.
pub fn player_trade_accept(cx: &mut Cx<'_>, client: u32, ship: u32, accepted: bool) {
    let trade = PlayerTrade {
        action: 0x40,
        target_player: ship,
        accepted,
        ..PlayerTrade::default()
    };
    player_trade(cx, client, trade);
}

/// `SendCommonPlayerTradeSetTradeMoney` (slot +0x1a0, action 0x04): the
/// offering player's ship and the money, as an `int`.
pub fn player_trade_money(cx: &mut Cx<'_>, client: u32, ship: u32, money: u32) {
    let trade = PlayerTrade {
        action: 0x04,
        money: i32::from_ne_bytes(money.to_ne_bytes()),
        target_player: ship,
        ..PlayerTrade::default()
    };
    player_trade(cx, client, trade);
}

/// `SendCommonPlayerTradeAddTradeEquip` (slot +0x1a4, action 0x08): the
/// offering player's ship and the item's archetype, count, health and id.
pub fn player_trade_add_equip(cx: &mut Cx<'_>, client: u32, ship: u32, item: &EquipDesc) {
    let trade = PlayerTrade {
        action: 0x08,
        equip_arch_id: item.arch_id,
        equip_count: item.count,
        equip_health: item.health,
        equip_id: item.id,
        target_player: ship,
        ..PlayerTrade::default()
    };
    player_trade(cx, client, trade);
}

/// `SendCommonPlayerTradeDelTradeEquip` (slot +0x1a8, action 0x10): as
/// [`player_trade_add_equip`] without the health.
pub fn player_trade_del_equip(cx: &mut Cx<'_>, client: u32, ship: u32, item: &EquipDesc) {
    let trade = PlayerTrade {
        action: 0x10,
        equip_arch_id: item.arch_id,
        equip_count: item.count,
        equip_id: item.id,
        target_player: ship,
        ..PlayerTrade::default()
    };
    player_trade(cx, client, trade);
}

/// `SendCommonPlayerTradeRequestTrade` (slot +0x1ac, action 0x20): the
/// asking player's ship.
pub fn player_trade_request(cx: &mut Cx<'_>, client: u32, ship: u32) {
    let trade = PlayerTrade {
        action: 0x20,
        target_player: ship,
        ..PlayerTrade::default()
    };
    player_trade(cx, client, trade);
}

/// `SendCommonPlayerTradeStopTradeRequest` (slot +0x1b0, action 0x80): the
/// asking player's ship.
pub fn player_trade_stop_request(cx: &mut Cx<'_>, client: u32, ship: u32) {
    let trade = PlayerTrade {
        action: 0x80,
        target_player: ship,
        ..PlayerTrade::default()
    };
    player_trade(cx, client, trade);
}
