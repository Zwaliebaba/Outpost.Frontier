//! The environment's files on the original data (`outdir/`, decision O15;
//! work package P5.8).
//!
//! `every_asteroid_field_reads`: each `[Asteroids]` `file` of every system
//! file opens and reads; the totals of its sections are printed. Without
//! the data it prints a notice.

use std::path::{Path, PathBuf};

use game_app::environment::asteroid_field::AsteroidFieldFile;
use game_app::environment::nebula::NebulaFile;
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

fn uint_field(line: &str, key: &str) -> Option<u32> {
    let start = line.find(&format!("\"{key}\":"))? + key.len() + 3;
    line[start..]
        .split([',', '}'])
        .next()
        .and_then(|v| v.parse().ok())
}

/// `asteroid_field_records_equal_the_cpp_client`
/// (`OUTPOST_CLIENT_ORACLE_DIR`): each `asteroidfield` record of the C++
/// run's `layout.jsonl` equals the port's reading of that path at that
/// frame. Without the variable it prints a notice.
#[test]
fn asteroid_field_records_equal_the_cpp_client() {
    use game_app::oracle::environment::{asteroid_field_line, nebula_line};
    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let source = Host::new(data_dir());
    let text = std::fs::read_to_string(dir.join("layout.jsonl")).expect("layout.jsonl");
    let mut count = 0;
    for cpp in text
        .lines()
        .filter(|l| l.contains("\"rec\":\"asteroidfield\""))
    {
        let frame = uint_field(cpp, "frame").expect("a frame");
        let start = cpp.find("\"path\":\"").expect("a path") + 8;
        let end = start + cpp[start..].find('"').expect("the path's end");
        // The paths' escapes are `\\` only (ASCII).
        let path = cpp[start..end].replace("\\\\", "\\");
        let file = AsteroidFieldFile::load(&source, path.as_bytes());
        assert_eq!(
            asteroid_field_line(frame, path.as_bytes(), &file),
            cpp,
            "the field {path}"
        );
        count += 1;
    }
    println!("layout.jsonl: {count} asteroid field records equal");
    let mut nebulae = 0;
    for cpp in text.lines().filter(|l| l.contains("\"rec\":\"nebula\"")) {
        let frame = uint_field(cpp, "frame").expect("a frame");
        let start = cpp.find("\"path\":\"").expect("a path") + 8;
        let end = start + cpp[start..].find('"').expect("the path's end");
        let path = cpp[start..end].replace("\\\\", "\\");
        let file = NebulaFile::load(&source, path.as_bytes());
        assert_eq!(
            nebula_line(frame, path.as_bytes(), &file),
            cpp,
            "the nebula {path}"
        );
        nebulae += 1;
    }
    println!("layout.jsonl: {nebulae} nebula records equal");
}

/// Whether a `[section]` of the text (any case) has a `key =` line.
fn has_key(text: &[u8], section: &str, key: &str) -> bool {
    let mut current = String::new();
    for line in String::from_utf8_lossy(text).lines() {
        let line = line
            .split(';')
            .next()
            .unwrap_or("")
            .trim()
            .to_ascii_lowercase();
        if let Some(name) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            current = name.to_owned();
        } else if current == section && line.split('=').next().is_some_and(|k| k.trim() == key) {
            return true;
        }
    }
    false
}

/// `every_nebula_reads`: each `[Nebula]` `file` of every system file opens
/// and sets the values the nebula's constructor leaves unset (D1700).
#[test]
fn every_nebula_reads() {
    let root = data_dir();
    let systems = system_files(&root);
    if systems.is_empty() {
        println!("notice: outdir/ has no system files; nothing was read");
        return;
    }
    let source = Host::new(root);
    let (mut nebulae, mut clouds, mut exclusions) = (0, 0, 0);
    for rel in &systems {
        let system = StarSystemFile::load(&source, rel).expect("the system file");
        for zone_file in &system.nebulae {
            let n = NebulaFile::load(&source, &zone_file.file);
            let name = String::from_utf8_lossy(&zone_file.file);
            assert!(n.opened, "{name}");
            let text = std::fs::read(
                source
                    .locate(
                        &zone_file
                            .file
                            .iter()
                            .map(|&b| if b == b'\\' { b'/' } else { b })
                            .collect::<Vec<u8>>(),
                    )
                    .ok()
                    .flatten()
                    .expect("located")
                    .path,
            )
            .expect("reads");
            for (section, key) in [
                ("exterior", "bit_radius"),
                ("exterior", "color"),
                ("fog", "color"),
            ] {
                assert!(has_key(&text, section, key), "{name}: [{section}] {key}");
            }
            nebulae += 1;
            clouds += n.clouds.len();
            exclusions += n.exclusions.len();
        }
    }
    println!("{nebulae} nebulae: {clouds} cloud sets, {exclusions} exclusion zones");
    assert!(nebulae > 20 && clouds > 0);
}

