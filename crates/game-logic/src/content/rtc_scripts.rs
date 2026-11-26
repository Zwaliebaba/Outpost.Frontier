//! The RTC scripts (TU23, `Content/RtcScripts.cpp`; work package P3.5c4):
//! the `[CharacterEncounter]` file of a mission's cut scene, which TU21's
//! `StartRtcScript` loads and hands to the base's bar scene creator
//! ([`super::bar::create_bar_scene`]).

use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

use super::bar::{CharacterEncounter, RtcScriptRole};
use super::mission::{base_id_of, find_location, tstring};
use crate::api;
use crate::server::Cx;

/// `get_value_string(0)` as a `TString<N>`.
fn value(reader: &IniReader, size: usize) -> Vec<u8> {
    tstring(&reader.value_string_at(0), size)
}

/// The base of a `relocate_player` or `start_room` line: the script's, or
/// value 1's (`pub::GetBaseID`, 0 for an unknown one).
fn room_base(cx: &mut Cx<'_>, reader: &IniReader, script: &CharacterEncounter) -> u32 {
    if reader.is_value_empty(1) {
        script.base_id
    } else {
        base_id_of(cx, &reader.value_string_at(1))
    }
}

/// One `[CharacterEncounter]` line (`RtcScripts.cpp:58-155`): `Location`
/// (the base and its room), an action (`action` 2, `walkup` 1, `greet` 0,
/// `offer` 3, `decision` 4, `accept` 5, `decline` or `reject` 6, its text
/// the line's first value), `autoplay`, `mission_text_id` (read twice),
/// `nooffer_text_id`, `relocate_player` (`Spaceflight` or a room) and
/// `start_room`.
fn encounter_line(cx: &mut Cx<'_>, reader: &IniReader, script: &mut CharacterEncounter) {
    let kind = if reader.is_value(b"Location") {
        let base = value(reader, 0x20);
        let room = tstring(&reader.value_string_at(1), 0x40);
        script.base_id = base_id_of(cx, &base);
        script.location = find_location(script.base_id, &room);
        None
    } else if reader.is_value(b"action") {
        Some(2)
    } else if reader.is_value(b"walkup") {
        Some(1)
    } else if reader.is_value(b"greet") {
        Some(0)
    } else if reader.is_value(b"offer") {
        Some(3)
    } else if reader.is_value(b"decision") {
        Some(4)
    } else if reader.is_value(b"accept") {
        Some(5)
    } else if reader.is_value(b"decline") || reader.is_value(b"reject") {
        Some(6)
    } else {
        if reader.is_value(b"autoplay") {
            script.autoplay = true;
        } else if reader.is_value(b"mission_text_id") {
            script.loaded_mission_text_id = reader.value_int(0);
            script.mission_text_id = reader.value_int(0);
        } else if reader.is_value(b"nooffer_text_id") {
            script.no_offer_text_id = reader.value_int(0);
        } else if reader.is_value(b"relocate_player") {
            let room = value(reader, 0x40);
            script.relocate_location = if eq_ignore_case(&room, b"Spaceflight") {
                api::gf::spaceflight_location()
            } else {
                let base = room_base(cx, reader, script);
                find_location(base, &room)
            };
        } else if reader.is_value(b"start_room") {
            let room = value(reader, 0x40);
            let base = room_base(cx, reader, script);
            script.start_room_location = find_location(base, &room);
        }
        None
    };
    if let Some(kind) = kind {
        script.actions.push((kind, value(reader, 0x40)));
    }
}

/// The specific NPC of a name (`GSpecificNpcs.find(CreateID(Name))`).
fn specific_npc(cx: &Cx<'_>, name: &[u8]) -> Option<super::npc_desc::NpcCharacterDesc> {
    let key = create_id(c_str(name));
    cx.logic.content.state.specific_npcs.get(&key).cloned()
}

/// `GetCharacterPlacementByName(Location, Name, Spot)` into a slot: the
/// index of a known placement, -1 when the room has none of the name, the
/// slot left for an unknown location; true for a known placement.
fn placement_into(cx: &mut Cx<'_>, location: u32, name: &[u8], slot: &mut i32) -> bool {
    match api::gf::character_placement_by_name(cx, location, c_str(name)) {
        Ok(index) => {
            *slot = i32::try_from(index).unwrap_or(i32::MAX);
            true
        }
        Err(e) => {
            if e == api::PubError::NOT_FOUND {
                *slot = -1;
            }
            false
        }
    }
}

/// One `[Char]` section (`RtcScripts.cpp:174-209`): the role (`actor`,
/// which also names the specific NPC of the same name unless one is set),
/// its `spot` (the call's result is not tested), the specific `npc`
/// (replacing any) and its `fidget`.
fn role(cx: &mut Cx<'_>, reader: &mut IniReader, location: u32) -> RtcScriptRole {
    let mut role = RtcScriptRole {
        spot: -1,
        ..RtcScriptRole::default()
    };
    while reader.read_value() {
        if reader.is_value(b"actor") {
            role.role = value(reader, 0x20);
            if role.character.is_none() {
                role.character = specific_npc(cx, &reader.value_string_at(0));
            }
        } else if reader.is_value(b"spot") {
            let name = reader.value_string_at(0);
            let _ = placement_into(cx, location, &name, &mut role.spot);
        } else if reader.is_value(b"npc") {
            if let Some(npc) = specific_npc(cx, &reader.value_string_at(0)) {
                role.character = Some(npc);
            }
        } else if reader.is_value(b"fidget") {
            role.fidget = c_str(&reader.value_string_at(0)).to_vec();
        }
    }
    role
}

/// `FCharacterEncounter::Load(Path)` (`RtcScripts.cpp:44-213`): the file
/// through `OpenContentDataIni`, then each `[CharacterEncounter]`,
/// `[Reserve]` (the `spot`s whose placement resolves) and `[Char]`
/// section. `None` when the file does not open (`Load`'s false), the
/// script otherwise.
///
/// The constructor leaves `LocationId` and `BaseId` unset; a script
/// without a `Location` line reads them as 0 here (D1040).
pub fn load(cx: &mut Cx<'_>, path: &[u8]) -> Option<CharacterEncounter> {
    let source = super::data_source(cx);
    let mut reader = game_sim::data::open_ini(&*source, c_str(path))?;
    let mut script = CharacterEncounter::default();
    while reader.read_header() {
        if reader.is_header(b"CharacterEncounter") {
            while reader.read_value() {
                encounter_line(cx, &reader, &mut script);
            }
        } else if reader.is_header(b"Reserve") {
            while reader.read_value() {
                if !reader.is_value(b"spot") {
                    continue;
                }
                let name = reader.value_string_at(0);
                let mut spot = 0;
                if placement_into(cx, script.location, &name, &mut spot) {
                    script.reserved_spots.push(spot);
                }
            }
        } else if reader.is_header(b"Char") {
            let made = role(cx, &mut reader, script.location);
            script.roles.push(made);
        }
    }
    Some(script)
}
