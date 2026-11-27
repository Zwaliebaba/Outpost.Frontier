//! The client's space files on the original data (`outdir/`, decision
//! O15; work package P5.4a).
//!
//! - `every_system_file_reads`: each file opens; its `[Object]`,
//!   `[LightSource]`, `[Asteroids]` and `[Dust]` sections are counted as
//!   the file's own header lines count them; no `[Archetype]` key is
//!   unknown; and each `[Object]` position reads that section's first
//!   value. Without the data it prints a notice.
//! - `cameras_ini_sets_every_camera`: `SpaceAppLoadCamerasIni` on
//!   `cameras.ini` sets the cameras it names. The file's `[ChaseCamera]`
//!   is no section the C++ reads (it reads `[ThirdPersonCamera]`), so the
//!   one warning is the reference's too.

use std::path::{Path, PathBuf};

use game_app::space_camera::{SpaceCameraMode, SpaceCameras};
use game_app::star_system::{Preload, StarSystemFile};
use neuron_client::ui::camera::Camera;
use neuron_client::ui::widescreen::Surface;
use neuron_core::ini::IniReader;
use neuron_core::vfs::Host;

fn data_dir() -> PathBuf {
    std::env::var_os("OUTPOST_DATA_DIR").map_or_else(
        || PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data"),
        PathBuf::from,
    )
}

/// The system files: `SYSTEMS\<dir>\<dir>.ini`, matched without case.
fn system_files(root: &Path) -> Vec<(Vec<u8>, PathBuf)> {
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
                let rel = format!("UNIVERSE\\SYSTEMS\\{name}\\{file_name}");
                out.push((rel.into_bytes(), file.path()));
            }
        }
    }
    out.sort();
    out
}

/// Header lines `[name]` in the text, without regard to case.
fn headers(text: &[u8], name: &str) -> usize {
    let want = format!("[{name}]");
    text.split(|&b| b == b'\n')
        .map(|line| String::from_utf8_lossy(line).trim().to_ascii_lowercase())
        .filter(|line| line.starts_with(&want.to_ascii_lowercase()))
        .count()
}

#[test]
fn every_system_file_reads() {
    let root = data_dir();
    let files = system_files(&root);
    if files.is_empty() {
        println!("notice: outdir/ has no system files; nothing was read");
        return;
    }
    let source = Host::new(root);
    for (rel, full) in &files {
        let name = String::from_utf8_lossy(rel);
        let text = std::fs::read(full).expect("reads");
        let file = StarSystemFile::load(&source, rel).unwrap_or_else(|| panic!("{name} opens"));
        assert_eq!(file.objects.len(), headers(&text, "Object"), "{name}");
        assert_eq!(
            file.light_sources.len(),
            headers(&text, "LightSource"),
            "{name}"
        );
        assert_eq!(file.asteroids.len(), headers(&text, "Asteroids"), "{name}");
        assert_eq!(file.dust.len(), headers(&text, "Dust"), "{name}");
        assert!(
            !file
                .preloads
                .iter()
                .any(|p| matches!(p, Preload::Unknown(_))),
            "{name}: {:?}",
            file.reports
        );
        let mut reader = IniReader::new();
        assert!(reader.open(rel, file.contents.clone()));
        for &at in &file.objects {
            reader.seek(at);
            assert!(reader.read_value(), "{name}: an [Object] has a value");
            assert!(
                reader.is_value(b"nickname"),
                "{name}: {:?}",
                String::from_utf8_lossy(&reader.line())
            );
        }
    }
    println!("{} system files read", files.len());
}

#[test]
fn cameras_ini_sets_every_camera() {
    let Ok(text) = std::fs::read(data_dir().join("cameras.ini")) else {
        println!("notice: outdir/ has no cameras.ini; nothing was read");
        return;
    };
    let mut cams = SpaceCameras::new(Surface::default());
    let mut win = Camera::new(Surface::default());
    let reports = cams.load_ini(Some(&mut win), b"cameras.ini", text);
    assert_eq!(
        reports,
        ["*** WARNING: Unknown camera type? (ChaseCamera)"],
        "the C++ warns only about [ChaseCamera]"
    );
    let cockpit = cams.camera(SpaceCameraMode::FirstPerson);
    assert_eq!(cockpit.camera.z_near, 2.0);
    assert_eq!(cockpit.base_half_fov_x, 35.0);
    assert_eq!(win.half_fov_x_deg, (54.431_999_2f64 * 0.5) as f32);
}
