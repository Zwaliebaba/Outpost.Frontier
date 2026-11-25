//! The top-level `pub::` getters (`Server/server_pub.cpp:131-762`,
//! `:2072`, `:7971-8040`, `Server\pubMisc.cpp`): goods, loadouts,
//! archetypes, costumes, the universe's systems and bases with their
//! files and names, the bases' rooms, the game time and the process's
//! side.
//!
//! The 47 exports in the C++ order. Part 14 ported the ten system getters,
//! `SetTimer` lives with the bus ([`super::controller`]); this part the
//! rest but for three, of which part 21 ported `Save` ([`crate::saves`]);
//! the other two are not ported:
//!
//! | Export | Line | Port |
//! | --- | --- | --- |
//! | `IsCommodity` | 136 | [`is_commodity`] |
//! | `GetCargoHoldSize` | 147 | [`cargo_hold_size`] |
//! | `GetLoadoutName` | 163 | [`loadout_name`] |
//! | `GetLoadoutID` | 181 | [`loadout_id`] |
//! | `GetEquipmentID` | 194 | [`equipment_id`] |
//! | `GetGoodID` | 207 | [`good_id`] |
//! | `TranslateArchToGood` | 221 | [`translate_arch_to_good`] |
//! | `GetVoiceID` | 231 | [`voice_id`] |
//! | `GetCostumeID` | 242 | [`costume_id`] |
//! | `GetDataPath` | 254 | none: the port has no global data file system (D690) |
//! | `GetTime` | 258 | [`time`] |
//! | `GetRwTime` | 265 | [`rw_time`] |
//! | `NextSystemID` | 276 | [`next_system_id`] |
//! | `NextBaseID` | 294 | [`next_base_id`] |
//! | `GetSystemID` | 313 | [`system_id`] |
//! | `GetSystemGateConnection` | 323 | [`system_gate_connection`] |
//! | `GetBaseID` | 336 | [`base_id`] |
//! | `BuildSystemReader` | 351 | [`build_system_reader`] |
//! | `BuildBaseReader` | 369 | [`build_base_reader`] |
//! | `GetSystemNickname` | 386 | [`system_nickname`] |
//! | `GetBaseNickname` | 399 | [`base_nickname`] |
//! | `GetBaseStridName` | 412 | [`base_strid_name`] |
//! | `GetLocations` | 426 | [`locations`] |
//! | `GetLocationNickname` | 450 | [`location_nickname`] |
//! | `GetBases` | 463 | [`bases`] |
//! | `GetSystem` | 477 | [`base_system`] |
//! | `GetNicknameId` | 487 | [`nickname_id`] |
//! | `MakeId` | 491 | [`make_id`] |
//! | `DebugPrint` | 498 | [`debug_print`] |
//! | `GetNavMapScale` | 511 | [`nav_map_scale`] |
//! | `ReportFreeTerminal` | 535 | [`report_free_terminal`] |
//! | `DLL_LoadLibrary` | 548 | none: controllers come from the linked factory (D691) |
//! | `GetFullHealth` | 572 | [`full_health`] |
//! | `GetShipArchSTRID` | 591 | [`ship_arch_strid`] |
//! | `GetType` | 608 | [`ship_type`] |
//! | `GetSolarType` | 624 | [`solar_type`] |
//! | `GetMaxHitPoints` | 640 | [`max_hit_points`] |
//! | `GetGoodProperties` | 659 | [`good_properties`] |
//! | `TranslateSystemToMsgIdPrefix` | 679 | [`translate_system_to_msg_id_prefix`] |
//! | `SinglePlayer` | 690 | [`single_player`] |
//! | `GetLoadout` | 702 | [`loadout`] |
//! | `Save` | 2072 | [`save`] |
//! | `SetTimer` | 6364 | [`super::controller::set_timer`] |
//! | `FindHardpoint` | 7971 | [`find_hardpoint`] |
//! | `GetShipID` | 7992 | [`ship_id`] |
//! | `TranslateGoodToMsgIdPrefix` | 8013 | [`translate_good_to_msg_id_prefix`] |
//! | `TranslateShipToMsgIdPrefix` | 8029 | [`translate_ship_to_msg_id_prefix`] |
//!
//! Where the C++ writes an out-parameter and then answers a failure, the
//! port answers the failure alone; each function's comment says what the
//! C++ leaves behind. `Universe::get_system`, `get_base` and `get_zone`
//! and `BaseDataList::get_base_data` and `get_room_data` report a miss as
//! Common does ([`get_system`], [`get_base`], [`get_zone`],
//! [`base_data_exists`], [`room_data`]).

