//! The character save file (work package P3.2): the record a save's
//! `[Player]` section loads into (`PlayerData::FSavedCharacter`,
//! `include/common_server_playerdb.h:568`), its reader
//! (`PlayerData_LoadSaveRecord`, `Server/server_savegame.cpp:343-625`) and
//! the name-only read of the account scan (`PlayerDB::ReadCharacterName`,
//! `server_savegame.cpp:346`).
//!
//! A save is read through [`neuron_server::save::read_save`] first: plain
//! text (decision O12), an `FLS1` file, or a refused `FLSG` one.

use game_sim::data::Reports;
use game_sim::data::archetype::ArchetypeDb;
use game_sim::data::costume::{Costume, CostumeDescriptions, MAX_ACCESSORIES, nickname_id};
use game_sim::data::equip_desc::EquipDesc;
use game_sim::data::loadout::{read_cargo_line, read_equip_line};
use game_sim::data::reputation::{Relation, Reputation};
use game_sim::data::string_cache::StringCache;
use game_sim::data::universe::Universe;
use game_sim::data::zone::euler_matrix;
use game_sim::protocol::fmtstr::{FmtStr, FmtValue, TAG_NESTED};
use neuron_core::id::create_id;
use neuron_core::id::make_location_id;
use neuron_core::id::sub_object::EquipmentIds;
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};
use neuron_core::msvcrt::atoi;
use neuron_core::num::trunc_to_i32_via_i64;
use neuron_core::util::wide_hex::hex_to_wide;
use neuron_server::save::{SaveContents, read_save};

/// The `__FILE__` every report of the reader carries.
const SOURCE: &str = "Server\\PlayerDB.cpp";

/// `CollisionGroupDesc`: a collision group's id and health.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CollisionGroupDesc {
    /// The group's sub-object id.
    pub id: u16,
    /// Its health, 1 when undamaged.
    pub health: f32,
}

/// `PlayerData::FSavedCharacter`: what a save's `[Player]` section holds.
#[derive(Clone, Debug, PartialEq)]
pub struct SavedCharacter {
    /// `name`, from hex.
    pub name: Vec<u16>,
    /// `description`, from hex.
    pub description: Vec<u16>,
    /// `descrip_strid`.
    pub descrip_str_id: u32,
    /// `tstamp`'s second value.
    pub timestamp_low: u32,
    /// `tstamp`'s first value.
    pub timestamp_high: u32,
    /// `ship_archetype`.
    pub ship_archetype: u32,
    /// `money`.
    pub money: i32,
    /// `num_kills`.
    pub num_kills: i32,
    /// `num_misn_successes`.
    pub num_mission_successes: i32,
    /// `num_misn_failures`.
    pub num_mission_failures: i32,
    /// `hull_status` (1 by default).
    pub hull_status: f32,
    /// `equip` and `cargo`, in file order.
    pub equipment: Vec<EquipDesc>,
    /// The ship's collision groups.
    pub collision_groups: Vec<CollisionGroupDesc>,
    /// `base_hull_status` (1 by default).
    pub base_hull_status: f32,
    /// `base_equip` and `base_cargo`.
    pub base_equipment: Vec<EquipDesc>,
    /// The base copy of the collision groups.
    pub base_collision_groups: Vec<CollisionGroupDesc>,
    /// `base`.
    pub base_id: u32,
    /// `last_base` (not reset by a second `[Player]`).
    pub last_base_id: u32,
    /// `location`.
    pub location_id: u32,
    /// `system`.
    pub system_id: u32,
    /// `pos`.
    pub position: [f32; 3],
    /// `rotate` as a matrix.
    pub orientation: [[f32; 3]; 3],
    /// `starting_ring`.
    pub starting_ring: u32,
    /// `rank`.
    pub rank: u32,
    /// `house`, `group` and `initial_rep`.
    pub relations: Vec<Relation>,
    /// `rep_group` (`0xffffffff` when none).
    pub rep_group: u32,
    /// The `com_*` costume keys.
    pub com_costume: Costume,
    /// `voice`, at most 31 bytes.
    pub voice: Vec<u8>,
    /// The bare costume keys.
    pub base_costume: Costume,
    /// The equipment-id counter that numbers `equipment` and
    /// `base_equipment`.
    pub equip_ids: EquipmentIds,
    /// The `wg` lines, each `"wg = ..."` and a newline.
    pub weapon_groups: Vec<u8>,
    /// `log`.
    pub mission_log: Vec<FmtStr>,
    /// `interface`.
    pub interface: u32,
    /// `visit`: system id and flags, by id.
    pub visits: std::collections::BTreeMap<u32, u8>,
}

