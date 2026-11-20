//! Every room of the original data (`outdir/`) through
//! `game_sim::data::room::read_room_file`: the set scripts load, and the
//! bars have their character placements.

use std::path::Path;

use game_sim::data::Reports;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::data::room::read_room_file;
use neuron_core::vfs::Host;

#[test]
fn rooms_read_their_placements() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let Some(config) = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no rooms");
        return;
    };
    let data_host = Host::new(root.join("data"));
    let mut data = GameData::new();
    data.load(&data_host, &config, None);
    let mut rooms = 0;
    let mut with_placements = 0;
    let mut placements = 0;
    let mut reports = Reports::default();
    for base in &data.bases.bases {
        for room in base.rooms.iter().flatten() {
            rooms += 1;
            let file = read_room_file(&data_host, &room.file, &mut reports);
            if !file.placements.is_empty() {
                with_placements += 1;
            }
            placements += file.placements.len();
            for p in &file.placements {
                assert!((0..=3).contains(&p.posture), "{p:?}");
            }
        }
    }
    println!(
        "{rooms} rooms, {with_placements} with placements, {placements} placements, {} reports",
        reports.0.len()
    );
    assert!(rooms > 100);
    assert!(with_placements > 20, "{with_placements}");
}
