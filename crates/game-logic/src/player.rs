//! The player database (work package P3.2): `PlayerData`
//! (`include/common_server_playerdb.h`), one per session slot of
//! [`neuron_server::session::SessionTable`], and its cash, equipment and
//! ship functions (`Server/server_classes.cpp:1270-1855`).
//!
//! Each function takes the server context and a session id: it borrows the
//! slot only between calls that can dispatch, sends what the C++ sends to
//! the client through [`crate::client`], and broadcasts `PlayerWorthChanged`
//! or `PlayerShipArchSet` where the C++ does. The C++ indexes the player
//! array without a check; these functions do nothing for an id outside it
//! (D598).
//!
//! `PlayerData`'s other fields (the account, the saved characters, the
//! trade, the visits, the group) arrive with the work that uses them.

use game_sim::data::equip_desc::EquipDesc;
use game_sim::data::equip_desc::SUB_OBJECT_ID_NONE;
use game_sim::data::equip_desc::list;
use game_sim::data::loadout::add_equipment_item;
use game_sim::data::string_cache::CacheString;
use neuron_core::id::sub_object::EquipmentIds;
use neuron_server::controller::ControllerId;
use neuron_server::session::SessionSlot;

use crate::character_file::CollisionGroupDesc;
use crate::client;
use crate::events::ControllerMessage;
use crate::server::Cx;

/// The most cash a player can have (`999999999`).
pub const CASH_CEILING: i32 = 999_999_999;

/// One session slot's player (`PlayerData`).
#[derive(Clone, Debug, PartialEq)]
pub struct PlayerData {
    /// The client id (+0x348); 0 for a free slot.
    pub client_id: u32,
    /// The selected character's id (+0x64), empty when none is selected.
    pub selected_character: Vec<u8>,
    /// Cash (+0x31c).
    pub cash: i32,
    /// What the player earned (+0x320), for the cheat check.
    pub total_earned: i32,
    /// The ship archetype (+0x264).
    pub ship_archetype: u32,
    /// The hull's health, 0 to 1 (+0x268).
    pub relative_health: f32,
    /// The ship's collision groups (+0x26c).
    pub collision_groups: Vec<CollisionGroupDesc>,
    /// The equipment and cargo (+0x278), in list order.
    pub equipment: Vec<EquipDesc>,
    /// The ship at the last equipment snapshot (+0x324).
    pub equip_snapshot_arch: u32,
    /// The equipment at the last snapshot (+0x328).
    pub equip_snapshot: Vec<EquipDesc>,
    /// The equipment-id counter (+0x3c8).
    pub equip_ids: EquipmentIds,
    /// The base the player is in (+0x3ec).
    pub current_base: u32,
    /// The base `last_base` names (+0x3f0).
    pub last_base_id: u32,
}

impl Default for PlayerData {
    /// The slot constructor: everything zero or empty, the health 1.
    fn default() -> Self {
        Self {
            client_id: 0,
            selected_character: Vec::new(),
            cash: 0,
            total_earned: 0,
            ship_archetype: 0,
            relative_health: 1.0,
            collision_groups: Vec::new(),
            equipment: Vec::new(),
            equip_snapshot_arch: 0,
            equip_snapshot: Vec::new(),
            equip_ids: EquipmentIds::default(),
            current_base: 0,
            last_base_id: 0,
        }
    }
}

impl SessionSlot for PlayerData {
    fn client_id(&self) -> u32 {
        self.client_id
    }
}

/// `PlayerData_EquipGateOpen`: outside a multiplayer server always; on one
/// only once a character is selected.
fn equip_gate_open(cx: &Cx<'_>, slot: &PlayerData) -> bool {
    !cx.logic.multiplayer_server || !slot.selected_character.is_empty()
}

/// The slot of `session`.
fn slot<'s>(cx: &'s mut Cx<'_>, session: u32) -> Option<&'s mut PlayerData> {
    cx.logic.players.get_mut(session)
}

/// Broadcasts `PlayerWorthChanged` for the client.
fn worth_changed(cx: &mut Cx<'_>, client: u32) {
    let mut message = ControllerMessage::PlayerWorthChanged { client };
    let _ = cx.dispatch(ControllerId::BROADCAST, &mut message);
}