impl Default for SavedCharacter {
    /// The record constructor: `RepGroup` `0xffffffff`, both hull statuses
    /// 1, the rest zero or empty (what the C++ leaves uninitialised is zero,
    /// as the reference sets it).
    fn default() -> Self {
        Self {
            name: Vec::new(),
            description: Vec::new(),
            descrip_str_id: 0,
            timestamp_low: 0,
            timestamp_high: 0,
            ship_archetype: 0,
            money: 0,
            num_kills: 0,
            num_mission_successes: 0,
            num_mission_failures: 0,
            hull_status: 1.0,
            equipment: Vec::new(),
            collision_groups: Vec::new(),
            base_hull_status: 1.0,
            base_equipment: Vec::new(),
            base_collision_groups: Vec::new(),
            base_id: 0,
            last_base_id: 0,
            location_id: 0,
            system_id: 0,
            position: [0.0; 3],
            orientation: [[0.0; 3]; 3],
            starting_ring: 0,
            rank: 0,
            relations: Vec::new(),
            rep_group: 0xFFFF_FFFF,
            com_costume: Costume::default(),
            voice: Vec::new(),
            base_costume: Costume::default(),
            equip_ids: EquipmentIds::default(),
            weapon_groups: Vec::new(),
            mission_log: Vec::new(),
            interface: 0,
            visits: std::collections::BTreeMap::new(),
        }
    }
}

/// The game data the reader looks names up in.
#[derive(Clone, Copy)]
pub struct SaveData<'a> {
    /// Systems and bases.
    pub universe: &'a Universe,
    /// Ships and their collision groups.
    pub archetypes: &'a ArchetypeDb,
    /// Reputation groups.
    pub reputation: &'a Reputation,
    /// Body parts, accessories and costumes.
    pub costumes: &'a CostumeDescriptions,
}

/// `CopyBoundedString` into a `TString<size>`: at most `size - 1` bytes, cut
/// at a NUL.
fn bounded(text: &[u8], size: usize) -> Vec<u8> {
    let text = neuron_core::hash::c_str(text);
    text[..text.len().min(size - 1)].to_vec()
}

/// `HexToWide` into a 0x400 buffer, then `wcslen`.
fn wide_from_hex(text: &[u8]) -> Vec<u16> {
    let wide = hex_to_wide(text);
    let end = wide.iter().position(|&c| c == 0).unwrap_or(wide.len());
    wide[..end].to_vec()
}

/// `get_undamaged_collision_group_list`: every group of the ship, health 1.
/// A ship the archetypes lack has none (D599).
fn undamaged_groups(data: SaveData<'_>, ship: u32) -> Vec<CollisionGroupDesc> {
    data.archetypes
        .ships
        .get(&ship)
        .map(|s| {
            s.eq_obj
                .collision_groups
                .iter()
                .map(|g| CollisionGroupDesc {
                    id: g.id,
                    health: 1.0,
                })
                .collect()
        })
        .unwrap_or_default()
}

/// One `log` value list back into a format string (`ReadMissionLogEntry`):
/// the mad-lib id, the count, then per value its data, tag and flags; a
/// value of tag 0x21 is a nested string read the same way.
fn read_mission_log_entry(reader: &IniReader, index: &mut u32) -> FmtStr {
    let mad_lib_id = reader.value_int(*index) as u32;
    *index += 1;
    let count = reader.value_int(*index);
    *index += 1;
    let mut out = FmtStr::new(mad_lib_id);
    for _ in 0..count.max(0) {
        let data = reader.value_int(*index);
        let tag = reader.value_int(*index + 1);
        let flags = reader.value_int(*index + 2) as u8;
        *index += 3;
        let tag16 = tag as u16;
        let value = if tag16 == TAG_NESTED {
            FmtValue::Nested {
                flags,
                fmt: read_mission_log_entry(reader, index),
            }
        } else {
            FmtValue::Value {
                tag: tag16,
                flags,
                value: data as u32,
            }
        };
        out.values.push(value);
    }
    out
}