use game_sim::data::base::{BaseData, Room};
use game_sim::data::config::GameData;
use game_sim::data::equip_desc::EquipDesc;
use game_sim::data::goods::GoodCategory;
use game_sim::data::universe::{Base, System};
use game_sim::data::zone::Zone;
use game_sim::data::{DataSource, Reports};
use neuron_core::hash::c_str;
use neuron_core::id::{create_id, make_location_id};
use neuron_core::ini::IniReader;
use neuron_core::math::Transform;
use neuron_core::num::trunc_to_i32_via_i64;

use super::PubError;
use crate::events::ControllerMessage;
use crate::server::Cx;

const SYSTEMS: &str = "Common\\Systems.cpp";
const PUB_MISC: &str = "Server\\pubMisc.cpp";
const BASE_DATA: &str = "Common\\BaseData.cpp";

/// `GetLocations`' seven room names, in its order.
const ROOM_NAMES: [&[u8]; 7] = [
    b"Bar",
    b"Deck",
    b"Trader",
    b"Equipment",
    b"Cityscape",
    b"ShipDealer",
    b"Planetside",
];

/// `IdIsValid`'s mask.
const VALID_ID: u32 = 0x3fff_ffff;

/// `%#X`: `0` for zero, else `0X` and the upper-case digits.
fn alt_hex(value: u32) -> String {
    if value == 0 {
        "0".to_owned()
    } else {
        format!("0X{value:X}")
    }
}

/// `Universe::get_system`: none for an id whose low 30 bits are 0; a miss
/// is reported.
pub fn get_system<'d>(data: &'d GameData, reports: &mut Reports, id: u32) -> Option<&'d System> {
    if id & VALID_ID == 0 {
        return None;
    }
    let found = data.universe.systems.get(&id);
    if found.is_none() {
        reports.push(
            SYSTEMS,
            0xbd8,
            format!("*** WARNING: get_system({}) failed.", alt_hex(id)),
        );
    }
    found
}

/// `Universe::get_zone(id)`: no mask test; a miss is reported (the id as
/// a signed number).
pub fn get_zone<'d>(data: &'d GameData, reports: &mut Reports, id: u32) -> Option<&'d Zone> {
    let found = data.universe.zones.get(&id);
    if found.is_none() {
        reports.push(
            SYSTEMS,
            0xc8e,
            format!("*** WARNING: get_zone({}) failed", id as i32),
        );
    }
    found
}

/// `Universe::get_zone(system, name)`: the zone of `CreateID(name)` in the
/// global map (the system is not used); a miss is reported.
pub fn get_zone_by_name<'d>(
    data: &'d GameData,
    reports: &mut Reports,
    name: &[u8],
) -> Option<&'d Zone> {
    let found = data.universe.zones.get(&create_id(name));
    if found.is_none() {
        reports.push(
            SYSTEMS,
            0xc9c,
            format!(
                "*** WARNING: get_zone({}) failed",
                String::from_utf8_lossy(name)
            ),
        );
    }
    found
}

/// `Universe::get_base`: none for an id whose low 30 bits are 0; a miss is
/// reported.
pub fn get_base<'d>(data: &'d GameData, reports: &mut Reports, id: u32) -> Option<&'d Base> {
    if id & VALID_ID == 0 {
        return None;
    }
    let found = data.universe.bases.get(&id);
    if found.is_none() {
        reports.push(
            SYSTEMS,
            0xbf3,
            format!("*** WARNING: get_base({}) failed.", alt_hex(id)),
        );
    }
    found
}