/// `PlayerData_AddEquipment`: a new item, or more of an existing cargo
/// line for a combinable good (unless `no_combine`), numbered from the
/// slot's counter. Sends `SetAddItem` when `notify` and broadcasts
/// `PlayerWorthChanged`. Returns the item's id, or `SUBOBJ_ID_NONE` when
/// the gate is shut.
#[allow(clippy::too_many_arguments)]
pub fn add_equipment(
    cx: &mut Cx<'_>,
    session: u32,
    arch_id: u32,
    hardpoint: CacheString,
    count: i32,
    health: f32,
    equipped: u8,
    no_combine: bool,
    notify: bool,
) -> u16 {
    let multiplayer_server = cx.logic.multiplayer_server;
    let data = cx.world.data();
    let combinable = data.goods.arch_is_combinable(arch_id) && !no_combine;
    let strings = cx.world.strings();
    let Some(player) = cx.logic.players.get_mut(session) else {
        return SUB_OBJECT_ID_NONE;
    };
    if multiplayer_server && player.selected_character.is_empty() {
        return SUB_OBJECT_ID_NONE;
    }
    let id = if combinable {
        match list::find_matching_cargo(&player.equipment, &data.goods, arch_id, 0, 1.0) {
            Some(at) => player.equipment[at].id,
            None => player.equip_ids.create(),
        }
    } else {
        player.equip_ids.create()
    };
    let mut desc = EquipDesc::new(strings);
    desc.arch_id = arch_id;
    desc.id = id;
    desc.hardpoint = hardpoint;
    desc.count = count;
    desc.health = health;
    desc.equipped = equipped;
    // A line that matches only by archetype makes the reference scan for
    // ever; here the item is not added (D115).
    let combined = i32::from(
        add_equipment_item(&mut player.equipment, &desc, combinable, strings).unwrap_or(false),
    );
    let client_id = player.client_id;
    if notify {
        client::set_add_item(cx, client_id, combined, &desc);
    }
    worth_changed(cx, client_id);
    id
}

/// `PlayerData_RemoveEquipment`: removes `count` of the item (-1 for all);
/// sends `SetRemoveItem` when `notify`; broadcasts `PlayerWorthChanged`.
pub fn remove_equipment(cx: &mut Cx<'_>, session: u32, id: u16, count: i32, notify: bool) {
    let Some(player) = cx.logic.players.get(session) else {
        return;
    };
    if !equip_gate_open(cx, player) {
        return;
    }
    let Some(player) = slot(cx, session) else {
        return;
    };
    let result = list::remove_equipment_item(&mut player.equipment, id, count);
    let client_id = player.client_id;
    if notify {
        client::set_remove_item(cx, client_id, result, id, count);
    }
    worth_changed(cx, client_id);
}

/// `PlayerData_RemoveCargo`: the cargo line that matches the good, count
/// and health, through [`remove_equipment`]; a warning when there is none.
pub fn remove_cargo(
    cx: &mut Cx<'_>,
    session: u32,
    arch_id: u32,
    count: i32,
    health: f32,
    notify: bool,
) {
    let Some(player) = cx.logic.players.get(session) else {
        return;
    };
    let found = list::find_matching_cargo(
        &player.equipment,
        &cx.world.data().goods,
        arch_id,
        count,
        health,
    )
    .map(|at| player.equipment[at].id);
    match found {
        Some(id) => remove_equipment(cx, session, id, count, notify),
        None => neuron_core::diag::dump(
            neuron_core::diag::DumpFlags::WARNING,
            "Server\\PlayerDB.cpp(3562) : *** WARNING: Tried to remove cargo player doesn't have!",
        ),
    }
}

/// `PlayerData_ModifyEquipment`: the item's hardpoint, count, health and
/// mount; nothing for an unknown id; broadcasts `PlayerWorthChanged`.
pub fn modify_equipment(
    cx: &mut Cx<'_>,
    session: u32,
    id: u16,
    hardpoint: CacheString,
    count: i32,
    health: f32,
    equipped: u8,
) {
    let Some(player) = cx.logic.players.get(session) else {
        return;
    };
    if !equip_gate_open(cx, player) {
        return;
    }
    let Some(player) = slot(cx, session) else {
        return;
    };
    let Some(at) = list::find_equipment_item(&player.equipment, id) else {
        return;
    };
    let item = &mut player.equipment[at];
    item.hardpoint = hardpoint;
    item.count = count;
    item.health = health;
    item.equipped = equipped;
    let client_id = player.client_id;
    worth_changed(cx, client_id);
}

/// `PlayerData_SetRelativeHealth`.
pub fn set_relative_health(cx: &mut Cx<'_>, session: u32, health: f32, notify: bool) {
    let Some(player) = slot(cx, session) else {
        return;
    };
    player.relative_health = health;
    let client_id = player.client_id;
    if notify {
        client::set_hull_status(cx, client_id, health);
    }
}

