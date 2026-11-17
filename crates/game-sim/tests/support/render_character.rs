//! Dump lines of the character databases (body parts, costumes, the new
//! character database, the performance slider) and of the static vibes
//! (`CppSource/Oracle/oracle_game_character.cpp` writes the same).

use std::collections::BTreeSet;

use game_sim::data::config::GameData;
use game_sim::data::costume::CostumeDescriptions;
use game_sim::data::new_char::NewCharDb;
use game_sim::data::rtc_slider::RtcSlider;
use game_sim::data::universe::Universe;
use game_sim::data::vibe::VibeDb;
use game_sim::data::{DataSource, Reports, open_ini};
use neuron_core::id::create_id;

use super::f32_text as f;
use super::render::{Line, json_text};

fn id(v: u32) -> String {
    format!("{v:08x}")
}

fn index(v: Option<usize>) -> String {
    v.map_or_else(|| "-1".to_owned(), |i| i.to_string())
}

fn texts(list: &[Vec<u8>]) -> String {
    list.iter()
        .map(|t| json_text(t))
        .collect::<Vec<_>>()
        .join(";")
}

fn ids<'a>(list: impl IntoIterator<Item = &'a u32>) -> String {
    list.into_iter()
        .map(|&v| id(v))
        .collect::<Vec<_>>()
        .join(";")
}

/// Kind `characters`, file `bodyparts`: the name lists, the petal lists,
/// the switch tables, then the parts of each type by id.
pub fn bodyparts(db: &CostumeDescriptions) -> Vec<String> {
    let mut lines = Vec::new();
    for name in &db.animation_names {
        lines.push(Line::new().add("anim", json_text(name)).done());
    }
    for name in &db.preload_names {
        lines.push(Line::new().add("preload", json_text(name)).done());
    }
    for (i, list) in db.petal_animations.iter().enumerate() {
        lines.push(
            Line::new()
                .add("petal_list", i.to_string())
                .add("anims", texts(list))
                .done(),
        );
    }
    for (i, table) in db.detail_switch_tables.iter().enumerate() {
        let pairs: Vec<String> = table
            .table
            .iter()
            .map(|(k, v)| format!("{},{}", f(*k), f(*v)))
            .collect();
        lines.push(
            Line::new()
                .add("switch_table", i.to_string())
                .add("fov_x_half", f(table.fov_x_half))
                .add("size_x", f(table.size_x))
                .add("table", pairs.join(";"))
                .done(),
        );
    }
    for (kind, parts) in db.bodyparts.iter().enumerate() {
        for part in parts.values() {
            lines.push(
                Line::new()
                    .add("part_type", kind.to_string())
                    .add("part", id(part.id))
                    .add("mesh", json_text(&part.mesh))
                    .add("sex", part.sex.to_string())
                    .add("switch", index(part.switch))
                    .add("petal", index(part.petal_animations))
                    .done(),
            );
        }
    }
    lines
}

/// Kind `characters`, file `accessories`.
pub fn accessories(db: &CostumeDescriptions) -> Vec<String> {
    db.accessories
        .values()
        .map(|a| {
            Line::new()
                .add("accessory", id(a.id))
                .add("mesh", json_text(&a.mesh))
                .add("hardpoint", json_text(&a.accessory_hardpoint))
                .add("body_hardpoint", json_text(&a.character_hardpoint))
                .add("materials", texts(&a.materials))
                .done()
        })
        .collect()
}

/// Kind `characters`, file `costumes`: parts by type (body, head, left,
/// right), 0 for none.
pub fn costumes(db: &CostumeDescriptions) -> Vec<String> {
    db.costumes
        .values()
        .map(|c| {
            let parts: Vec<String> = c.parts.iter().map(|p| id(p.unwrap_or(0))).collect();
            Line::new()
                .add("costume", id(c.id))
                .add("gender", c.gender.to_string())
                .add("parts", parts.join(","))
                .add("accessories", ids(&c.accessories))
                .done()
        })
        .collect()
}

/// Kind `characters`, file `new_char`: factions, packages, pilots, extras,
/// each by id.
pub fn new_char(db: &NewCharDb) -> Vec<String> {
    let mut lines = Vec::new();
    for x in db.factions.values() {
        lines.push(
            Line::new()
                .add("faction", id(x.nickname))
                .add("rep_group", json_text(&x.rep_group))
                .add("bases", ids(&x.bases))
                .add("packages", ids(&x.packages))
                .add("pilots", ids(&x.pilots))
                .done(),
        );
    }
    for x in db.packages.values() {
        lines.push(
            Line::new()
                .add("package", id(x.nickname))
                .add("strid_name", x.strid_name.to_string())
                .add("strid_desc", x.strid_desc.to_string())
                .add("ship", json_text(&x.ship))
                .add("loadout", json_text(&x.loadout))
                .add("money", x.money.to_string())
                .done(),
        );
    }
    for x in db.pilots.values() {
        lines.push(
            Line::new()
                .add("pilot", id(x.nickname))
                .add("body", json_text(&x.body))
                .add("body_anims", texts(&x.body_anims))
                .add("comm", json_text(&x.comm))
                .add("comm_anims", texts(&x.comm_anims))
                .add("voice", json_text(&x.voice))
                .add("thumb", json_text(&x.thumb))
                .done(),
        );
    }
    for (key, value) in &db.extras {
        lines.push(
            Line::new()
                .add("extra", id(*key))
                .add("value", json_text(value))
                .done(),
        );
    }
    lines
}

