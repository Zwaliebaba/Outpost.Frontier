//! The character save writer (work package P3.2): the character templates
//! next to the executable read, applied to a slot and written again through
//! [`save_writer::save_text`], against `Server.dll`'s
//! `PlayerData_SerializeToFile` as `OracleWriteSaveRecord` runs it
//! (`CppSource/Server/server_savegame.cpp`, `Oracle.exe dump`, kind
//! `saves_written`).
//!
//! `OUTPOST_DATA_DIR` names the dump root (`EXE` and `DATA`) and
//! `OUTPOST_ORACLE_DIR` the oracle's output; without the first the test
//! prints a notice and passes. The `tstamp` line is the local time on both
//! sides and is compared by form only. The tests on `outdir/` write the
//! templates and read the text back.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::character_file::{SaveData, SavedCharacter, load_save_record};
use game_logic::player::{self, apply_saved_character};
use game_logic::save_writer::{self, format_mission_log_entry};
use game_logic::server::{Cx, Logic, RecordingSink, VibeIds};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::WorldConfig;
use game_sim::protocol::fmtstr::{FmtStr, FmtValue};
use neuron_core::msvcrt::{text_mode_read, text_mode_write};
use neuron_core::vfs::Host;
use neuron_server::save::read_save;

/// The files of the kind, in the oracle's order. `mpnewcharacter.fl` is not
/// written: its system is the unfilled `%%HOME_SYSTEM%%` placeholder, which
/// the C++ writer dereferences as a missing system (D601).
const SAVES: [&str; 1] = ["newplayer.fl"];

/// The data under a root and the records of the templates found in `exe`,
/// read before the world exists so their strings are in its cache.
fn load(exe: &Path, data_dir: &Path) -> Option<(GameData, Vec<(String, SavedCharacter)>)> {
    let config = DataConfig::read(&Host::new(exe), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(data_dir), &config, None);
    let mut records = Vec::new();
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
        load_save_record(
            &text,
            &path,
            &mut record,
            save,
            &mut data.strings,
            &mut reports,
        );
        records.push((name.to_owned(), record));
    }
    Some((data, records))
}

/// Each record applied to its own slot (sessions 1, 2, ... as client 1)
/// of account `OracleSaves` and written; the file's bytes per record.
fn write_all(data: GameData, records: &[(String, SavedCharacter)]) -> Vec<(String, Vec<u8>)> {
    let mut world = World::new(
        Arc::new(data),
        WorldConfig {
            single_player: true,
            ..WorldConfig::default()
        },
    );
    let mut logic = Logic::new(Box::new(RecordingSink::default()), records.len(), false);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let mut out = Vec::new();
    for (index, (name, record)) in records.iter().enumerate() {
        let session = u32::try_from(index + 1).expect("session");
        cx.logic
            .players
            .get_mut(session)
            .expect("slot")
            .account_name = "OracleSaves".encode_utf16().collect();
        apply_saved_character(&mut cx, session, record, 1);
        let written = save_writer::save_text(&mut cx, session, None, 0, (0, 0)).expect("written");
        out.push((name.clone(), text_mode_write(&written.text)));
    }
    out
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut out, b| {
        let _ = write!(out, "{b:02x}");
        out
    })
}

fn unhex(text: &str) -> Vec<u8> {
    (0..text.len() / 2)
        .filter_map(|i| u8::from_str_radix(&text[2 * i..2 * i + 2], 16).ok())
        .collect()
}

/// The file's lines, each with its `\n`.
fn lines(bytes: &[u8]) -> Vec<Vec<u8>> {
    bytes
        .split_inclusive(|&b| b == b'\n')
        .map(<[u8]>::to_vec)
        .collect()
}