/// `BaseDataList::get_base_data`: a miss is reported.
pub fn base_data_exists<'d>(
    data: &'d GameData,
    reports: &mut Reports,
    id: u32,
) -> Option<&'d BaseData> {
    let found = data.bases.base(id);
    if found.is_none() {
        reports.push(
            BASE_DATA,
            0x1e8,
            format!("*** ERROR: Could not find base {id}"),
        );
    }
    found
}

/// `BaseDataList::get_room_data`: a miss is reported.
pub fn room_data<'d>(data: &'d GameData, reports: &mut Reports, id: u32) -> Option<&'d Room> {
    let found = data.bases.room(id);
    if found.is_none() {
        reports.push(
            BASE_DATA,
            0x1fc,
            format!("*** ERROR: Could not find room {id}"),
        );
    }
    found
}

/// The empty-name test of the name getters (`Nickname == nullptr ||
/// Nickname[0] == '\0'`).
fn empty_name(name: &[u8]) -> bool {
    name.first().is_none_or(|&b| b == 0)
}

/// The C++'s `(id & 0x3fffffff) ? 0 : -2` after it has written the id.
fn valid_or_bad_id(id: u32) -> Result<u32, PubError> {
    if id & VALID_ID != 0 {
        Ok(id)
    } else {
        Err(PubError::BAD_ID)
    }
}

/// `IsCommodity`: the good's category is `Commodity`.
///
/// # Errors
///
/// -4 for an unknown good (the C++ leaves the answer unwritten).
pub fn is_commodity(cx: &Cx<'_>, good: u32) -> Result<bool, PubError> {
    cx.world
        .data()
        .goods
        .find_by_id(good)
        .map(|g| g.category == GoodCategory::Commodity)
        .ok_or(PubError::BAD_ARGUMENT)
}

/// `GetCargoHoldSize`: the ship archetype's `hold_size`, truncated through
/// 64 bits (`ftol`), low dword.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 for an unknown ship (the C++
/// writes 0 first in both cases).
pub fn cargo_hold_size(cx: &Cx<'_>, ship: u32) -> Result<u32, PubError> {
    if ship & VALID_ID == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    let arch = cx
        .world
        .data()
        .archetypes
        .ships
        .get(&ship)
        .ok_or(PubError::BAD_ID)?;
    Ok(trunc_to_i32_via_i64(f64::from(arch.hold_size)) as u32)
}

/// `GetLoadoutName`: the C++ tests the caller's buffer, not the name: a
/// buffer whose first byte is 0 (or that is empty) answers -2. Then the
/// loadout's nickname id as `ID_String::get_string` spells it (`0x` and
/// lower-case hex) goes into the buffer by `strncpy` with the buffer's
/// length as `MaxLen`: zeros fill the rest, and no terminator is forced.
///
/// # Errors
///
/// -2 for an empty buffer or an unknown loadout (the buffer is left).
pub fn loadout_name(cx: &Cx<'_>, loadout: u32, buffer: &mut [u8]) -> Result<(), PubError> {
    if empty_name(buffer) {
        return Err(PubError::BAD_ID);
    }
    let found = cx
        .world
        .data()
        .loadouts
        .get(loadout)
        .ok_or(PubError::BAD_ID)?;
    let name = neuron_core::id::IdString(found.nickname_id).to_hex_string();
    let name = name.as_bytes();
    for (i, b) in buffer.iter_mut().enumerate() {
        *b = name.get(i).copied().unwrap_or(0);
    }
    Ok(())
}

/// `GetLoadoutID`: `Loadout::GetID`, which is `CreateID(name)`.
///
/// # Errors
///
/// -4 for an empty name; -2 when the id is 0.
pub fn loadout_id(name: &[u8]) -> Result<u32, PubError> {
    if empty_name(name) {
        return Err(PubError::BAD_ARGUMENT);
    }
    match create_id(c_str(name)) {
        0 => Err(PubError::BAD_ID),
        id => Ok(id),
    }
}

