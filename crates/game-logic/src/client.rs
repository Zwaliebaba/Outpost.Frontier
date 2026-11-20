//! The server's `IClient` calls as typed messages (work package P3.2): each
//! function builds the packet rpclocal's sender builds from the slot's
//! arguments (`rpclocal/rpc_client_proxy.cpp`, `rpc_payload_ch2.cpp`,
//! `rpc_containers.cpp`) and hands it to the context's client sink, which
//! queues it (record 0002). More slots arrive with the work that calls them.

use game_sim::data::equip_desc::EquipDesc;
use game_sim::data::string_cache::StringCache;
use game_sim::protocol::ToClient;
use game_sim::protocol::records::{CollisionGroup, EquipRecord};
use game_sim::protocol::to_client::{
    SetAddItem, SetCash, SetCollisionGroups, SetEquipment, SetHullStatus, SetRemoveItem,
    SetShipArch,
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
