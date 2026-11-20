//! The `saves` data dump (work package P3.2): the character templates next
//! to the executable (`EXE/newplayer.fl`, `EXE/mpnewcharacter.fl`) through
//! [`load_save_record`], rendered as `Server.dll`'s `OracleDumpSaveRecord`
//! writes them (`CppSource/Server/server_savegame.cpp`), which `Oracle.exe
//! dump` runs (`CppSource/Oracle/oracle_game_data.cpp`).
//!
//! `OUTPOST_DATA_DIR` names the dump root (`EXE` and `DATA`) and
//! `OUTPOST_ORACLE_DIR` the oracle's output; without the first the test
//! prints a notice and passes, without the second it renders but compares
//! nothing. The test `saves_render_on_outdir` renders the repository's
//! `outdir/` either way.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use game_logic::character_file::{CollisionGroupDesc, SaveData, SavedCharacter, load_save_record};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::data::costume::{Costume, MAX_ACCESSORIES};
use game_sim::data::equip_desc::EquipDesc;
use game_sim::data::string_cache::StringCache;
use neuron_core::vfs::Host;
use neuron_server::save::read_save;

/// The files of the kind, in the oracle's order.
const SAVES: [&str; 2] = ["newplayer.fl", "mpnewcharacter.fl"];

fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut out, b| {
        let _ = write!(out, "{b:02x}");
        out
    })
}

fn wide_hex(units: &[u16]) -> String {
    units.iter().fold(String::new(), |mut out, u| {
        let _ = write!(out, "{u:04x}");
        out
    })
}

fn line(out: &mut Vec<String>, key: &str, value: &str) {
    out.push(format!("{{\"k\":\"{key}\",\"v\":\"{value}\"}}"));
}

fn bits(value: f32) -> String {
    format!("{:08x}", value.to_bits())
}

fn equip(out: &mut Vec<String>, key: &str, list: &[EquipDesc], strings: &StringCache) {
    for e in list {
        let value = format!(
            "{},{:08x},{},{},{},{},{},{}",
            e.id,
            e.arch_id,
            hex(strings.get(e.hardpoint)),
            e.equipped,
            bits(e.health),
            e.count,
            u32::from(e.mission),
            e.owner
        );
        line(out, key, &value);
    }
}

fn groups(out: &mut Vec<String>, key: &str, list: &[CollisionGroupDesc]) {
    for g in list {
        line(out, key, &format!("{},{}", g.id, bits(g.health)));
    }
}

fn costume(out: &mut Vec<String>, key: &str, c: &Costume) {
    let mut text: Vec<String> = c.part_ids.iter().map(|p| format!("{p:08x}")).collect();
    text.extend(
        c.accessory_ids
            .iter()
            .take(MAX_ACCESSORIES)
            .map(|a| format!("{a:08x}")),
    );
    line(out, key, &text.join(","));
}

/// The lines `OracleDumpSaveRecord` writes for a record.
fn render(saw: bool, r: &SavedCharacter, strings: &StringCache) -> Vec<String> {
    let mut out = Vec::new();
    line(&mut out, "saw", &u32::from(saw).to_string());
    line(&mut out, "name", &wide_hex(&r.name));
    line(&mut out, "description", &wide_hex(&r.description));
    line(&mut out, "descrip_strid", &r.descrip_str_id.to_string());
    line(
        &mut out,
        "tstamp_high",
        &format!("{:08x}", r.timestamp_high),
    );
    line(&mut out, "tstamp_low", &format!("{:08x}", r.timestamp_low));
    line(
        &mut out,
        "ship_archetype",
        &format!("{:08x}", r.ship_archetype),
    );
    line(&mut out, "money", &(r.money as u32).to_string());
    line(&mut out, "num_kills", &(r.num_kills as u32).to_string());
    line(
        &mut out,
        "num_misn_successes",
        &(r.num_mission_successes as u32).to_string(),
    );
    line(
        &mut out,
        "num_misn_failures",
        &(r.num_mission_failures as u32).to_string(),
    );
    line(&mut out, "hull_status", &bits(r.hull_status));
    equip(&mut out, "equip", &r.equipment, strings);
    groups(&mut out, "collision_group", &r.collision_groups);
    line(&mut out, "base_hull_status", &bits(r.base_hull_status));
    equip(&mut out, "base_equip", &r.base_equipment, strings);
    groups(&mut out, "base_collision_group", &r.base_collision_groups);
    line(&mut out, "base", &format!("{:08x}", r.base_id));
    line(&mut out, "last_base", &format!("{:08x}", r.last_base_id));
    line(&mut out, "location", &format!("{:08x}", r.location_id));
    line(&mut out, "system", &format!("{:08x}", r.system_id));
    for p in r.position {
        line(&mut out, "pos", &bits(p));
    }
    for row in r.orientation {
        for v in row {
            line(&mut out, "orient", &bits(v));
        }
    }
    line(
        &mut out,
        "starting_ring",
        &format!("{:08x}", r.starting_ring),
    );
    line(&mut out, "rank", &r.rank.to_string());
    for rel in &r.relations {
        line(
            &mut out,
            "relation",
            &format!("{:08x},{}", rel.group_id, bits(rel.feeling)),
        );
    }
    line(&mut out, "rep_group", &format!("{:08x}", r.rep_group));
    costume(&mut out, "com_costume", &r.com_costume);
    line(&mut out, "voice", &hex(&r.voice));
    costume(&mut out, "costume", &r.base_costume);
    line(&mut out, "wg", &hex(&r.weapon_groups));
    for entry in &r.mission_log {
        let flat = entry.flatten().map(|b| hex(&b)).unwrap_or_default();
        line(&mut out, "log", &flat);
    }
    line(&mut out, "interface", &r.interface.to_string());
    for (system, flags) in &r.visits {
        line(&mut out, "visit", &format!("{system:08x},{flags}"));
    }
    out
}