/// `GetEquipmentID`: `Archetype::GetEquipmentID`, the name's id whether an
/// archetype has it or not.
///
/// # Errors
///
/// -4 for an empty name; -2 when the id's low 30 bits are 0 (the C++
/// writes the id all the same).
pub fn equipment_id(name: &[u8]) -> Result<u32, PubError> {
    if empty_name(name) {
        return Err(PubError::BAD_ARGUMENT);
    }
    valid_or_bad_id(create_id(c_str(name)))
}

/// `GetGoodID`: the good of a nickname (`GoodList::find_by_nickname`).
///
/// # Errors
///
/// -4 for an empty name; -2 for an unknown good, or a good id whose low
/// 30 bits are 0 (written all the same).
pub fn good_id(cx: &Cx<'_>, name: &[u8]) -> Result<u32, PubError> {
    if empty_name(name) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let good = cx
        .world
        .data()
        .goods
        .find_by_name(c_str(name))
        .ok_or(PubError::BAD_ID)?;
    valid_or_bad_id(good.id)
}

/// `TranslateArchToGood`: `Arch2Good`.
///
/// # Errors
///
/// -4 for archetype 0 (not masked); -2 when the good's low 30 bits are 0
/// (written all the same).
pub fn translate_arch_to_good(cx: &Cx<'_>, arch: u32) -> Result<u32, PubError> {
    if arch == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    valid_or_bad_id(cx.world.data().goods.arch_to_good(arch))
}

/// `GetVoiceID`: `CreateID(name)`.
///
/// # Errors
///
/// -4 for an empty name.
pub fn voice_id(name: &[u8]) -> Result<u32, PubError> {
    if empty_name(name) {
        return Err(PubError::BAD_ARGUMENT);
    }
    Ok(create_id(c_str(name)))
}

/// `GetCostumeID`: the costume of a nickname
/// (`CostumeDescriptions::find_costume`), its id.
///
/// # Errors
///
/// -4 for an empty name; -2 for an unknown costume or one whose id is 0
/// (the C++ writes 0 or the id first).
pub fn costume_id(cx: &Cx<'_>, name: &[u8]) -> Result<i32, PubError> {
    if empty_name(name) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let id = cx
        .world
        .data()
        .costumes
        .find_costume(game_sim::data::costume::nickname_id(c_str(name)))
        .map_or(0, |c| c.id as i32);
    if id != 0 {
        Ok(id)
    } else {
        Err(PubError::BAD_ID)
    }
}

/// `GetTime`: the game time in days (the server clock times the universe's
/// `DaysPerSecond`). The C++ always answers 0.
#[must_use]
pub fn time(cx: &Cx<'_>) -> f64 {
    cx.logic.clock.now() * cx.world.data().universe.days_per_second
}

/// `GetRwTime`: the game time in seconds. The C++ always answers 0.
#[must_use]
pub fn rw_time(cx: &Cx<'_>) -> f64 {
    cx.logic.clock.now()
}

/// `NextSystemID`: the system after `id` in id order (the first for 0);
/// false, with `id` left alone, past the last or for an id the walk does
/// not find.
pub fn next_system_id(cx: &Cx<'_>, id: &mut u32) -> bool {
    let systems = &cx.world.data().universe.systems;
    let mut walk = systems.keys();
    let mut current = walk.next();
    if *id != 0 {
        while let Some(&system) = current {
            current = walk.next();
            if system == *id {
                break;
            }
        }
    }
    match current {
        Some(&next) => {
            *id = next;
            true
        }
        None => false,
    }
}

/// `NextBaseID`: the [`next_system_id`] walk over the bases.
pub fn next_base_id(cx: &Cx<'_>, id: &mut u32) -> bool {
    let bases = &cx.world.data().universe.bases;
    let mut walk = bases.keys();
    let mut current = walk.next();
    if *id != 0 {
        while let Some(&base) = current {
            current = walk.next();
            if base == *id {
                break;
            }
        }
    }
    match current {
        Some(&next) => {
            *id = next;
            true
        }
        None => false,
    }
}

