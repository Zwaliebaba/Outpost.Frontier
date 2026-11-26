//! `layout.jsonl`'s records of the preferences screen's lists
//! (`OracleOnKeyListLoaded`, `OracleOnOptListLoaded`, `oracle_mode.cpp`): a
//! `keylist` head with the three groups' caption ids and a `keyrow` per row
//! of each group; an `optlist` head and an `opt` per row of the three
//! sections. The page, window-tree and HUD records follow with their screens.

use super::json::OracleJson;
use crate::keymap::nickname;
use crate::preferences::{KeyRowKey, PrefOption, Preferences};

fn head(frame: u32, rec: &str) -> OracleJson {
    let mut j = OracleJson::new();
    j.uint("frame", frame).str("rec", rec);
    j
}

fn index(i: usize) -> u32 {
    u32::try_from(i).unwrap_or(u32::MAX)
}

fn key(j: &mut OracleJson, name: &str, key: &KeyRowKey) {
    j.begin_array(name)
        .item_str_bytes(nickname(key.nickname_id).as_bytes())
        .item_int(key.virtual_key)
        .item_uint(key.modifiers)
        .end_array();
}

/// The lines `LoadKeyList(path)` writes at `frame`.
#[must_use]
pub fn key_list_lines(p: &Preferences, frame: u32, path: &str) -> Vec<String> {
    let mut lines = Vec::new();
    let mut j = head(frame, "keylist");
    j.str("path", path).begin_array("group_names");
    for &ids in &p.group_name_ids {
        j.item_int(ids);
    }
    j.end_array();
    lines.push(j.finish());
    for (group, rows) in (0i32..).zip(&p.key_groups) {
        for (i, row) in rows.iter().enumerate() {
            let mut j = head(frame, "keyrow");
            j.str("path", path).int("group", group).uint("i", index(i));
            key(&mut j, "primary", &row.primary);
            key(&mut j, "alt", &row.alt);
            j.int("name_ids", row.name_ids)
                .int("info_ids", row.info_ids)
                .uint("cmd", row.command_id)
                .int("kind", row.binding_kind)
                .uint("editable", u32::from(row.editable));
            lines.push(j.finish());
        }
    }
    lines
}

fn opt_rows(lines: &mut Vec<String>, frame: u32, path: &str, section: &str, rows: &[PrefOption]) {
    for (i, row) in rows.iter().enumerate() {
        let mut j = head(frame, "opt");
        j.str("path", path)
            .str("section", section)
            .uint("i", index(i))
            .int("id", row.id)
            .int("info_ids", row.info_ids)
            .int("default", row.default)
            .int("min", row.min)
            .int("max", row.max)
            .uint("slider", u32::from(row.slider))
            .uint("byte15", u32::from(row.byte15))
            .uint("header", u32::from(row.header));
        lines.push(j.finish());
    }
}

/// The lines `LoadOptList(path)` writes at `frame` after a load that
/// opened the file (one that does not writes nothing).
#[must_use]
pub fn opt_list_lines(p: &Preferences, frame: u32, path: &str) -> Vec<String> {
    let mut lines = Vec::new();
    let mut j = head(frame, "optlist");
    j.str("path", path)
        .bool("opened", true)
        .uint("seed_mode", u32::from(p.seed_mode));
    lines.push(j.finish());
    opt_rows(&mut lines, frame, path, "Interface", &p.interface);
    opt_rows(&mut lines, frame, path, "Audio", &p.audio);
    opt_rows(&mut lines, frame, path, "Video", &p.video);
    lines
}
