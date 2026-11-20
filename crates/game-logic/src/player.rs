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
//! `PlayerData`'s other fields (the trade, the group) arrive with the work
//! that uses them.

use std::collections::BTreeMap;

use game_sim::data::costume::Costume;
use game_sim::data::equip_desc::EquipDesc;
use game_sim::data::equip_desc::SUB_OBJECT_ID_NONE;
use game_sim::data::equip_desc::list;
use game_sim::data::loadout::add_equipment_item;
use game_sim::data::string_cache::CacheString;
use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::id::sub_object::EquipmentIds;
use neuron_server::controller::ControllerId;
use neuron_server::session::SessionSlot;

use crate::character_file::{CollisionGroupDesc, SavedCharacter};
use crate::client;
use crate::events::ControllerMessage;
use crate::server::Cx;

/// The most cash a player can have (`999999999`).
pub const CASH_CEILING: i32 = 999_999_999;

/// One session slot's player (`PlayerData`).
#[derive(Clone, Debug, PartialEq)]
pub struct PlayerData {
    /// The account name (+0x00), wide, without its terminator.
    pub account_name: Vec<u16>,
    /// The client id (+0x348); 0 for a free slot.
    pub client_id: u32,
    /// The selected character's id (+0x64), empty when none is selected.
    pub selected_character: Vec<u8>,
    /// The account's characters by id (`saved_characters`, +0x404).
    pub saved_characters: BTreeMap<Vec<u8>, SavedCharacter>,
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
    /// The system the player is in (+0x3e0).
    pub system_id: u32,
    /// The player's ship object (+0x3e4); 0 when none.
    pub ship_object_id: u32,
    /// The ship created for the player (+0x3e8).
    pub created_ship_id: u32,
    /// The base the player is in (+0x3ec).
    pub current_base: u32,
    /// The base `last_base` names (+0x3f0).
    pub last_base_id: u32,
    /// The room in the base (+0x3f8).
    pub current_location: u32,
    /// +0x3fc, cleared with the location.
    pub field_3fc: u32,
    /// The base the player last left (+0x410).
    pub exited_base_id: u32,
    /// `starting_ring` (+0x414).
    pub starting_ring: u32,
    /// The reputation vibe (+0x318); -1 when none.
    pub rep_vibe: i32,
    /// The rank (+0x284), which a single-player save writes.
    pub rank_level: u32,
    /// The comm costume (+0x288).
    pub com_costume: Costume,
    /// The voice (+0x2c4), at most 31 bytes.
    pub voice: Vec<u8>,
    /// The base costume (+0x2e4).
    pub costume: Costume,
    /// Where the ship starts in space (+0x2a4).
    pub initial_pos: [f32; 3],
    /// The ship's starting orientation (+0x2b0), row by row.
    pub initial_orient: [[f32; 3]; 3],
    /// `num_kills` (+0x334).
    pub num_kills: i32,
    /// `num_misn_successes` (+0x338).
    pub num_mission_successes: i32,
    /// `num_misn_failures` (+0x33c).
    pub num_mission_failures: i32,
    /// The next autosave is skipped (+0x340).
    pub skip_next_autosave: bool,
    /// Saves that failed in a row (+0x344).
    pub save_failure_count: i32,
    /// A cheat was reported for this character (+0x34c).
    pub cheat_reported: bool,
    /// The `wg` lines (+0x380), each a whole line with its newline.
    pub weapon_groups: Vec<u8>,
    /// The mission log (+0x390).
    pub mission_log: Vec<FmtStr>,
    /// `interface` (+0x39c).
    pub interface_state: u32,
    /// The visited systems and objects with their flags (+0x3a0), by id.
    pub visits: BTreeMap<u32, u8>,
    /// Visits not yet committed (+0x3b8), in order.
    pub pending_visits: Vec<(u32, u8)>,
}