/// `GetSystemID`: the system of a nickname (`Universe::get_system_id`, a
/// miss reported).
///
/// # Errors
///
/// -4 for an empty name; -2 when no system has it.
pub fn system_id(cx: &mut Cx<'_>, name: &[u8]) -> Result<u32, PubError> {
    if name.first().is_none_or(|&b| b == 0) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let id = cx
        .world
        .data()
        .universe
        .get_system_id(name, &mut cx.logic.reports);
    if id & VALID_ID == 0 {
        return Err(PubError::BAD_ID);
    }
    Ok(id)
}

/// `GetSystemGateConnection`: the system a gate leads to
/// (`Universe::get_gate_system`).
///
/// # Errors
///
/// -2 when the answer's low 30 bits are 0 (the C++ writes it all the
/// same).
pub fn system_gate_connection(cx: &Cx<'_>, gate: u32) -> Result<u32, PubError> {
    let system = cx.world.data().universe.gate_system(gate);
    if system & VALID_ID != 0 {
        Ok(system)
    } else {
        Err(PubError::BAD_ID)
    }
}

/// `GetBaseID`: the base of a nickname (`Universe::get_base_id`, a miss
/// reported).
///
/// # Errors
///
/// -4 for an empty name and for an unknown base alike (the C++ writes 0
/// first).
pub fn base_id(cx: &mut Cx<'_>, name: &[u8]) -> Result<u32, PubError> {
    if empty_name(name) {
        return Err(PubError::BAD_ARGUMENT);
    }
    let id = cx
        .world
        .data()
        .universe
        .lookup_base(c_str(name), &mut cx.logic.reports);
    if id & VALID_ID == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    Ok(id)
}

/// `BuildSystemReader`: a reader open on the system's file.
///
/// # Errors
///
/// -2 for an unknown system, or a file that does not open (reported).
pub fn build_system_reader(
    cx: &mut Cx<'_>,
    source: &dyn DataSource,
    system: u32,
) -> Result<IniReader, PubError> {
    let data = cx.world.data_arc();
    let sys = get_system(&data, &mut cx.logic.reports, system).ok_or(PubError::BAD_ID)?;
    let path = data
        .universe
        .system_file_path(cx.world.strings().get(sys.file));
    match game_sim::data::open_ini(source, &path) {
        Some(reader) => Ok(reader),
        None => {
            cx.logic.reports.push(
                PUB_MISC,
                0x2b3,
                format!(
                    "*** ERROR: missing System file '{}'",
                    String::from_utf8_lossy(&path)
                ),
            );
            Err(PubError::BAD_ID)
        }
    }
}

/// `BuildBaseReader`: a reader open on the base's file, which is relative
/// to the data folder (`GetFileSysPath` and `_makepath`; the source is
/// rooted there). A file that does not open is not reported.
///
/// # Errors
///
/// -2 for an unknown base or a file that does not open.
pub fn build_base_reader(
    cx: &mut Cx<'_>,
    source: &dyn DataSource,
    base: u32,
) -> Result<IniReader, PubError> {
    let data = cx.world.data_arc();
    let found = get_base(&data, &mut cx.logic.reports, base).ok_or(PubError::BAD_ID)?;
    let path = c_str(cx.world.strings().get(found.file)).to_vec();
    game_sim::data::open_ini(source, &path).ok_or(PubError::BAD_ID)
}

/// `GetSystemNickname`: the C++'s answer and what it copies. An unknown
/// system answers 0 and copies nothing; a nickname (with its terminator)
/// longer than `max_len` answers the length it needs; otherwise 0 and the
/// nickname, without the terminator.
pub fn system_nickname(cx: &mut Cx<'_>, max_len: u32, system: u32) -> (u32, Option<Vec<u8>>) {
    let data = cx.world.data_arc();
    let Some(sys) = get_system(&data, &mut cx.logic.reports, system) else {
        return (0, None);
    };
    let nickname = neuron_core::hash::c_str(cx.world.strings().get(sys.nickname)).to_vec();
    let needed = nickname.len() as u32 + 1;
    if max_len < needed {
        return (needed, None);
    }
    (0, Some(nickname))
}