/// A texture library that has every texture: the shapes load whatever the
/// archives hold.
struct AnyTextures;

impl neuron_client::ui::shape::ShapeTextures for AnyTextures {
    fn find(&mut self, _: &[u8]) -> Option<u32> {
        Some(1)
    }
    fn add_ref(&mut self, _: u32) {}
    fn release(&mut self, _: u32) {}
    fn load_group(&mut self, _: &[u8]) -> Option<i32> {
        Some(1)
    }
    fn garbage_collect(&mut self, _: i32) {}
    fn generic_slot_4(&mut self) -> u32 {
        0
    }
    fn set_generic_slot_4(&mut self, _: u32) {}
    fn frame_handle(&mut self, _: u32, _: i32) -> u32 {
        1
    }
    fn texture_dim(&mut self, _: u32) -> (u32, u32) {
        (256, 256)
    }
}

/// A shape database of the given panel files (each through `FShapeDB::Load`).
fn shapes(source: &Host, panels: &[Vec<u8>]) -> neuron_client::ui::shape::ShapeDb {
    let mut db = neuron_client::ui::shape::ShapeDb::new();
    for panel in panels {
        let rel: Vec<u8> = panel
            .iter()
            .map(|&b| if b == b'\\' { b'/' } else { b })
            .collect();
        let found = source.locate(&rel).ok().flatten().expect("a panel file");
        db.load(&std::fs::read(found.path).expect("reads"), &mut AnyTextures);
    }
    db
}

/// `every_named_shape_is_in_its_panels`: each shape a field's band,
/// exclusion bands and billboards name, and each shape a nebula's exterior
/// and clouds name, is in the shape database its own texture panels make
/// (the reference warns and stores no shape otherwise). The misses are
/// listed; there are none.
#[test]
fn every_named_shape_is_in_its_panels() {
    let root = data_dir();
    let systems = system_files(&root);
    if systems.is_empty() {
        println!("notice: outdir/ has no system files; nothing was read");
        return;
    }
    let source = Host::new(root);
    let mut misses = Vec::new();
    let mut named = 0;
    let mut check = |db: &neuron_client::ui::shape::ShapeDb, owner: &[u8], name: &[u8]| {
        named += 1;
        if db.find(name).is_none() {
            misses.push(format!(
                "{}: {}",
                String::from_utf8_lossy(owner),
                String::from_utf8_lossy(name)
            ));
        }
    };
    for rel in &systems {
        let system = StarSystemFile::load(&source, rel).expect("the system file");
        for z in &system.asteroids {
            let f = AsteroidFieldFile::load(&source, &z.file);
            let db = shapes(&source, &f.texture_panels);
            let bands = f.band.iter().chain(&f.exclusion_bands);
            for name in bands
                .filter_map(|b| b.shape.as_deref())
                .chain(f.billboards.iter().filter_map(|b| b.shape.as_deref()))
            {
                check(&db, &z.file, name);
            }
        }
        for z in &system.nebulae {
            let n = NebulaFile::load(&source, &z.file);
            let db = shapes(&source, &n.texture_panels);
            let exterior = n.exterior.shapes.iter().chain(&n.exterior.fill_shape);
            for name in exterior.chain(n.clouds.iter().flat_map(|c| &c.puff_shapes)) {
                check(&db, &z.file, name);
            }
        }
    }
    println!("{named} shapes named; misses: {misses:?}");
    assert!(misses.is_empty(), "{misses:?}");
}