/// Loads the data under a root with `ini` (relative to `exe`) and the
/// `DATA` folder `data`, then renders each save found in `exe`.
fn render_root(exe: &Path, ini: &str, data_dir: &Path) -> Option<Vec<(String, Vec<String>)>> {
    let config = DataConfig::read(&Host::new(exe), ini.as_bytes())?;
    let mut data = GameData::new();
    data.load(&Host::new(data_dir), &config, None);
    let mut out = Vec::new();
    for name in SAVES {
        let Ok(bytes) = std::fs::read(exe.join(name)) else {
            continue;
        };
        let text = read_save(&bytes).text().unwrap_or_default().to_vec();
        let mut record = SavedCharacter::default();
        let mut reports = game_sim::data::Reports::default();
        let save = SaveData {
            universe: &data.universe,
            archetypes: &data.archetypes,
            reputation: &data.reputation,
            costumes: &data.costumes,
        };
        let path = exe.join(name).display().to_string();
        let saw = load_save_record(
            &text,
            &path,
            &mut record,
            save,
            &mut data.strings,
            &mut reports,
        );
        out.push((name.to_owned(), render(saw, &record, &data.strings)));
    }
    Some(out)
}

/// Compares a rendered file with the oracle's; `orient` values (sines and
/// cosines, Tier B) within 4 ulps.
fn compare(name: &str, oracle: &str, rendered: &[String]) -> Vec<String> {
    let mut problems = Vec::new();
    let want: Vec<&str> = oracle.lines().collect();
    if want.len() != rendered.len() {
        problems.push(format!(
            "{name}: {} lines, the oracle has {}",
            rendered.len(),
            want.len()
        ));
    }
    for (i, (w, g)) in want.iter().zip(rendered).enumerate() {
        if *w == g {
            continue;
        }
        let value = |t: &str| {
            t.get(t.len().saturating_sub(10)..t.len().saturating_sub(2))
                .and_then(|h| u32::from_str_radix(h, 16).ok())
        };
        let orient = w.starts_with("{\"k\":\"orient\"") && g.starts_with("{\"k\":\"orient\"");
        let within = orient
            && matches!((value(w), value(g)), (Some(a), Some(b))
                if (a >> 31) == (b >> 31) && a.abs_diff(b) <= 4);
        if !within {
            problems.push(format!("{name} line {}: oracle {w}, port {g}", i + 1));
        }
    }
    problems
}

#[test]
fn data_dump_saves() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_DATA_DIR is not set; the saves data dump was skipped");
        return;
    };
    let exe = root.join("EXE");
    let Some(files) = render_root(&exe, "OutpostFrontier.ini", &root.join("DATA")) else {
        println!("notice: no EXE/OutpostFrontier.ini under the dump root");
        return;
    };
    let Some(oracle) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        println!(
            "notice: OUTPOST_ORACLE_DIR is not set; {} saves rendered, nothing compared",
            files.len()
        );
        return;
    };
    let listed = std::fs::read_to_string(oracle.join("saves/files.txt")).unwrap_or_default();
    let listed: Vec<&str> = listed.lines().collect();
    let names: Vec<&str> = files.iter().map(|(n, _)| n.as_str()).collect();
    assert_eq!(listed, names, "the files the oracle dumped");
    let mut problems = Vec::new();
    for (name, rendered) in &files {
        let text = std::fs::read_to_string(oracle.join("saves").join(format!("{name}.jsonl")))
            .unwrap_or_else(|e| panic!("saves/{name}.jsonl: {e}"));
        problems.extend(compare(name, &text, rendered));
    }
    for p in problems.iter().take(25) {
        println!("{p}");
    }
    assert!(
        problems.is_empty(),
        "{} differences in the saves dump",
        problems.len()
    );
    println!("saves: {} files equal the oracle", files.len());
}

#[test]
fn saves_render_on_outdir() {
    let outdir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let Some(files) = render_root(&outdir, "OutpostFrontier.ini", &outdir.join("data")) else {
        println!("outdir/ is missing: no saves");
        return;
    };
    assert_eq!(files.len(), 2);
    assert!(
        files
            .iter()
            .all(|(_, lines)| lines[0] == "{\"k\":\"saw\",\"v\":\"1\"}")
    );
}
