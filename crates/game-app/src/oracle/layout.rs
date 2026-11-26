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

/// The lines `UiPageDescLoad(path)` writes at `frame` (`OracleOnPageDescLoaded`):
/// a `page` head and a `widget` record per entry of a page that loaded.
#[must_use]
pub fn page_lines(
    desc: &neuron_client::ui::page::PageDesc,
    frame: u32,
    path: &[u8],
) -> Vec<String> {
    use neuron_client::ui::page::{EntryPayload, Style};
    let entries: &[_] = if desc.ok { &desc.entries } else { &[] };
    let mut lines = Vec::new();
    let mut j = head(frame, "page");
    j.str_bytes("path", path)
        .bool("ok", desc.ok)
        .float("aspect", desc.aspect)
        .uint("entries", index(entries.len()));
    lines.push(j.finish());
    for (i, entry) in entries.iter().enumerate() {
        let mut j = head(frame, "widget");
        j.str_bytes("path", path)
            .uint("i", index(i))
            .str_bytes("type", &entry.type_name)
            .str_bytes("name", &entry.name)
            .begin_array("rect");
        for v in [entry.left, entry.top, entry.right, entry.bottom] {
            j.item_float(v);
        }
        j.end_array().begin_object("payload");
        match &entry.payload {
            EntryPayload::Text {
                right,
                bottom,
                caption,
            } => {
                j.float("right", *right)
                    .float("bottom", *bottom)
                    .wstr("caption", caption);
            }
            EntryPayload::TextMsgButton {
                right,
                bottom,
                caption,
                extra_int,
                extra_id,
                style,
            } => {
                j.float("right", *right)
                    .float("bottom", *bottom)
                    .wstr("caption", caption)
                    .int("extra_int", *extra_int)
                    .int("extra_id", *extra_id);
                match style {
                    Style::None => j.null("style"),
                    Style::Default => j.str("style", "<default>"),
                    Style::Named(name) => j.str_bytes("style", name),
                };
            }
            EntryPayload::WinObject {
                right,
                bottom,
                extra_name,
                extra_float,
            } => {
                j.float("right", *right)
                    .float("bottom", *bottom)
                    .str_bytes("extra_name", extra_name)
                    .float("extra_float", *extra_float);
            }
            EntryPayload::Table {
                right,
                bottom,
                int5,
                int6,
            } => {
                j.float("right", *right)
                    .float("bottom", *bottom)
                    .int("int5", *int5)
                    .int("int6", *int6);
            }
            EntryPayload::ScrollCycle {
                right,
                bottom,
                extra,
                flag,
            } => {
                j.float("right", *right)
                    .float("bottom", *bottom)
                    .begin_array("extra");
                for v in extra {
                    j.item_float(*v);
                }
                j.end_array().uint("flag", u32::from(*flag));
            }
            EntryPayload::Bounds { right, bottom } => {
                j.float("right", *right).float("bottom", *bottom);
            }
        }
        j.end_object();
        lines.push(j.finish());
    }
    lines
}