/// `GetBaseNickname`: as [`system_nickname`] over the bases.
pub fn base_nickname(cx: &mut Cx<'_>, max_len: u32, base: u32) -> (u32, Option<Vec<u8>>) {
    let data = cx.world.data_arc();
    let Some(found) = get_base(&data, &mut cx.logic.reports, base) else {
        return (0, None);
    };
    let nickname = c_str(cx.world.strings().get(found.nickname)).to_vec();
    let needed = nickname.len() as u32 + 1;
    if needed > max_len {
        return (needed, None);
    }
    (0, Some(nickname))
}

/// `GetBaseStridName`: the base's `strid_name`.
///
/// # Errors
///
/// -2 for an unknown base.
pub fn base_strid_name(cx: &mut Cx<'_>, base: u32) -> Result<u32, PubError> {
    let data = cx.world.data_arc();
    get_base(&data, &mut cx.logic.reports, base)
        .map(|b| b.strid_name as u32)
        .ok_or(PubError::BAD_ID)
}

/// `GetLocations`: the rooms of a base among the seven fixed names
/// (`MakeLocationID`), in that order, at most `max_count`. Every name is
/// tried, and each room the base data lacks is reported; a base without
/// base data (reported) has none. The C++ always answers 0.
pub fn locations(cx: &mut Cx<'_>, base: u32, max_count: u32) -> Vec<u32> {
    let data = cx.world.data_arc();
    let mut out = Vec::new();
    if base_data_exists(&data, &mut cx.logic.reports, base).is_none() {
        return out;
    }
    for name in ROOM_NAMES {
        let room = make_location_id(base, name);
        if room_data(&data, &mut cx.logic.reports, room).is_some() && (out.len() as u32) < max_count
        {
            out.push(room);
        }
    }
    out
}

/// `GetLocationNickname`: as [`system_nickname`] over the rooms. The C++
/// reads an unknown room's nickname through null; the port answers 0 and
/// copies nothing, after the miss is reported (D692).
pub fn location_nickname(cx: &mut Cx<'_>, max_len: u32, room: u32) -> (u32, Option<Vec<u8>>) {
    let data = cx.world.data_arc();
    let Some(found) = room_data(&data, &mut cx.logic.reports, room) else {
        return (0, None);
    };
    let nickname = c_str(&found.nickname).to_vec();
    let needed = nickname.len() as u32 + 1;
    if needed > max_len {
        return (needed, None);
    }
    (0, Some(nickname))
}

/// `GetBases`: the bases of a system in id order (the whole walk), at most
/// `max_count`. The C++ always answers 0.
#[must_use]
pub fn bases(cx: &Cx<'_>, system: u32, max_count: u32) -> Vec<u32> {
    let mut out = Vec::new();
    for (&id, base) in &cx.world.data().universe.bases {
        if base.system == system && (out.len() as u32) < max_count {
            out.push(id);
        }
    }
    out
}

/// `GetSystem`: the system of a base.
///
/// # Errors
///
/// -2 for an unknown base (the C++ writes 0 first).
pub fn base_system(cx: &mut Cx<'_>, base: u32) -> Result<u32, PubError> {
    let data = cx.world.data_arc();
    get_base(&data, &mut cx.logic.reports, base)
        .map(|b| b.system)
        .ok_or(PubError::BAD_ID)
}

/// `GetNicknameId`: `CreateID`.
#[must_use]
pub fn nickname_id(name: &[u8]) -> u32 {
    create_id(name)
}

/// `MakeId`.
#[must_use]
pub fn make_id(name: &[u8]) -> u16 {
    neuron_core::id::make_id(name)
}

/// `DebugPrint`: level 1 reports a warning, level 2 an error, any other
/// level nothing.
pub fn debug_print(cx: &mut Cx<'_>, message: &[u8], level: i32) {
    let text = String::from_utf8_lossy(c_str(message));
    match level {
        1 => cx
            .logic
            .reports
            .push(PUB_MISC, 0x3c5, format!("*** WARNING: pub: {text}")),
        2 => cx
            .logic
            .reports
            .push(PUB_MISC, 0x3c6, format!("*** ERROR: pub: {text}")),
        _ => {}
    }
}