/// The slider values the `rtc_slider` dump probes (bits): around each
/// threshold of the original file, and a few others.
pub const SLIDER_PROBES: [u32; 17] = [
    0xbfc0_0000, // -1.5
    0xbf80_0000, // -1
    0xbf7f_ffff, // just above -1
    0x8000_0001, // just below 0
    0x0000_0000, // 0
    0x3e2e_147a, // just below 0.17
    0x3e2e_147b, // 0.17
    0x3f02_8f5b, // just below 0.51
    0x3f02_8f5c, // 0.51
    0x3f3f_ffff, // just below 0.75
    0x3f40_0000, // 0.75
    0x3f54_7ae0, // just below 0.83
    0x3f54_7ae1, // 0.83
    0x3f80_0000, // 1
    0x4000_0000, // 2
    0x7f80_0000, // +inf
    0x7fc0_0000, // NaN
];

/// The ids of every `set` and `set_less` name of the `[RtcSlider]`
/// sections of the files the `[Data]` list names.
pub fn rtc_names(source: &dyn DataSource, files: &[Vec<u8>]) -> BTreeSet<u32> {
    let mut out = BTreeSet::new();
    for file in files {
        let Some(mut reader) = open_ini(source, file) else {
            continue;
        };
        while reader.read_header() {
            if !reader.is_header(b"RtcSlider") {
                continue;
            }
            while reader.read_value() {
                if reader.is_value(b"set") || reader.is_value(b"set_less") {
                    out.insert(create_id(&reader.value_string_at(0)));
                }
            }
        }
    }
    out
}

/// Kind `characters`, file `rtc_slider`: the frame cap at each probe, then
/// per name the enabled answers at each probe.
pub fn rtc_slider(slider: &RtcSlider, names: &BTreeSet<u32>) -> Vec<String> {
    let mut reports = Reports::default();
    let mut probe = slider.clone();
    let mut lines = Vec::new();
    for bits in SLIDER_PROBES {
        probe.slider = f32::from_bits(bits);
        lines.push(
            Line::new()
                .add("slider", format!("{bits:08x}"))
                .add("fps_cap", f(probe.fps_cap(&mut reports)))
                .done(),
        );
    }
    for &name in names {
        let mut answers = String::new();
        for bits in SLIDER_PROBES {
            probe.slider = f32::from_bits(bits);
            answers.push(if probe.is_enabled_id(name, &mut reports) {
                '1'
            } else {
                '0'
            });
        }
        lines.push(
            Line::new()
                .add("setting", id(name))
                .add("enabled", answers)
                .done(),
        );
    }
    lines
}

/// The nickname ids of every object of every system file, in order.
pub fn object_ids(data: &GameData, source: &dyn DataSource) -> BTreeSet<u32> {
    let u: &Universe = &data.universe;
    let mut reports = Reports::default();
    let mut out = BTreeSet::new();
    for s in u.systems.values() {
        for o in u.system_objects(source, s, &data.strings, &mut reports) {
            out.insert(create_id(&o.nickname));
        }
    }
    out
}

/// Kind `vibes`, file `vibes`: the count, then per object id whether the
/// static store finds it and, if so, its record. The lookups go through
/// the store's cache in the same order as the C++ calls.
pub fn vibes(db: &VibeDb, probes: &BTreeSet<u32>) -> Vec<String> {
    let mut db = db.clone();
    let mut lines = vec![Line::new().add("count", db.count().to_string()).done()];
    for &probe in probes {
        let key = i32::from_ne_bytes(probe.to_ne_bytes());
        let mut line = Line::new();
        line.add("vibe", id(probe));
        let found = db.verify(key);
        line.add("found", u8::from(found).to_string());
        if let Some(v) = found.then(|| db.get(key).cloned()).flatten() {
            line.add("affiliation", id(v.affiliation))
                .add("rank", v.rank.to_string())
                .add("infocard", v.infocard.to_string())
                .add(
                    "name",
                    format!("{},{}", v.name1.mad_lib_id, v.name2.mad_lib_id),
                );
        }
        lines.push(line.done());
    }
    lines
}
