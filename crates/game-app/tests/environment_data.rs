//! The environment's files on the original data (`outdir/`, decision O15;
//! work package P5.8).
//!
//! `every_asteroid_field_reads`: each `[Asteroids]` `file` of every system
//! file opens and reads; the totals of its sections are printed. Without
//! the data it prints a notice.

use std::path::{Path, PathBuf};

use game_app::environment::asteroid_field::AsteroidFieldFile;
use game_app::star_system::StarSystemFile;
use neuron_core::vfs::Host;

fn data_dir() -> PathBuf {
    std::env::var_os("OUTPOST_DATA_DIR").map_or_else(
        || PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data"),
        PathBuf::from,
    )
}

/// The system files: `UNIVERSE\SYSTEMS\<dir>\<dir>.ini`.
fn system_files(root: &Path) -> Vec<Vec<u8>> {
    let mut out = Vec::new();
    let Ok(dirs) = std::fs::read_dir(root.join("UNIVERSE/SYSTEMS")) else {
        return out;
    };
    for dir in dirs.flatten() {
        let name = dir.file_name().to_string_lossy().into_owned();
        let Ok(files) = std::fs::read_dir(dir.path()) else {
            continue;
        };
        for file in files.flatten() {
            let file_name = file.file_name().to_string_lossy().into_owned();
            if file_name.eq_ignore_ascii_case(&format!("{name}.ini")) {
                out.push(format!("UNIVERSE\\SYSTEMS\\{name}\\{file_name}").into_bytes());
            }
        }
    }
    out.sort();
    out
}

#[test]
fn every_asteroid_field_reads() {
    let root = data_dir();
    let systems = system_files(&root);
    if systems.is_empty() {
        println!("notice: outdir/ has no system files; nothing was read");
        return;
    }
    let source = Host::new(root);
    let (mut fields, mut bands, mut exclusions, mut billboards, mut dynamic) = (0, 0, 0, 0, 0);
    for rel in &systems {
        let system = StarSystemFile::load(&source, rel).expect("the system file");
        for zone_file in &system.asteroids {
            let f = AsteroidFieldFile::load(&source, &zone_file.file);
            assert!(
                f.opened,
                "{}: {}",
                String::from_utf8_lossy(rel),
                String::from_utf8_lossy(&zone_file.file)
            );
            fields += 1;
            bands += usize::from(f.band.is_some());
            exclusions += f.exclusion_bands.len();
            billboards += usize::from(f.billboards.is_some());
            dynamic += f.dynamic.len();
            for b in f.band.iter().chain(&f.exclusion_bands) {
                assert!(b.render_parts >= 1 && b.vert_increase >= 1);
            }
        }
    }
    println!(
        "{} systems: {fields} asteroid fields, {bands} bands, {exclusions} exclusion bands, \
         {billboards} billboard sets, {dynamic} dynamic asteroid sets",
        systems.len()
    );
    assert!(fields > 20 && exclusions > 0 && billboards > 0 && dynamic > 0);
}