/// `GetNavMapScale`.
///
/// # Errors
///
/// -1 for an unknown system (the C++ writes 0 first).
pub fn nav_map_scale(cx: &mut Cx<'_>, system: u32) -> Result<f32, PubError> {
    let data = cx.world.data_arc();
    get_system(&data, &mut cx.logic.reports, system)
        .map(|s| s.nav_map_scale)
        .ok_or(PubError::FAILED)
}

/// `ReportFreeTerminal`: SpaceSupport's answer to a launch's
/// `FreeTerminalRequest`. The C++ stores the second argument in a global
/// that the launch reads back; the port's global is the message's
/// `reported` field ([`crate::dock::launch_position`]), which this writes
/// when the message is that request. The first argument is never read.
/// Always 0.
pub fn report_free_terminal(
    message: &mut ControllerMessage<'_>,
    _first: u32,
    terminal: i32,
) -> i32 {
    if let ControllerMessage::FreeTerminalRequest { reported, .. } = message {
        *reported = terminal;
    }
    0
}

/// `GetFullHealth`: the ship archetype's hit points, truncated through 64
/// bits, low dword.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 for an unknown ship.
pub fn full_health(cx: &Cx<'_>, ship: u32) -> Result<u32, PubError> {
    if ship & VALID_ID == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    let arch = cx
        .world
        .data()
        .archetypes
        .ships
        .get(&ship)
        .ok_or(PubError::BAD_ID)?;
    Ok(trunc_to_i32_via_i64(f64::from(arch.eq_obj.root.hit_pts)) as u32)
}

/// `GetShipArchSTRID`: the ship archetype's `ids_name`.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when the name's low 30 bits
/// are 0 (written all the same). The C++ reads an unknown ship through
/// null; the port answers -2 and writes nothing (D693).
pub fn ship_arch_strid(cx: &Cx<'_>, ship: u32) -> Result<u32, PubError> {
    if ship & VALID_ID == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    let arch = cx
        .world
        .data()
        .archetypes
        .ships
        .get(&ship)
        .ok_or(PubError::BAD_ID)?;
    valid_or_bad_id(arch.eq_obj.root.ids_name as u32)
}

/// `GetType`: the ship archetype's object type.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 for an unknown ship.
pub fn ship_type(cx: &Cx<'_>, ship: u32) -> Result<u32, PubError> {
    if ship & VALID_ID == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    cx.world
        .data()
        .archetypes
        .ships
        .get(&ship)
        .map(|a| a.eq_obj.root.object_type)
        .ok_or(PubError::BAD_ID)
}

/// `GetSolarType`: as [`ship_type`] over the solar archetypes.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 for an unknown solar.
pub fn solar_type(cx: &Cx<'_>, solar: u32) -> Result<u32, PubError> {
    if solar & VALID_ID == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    cx.world
        .data()
        .archetypes
        .solars
        .get(&solar)
        .map(|a| a.eq_obj.root.object_type)
        .ok_or(PubError::BAD_ID)
}

/// `GetMaxHitPoints`: the solar archetype's hit points, truncated through
/// 64 bits, low dword.
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 for an unknown solar.
pub fn max_hit_points(cx: &Cx<'_>, solar: u32) -> Result<i32, PubError> {
    if solar & VALID_ID == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    cx.world
        .data()
        .archetypes
        .solars
        .get(&solar)
        .map(|a| trunc_to_i32_via_i64(f64::from(a.eq_obj.root.hit_pts)))
        .ok_or(PubError::BAD_ID)
}

/// `GetGoodProperties`: the volume and the mass of the good's equipment
/// archetype (`Good2Arch`).
///
/// # Errors
///
/// -4 for an id whose low 30 bits are 0; -2 when the good has no known
/// equipment archetype.
pub fn good_properties(cx: &Cx<'_>, good: u32) -> Result<(f32, f32), PubError> {
    if good & VALID_ID == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    let data = cx.world.data();
    let arch = data.goods.good_to_arch(good);
    data.archetypes
        .equipment
        .get(&arch)
        .map(|e| (e.volume, e.root.mass))
        .ok_or(PubError::BAD_ID)
}