/// The costume keys (`LoadCostumeKey`), matched exactly and case
/// sensitively on the key name or its `com_` suffix. `false` when the key is
/// none of them.
fn load_costume_key(
    reader: &IniReader,
    data: SaveData<'_>,
    target: &mut Costume,
    suffix: &[u8],
    file: &str,
    reports: &mut Reports,
) -> bool {
    let value = reader.value_string();
    let part = |kind: usize, reports: &mut Reports| -> u32 {
        let id = data
            .costumes
            .find_bodypart(kind, nickname_id(&value))
            .map_or(0, |p| p.id);
        if id == 0 {
            reports.push(
                SOURCE,
                0x25a,
                format!(
                    "*** WARNING: invalid costume part ({}) in ({file})",
                    String::from_utf8_lossy(&value)
                ),
            );
        }
        id
    };
    match suffix {
        b"body" => target.part_ids[1] = part(0, reports),
        b"head" => target.part_ids[0] = part(1, reports),
        b"lefthand" => target.part_ids[2] = part(2, reports),
        b"righthand" => target.part_ids[3] = part(3, reports),
        b"hair" => reports.push(
            SOURCE,
            0x286,
            format!("*** WARNING: hair is no longer a valid bodypart in character ({file})"),
        ),
        b"accessory" => {
            let id = data
                .costumes
                .accessories
                .get(&nickname_id(&value))
                .map_or(0, |a| a.id);
            if id == 0 {
                reports.push(
                    SOURCE,
                    0x263,
                    format!(
                        "*** WARNING: invalid accessory ({}) in character ({file})",
                        String::from_utf8_lossy(&value)
                    ),
                );
            }
            // An id of 0 is appended too; past the array's eight the C++
            // writes on (D600).
            if target.accessory_ids.len() < MAX_ACCESSORIES {
                target.accessory_ids.push(id);
            }
        }
        b"costume" => *target = data.costumes.get_costume(nickname_id(&value), reports),
        _ => return false,
    }
    true
}

/// `collision_group` and `base_collision_group` (`LoadCollisionGroupKey`).
fn load_collision_group_key(
    reader: &mut IniReader,
    data: SaveData<'_>,
    ship: u32,
    groups: &mut [CollisionGroupDesc],
    lines: (u32, u32),
    strings: &mut StringCache,
    reports: &mut Reports,
) {
    let raw = reader.value_string_at(0);
    let name = strings.alloc(&raw, false);
    let shown = String::from_utf8_lossy(&raw).into_owned();
    if ship == 0 {
        reports.push(
            SOURCE,
            lines.1,
            format!("*** WARNING: collision_group {shown} specified BEFORE ship. Skipping."),
        );
        return;
    }
    let Some(arch) = data.archetypes.ships.get(&ship) else {
        // The C++ calls through the null ship here (D599).
        return;
    };
    let Some(group) = arch.eq_obj.group_by_name(name) else {
        reports.push(
            SOURCE,
            lines.0,
            format!(
                "*** WARNING: collision_group({shown}) not found on ship({ship:#x}). Skipping."
            ),
        );
        return;
    };
    if let Some(desc) = groups.iter_mut().find(|d| d.id == group.id) {
        desc.health = reader.value_float(1) as f32;
    }
}

/// `PlayerData_LoadSaveRecord` over a save's text: every `[Player]` section
/// read into `record`, which keeps what a section does not reset. `true`
/// when a `[Player]` section was seen. `path` names the file in reports.
pub fn load_save_record(
    text: &[u8],
    path: &str,
    record: &mut SavedCharacter,
    data: SaveData<'_>,
    strings: &mut StringCache,
    reports: &mut Reports,
) -> bool {
    let mut reader = IniReader::new();
    if !reader.open(path.as_bytes(), text.to_vec()) {
        return false;
    }
    let mut saw_player = false;
    while reader.read_header() {
        let mut comment_depth = 0i32;
        if !reader.is_header(b"Player") {
            continue;
        }
        record.position = [0.0; 3];
        record.orientation = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]];
        saw_player = true;
        record.system_id = 0;
        record.base_id = 0;
        record.location_id = 0;
        record.description.clear();
        record.descrip_str_id = 0;
        record.money = 0x7B0;
        record.interface = 3;
        record.weapon_groups.clear();
        record.visits.clear();

        while reader.read_value() {
            if reader.is_value(b"/*") {
                comment_depth += 1;
            } else if reader.is_value(b"*/") {
                comment_depth -= 1;
            }
            if comment_depth > 0 {
                continue;
            }
            read_value(&mut reader, path, record, data, strings, reports);
        }
    }
    if make_location_id(record.base_id, b"Deck2") == record.location_id {
        record.location_id = make_location_id(record.base_id, b"Deck");
    }
    saw_player
}

