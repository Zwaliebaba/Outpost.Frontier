//! Model replay of the synthetic renderer traces (record 0004, point 15):
//! every `tests/vectors/render/<name>/trace.jsonl`, written by
//! `tests/fixtures/render/make_render_traces.py` in the format the proxy
//! writes, is applied to the platform-free device, and every value the C++
//! handed back must equal the model's. `Oracle.exe render-replay` plays the
//! same traces into DX8RENDERER and writes the `expected.bmp` images the
//! Windows image replay compares with.

#![cfg(feature = "render-trace")]

use std::fs;
use std::path::{Path, PathBuf};

use neuron_client::render::ff::defaults::Defaults;
use neuron_client::render::ff::state::{PipelineProperty, RenderStateId};
use neuron_client::render::trace::json::Json;
use neuron_client::render::trace::{BlobSource, Line, ModelReplay, parse_line};

/// Pipeline slot 6 after `CreateBuffers` is the stage count DX8RENDERER's
/// `ApplyDeviceCaps` stores there, not the table's default; the traces
/// record 7 (eight blend stages).
const STAGE_COUNT_SLOT: i32 = 6;

fn root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/vectors/render")
}

/// The trace folders, in name order.
fn traces() -> Vec<PathBuf> {
    let mut found: Vec<PathBuf> = fs::read_dir(root())
        .expect("tests/vectors/render exists")
        .filter_map(Result::ok)
        .map(|e| e.path())
        .filter(|p| p.join("trace.jsonl").is_file())
        .collect();
    found.sort();
    found
}

fn fnv1a64(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf2_9ce4_8422_2325, |h, &b| {
        (h ^ u64::from(b)).wrapping_mul(0x0000_0100_0000_01b3)
    })
}

/// `blobs/<hash>.bin` beside the trace; each file's content must hash to
/// its name.
struct FolderBlobs(PathBuf);

impl BlobSource for FolderBlobs {
    fn blob(&self, hash: &str) -> Option<Vec<u8>> {
        let bytes = fs::read(self.0.join("blobs").join(format!("{hash}.bin"))).ok()?;
        assert_eq!(
            format!("{:016x}", fnv1a64(&bytes)),
            hash,
            "{}: blob content does not match its name",
            self.0.display()
        );
        Some(bytes)
    }
}

/// The back-buffer size of the trace's `CreateBuffers` (words 2 and 3 of
/// its mode).
fn back_buffer(text: &str) -> (u32, u32) {
    for line in text.lines() {
        if let Ok(Line::Call(call)) = parse_line(line)
            && call.method == "CreateBuffers"
        {
            let mode = call
                .args
                .get("Mode")
                .and_then(Json::as_array)
                .expect("Mode");
            let word = |i: usize| {
                u32::from_str_radix(mode[i].as_str().expect("a hex word"), 16).expect("a hex word")
            };
            return (word(2), word(3));
        }
    }
    panic!("no CreateBuffers line");
}

/// `ModelReplay::replay`, line by line, naming the line that fails.
///
/// Every selected frame's snapshot ends with the stream selection
/// (README.md, "Recorded frames"); right after `CreateBuffers` it is
/// `{"Handle":0}` without a `ret`, which the reader applies as no buffer
/// and does not compare.
fn replay_trace(replay: &mut ModelReplay<'_>, name: &str, text: &str) -> u64 {
    let mut applied = 0;
    for line in text.lines().filter(|l| !l.trim().is_empty()) {
        match parse_line(line).unwrap_or_else(|e| panic!("{name}: {e}")) {
            Line::Call(call) => {
                replay
                    .apply(&call)
                    .unwrap_or_else(|e| panic!("{name}: seq {}: {e}", call.seq));
                applied += 1;
            }
            Line::Defaults(d) => replay
                .apply_defaults(&d)
                .unwrap_or_else(|e| panic!("{name}: {e}")),
            _ => {}
        }
    }
    applied
}

#[test]
fn every_synthetic_trace_replays_on_the_model() {
    let folders = traces();
    assert!(!folders.is_empty(), "no traces under {}", root().display());
    let builtin = Defaults::builtin();
    for folder in &folders {
        let name = folder.file_name().unwrap().to_string_lossy().into_owned();
        let text = fs::read_to_string(folder.join("trace.jsonl")).unwrap();

        // The defaults line is DX8RENDERER's built-in table: the generator
        // reads it from the C++, the model has its own copy.
        let mut defaults_lines = 0;
        let mut images = 0;
        for line in text.lines() {
            match parse_line(line).unwrap_or_else(|e| panic!("{name}: {e}")) {
                Line::Defaults(d) => {
                    defaults_lines += 1;
                    assert_eq!(d.render.len(), RenderStateId::ALL.len(), "{name}");
                    for (number, value) in d.render {
                        let id = RenderStateId::from_raw(number).expect("a state of the table");
                        assert_eq!(value, builtin.render_state(id), "{name}: state {number}");
                    }
                    for (id, value) in d.pipeline {
                        let Some(p) = u32::try_from(id).ok().and_then(PipelineProperty::from_raw)
                        else {
                            assert_eq!(value, 0, "{name}: pipeline slot {id}");
                            continue;
                        };
                        if id != STAGE_COUNT_SLOT {
                            assert_eq!(value as u32, p.builtin_default(), "{name}: slot {id}");
                        }
                    }
                }
                Line::FrameImage { frame, image, .. } => {
                    images += 1;
                    assert_eq!((frame, image), (1, None), "{name}");
                }
                _ => {}
            }
        }
        assert!(defaults_lines >= 1, "{name}: no defaults line");
        assert_eq!(images, 1, "{name}: one selected frame");

        let (width, height) = back_buffer(&text);
        let blobs = FolderBlobs(folder.clone());
        let mut replay = ModelReplay::new(Defaults::builtin(), width, height, &blobs);
        let applied = replay_trace(&mut replay, &name, &text);
        assert!(applied > 0, "{name}");
        assert!(
            replay.mismatches.is_empty(),
            "{name}: {:?}",
            replay.mismatches
        );

        let backend = &replay.device.backend;
        assert_eq!(backend.presents, 2, "{name}: frames 0 and 1 are presented");
        assert!(!backend.clears.is_empty(), "{name}: the frame clears");
        if name != "clear" {
            assert!(!backend.draws.is_empty(), "{name}: the frame draws");
        }

        check_expected_image(folder, &name, width, height);
    }
}

/// When the C++ image is committed it is a 24-bit BMP of the back buffer.
fn check_expected_image(folder: &Path, name: &str, width: u32, height: u32) {
    let Ok(bmp) = fs::read(folder.join("expected.bmp")) else {
        println!("{name}: no expected.bmp yet (Oracle.exe render-replay writes it)");
        return;
    };
    let u16_at = |at: usize| u16::from_le_bytes([bmp[at], bmp[at + 1]]);
    let u32_at = |at: usize| u32::from_le_bytes([bmp[at], bmp[at + 1], bmp[at + 2], bmp[at + 3]]);
    assert!(bmp.len() >= 54 && &bmp[..2] == b"BM", "{name}: not a BMP");
    assert_eq!(u32_at(18), width, "{name}: width");
    assert_eq!(u32_at(22), height, "{name}: height");
    assert_eq!(u16_at(28), 24, "{name}: bits per pixel");
    let row = (width as usize * 3).next_multiple_of(4);
    assert_eq!(bmp.len(), 54 + row * height as usize, "{name}: size");
}
