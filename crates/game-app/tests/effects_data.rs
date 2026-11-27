//! The effect archetypes of the original data (`outdir/`, decision O15;
//! work package P5.6): every `[Data] effects` file of `OutpostFrontier.ini`
//! in order (`EffectArchLoadIni`).
//!
//! - `the_shipped_effects_read`: the effect types, the two beam defaults,
//!   the visual effects and the effect records the files hold, each
//!   effect's visual and beam parts found, and no report but the sound
//!   lookups the test leaves out.
//!
//! - `data_dump_effect_arch`: the same files from the dump root
//!   (`OUTPOST_DATA_DIR`, `DATA` and `EXE`) against `Oracle.exe dump`'s
//!   `effect_arch/effects.jsonl` (`OUTPOST_ORACLE_DIR`), line for line, the
//!   lines of `oracle_effect_arch.cpp` (`support/effect_tables.rs`) as case
//!   `data`, then each effect's `snd_effect` name. Both sides find no
//!   sound, so the names are compared as read. Nothing it reads or writes
//!   is committed.
//!
//! Without the data they print a notice.

use std::fs;
use std::path::{Path, PathBuf};

use game_app::effect_arch::{EFFECT_TYPE_COUNT, EffectArchDb, effect_type_index};
use game_sim::data::config::DataConfig;
use neuron_core::vfs::Host;

#[path = "support/effect_tables.rs"]
mod effect_tables;

#[test]
fn the_shipped_effects_read() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let Some(config) = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini") else {
        println!("notice: outdir/ has no data; nothing was read");
        return;
    };
    let db = EffectArchDb::from_config(&Host::new(root.join("data")), &config, &|_| true);
    println!(
        "{} effects, {} visual effects, {} beams, {} reports",
        db.effects.len(),
        db.vis_effects.len(),
        db.beams.len(),
        db.reports.len()
    );
    for r in db.reports.iter().take(10) {
        println!("  {r}");
    }
    // effects.ini and effects_explosion.ini: 606 and 99 sections, every
    // nickname unique.
    assert_eq!(db.effects.len(), 705);
    assert!(db.vis_effects.len() > 1000);
    let spear = db.beam(b"default_beam_spear").expect("the spear");
    assert_eq!(spear.segment_mode, 0);
    assert_eq!(
        db.beam(b"default_beam_bolt")
            .expect("the bolt")
            .segment_mode,
        1
    );
    // effect_types.ini overlays the exe's table.
    let small = &db.types[effect_type_index(b"EFT_EXPLOSION_SMALL")];
    assert_eq!(
        (small.radius, small.pbubble_near, small.pbubble_far),
        (20.0, 500.0, 1000.0)
    );
    assert!(
        db.lods[3].min_screen_size >= 0.0,
        "[EffectLOD] lod_type_small read"
    );
    // Every record's type is known, and its parts were found.
    assert!(
        db.effects
            .values()
            .all(|e| e.effect_type < EFFECT_TYPE_COUNT)
    );
    let with_vis = db
        .effects
        .values()
        .filter(|e| e.vis_effect.is_some())
        .count();
    println!("{with_vis} effects with a visual effect");
    assert_eq!(with_vis, 660);
    assert!(
        db.reports.is_empty(),
        "the shipped effects read without a report: {:?}",
        &db.reports[..db.reports.len().min(5)]
    );
}

#[test]
fn data_dump_effect_arch() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_DATA_DIR is not set; the effect_arch data dump was skipped");
        return;
    };
    let oracle = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from);
    // `RunEffectArchDataDump`: the [Data] effects values in order, each
    // under DATA.
    let Some(config) = DataConfig::read(&Host::new(root.join("EXE")), b"OutpostFrontier.ini")
    else {
        // A comparison that judges nothing fails (the conductor's rule,
        // after #209).
        assert!(
            oracle.is_none(),
            "no EXE/OutpostFrontier.ini under OUTPOST_DATA_DIR: the effect_arch dump judges nothing"
        );
        println!("notice: no EXE/OutpostFrontier.ini; the effect_arch data dump was skipped");
        return;
    };
    let mut lines = Vec::new();
    let mut db = EffectArchDb::default();
    let source = Host::new(root.join("DATA"));
    for entry in &config.entries {
        if entry.key.eq_ignore_ascii_case(b"effects") {
            let grown = db.load_ini(&source, &entry.value, &|_| false);
            lines.push(format!(
                "{{\"case\":\"data\",\"ini\":\"{}\",\"r\":\"{grown}\"}}",
                effect_tables::hex(&entry.value)
            ));
        }
    }
    effect_tables::render("data", &db, &mut lines);
    // The sound names, which neither side resolves.
    for (id, e) in &db.effects {
        lines.push(format!(
            "{{\"case\":\"data\",\"sound\":\"{id:08x}\",\"v\":\"{}\"}}",
            e.snd_effect_name
                .as_deref()
                .map_or_else(|| "-".to_owned(), effect_tables::hex)
        ));
    }
    println!(
        "effect_arch: {} effects, {} visual effects, {} beams, {} lines",
        db.effects.len(),
        db.vis_effects.len(),
        db.beams.len(),
        lines.len()
    );
    assert!(
        !db.effects.is_empty(),
        "the [Data] effects files hold no effect"
    );
    let Some(oracle) = oracle else {
        println!("notice: OUTPOST_ORACLE_DIR is not set; the effect_arch dump was not compared");
        return;
    };
    let path = oracle.join("effect_arch").join("effects.jsonl");
    let text = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let expected: Vec<&str> = text.lines().collect();
    let differing: Vec<usize> = (0..expected.len().max(lines.len()))
        .filter(|&i| expected.get(i).copied() != lines.get(i).map(String::as_str))
        .collect();
    for &i in differing.iter().take(25) {
        println!("line {}:", i + 1);
        println!("  oracle: {}", expected.get(i).copied().unwrap_or("(none)"));
        println!(
            "  port:   {}",
            lines.get(i).map_or("(none)", String::as_str)
        );
    }
    assert!(
        differing.is_empty(),
        "effect_arch: {} of {} lines differ from the oracle ({} oracle lines)",
        differing.len(),
        lines.len(),
        expected.len()
    );
}
