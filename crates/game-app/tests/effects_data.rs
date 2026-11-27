//! The effect archetypes of the original data (`outdir/`, decision O15;
//! work package P5.6): every `[Data] effects` file of `OutpostFrontier.ini`
//! in order (`EffectArchLoadIni`).
//!
//! - `the_shipped_effects_read`: the effect types, the two beam defaults,
//!   the visual effects and the effect records the files hold, each
//!   effect's visual and beam parts found, and no report but the sound
//!   lookups the test leaves out.
//!
//! Without the data it prints a notice.

use std::path::Path;

use game_app::effect_arch::{EFFECT_TYPE_COUNT, EffectArchDb, effect_type_index};
use game_sim::data::config::DataConfig;
use neuron_core::vfs::Host;

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