impl Default for PlayerData {
    /// The slot constructor (`PlayerData::PlayerData`): everything zero or
    /// empty, no vibe, the identity orientation.
    fn default() -> Self {
        Self {
            account_name: Vec::new(),
            client_id: 0,
            selected_character: Vec::new(),
            saved_characters: BTreeMap::new(),
            cash: 0,
            total_earned: 0,
            ship_archetype: 0,
            relative_health: 0.0,
            collision_groups: Vec::new(),
            equipment: Vec::new(),
            equip_snapshot_arch: 0,
            equip_snapshot: Vec::new(),
            equip_ids: EquipmentIds::default(),
            system_id: 0,
            ship_object_id: 0,
            created_ship_id: 0,
            current_base: 0,
            last_base_id: 0,
            current_location: 0,
            field_3fc: 0,
            exited_base_id: 0,
            starting_ring: 0,
            rep_vibe: -1,
            rank_level: 0,
            com_costume: Costume::default(),
            voice: Vec::new(),
            costume: Costume::default(),
            initial_pos: [0.0; 3],
            initial_orient: [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]],
            num_kills: 0,
            num_mission_successes: 0,
            num_mission_failures: 0,
            skip_next_autosave: false,
            save_failure_count: 0,
            cheat_reported: false,
            weapon_groups: Vec::new(),
            mission_log: Vec::new(),
            interface_state: 0,
            visits: BTreeMap::new(),
            pending_visits: Vec::new(),
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

/// `PlayerData_ApplySavedCharacter`: the slot of `session` takes the
/// record's state, with a new vibe for client `client` (the old one is
/// freed in the vibe store but its id is not given back, as in the C++).
/// The base the record names gives the system; a base the universe lacks
/// leaves the record's system, where the C++ calls through null (D601).
pub fn apply_saved_character(cx: &mut Cx<'_>, session: u32, record: &SavedCharacter, client: u32) {
    let Some(old_vibe) = cx.logic.players.get(session).map(|s| s.rep_vibe) else {
        return;
    };
    let system = if record.base_id & 0x3FFF_FFFF != 0 {
        cx.world
            .data()
            .universe
            .bases
            .get(&record.base_id)
            .map_or(record.system_id, |b| b.system)
    } else {
        record.system_id
    };
    let vibe = cx.logic.vibe_ids.alloc();
    {
        let (vibes, reputation) = cx.world.vibes();
        if old_vibe != -1 {
            vibes.free(old_vibe);
        }
        vibes.alloc(vibe);
        vibes.set_client_id(vibe, client);
        let relations: Vec<(u32, f32)> = record
            .relations
            .iter()
            .map(|r| (r.group_id, r.feeling))
            .collect();
        // The count travels as a byte.
        let count = relations.len() & 0xFF;
        vibes.set(
            vibe,
            record.rep_group,
            record.rank,
            &relations[..count],
            &FmtStr::default(),
            &FmtStr::default(),
            &record.name,
            reputation,
        );
    }
    let Some(slot) = cx.logic.players.get_mut(session) else {
        return;
    };
    slot.current_base = record.base_id;
    slot.last_base_id = record.last_base_id;
    if record.base_id & 0x3FFF_FFFF != 0 {
        slot.last_base_id = record.base_id;
    }
    slot.system_id = system;
    slot.rep_vibe = vibe;
    slot.com_costume.clone_from(&record.com_costume);
    slot.voice = record.voice.iter().copied().take(0x1F).collect();
    slot.costume.clone_from(&record.base_costume);
    slot.initial_pos = record.position;
    slot.initial_orient = record.orientation;
    slot.ship_archetype = record.ship_archetype;
    slot.relative_health = record.hull_status;
    slot.equipment.clone_from(&record.equipment);
    slot.collision_groups.clone_from(&record.collision_groups);
    slot.cash = record.money;
    slot.equip_ids = record.equip_ids.clone();
    slot.num_kills = record.num_kills;
    slot.num_mission_successes = record.num_mission_successes;
    slot.num_mission_failures = record.num_mission_failures;
    slot.save_failure_count = 0;
    // The record's location is copied, then cleared.
    slot.current_location = 0;
    slot.field_3fc = 0;
    slot.ship_object_id = 0;
    slot.created_ship_id = 0;
    slot.exited_base_id = 0;
    slot.skip_next_autosave = false;
    slot.starting_ring = record.starting_ring;
    slot.rank_level = record.rank;
    slot.weapon_groups.clone_from(&record.weapon_groups);
    slot.visits.clone_from(&record.visits);
    slot.mission_log.clone_from(&record.mission_log);
    slot.interface_state = record.interface;
}

/// `PlayerData_FlushPendingVisits`: each pending visit is committed with
/// flag 8 and the flags already held.
pub fn flush_pending_visits(slot: &mut PlayerData) {
    for (system, flags) in std::mem::take(&mut slot.pending_visits) {
        let mut flags = flags | 0x08;
        if let Some(&held) = slot.visits.get(&system) {
            flags |= held;
        }
        slot.visits.insert(system, flags);
    }
}

/// `PlayerData_ShouldQueueVisit`: not when the flags with those held have
/// both 0x10 and 0x08.
#[must_use]
pub fn should_queue_visit(slot: &PlayerData, key: u32, flags: u8) -> bool {
    let flags = flags | slot.visits.get(&key).copied().unwrap_or(0);
    !(flags & 0x10 != 0 && flags & 0x08 != 0)
}

/// `PlayerData_QueueVisit`.
pub fn queue_visit(slot: &mut PlayerData, key: u32, flags: u8) {
    slot.pending_visits.push((key, flags));
}

/// `PlayerData_ReloadShipData`: the ship goes back to the selected
/// character's last-base state; of the current list only the cargo that
/// is a shield battery, repair kit, munition, mine or countermeasure stays
/// after the base's mounted equipment. An item whose archetype is unknown
/// is dropped, where the C++ calls through null (D601). The C++ reports an
/// error outside a multiplayer server and runs on.
pub fn reload_ship_data(cx: &mut Cx<'_>, session: u32) {
    let equipment = &cx.world.data().archetypes.equipment;
    let Some(slot) = cx.logic.players.get_mut(session) else {
        return;
    };
    slot.current_base = slot.last_base_id;
    let PlayerData {
        saved_characters,
        selected_character,
        relative_health,
        collision_groups,
        equipment: items,
        ..
    } = slot;
    let record = saved_characters
        .entry(selected_character.clone())
        .or_default();
    *relative_health = record.base_hull_status;
    collision_groups.clone_from(&record.base_collision_groups);
    let mut kept: Vec<EquipDesc> = record
        .base_equipment
        .iter()
        .filter(|e| e.equipped != 0)
        .cloned()
        .collect();
    kept.extend(
        items
            .iter()
            .filter(|e| e.equipped == 0)
            .filter(|e| {
                equipment
                    .get(&e.arch_id)
                    .is_some_and(|a| matches!(a.class(), 0x1A | 0x1B | 0x1F | 0x20 | 0x21))
            })
            .cloned(),
    );
    record.base_equipment.clone_from(&kept);
    record.equipment.clone_from(&kept);
    *items = kept;
}

/// `PlayerData_CheckForCheats`: never in single player; always once a
/// cheat was reported; never outside a base. The four checks after that
/// (equipment beyond the ship, equipment the base does not sell, worth
/// above what was earned, a hold over capacity) need P3.3's base database;
/// until then they find nothing.
pub fn check_for_cheats(cx: &mut Cx<'_>, session: u32) -> bool {
    if cx.world.config().single_player {
        return false;
    }
    cx.logic
        .players
        .get(session)
        .is_some_and(|slot| slot.cheat_reported)
}

/// The end of `PlayerData_SerializeToFile` on a multiplayer server: the
/// selected character's record takes the slot's state and what the writer
/// printed.
pub fn refresh_saved_record(cx: &mut Cx<'_>, session: u32, written: &crate::save_writer::Written) {
    let Some(slot) = cx.logic.players.get_mut(session) else {
        return;
    };
    let record = slot
        .saved_characters
        .entry(slot.selected_character.clone())
        .or_default();
    record.hull_status = slot.relative_health;
    record.equipment.clone_from(&slot.equipment);
    record.equip_ids = slot.equip_ids.clone();
    record.collision_groups.clone_from(&slot.collision_groups);
    record.money = slot.cash;
    record.num_kills = slot.num_kills;
    record.num_mission_successes = slot.num_mission_successes;
    record.num_mission_failures = slot.num_mission_failures;
    record.rank = written.rank;
    record.visits.clone_from(&slot.visits);
    record.weapon_groups.clone_from(&slot.weapon_groups);
    record.base_id = slot.current_base;
    record.last_base_id = slot.last_base_id;
    record.position = written.position;
    record.orientation = written.orientation;
    record.ship_archetype = slot.ship_archetype;
    record.location_id = slot.current_location;
    record.system_id = slot.system_id;
    record.mission_log.clone_from(&slot.mission_log);
    record.starting_ring = slot.starting_ring;
    record.com_costume.clone_from(&slot.com_costume);
    record.voice = slot.voice.iter().copied().take(0x1F).collect();
    record.base_costume.clone_from(&slot.costume);
    record.interface = slot.interface_state;
    record.relations = written
        .relations
        .iter()
        .map(|&(group_id, feeling)| game_sim::data::reputation::Relation { group_id, feeling })
        .collect();
}