/// One key of a `[Player]` section.
fn read_value(
    reader: &mut IniReader,
    path: &str,
    record: &mut SavedCharacter,
    data: SaveData<'_>,
    strings: &mut StringCache,
    reports: &mut Reports,
) {
    let value = reader.value_string();
    let invalid = |reports: &mut Reports, line: u32, what: &str| {
        reports.push(
            SOURCE,
            line,
            format!(
                "*** ERROR: game ({path}) has invalid {what} name ({}) in player file ({path})",
                String::from_utf8_lossy(&value)
            ),
        );
    };
    if reader.is_value(b"name") {
        record.name = wide_from_hex(&value);
    } else if reader.is_value(b"description") {
        record.description = wide_from_hex(&value);
    } else if reader.is_value(b"descrip_strid") {
        record.descrip_str_id = reader.value_int(0) as u32;
    } else if reader.is_value(b"tstamp") {
        if reader.is_value_empty(0) || reader.is_value_empty(1) {
            record.timestamp_high = 0;
            record.timestamp_low = 0;
        } else {
            record.timestamp_high = reader.value_int(0) as u32;
            record.timestamp_low = reader.value_int(1) as u32;
        }
    } else if reader.is_value(b"ship_archetype") {
        // MSVCRT `atoi` wraps, so a hash above `INT_MAX` round-trips.
        let mut ship = atoi(&value) as u32;
        if ship == 0 {
            ship = create_id(&value);
        }
        record.ship_archetype = ship;
        record.collision_groups = undamaged_groups(data, ship);
        record.base_collision_groups = undamaged_groups(data, ship);
    } else if reader.is_value(b"system") {
        record.system_id = data.universe.get_system_id(&value, reports);
        if record.system_id == 0 {
            invalid(reports, 0x376, "system");
        }
    } else if reader.is_value(b"base") {
        record.base_id = data.universe.lookup_base(&value, reports);
        if record.base_id == 0 {
            invalid(reports, 0x37E, "base");
        }
    } else if reader.is_value(b"last_base") {
        record.last_base_id = data.universe.lookup_base(&value, reports);
        if record.last_base_id == 0 {
            invalid(reports, 0x386, "last_base");
        }
    } else if reader.is_value(b"location") {
        record.location_id = reader.value_int(0) as u32;
    } else if reader.is_value(b"rank") {
        record.rank = reader.value_int(0) as u32;
    } else if reader.is_value(b"group") || reader.is_value(b"initial_rep") {
        let group = data.reputation.get_id(&bounded(&value, 0x10));
        record.relations.clear();
        for (other, _) in data.reputation.groups() {
            let mut feeling = data
                .reputation
                .feelings_towards(other, group)
                .1
                .unwrap_or(0.0);
            // `!(f < 1.0)`: NaN too.
            if feeling.is_nan() || f64::from(feeling) >= 1.0 {
                feeling = 0.95;
            }
            record.relations.push(Relation {
                group_id: other,
                feeling,
            });
        }
    } else if reader.is_value(b"rep_group") {
        let name = if reader.is_value_empty(0) {
            Vec::new()
        } else {
            bounded(&reader.value_string_at(0), 0x10)
        };
        record.rep_group = data.reputation.get_id(&name);
    } else if reader.is_value(b"house") {
        let feeling = reader.value_float(0) as f32;
        let group = data
            .reputation
            .get_id(&bounded(&reader.value_string_at(1), 0x10));
        record.relations.push(Relation {
            group_id: group,
            feeling,
        });
    } else if reader.is_value(b"voice") {
        record.voice = bounded(&value, 0x20);
    } else if !read_costume(reader, path, record, data, reports) {
        read_numbers_and_lists(reader, record, data, strings, reports);
    }
}

/// The costume block on the raw key name: the `com_` suffix for the com
/// costume, else the whole name for the base costume.
fn read_costume(
    reader: &IniReader,
    path: &str,
    record: &mut SavedCharacter,
    data: SaveData<'_>,
    reports: &mut Reports,
) -> bool {
    let key = reader.name();
    if let Some(suffix) = key.strip_prefix(b"com_")
        && load_costume_key(reader, data, &mut record.com_costume, suffix, path, reports)
    {
        return true;
    }
    load_costume_key(reader, data, &mut record.base_costume, &key, path, reports)
}

