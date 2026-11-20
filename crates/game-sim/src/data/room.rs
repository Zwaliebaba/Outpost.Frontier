//! A room's own file as the server reads it (work package P3.3,
//! `RoomData::load`, `Common/common_roomdata.cpp`): the set script the
//! `[Room_Info]` block names (`RoomData::read_set_script`), whose camera
//! and marker setpoints become the room's character placements, and the
//! `[CharacterPlacement]` blocks that give a placement its start script.
//!
//! A setpoint's properties come from its name (`SetpointProperties::
//! extract_from_name`, `Common/common_roominfo.cpp`): `zs|zg / pc|npc|
//! none|rpc / segment / zone / name / posture`, lowered first. Only names
//! with the `zs` or `zg` prefix make placements. The rest of the room file
//! (cameras, sounds, hotspots, ship placements, scenes) belongs to the
//! client's base screens and is not read here.

use neuron_core::hash::{eq_ignore_case, stricmp};
use neuron_core::scene_script::entity::kind;
use neuron_core::scene_script::{BuildState, Vm, build, load};

use super::{DataSource, Reports, lossy, open_ini};

const ROOM_DATA: &str = "Common\\RoomData.cpp";
const SETPOINT: &str = "Common\\SetpointProperties.cpp";
const SCENE_PROGRAM: &str = "Common\\SceneProgram.cpp";

/// The properties a setpoint's name encodes (`SetpointProperties`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SetpointProps {
    /// The name has the `zs` or `zg` prefix.
    pub valid: bool,
    /// `zs` (a zone script) rather than `zg`.
    pub zone_script: bool,
    /// `pc` 1, `npc` 0, `none` -1, `rpc` 2.
    pub char_type: i32,
    /// The third token.
    pub segment: Vec<u8>,
    /// The fourth token, `strtol` base 10.
    pub zone: i32,
    /// The fifth token.
    pub name: Vec<u8>,
    /// `stand` 0, `sitlow` 1, `sithigh` 2, anything else 3.
    pub posture: i32,
}

/// `strtol(text, &end, 10)`: the value and whether the whole text was read.
fn strtol(text: &[u8]) -> (i32, bool) {
    let mut at = text
        .iter()
        .position(|b| !b.is_ascii_whitespace())
        .unwrap_or(text.len());
    let negative = text.get(at) == Some(&b'-');
    if matches!(text.get(at), Some(b'-' | b'+')) {
        at += 1;
    }
    let start = at;
    let mut value: i64 = 0;
    while let Some(d) = text.get(at).filter(|b| b.is_ascii_digit()) {
        value = (value * 10 + i64::from(d - b'0')).min(i64::from(u32::MAX) + 1);
        at += 1;
    }
    if at == start {
        return (0, text.is_empty());
    }
    let value = if negative { -value } else { value };
    let clamped = value.clamp(i64::from(i32::MIN), i64::from(i32::MAX)) as i32;
    (clamped, at == text.len())
}

impl SetpointProps {
    /// `extract_from_name`: the properties of `name`. A name with too few
    /// tokens is reported; the C++ then compares a null token and crashes,
    /// where the port reads a missing token as empty (D610).
    #[must_use]
    pub fn from_name(full: &[u8], reports: &mut Reports) -> Self {
        let lowered = full.to_ascii_lowercase();
        // strtok: empty tokens are skipped.
        let mut tokens = lowered
            .split(|&b| b == b'/')
            .filter(|t| !t.is_empty())
            .map(<[u8]>::to_vec);
        let prefix = tokens.next();
        let char_type = tokens.next();
        let segment = tokens.next();
        let zone = tokens.next();
        let name = tokens.next();
        let posture = tokens.next().unwrap_or_default();
        let mut props = Self::default();
        let prefix_text = prefix.clone().unwrap_or_default();
        if prefix_text != b"zs" && prefix_text != b"zg" {
            return props;
        }
        props.valid = true;
        let shown = lossy(full);
        if char_type.is_none() || segment.is_none() || zone.is_none() || name.is_none() {
            reports.push(
                SETPOINT,
                0x78,
                format!("*** ERROR: setpoint name {shown} does not match naming convention"),
            );
        }
        props.zone_script = prefix_text == b"zs";
        let char_type = char_type.unwrap_or_default();
        props.char_type = match char_type.as_slice() {
            b"pc" => 1,
            b"npc" => 0,
            b"none" => -1,
            b"rpc" => 2,
            other => {
                reports.push(
                    SETPOINT,
                    0x9e,
                    format!(
                        "*** ERROR: setpoint character type {} (of {shown}) is not pc, npc, none, or rpc",
                        lossy(other)
                    ),
                );
                0
            }
        };
        props.segment = segment.unwrap_or_default();
        let zone = zone.unwrap_or_default();
        let (value, whole) = strtol(&zone);
        props.zone = value;
        if !whole {
            reports.push(
                SETPOINT,
                0xac,
                format!(
                    "*** ERROR: setpoint zone number {} (of {shown}) is not a number",
                    lossy(&zone)
                ),
            );
        }
        props.name = name.unwrap_or_default();
        props.posture = match posture.as_slice() {
            b"stand" => 0,
            b"sitlow" => 1,
            b"sithigh" => 2,
            other => {
                if !props.zone_script {
                    reports.push(
                        SETPOINT,
                        0xc8,
                        format!(
                            "*** ERROR: invalid posture {} in setpoint marker {shown} - if this is a special relocation marker, mark it with Zs",
                            lossy(other)
                        ),
                    );
                }
                3
            }
        };
        props
    }
}