/// `PlayerData_SetCollisionGroups`: sends the groups.
pub fn set_collision_groups(cx: &mut Cx<'_>, session: u32, groups: &[CollisionGroupDesc]) {
    let Some(player) = slot(cx, session) else {
        return;
    };
    player.collision_groups = groups.to_vec();
    let client_id = player.client_id;
    client::set_collision_groups(cx, client_id, groups);
}

/// `PlayerData_SnapshotEquipment`.
pub fn snapshot_equipment(player: &mut PlayerData) {
    player.equip_snapshot_arch = player.ship_archetype;
    player.equip_snapshot.clone_from(&player.equipment);
}

/// `PlayerData_GetCash`: clamped to 0 ..= 999 999 999.
#[must_use]
pub fn cash(player: &PlayerData) -> i32 {
    player.cash.clamp(0, CASH_CEILING)
}

/// `PlayerData_SetCash`: refused on a multiplayer server before a character
/// is selected; clamped; sends `SetCash`; broadcasts `PlayerWorthChanged`.
pub fn set_cash(cx: &mut Cx<'_>, session: u32, cash: i32) {
    let multiplayer_server = cx.logic.multiplayer_server;
    let Some(player) = slot(cx, session) else {
        return;
    };
    if multiplayer_server && player.selected_character.is_empty() {
        return;
    }
    let cash = cash.clamp(0, CASH_CEILING);
    player.cash = cash;
    let client_id = player.client_id;
    client::set_cash(cx, client_id, cash);
    worth_changed(cx, client_id);
}

/// `PlayerData_AdjustCash`: the earned total grows first (even when the
/// change is then refused), then as [`set_cash`] with cash plus `amount`
/// (wrapping, as the C++'s int addition does on x86).
pub fn adjust_cash(cx: &mut Cx<'_>, session: u32, amount: i32, count_earned: bool) {
    let multiplayer_server = cx.logic.multiplayer_server;
    let Some(player) = slot(cx, session) else {
        return;
    };
    if count_earned {
        player.total_earned = player.total_earned.wrapping_add(amount);
    }
    let new_cash = player.cash.wrapping_add(amount);
    if multiplayer_server && player.selected_character.is_empty() {
        return;
    }
    let new_cash = new_cash.clamp(0, CASH_CEILING);
    player.cash = new_cash;
    let client_id = player.client_id;
    client::set_cash(cx, client_id, new_cash);
    worth_changed(cx, client_id);
}

/// `PlayerData_SetShipArch`: an unknown non-zero ship is refused; a new
/// ship's collision groups replace the old ones (none for ship 0); sends
/// `SetShipArch`; broadcasts `PlayerShipArchSet`.
pub fn set_ship_arch(cx: &mut Cx<'_>, session: u32, ship: u32) {
    let groups: Option<Vec<CollisionGroupDesc>> =
        cx.world.data().archetypes.ships.get(&ship).map(|s| {
            s.eq_obj
                .collision_groups
                .iter()
                .map(|g| CollisionGroupDesc {
                    id: g.id,
                    health: 1.0,
                })
                .collect()
        });
    if ship != 0 && groups.is_none() {
        return;
    }
    let Some(player) = slot(cx, session) else {
        return;
    };
    if player.ship_archetype != ship {
        player.collision_groups.clear();
        if ship & 0x3FFF_FFFF != 0
            && let Some(groups) = groups
        {
            player.collision_groups = groups;
        }
    }
    player.ship_archetype = ship;
    let client_id = player.client_id;
    client::set_ship_arch(cx, client_id, ship);
    let mut message = ControllerMessage::PlayerShipArchSet { client: client_id };
    let _ = cx.dispatch(ControllerId::BROADCAST, &mut message);
}

/// `PlayerData_LoadEquipment`: the list replaces the equipment; items
/// without an id are numbered from the slot's counter; sends
/// `SetEquipment`; broadcasts `PlayerWorthChanged`.
pub fn load_equipment(cx: &mut Cx<'_>, session: u32, items: &[EquipDesc]) {
    let Some(player) = cx.logic.players.get(session) else {
        return;
    };
    if !equip_gate_open(cx, player) {
        return;
    }
    let Some(player) = slot(cx, session) else {
        return;
    };
    player.equipment = items.to_vec();
    for item in &mut player.equipment {
        if item.id == SUB_OBJECT_ID_NONE {
            item.id = player.equip_ids.create();
        }
    }
    let client_id = player.client_id;
    let equipment = player.equipment.clone();
    client::set_equipment(cx, client_id, &equipment);
    worth_changed(cx, client_id);
}
