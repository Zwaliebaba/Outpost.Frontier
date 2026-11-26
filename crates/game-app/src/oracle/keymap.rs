//! `keymap.jsonl` (`OracleOnKeyMapLoaded`, `oracle_mode.cpp`): after each
//! load phase (`base` after `KeyMap.ini`, `user` after `UserKeyMap.ini`), a
//! `load` record, a `cmd` record per command in list order, a `group` record
//! per `[KeyMap]`, and the active groups, newest first.

use super::json::OracleJson;
use crate::keymap::{KeyMap, nickname};

fn head(frame: u32, rec: &str) -> OracleJson {
    let mut j = OracleJson::new();
    j.uint("frame", frame).str("rec", rec);
    j
}

fn count(n: usize) -> u32 {
    u32::try_from(n).unwrap_or(u32::MAX)
}

/// The lines one load phase writes, at `frame`.
#[must_use]
pub fn phase_lines(map: &KeyMap, frame: u32, phase: &str, result: i32) -> Vec<String> {
    let mut lines = Vec::new();
    let mut j = head(frame, "load");
    j.str("phase", phase)
        .int("result", result)
        .uint("cmds", count(map.commands.len()))
        .uint("groups", count(map.groups.len()));
    lines.push(j.finish());

    for (i, cmd) in map.commands.iter().enumerate() {
        let mut j = head(frame, "cmd");
        j.str("phase", phase)
            .uint("i", count(i))
            .str("nickname", nickname(cmd.nickname_id))
            .uint("id", cmd.nickname_id)
            .uint("ids_name", cmd.ids_name)
            .uint("ids_info", cmd.ids_info)
            .uint("state", cmd.state)
            .begin_array("keys");
        for binding in &cmd.bindings {
            j.begin_array_item()
                .item_int(binding.virtual_key)
                .item_uint(binding.modifiers)
                .end_array();
        }
        j.end_array();
        lines.push(j.finish());
    }

    for (i, group) in map.groups.iter().enumerate() {
        let mut j = head(frame, "group");
        j.str("phase", phase)
            .uint("i", count(i))
            .str_bytes("nickname", &group.nickname)
            .begin_array("cmds");
        for &cmd in &group.commands {
            j.item_int(i32::try_from(cmd).unwrap_or(-1));
        }
        j.end_array();
        lines.push(j.finish());
    }

    let mut j = head(frame, "active");
    j.str("phase", phase).begin_array("groups");
    for &group in &map.active {
        j.item_str_bytes(&map.groups[group].nickname);
    }
    j.end_array();
    lines.push(j.finish());
    lines
}
