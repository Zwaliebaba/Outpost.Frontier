//! Image replay of the synthetic render traces (record 0004, points 12, 14
//! and 15): each `tests/vectors/render/<name>/trace.jsonl` with a committed
//! `expected.bmp` (the C++ oracle's image, `Oracle.exe render-replay`) is
//! played into the Direct3D 12 backend on WARP, the scene is read back after
//! the frame's `SwapBuffers`, before the gamma blit, and compared with the
//! expected image by the Tier C metric. A failing frame writes its image and
//! a difference image beside the test output.
#![cfg(all(windows, feature = "render-trace"))]

use std::path::{Path, PathBuf};

use neuron_client::platform::d3d12::AdapterChoice;
use neuron_client::platform::d3d12::backend::D3d12Backend;
use neuron_client::render::compare::{STARTING, compare};
use neuron_client::render::ff::defaults::Defaults;
use neuron_client::render::trace::{DirBlobs, ModelReplay};
use neuron_core::image::{RgbImage, bmp, write};

fn traces() -> Vec<PathBuf> {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/vectors/render");
    let Ok(entries) = std::fs::read_dir(&root) else {
        return Vec::new();
    };
    let mut dirs: Vec<PathBuf> = entries
        .filter_map(Result::ok)
        .map(|e| e.path())
        .filter(|p| p.join("trace.jsonl").is_file())
        .collect();
    dirs.sort();
    dirs
}

#[test]
fn synthetic_traces_draw_their_expected_images() {
    let dirs = traces();
    let mut compared = 0;
    let mut failures = Vec::new();
    let out = Path::new(env!("CARGO_TARGET_TMPDIR")).join("render-image-replay");
    for dir in &dirs {
        let name = dir.file_name().unwrap().to_string_lossy().into_owned();
        let Ok(expected) = std::fs::read(dir.join("expected.bmp")) else {
            println!("{name}: no expected.bmp yet; image replay skipped");
            continue;
        };
        let expected = bmp::decode(&expected).unwrap_or_else(|e| panic!("{name}: {e}"));
        let backend = match D3d12Backend::new(AdapterChoice::Warp, expected.width, expected.height)
        {
            Ok(b) => b,
            // CI's Windows runner has WARP: a missing device there is a
            // failure, not a skip.
            Err(e) if std::env::var_os("GITHUB_ACTIONS").is_some() => {
                panic!("no WARP device on CI: {e}")
            }
            Err(e) => {
                println!("no WARP device here ({e}); nothing to test");
                return;
            }
        };
        let text = std::fs::read_to_string(dir.join("trace.jsonl")).unwrap();
        let blobs = DirBlobs(dir.join("blobs"));
        let mut replay = ModelReplay::with_backend(
            Defaults::builtin(),
            expected.width,
            expected.height,
            backend,
            &blobs,
        );
        let frames = replay
            .replay_frames(&text)
            .unwrap_or_else(|e| panic!("{name}: {e}"));
        let backend = &replay.device.backend;
        assert!(
            backend.errors.is_empty(),
            "{name}: {:?}\n{:?}",
            backend.errors,
            backend.debug_messages()
        );
        // The expected image is of the trace's last selected frame.
        let Some((w, h, scene)) = frames.into_iter().rev().find_map(|f| f.scene) else {
            panic!("{name}: no frame image was read back");
        };
        let actual = RgbImage::from_bgra(w, h, &scene);
        let result =
            compare(&expected, &actual, STARTING).unwrap_or_else(|e| panic!("{name}: {e}"));
        compared += 1;
        println!(
            "{name}: {} of {} pixels do not match, mean error {:.3}, largest {}",
            result.mismatched, result.pixels, result.mean_error, result.max_error
        );
        if !result.passed {
            std::fs::create_dir_all(&out).unwrap();
            let save = |suffix: &str, image: &RgbImage| {
                let path = out.join(format!("{name}-{suffix}.bmp"));
                let file = write::bmp(image.width, image.height, &image.to_bgra()).unwrap();
                std::fs::write(&path, file).unwrap();
                path
            };
            save("actual", &actual);
            let diff = save("difference", &result.difference);
            failures.push(format!("{name} (see {})", diff.display()));
        }
    }
    println!("{compared} of {} traces compared", dirs.len());
    assert!(failures.is_empty(), "frames failed: {failures:?}");
}