/// A place a character can stand in a room (`CharPlaceInfo`).
#[derive(Clone, Debug, PartialEq)]
pub struct CharacterPlacement {
    /// The setpoint's name, as the script gives it.
    pub name: Vec<u8>,
    /// The position.
    pub pos: [f32; 3],
    /// The orientation, row by row.
    pub orient: [f32; 9],
    /// The character type.
    pub char_type: i32,
    /// The posture.
    pub posture: i32,
    /// The setpoint is a zone script.
    pub zone_script: bool,
    /// `start_script` of the `[CharacterPlacement]` block naming it.
    pub start_script: Option<Vec<u8>>,
}

/// What the server knows of a room's file.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RoomFile {
    /// The character placements, in the set script's entity order.
    pub placements: Vec<CharacterPlacement>,
}

impl RoomFile {
    /// `find_CharacterPlacement`: the last placement of that name
    /// (`stricmp`), as the C++ loop keeps the last match.
    #[must_use]
    pub fn find_placement(&self, name: &[u8]) -> Option<usize> {
        self.placements
            .iter()
            .rposition(|p| stricmp(&p.name, name).is_eq())
    }
}

/// `RoomData::read_set_script`: the camera and marker entities of the
/// script, as setpoints; the valid ones become placements. A script that
/// cannot be read or built gives none (`SceneProgramLoad` fails).
fn read_set_script(
    source: &dyn DataSource,
    path: &[u8],
    room: &mut RoomFile,
    reports: &mut Reports,
) {
    if path.is_empty() {
        reports.push(
            SCENE_PROGRAM,
            0x18,
            "*** WARNING: Skipping null script filename...",
        );
        return;
    }
    let Some(bytes) = source.read(path) else {
        reports.push(
            SCENE_PROGRAM,
            0x30,
            format!("*** ERROR: Cannot load script file {}", lossy(path)),
        );
        return;
    };
    let mut vm = Vm::new();
    let mut state = BuildState::new();
    let Ok(summary) = load(&mut vm, &bytes) else {
        return;
    };
    let Ok(scene) = build(&mut vm, &mut state, &bytes, summary, true) else {
        return;
    };
    for entity in scene.entities() {
        if entity.kind != kind::CAMERA && entity.kind != kind::MARKER {
            continue;
        }
        // The C++ takes `strlen` of a missing name and crashes (D610).
        let Some(name) = entity.name.clone() else {
            continue;
        };
        let props = SetpointProps::from_name(&name, reports);
        if !props.valid {
            continue;
        }
        room.placements.push(CharacterPlacement {
            name,
            pos: entity.pos,
            orient: entity.orient,
            char_type: props.char_type,
            posture: props.posture,
            zone_script: props.zone_script,
            start_script: None,
        });
    }
}

/// `RoomData::read_from_ini` as the server needs it: the set scripts of the
/// `[Room]`/`[Room_Info]` blocks and the `[CharacterPlacement]` blocks.
#[must_use]
pub fn read_room_file(source: &dyn DataSource, path: &[u8], reports: &mut Reports) -> RoomFile {
    let mut room = RoomFile::default();
    let Some(mut reader) = open_ini(source, path) else {
        return room;
    };
    while reader.read_header() {
        if reader.is_header(b"Room") || reader.is_header(b"Room_Info") {
            while reader.read_value() {
                if reader.is_value(b"set_script") {
                    let script = reader.value_string();
                    read_set_script(source, &script, &mut room, reports);
                }
            }
        } else if reader.is_header(b"CharacterPlacement") {
            let mut name: Option<Vec<u8>> = None;
            let mut start: Option<Vec<u8>> = None;
            while reader.read_value() {
                if reader.is_value(b"name") {
                    name = Some(reader.value_string());
                } else if reader.is_value(b"start_script") {
                    start = Some(reader.value_string());
                } else if reader.is_value(b"zoom_in") || reader.is_value(b"zoom_out") {
                    let (key, line) = if reader.is_value(b"zoom_in") {
                        ("zoom_in", 0x635)
                    } else {
                        ("zoom_out", 0x639)
                    };
                    reports.push(
                        ROOM_DATA,
                        line,
                        format!("*** ERROR: {key} in file {} is obsolete", lossy(path)),
                    );
                }
            }
            let file = lossy(path);
            let Some(name) = name else {
                reports.push(
                    ROOM_DATA,
                    0x647,
                    format!("*** ERROR: [CharacterPlacement] in file {file} did not supply name"),
                );
                continue;
            };
            match room
                .placements
                .iter()
                .rposition(|p| eq_ignore_case(&name, &p.name))
            {
                Some(index) => room.placements[index].start_script = start,
                None => reports.push(
                    ROOM_DATA,
                    0x656,
                    format!(
                        "*** ERROR: [CharacterPlacement] in file {file} refers to setpoint {} which does not exist",
                        lossy(&name)
                    ),
                ),
            }
        }
    }
    room
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn setpoint_names() {
        let mut reports = Reports::default();
        let p = SetpointProps::from_name(b"Zg/NPC/Bar/2/Barkeep/SitLow", &mut reports);
        assert!(p.valid && !p.zone_script);
        assert_eq!((p.char_type, p.zone, p.posture), (0, 2, 1));
        assert_eq!(p.segment, b"bar");
        assert!(reports.0.is_empty());
        let p = SetpointProps::from_name(b"zs/pc/x/0/y", &mut reports);
        assert!(p.valid && p.zone_script);
        assert_eq!(p.posture, 3, "no posture: zone scripts are not reported");
        assert!(reports.0.is_empty());
        let p = SetpointProps::from_name(b"Camera01", &mut reports);
        assert!(!p.valid);
        let p = SetpointProps::from_name(b"zg/npc/x/1a/y/stand", &mut reports);
        assert_eq!(p.zone, 1);
        assert_eq!(reports.0.len(), 1, "zone number report");
    }
}