/// `TranslateSystemToMsgIdPrefix`: the system's `msg_id_prefix` (empty when
/// it has none).
///
/// # Errors
///
/// -4 for an unknown system.
pub fn translate_system_to_msg_id_prefix(
    cx: &mut Cx<'_>,
    system: u32,
) -> Result<Vec<u8>, PubError> {
    let data = cx.world.data_arc();
    get_system(&data, &mut cx.logic.reports, system)
        .map(|s| s.msg_id_prefix.clone())
        .ok_or(PubError::BAD_ARGUMENT)
}

/// `SinglePlayer`.
#[must_use]
pub fn single_player(cx: &Cx<'_>) -> bool {
    cx.world.config().single_player
}

/// `GetLoadout`: a copy of the loadout's items.
///
/// # Errors
///
/// -4 for loadout 0; -2 for an unknown loadout (the caller's vector is
/// left).
pub fn loadout(cx: &Cx<'_>, loadout: u32) -> Result<Vec<EquipDesc>, PubError> {
    if loadout == 0 {
        return Err(PubError::BAD_ARGUMENT);
    }
    cx.world
        .data()
        .loadouts
        .get(loadout)
        .map(|l| l.items.clone())
        .ok_or(PubError::BAD_ID)
}

/// `Save(player, type)`: the single-player save game; type 1 is the
/// autosave, any other a mission save with the type as the description's
/// text id ([`crate::saves::pub_save`]). Nothing on a multiplayer server.
/// The C++ always answers 0, so the port answers nothing.
pub fn save(cx: &mut Cx<'_>, player: u32, save_type: u32) {
    crate::saves::pub_save(cx, player, save_type);
}

/// `FindHardpoint`: a hardpoint of a ship archetype's model in its part's
/// frame (`Archetype::Load`, `HardpointPositionA`).
///
/// # Errors
///
/// -2 when the model does not load; -3 when it has no such hardpoint. The
/// C++ reads an unknown ship through null; the port answers -2 (D693).
pub fn find_hardpoint(cx: &mut Cx<'_>, name: &[u8], ship: u32) -> Result<Transform, PubError> {
    match cx.world.ship_archetype_hardpoint_lookup(ship, c_str(name)) {
        None => Err(PubError::BAD_ID),
        Some(None) => Err(PubError::NOT_FOUND),
        Some(Some(found)) => Ok(found),
    }
}

/// `GetShipID`: `Archetype::GetShipID`, the name's id whether a ship has it
/// or not.
///
/// # Errors
///
/// -4 for an empty name; -2 when the id's low 30 bits are 0 (written all
/// the same).
pub fn ship_id(name: &[u8]) -> Result<u32, PubError> {
    if empty_name(name) {
        return Err(PubError::BAD_ARGUMENT);
    }
    valid_or_bad_id(create_id(c_str(name)))
}

/// `TranslateGoodToMsgIdPrefix`: the good's `msg_id_prefix` (empty when it
/// has none).
///
/// # Errors
///
/// -4 for an unknown good.
pub fn translate_good_to_msg_id_prefix(cx: &Cx<'_>, good: u32) -> Result<Vec<u8>, PubError> {
    cx.world
        .data()
        .goods
        .find_by_id(good)
        .map(|g| g.msg_id_prefix.clone())
        .ok_or(PubError::BAD_ARGUMENT)
}

/// `TranslateShipToMsgIdPrefix`: the ship archetype's `msg_id_prefix`
/// (empty when it has none).
///
/// # Errors
///
/// -4 for an unknown ship.
pub fn translate_ship_to_msg_id_prefix(cx: &Cx<'_>, ship: u32) -> Result<Vec<u8>, PubError> {
    cx.world
        .data()
        .archetypes
        .ships
        .get(&ship)
        .map(|a| a.msg_id_prefix.clone())
        .ok_or(PubError::BAD_ARGUMENT)
}
