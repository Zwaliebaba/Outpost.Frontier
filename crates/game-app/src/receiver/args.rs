//! The IClient log's argument recorders for the slots the receiver handles
//! (`oracle_iclient_args.cpp`): each writes what the C++ proxy writes for
//! the argument the slot receives, rebuilt from the message that carried it.
//!
//! A plain struct goes as the hex of its `sizeof` bytes (`ORACLE_HEX_ARG`),
//! so its recorder lays the fields out as the C++ struct does
//! (`rpc_iclient_args.h`, `common_server_types.h`); bytes the sender never
//! sets are written as zeros where rpclocal zeroes them.

use game_sim::protocol::common::ActivateEquip;
use game_sim::protocol::to_client::{
    CharSelectVerified, Launch, RequestSpaceScript, SetMissionObjectives, SetReputation,
};

use crate::oracle::json::OracleJson;

/// `kRequestSpaceScriptNameSize`: the name field of
/// `FServerRequestSpaceScriptInfo`.
pub const SPACE_SCRIPT_NAME_SIZE: usize = 0x104;

/// `FServerCharSelectVerifiedInfo` (0x10 bytes): the vibe, the unused dword
/// (zero from rpclocal's receiver) and the server time.
#[must_use]
pub fn char_select_verified(info: &CharSelectVerified) -> Vec<u8> {
    let mut out = Vec::with_capacity(0x10);
    out.extend_from_slice(&info.rep_instance.to_le_bytes());
    out.extend_from_slice(&[0; 4]);
    out.extend_from_slice(&info.time.to_le_bytes());
    out
}

/// `FServerLaunchInfo` (0x28 bytes): the ship, the launch object, the
/// launch point, the orientation `w, x, y, z` and the position.
#[must_use]
pub fn launch(info: &Launch) -> Vec<u8> {
    let mut out = Vec::with_capacity(0x28);
    for word in [info.field_00, info.field_04, info.field_08] {
        out.extend_from_slice(&word.to_le_bytes());
    }
    for value in info.orientation.iter().chain(&info.position) {
        out.extend_from_slice(&value.to_le_bytes());
    }
    out
}

/// `XActivateEquip` (8 bytes): the object, the equipment id, the active
/// byte and one byte of padding, which rpclocal's receiver zeroes.
#[must_use]
pub fn activate_equip(info: &ActivateEquip) -> Vec<u8> {
    let mut out = Vec::with_capacity(8);
    out.extend_from_slice(&info.object_id.to_le_bytes());
    out.extend_from_slice(&info.equip_id.to_le_bytes());
    out.push(info.active);
    out.push(0);
    out
}

/// `FServerRequestSpaceScriptInfo` (0x118 bytes): the object, the name in
/// its 0x104 bytes (zeroed past the terminator, as rpclocal's receiver
/// leaves them), the position and the mode (`Field_0x114`).
#[must_use]
pub fn request_space_script(info: &RequestSpaceScript) -> Vec<u8> {
    let mut out = Vec::with_capacity(0x118);
    out.extend_from_slice(&info.field_00.to_le_bytes());
    let mut name = [0u8; SPACE_SCRIPT_NAME_SIZE];
    for (out, &byte) in name
        .iter_mut()
        .zip(info.name.iter().take_while(|&&b| b != 0))
    {
        *out = byte;
    }
    out.extend_from_slice(&name);
    for word in info.position {
        out.extend_from_slice(&word.to_le_bytes());
    }
    out.extend_from_slice(&info.priority().to_le_bytes());
    out
}

/// `OracleArg(FServerSetReputationInfo)`: the vibe, the attitude (single
/// player sends none; the multiplayer form's), the affiliation as
/// `Field_0x08`, `Field_0x0c` (no sender sets it), the relations (null when
/// there are none), the two names as present and the display name.
pub fn set_reputation(j: &mut OracleJson, name: &str, info: &SetReputation) {
    j.begin_object(name)
        .int("VibeHandle", i32::from_ne_bytes(info.target.to_ne_bytes()))
        .float("Attitude", info.attitude)
        .uint("Field_0x08", info.affiliation)
        .uint("Field_0x0c", 0)
        .uint(
            "RelationCount",
            u32::try_from(info.relations.len()).unwrap_or(u32::MAX),
        );
    if info.relations.is_empty() {
        j.null("Relations");
    } else {
        j.begin_array("Relations");
        for relation in &info.relations {
            j.begin_array_item()
                .item_uint(relation.group_id)
                .item_float(relation.feeling)
                .end_array();
        }
        j.end_array();
    }
    j.str("NameA", "present")
        .str("NameB", "present")
        .wstr("DisplayName", &info.display_name)
        .end_object();
}

/// `OracleArg(FServerSetMissionObjectivesInfo)`: the mission, the count,
/// the objectives' states (null for none), the two texts as present and
/// the state as `Field_0x3c`.
pub fn set_mission_objectives(j: &mut OracleJson, name: &str, info: &SetMissionObjectives) {
    j.begin_object(name).uint("Field_0x00", info.field_00).uint(
        "Count",
        u32::try_from(info.objectives.len()).unwrap_or(u32::MAX),
    );
    if info.objectives.is_empty() {
        j.null("States");
    } else {
        j.begin_array("States");
        for objective in &info.objectives {
            j.item_uint(u32::from(objective.state));
        }
        j.end_array();
    }
    j.str("TextA", "present")
        .str("TextB", "present")
        .uint("Field_0x3c", info.state)
        .end_object();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_structs_have_the_cpp_sizes_and_layouts() {
        let verified = char_select_verified(&CharSelectVerified {
            rep_instance: 1,
            time: f64::from_bits(0x400f_7777_77ee_eee6),
        });
        assert_eq!(hex(&verified), "0100000000000000e6eeee7777770f40");
        let launched = launch(&Launch {
            field_00: 11,
            field_04: 0,
            field_08: u32::MAX,
            position: [1.0, 2.0, 3.0],
            orientation: [4.0, 5.0, 6.0, 7.0],
        });
        assert_eq!(launched.len(), 0x28);
        assert_eq!(&launched[12..16], 4.0f32.to_le_bytes());
        assert_eq!(&launched[28..32], 1.0f32.to_le_bytes());
        let equip = activate_equip(&ActivateEquip {
            object_id: 12,
            equip_id: 42,
            active: 1,
        });
        assert_eq!(hex(&equip), "0c0000002a000100");
        let script = request_space_script(&RequestSpaceScript {
            field_00: 7,
            name: b"ab\0".to_vec(),
            ..RequestSpaceScript::default()
        });
        assert_eq!(script.len(), 0x118);
        assert_eq!(&script[4..7], b"ab\0");
        assert_eq!(&script[0x114..], 1u32.to_le_bytes());
    }

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|b| format!("{b:02x}")).collect()
    }
}
