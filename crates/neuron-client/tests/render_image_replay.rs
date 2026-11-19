//! Image replay of the synthetic render traces (record 0004, points 12, 14
//! and 15): each `tests/vectors/render/<name>/trace.jsonl` with a committed
//! `expected.bmp` (the C++ oracle's image, `Oracle.exe render-replay`) is
//! played into the Direct3D 12 backend on WARP, the scene is read back after
//! the frame's `SwapBuffers`, before the gamma blit, and compared with the
//! expected image by the Tier C metric. A failing frame writes its image and
//! a difference image beside the test output.
//!
//! Gate G4's recorded traces (`CppSource/RenderTrace`, on the original data)
//! are game data and stay out of the repository: `OUTPOST_RENDER_TRACES`
//! names a folder whose subfolders each hold one `trace.jsonl` with its
//! `blobs/`, and every frame image in them is compared with the frame the
//! Direct3D 12 backend draws (WARP, or the first hardware adapter with
//! `OUTPOST_RENDER_ADAPTER=hardware`). Results and failing images go to
//! `<trace>/replay/`. Without the variable the test prints a notice.
#![cfg(all(windows, feature = "render-trace"))]

use std::path::{Path, PathBuf};

use neuron_client::platform::d3d12::AdapterChoice;
use neuron_client::platform::d3d12::backend::D3d12Backend;
use neuron_client::render::compare::{STARTING, compare};
use neuron_client::render::ff::defaults::Defaults;
use neuron_client::render::trace::{BlobSource, DirBlobs, Line, ModelReplay, parse_line};
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
            let actual_path = save("actual", &actual);
            let diff = save("difference", &result.difference);
            // The image in the log too: CI keeps no test files.
            let file = std::fs::read(&actual_path).unwrap();
            println!("=== actual.bmp {name} base64 begin ===");
            println!("{}", base64(&file));
            println!("=== actual.bmp {name} base64 end ===");
            failures.push(format!("{name} (see {})", diff.display()));
        }
    }
    println!("{compared} of {} traces compared", dirs.len());
    assert!(failures.is_empty(), "frames failed: {failures:?}");
}

/// The first frame image's size in a trace text.
fn first_frame_size(text: &str) -> Option<(u32, u32)> {
    text.lines().find_map(|line| match parse_line(line) {
        Ok(Line::FrameImage {
            image: Some(_),
            width,
            height,
            ..
        }) => Some((u32::try_from(width).ok()?, u32::try_from(height).ok()?)),
        _ => None,
    })
}

fn save_bmp(path: &Path, image: &RgbImage) {
    let file = write::bmp(image.width, image.height, &image.to_bgra()).unwrap();
    std::fs::write(path, file).unwrap();
}

#[test]
fn recorded_traces_match_their_frame_images() {
    let Some(root) = std::env::var_os("OUTPOST_RENDER_TRACES").map(PathBuf::from) else {
        println!("OUTPOST_RENDER_TRACES is not set; no recorded traces to replay");
        return;
    };
    let adapter = match std::env::var("OUTPOST_RENDER_ADAPTER").as_deref() {
        Ok("hardware") => AdapterChoice::Hardware,
        _ => AdapterChoice::Warp,
    };
    let mut dirs: Vec<PathBuf> = std::fs::read_dir(&root)
        .unwrap_or_else(|e| panic!("{}: {e}", root.display()))
        .filter_map(Result::ok)
        .map(|e| e.path())
        .filter(|p| p.join("trace.jsonl").is_file())
        .collect();
    dirs.sort();
    assert!(
        !dirs.is_empty(),
        "no */trace.jsonl under {}",
        root.display()
    );
    let mut failures = Vec::new();
    let mut compared = 0;
    for dir in &dirs {
        let name = dir.file_name().unwrap().to_string_lossy().into_owned();
        let text = std::fs::read_to_string(dir.join("trace.jsonl")).unwrap();
        let Some((width, height)) = first_frame_size(&text) else {
            println!("{name}: no frame image in the trace; nothing to compare");
            continue;
        };
        let backend = D3d12Backend::new(adapter, width, height)
            .unwrap_or_else(|e| panic!("{name}: no {adapter:?} device: {e}"));
        let blobs = DirBlobs(dir.join("blobs"));
        let mut replay =
            ModelReplay::with_backend(Defaults::builtin(), width, height, backend, &blobs);
        let frames = replay
            .replay_frames(&text)
            .unwrap_or_else(|e| panic!("{name}: {e}"));
        let out = dir.join("replay");
        std::fs::create_dir_all(&out).unwrap();
        let mut report = format!(
            "{name}: {} getter mismatches, backend errors {:?}\n",
            replay.mismatches.len(),
            replay.device.backend.errors
        );
        report.push_str(&format!(
            "  answered from the trace {:?}\n",
            replay.answered
        ));
        for mismatch in replay.mismatches.iter().take(20) {
            report.push_str(&format!("  mismatch {mismatch:?}\n"));
        }
        for f in frames {
            let (Some(hash), Some((w, h, scene))) = (f.reference, f.scene) else {
                report.push_str(&format!("frame {}: no image on one side\n", f.frame));
                continue;
            };
            let Some(bytes) = blobs.blob(&hash) else {
                report.push_str(&format!("frame {}: blob {hash} missing\n", f.frame));
                continue;
            };
            let expected = bmp::decode(&bytes).unwrap_or_else(|e| panic!("{name}: {e}"));
            let actual = RgbImage::from_bgra(w, h, &scene);
            let result = compare(&expected, &actual, STARTING)
                .unwrap_or_else(|e| panic!("{name} frame {}: {e}", f.frame));
            compared += 1;
            report.push_str(&format!(
                "frame {}: {} of {} pixels do not match, mean error {:.3}, largest {}, {}\n",
                f.frame,
                result.mismatched,
                result.pixels,
                result.mean_error,
                result.max_error,
                if result.passed { "passed" } else { "FAILED" }
            ));
            if !result.passed {
                save_bmp(&out.join(format!("frame-{}-actual.bmp", f.frame)), &actual);
                save_bmp(
                    &out.join(format!("frame-{}-difference.bmp", f.frame)),
                    &result.difference,
                );
                failures.push(format!("{name} frame {}", f.frame));
            }
        }
        std::fs::write(out.join("report.txt"), &report).unwrap();
        print!("{report}");
    }
    println!("{compared} recorded frames compared");
    assert!(failures.is_empty(), "frames failed: {failures:?}");
}

/// Standard base64 with padding.
fn base64(bytes: &[u8]) -> String {
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let n = chunk
            .iter()
            .enumerate()
            .fold(0u32, |n, (i, &b)| n | u32::from(b) << (16 - 8 * i));
        for i in 0..4 {
            if i <= chunk.len() {
                out.push(ALPHABET[(n >> (18 - 6 * i) & 63) as usize] as char);
            } else {
                out.push('=');
            }
        }
    }
    out
}
