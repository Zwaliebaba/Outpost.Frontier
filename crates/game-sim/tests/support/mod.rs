//! Helpers shared by the oracle tests of game-sim. The C++ harness
//! (`CppSource/Oracle/oracle_game_*.cpp`) writes the same lines.
//!
//! The vector helpers are copied from `crates/neuron-core/tests/support/`;
//! test modules cannot be shared between crates.
//!
//! Each test target uses part of this module.
#![allow(dead_code)]

pub mod data;
pub mod game;
pub mod render;
pub mod render_universe;

use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

/// The synthetic oracle inputs, `tests/fixtures/oracle/`.
pub fn fixtures() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/oracle")
}

/// The bytes of a file under [`fixtures`].
pub fn read_fixture(relative: &str) -> Vec<u8> {
    let path = fixtures().join(relative);
    fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

/// The non-comment lines of an input list. Empty lines are inputs too.
pub fn input_lines(relative: &str) -> Vec<String> {
    let text = String::from_utf8(read_fixture(relative)).unwrap_or_default();
    text.lines()
        .filter(|l| !l.starts_with('#'))
        .map(str::to_owned)
        .collect()
}

/// Compares `lines` with the committed suite `tests/vectors/<name>`, or
/// writes it when `OUTPOST_BLESS_VECTORS` is set.
pub fn check_suite(name: &str, lines: &[String]) {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(name);
    let rendered: String = lines.iter().map(|l| format!("{l}\n")).collect();
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        fs::write(&path, rendered).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
        return;
    }
    let committed = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let committed: Vec<&str> = committed.lines().collect();
    for (index, (want, got)) in committed.iter().zip(lines).enumerate() {
        assert_eq!(*want, got, "{name} line {}", index + 1);
    }
    assert_eq!(committed.len(), lines.len(), "{name}: line count");
}

/// Lower-case hex of `bytes`.
pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut out, b| {
        let _ = write!(out, "{b:02x}");
        out
    })
}

/// The bits of an `f32`, eight hex digits.
pub fn f32_text(value: f32) -> String {
    format!("{:08x}", value.to_bits())
}

/// The bits of an `f64`, sixteen hex digits.
pub fn f64_text(value: f64) -> String {
    format!("{:016x}", value.to_bits())
}

/// An `f32` from its eight hex digits.
pub fn f32_hex(text: &str) -> f32 {
    f32::from_bits(u32::from_str_radix(text, 16).unwrap_or_else(|e| panic!("{text}: {e}")))
}

/// The bits of an `i32`.
pub fn bits32(value: i32) -> u32 {
    u32::from_ne_bytes(value.to_ne_bytes())
}
