//! The top-level `pub::` getters the system, zone and reputation families
//! lean on (`Server/server_pub.cpp:131-762`, `Server\pubMisc.cpp`): the
//! universe's systems by id and nickname, their files and names, and the
//! process's side. The others (goods, loadouts, bases, objects) come with
//! the families that need them.
//!
//! | Export | Line | Port |
//! | --- | --- | --- |
//! | `NextSystemID` | 276 | [`next_system_id`] |
//! | `GetSystemID` | 313 | [`system_id`] |
//! | `GetSystemGateConnection` | 323 | [`system_gate_connection`] |
//! | `BuildSystemReader` | 351 | [`build_system_reader`] |
//! | `GetSystemNickname` | 386 | [`system_nickname`] |
//! | `GetNicknameId` | 487 | [`nickname_id`] |
//! | `MakeId` | 491 | [`make_id`] |
//! | `GetNavMapScale` | 511 | [`nav_map_scale`] |
//! | `TranslateSystemToMsgIdPrefix` | 679 | [`translate_system_to_msg_id_prefix`] |
//! | `SinglePlayer` | 690 | [`single_player`] |
//!
//! `Universe::get_system` and `get_zone` report a miss as Common does
//! ([`get_system`], [`get_zone`]).

use game_sim::data::config::GameData;
use game_sim::data::universe::System;
use game_sim::data::zone::Zone;
use game_sim::data::{DataSource, Reports};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

use super::PubError;
use crate::server::Cx;

const SYSTEMS: &str = "Common\\Systems.cpp";
const PUB_MISC: &str = "Server\\pubMisc.cpp";

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