/// `tstamp = <u32>,<u32>\r\n`.
fn is_tstamp(line: &[u8]) -> bool {
    let Some(rest) = line.strip_prefix(b"tstamp = ") else {
        return false;
    };
    let Some(rest) = rest.strip_suffix(b"\r\n") else {
        return false;
    };
    let text = String::from_utf8_lossy(rest);
    let mut parts = text.split(',');
    let ok = |p: Option<&str>| p.is_some_and(|p| p.parse::<u32>().is_ok());
    ok(parts.next()) && ok(parts.next()) && parts.next().is_none()
}

#[test]
fn data_dump_saves_written() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_DATA_DIR is not set; the saves_written data dump was skipped");
        return;
    };
    let exe = root.join("EXE");
    let Some((data, records)) = load(&exe, &root.join("DATA")) else {
        println!("notice: no EXE/OutpostFrontier.ini under the dump root");
        return;
    };
    let files = write_all(data, &records);
    let Some(oracle) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        println!(
            "notice: OUTPOST_ORACLE_DIR is not set; {} saves written, nothing compared",
            files.len()
        );
        return;
    };
    let listed =
        std::fs::read_to_string(oracle.join("saves_written/files.txt")).unwrap_or_default();
    let listed: Vec<&str> = listed.lines().collect();
    let names: Vec<&str> = files.iter().map(|(n, _)| n.as_str()).collect();
    assert_eq!(listed, names, "the files the oracle wrote");
    assert!(
        !files.is_empty(),
        "no character templates in {}",
        exe.display()
    );
    let mut problems = Vec::new();
    for (name, bytes) in &files {
        let text =
            std::fs::read_to_string(oracle.join("saves_written").join(format!("{name}.jsonl")))
                .unwrap_or_else(|e| panic!("saves_written/{name}.jsonl: {e}"));
        let want: Vec<Vec<u8>> = text
            .lines()
            .map(|l| {
                let v = l
                    .strip_prefix("{\"k\":\"line\",\"v\":\"")
                    .and_then(|l| l.strip_suffix("\"}"))
                    .unwrap_or_else(|| panic!("{name}: line {l}"));
                unhex(v)
            })
            .collect();
        let got = lines(bytes);
        if want.len() != got.len() {
            problems.push(format!(
                "{name}: {} lines, the oracle has {}",
                got.len(),
                want.len()
            ));
        }
        for (i, (w, g)) in want.iter().zip(&got).enumerate() {
            if w == g || (is_tstamp(w) && is_tstamp(g)) {
                continue;
            }
            problems.push(format!(
                "{name} line {}: oracle {:?} ({}), port {:?} ({})",
                i + 1,
                String::from_utf8_lossy(w),
                hex(w),
                String::from_utf8_lossy(g),
                hex(g)
            ));
        }
    }
    for p in problems.iter().take(25) {
        println!("{p}");
    }
    assert!(
        problems.is_empty(),
        "{} differences in the saves_written dump",
        problems.len()
    );
    println!("saves_written: {} files equal the oracle", files.len());
}