/// The keys after the costume block.
fn read_numbers_and_lists(
    reader: &mut IniReader,
    record: &mut SavedCharacter,
    data: SaveData<'_>,
    strings: &mut StringCache,
    reports: &mut Reports,
) {
    // MSVCRT `ftol` on the unrounded double: a 64-bit truncation whose low
    // dword is stored.
    if reader.is_value(b"money") {
        record.money = trunc_to_i32_via_i64(reader.value_float(0));
    } else if reader.is_value(b"num_kills") {
        record.num_kills = trunc_to_i32_via_i64(reader.value_float(0));
    } else if reader.is_value(b"num_misn_successes") {
        record.num_mission_successes = trunc_to_i32_via_i64(reader.value_float(0));
    } else if reader.is_value(b"num_misn_failures") {
        record.num_mission_failures = trunc_to_i32_via_i64(reader.value_float(0));
    } else if reader.is_value(b"pos") {
        for (i, p) in record.position.iter_mut().enumerate() {
            *p = reader.value_float(i as u32) as f32;
        }
    } else if reader.is_value(b"rotate") {
        let angles = Vector {
            x: reader.value_float(0) as f32,
            y: reader.value_float(1) as f32,
            z: reader.value_float(2) as f32,
        };
        let m: Matrix = euler_matrix(angles);
        record.orientation = m.m;
    } else if reader.is_value(b"starting_ring") {
        record.starting_ring = create_id(&reader.value_string());
    } else if reader.is_value(b"equip") {
        load_equip_key(reader, record, false, false, strings);
    } else if reader.is_value(b"base_equip") {
        load_equip_key(reader, record, true, false, strings);
    } else if reader.is_value(b"cargo") {
        load_equip_key(reader, record, false, true, strings);
    } else if reader.is_value(b"base_cargo") {
        load_equip_key(reader, record, true, true, strings);
    } else if reader.is_value(b"collision_group") {
        let ship = record.ship_archetype;
        load_collision_group_key(
            reader,
            data,
            ship,
            &mut record.collision_groups,
            (0x41F, 0x424),
            strings,
            reports,
        );
    } else if reader.is_value(b"base_collision_group") {
        let ship = record.ship_archetype;
        load_collision_group_key(
            reader,
            data,
            ship,
            &mut record.base_collision_groups,
            (0x444, 0x449),
            strings,
            reports,
        );
    } else if reader.is_value(b"wg") {
        record.weapon_groups.extend_from_slice(&reader.line());
        record.weapon_groups.push(b'\n');
    } else if reader.is_value(b"hull_status") {
        record.hull_status = reader.value_float(0) as f32;
    } else if reader.is_value(b"base_hull_status") {
        record.base_hull_status = reader.value_float(0) as f32;
    } else if reader.is_value(b"visit") {
        let system = reader.value_int(0) as u32;
        let flags = reader.value_int(1) as u8;
        record.visits.insert(system, flags);
    } else if reader.is_value(b"interface") {
        record.interface = reader.value_int(0) as u32;
    } else if reader.is_value(b"log") {
        let mut index = 0;
        record
            .mission_log
            .push(read_mission_log_entry(reader, &mut index));
    }
}

/// `equip`, `base_equip`, `cargo`, `base_cargo` (`LoadEquipKey`): a new
/// line read into the list's end, numbered from the record's counter.
fn load_equip_key(
    reader: &mut IniReader,
    record: &mut SavedCharacter,
    base: bool,
    cargo: bool,
    strings: &mut StringCache,
) {
    let mut desc = EquipDesc::new(strings);
    if cargo {
        read_cargo_line(reader, &mut desc, strings);
    } else {
        read_equip_line(reader, &mut desc, strings);
    }
    desc.id = record.equip_ids.create();
    if base {
        record.base_equipment.push(desc);
    } else {
        record.equipment.push(desc);
    }
}

/// `PlayerDB::ReadCharacterName`: the first `name` of a `[Player]` section
/// outside block comments; empty for a refused file or one without.
#[must_use]
pub fn read_character_name(bytes: &[u8]) -> Vec<u16> {
    let (SaveContents::Plain(text) | SaveContents::Fls1(text)) = read_save(bytes) else {
        return Vec::new();
    };
    let mut reader = IniReader::new();
    if !reader.open_memory(&text) {
        return Vec::new();
    }
    while reader.read_header() {
        let mut comment_depth = 0i32;
        if !(reader.is_header(b"Player") && reader.read_value()) {
            continue;
        }
        loop {
            if reader.is_value(b"/*") {
                comment_depth += 1;
            } else if reader.is_value(b"*/") {
                comment_depth -= 1;
            }
            if comment_depth <= 0 && reader.is_value(b"name") {
                return wide_from_hex(&reader.value_string());
            }
            if !reader.read_value() {
                break;
            }
        }
    }
    Vec::new()
}