/// The fields a written save carries back: what the reader of the written
/// text finds equals what the template gave.
#[test]
fn written_templates_read_back_on_outdir() {
    let outdir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let Some((data, records)) = load(&outdir, &outdir.join("data")) else {
        println!("outdir/ is missing: no saves");
        return;
    };
    assert_eq!(records.len(), 1);
    let (data2, _) = load(&outdir, &outdir.join("data")).expect("data");
    let mut data2 = data2;
    let files = write_all(data, &records);
    for ((name, record), (_, bytes)) in records.iter().zip(&files) {
        assert!(bytes.windows(2).all(|w| w[1] != b'\n' || w[0] == b'\r'));
        assert!(bytes.starts_with(b"\r\n[Player]\r\n"), "{name}");
        let text = text_mode_read(bytes);
        let mut back = SavedCharacter::default();
        let mut reports = game_sim::data::Reports::default();
        let save = SaveData {
            universe: &data2.universe,
            archetypes: &data2.archetypes,
            reputation: &data2.reputation,
            costumes: &data2.costumes,
        };
        assert!(load_save_record(
            &text,
            name,
            &mut back,
            save,
            &mut data2.strings,
            &mut reports
        ));
        assert_eq!(back.money, record.money, "{name}");
        assert_eq!(back.ship_archetype, record.ship_archetype, "{name}");
        assert_eq!(back.base_id, record.base_id, "{name}");
        assert_eq!(back.system_id, record.system_id, "{name}");
        assert_eq!(back.rank, record.rank, "{name}");
        // A template with no faction joins the one it likes above 0.9 when
        // its vibe is set (the player auto-affiliation pass), and the save
        // names that one.
        if record.rep_group == u32::MAX {
            let best = record
                .relations
                .iter()
                .filter(|r| r.feeling > 0.9)
                .map(|r| r.group_id)
                .next();
            assert_eq!(Some(back.rep_group), best.or(Some(u32::MAX)), "{name}");
        } else {
            assert_eq!(back.rep_group, record.rep_group, "{name}");
        }
        assert_eq!(back.voice, record.voice, "{name}");
        assert_eq!(
            back.com_costume.part_ids, record.com_costume.part_ids,
            "{name}"
        );
        assert_eq!(
            back.base_costume.part_ids, record.base_costume.part_ids,
            "{name}"
        );
        assert_eq!(back.visits, record.visits, "{name}");
        assert_eq!(back.weapon_groups, record.weapon_groups, "{name}");
        assert_eq!(back.relations.len(), record.relations.len(), "{name}");
        let items = |list: &[game_sim::data::equip_desc::EquipDesc]| {
            list.iter()
                .map(|e| {
                    (
                        e.arch_id,
                        e.count,
                        e.equipped,
                        e.health.to_bits(),
                        e.mission,
                    )
                })
                .collect::<Vec<_>>()
        };
        assert_eq!(items(&back.equipment), items(&record.equipment), "{name}");
    }
}

#[test]
fn a_mission_log_entry_prints_nested_strings_after_a_bare_comma() {
    let mut inner = FmtStr::new(327_682);
    inner.values.push(FmtValue::Value {
        tag: 2,
        flags: 1,
        value: 7,
    });
    let mut entry = FmtStr::new(5);
    entry.values.push(FmtValue::Value {
        tag: 1,
        flags: 0,
        value: 4_000_000_000,
    });
    entry.values.push(FmtValue::Nested {
        flags: 0,
        fmt: inner,
    });
    assert_eq!(
        format_mission_log_entry(&entry),
        "5, 2, 4000000000, 1, 0, 0, 33, 0,327682, 1, 7, 2, 1"
    );
}

#[test]
fn vibe_ids_follow_the_hint_and_wrap() {
    let mut ids = VibeIds::default();
    assert_eq!(ids.alloc(), 1);
    assert_eq!(ids.alloc(), 2);
    ids.free(1);
    // The walk starts after the last id given.
    assert_eq!(ids.alloc(), 3);
    for expected in 4..4096 {
        assert_eq!(ids.alloc(), expected);
    }
    // Wrapped: 0 and the freed 1.
    assert_eq!(ids.alloc(), 0);
    assert_eq!(ids.alloc(), 1);
    assert_eq!(ids.alloc(), -1);
}

#[test]
fn pending_visits_are_committed_with_flag_8() {
    let mut slot = player::PlayerData::default();
    slot.visits.insert(10, 0x01);
    player::queue_visit(&mut slot, 10, 0x10);
    player::queue_visit(&mut slot, 11, 0x00);
    assert!(player::should_queue_visit(&slot, 10, 0x08));
    player::flush_pending_visits(&mut slot);
    assert!(slot.pending_visits.is_empty());
    assert_eq!(slot.visits.get(&10), Some(&0x19));
    assert_eq!(slot.visits.get(&11), Some(&0x08));
    assert!(!player::should_queue_visit(&slot, 10, 0));
}
