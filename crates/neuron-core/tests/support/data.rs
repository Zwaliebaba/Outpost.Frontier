//! Data dumps over the original game data (decision O15, Gate G1).
//!
//! `Oracle.exe dump <root> <oracle>` runs with `<root>` holding `DATA` and
//! `EXE` (in CI: a junction to `outdir/data` and a copy of the INI files in
//! `outdir/`). Each work package's dump writes `<oracle>/<kind>/files.txt`
//! and one `<oracle>/<kind>/<file>.jsonl` per file it covers. Its Rust test,
//! named `data_dump_<kind>`, renders the same lines through the port and
//! compares them with a [`Report`]. CI runs every such test with
//! `cargo test --release -p neuron-core --tests -- data_dump_`.
//!
//! Without `OUTPOST_DATA_DIR` a test prints a notice and passes. Without
//! `OUTPOST_ORACLE_DIR` it still renders every file (so a panic or an error
//! shows) but compares nothing. Nothing these tests read or write is
//! committed.

use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

/// The two trees under the dump root, as `Oracle.exe dump` lists them.
pub const TREES: [&str; 2] = ["DATA", "EXE"];

const MAX_REPORTED: usize = 25;

/// The folders a data dump reads.
pub struct DataDirs {
    /// `OUTPOST_DATA_DIR`: the folder that holds `DATA` and `EXE`.
    pub root: PathBuf,
    /// `OUTPOST_ORACLE_DIR`: the output of `Oracle.exe dump`, if set.
    pub oracle: Option<PathBuf>,
}

/// The folders from the environment, or `None` after printing a notice.
pub fn data_dirs(kind: &str) -> Option<DataDirs> {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_DATA_DIR is not set; the {kind} data dump was skipped");
        return None;
    };
    Some(DataDirs {
        root,
        oracle: std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from),
    })
}

fn list_tree(root: &Path, relative: &str, out: &mut Vec<String>) {
    let Ok(entries) = fs::read_dir(root.join(relative)) else {
        return;
    };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        let child = format!("{relative}/{name}");
        match fs::metadata(entry.path()) {
            Ok(meta) if meta.is_dir() => list_tree(root, &child, out),
            Ok(_) => out.push(child),
            Err(_) => {}
        }
    }
}

/// Every file under `DATA` and `EXE`, `/`-separated and sorted by bytes, as
/// the harness's `ListDataFiles` and `std::sort` give them.
pub fn list_files(root: &Path) -> Vec<String> {
    let mut files = Vec::new();
    for tree in TREES {
        list_tree(root, tree, &mut files);
    }
    files.sort();
    files
}

/// The first four bytes of a file, fewer for a shorter one.
pub fn magic(root: &Path, relative: &str) -> Vec<u8> {
    use std::io::Read as _;
    let mut bytes = Vec::new();
    if let Ok(file) = fs::File::open(root.join(relative)) {
        let _ = file.take(4).read_to_end(&mut bytes);
    }
    bytes
}

/// FNV-1a (64-bit) of `bytes` as 16 lower-case hex digits, as the
/// harness's `Fnv64`.
pub fn fnv64(bytes: &[u8]) -> String {
    let hash = bytes.iter().fold(0xCBF2_9CE4_8422_2325_u64, |hash, &b| {
        (hash ^ u64::from(b)).wrapping_mul(0x0000_0100_0000_01B3)
    });
    format!("{hash:016x}")
}

/// The comparison of one kind of data dump with the oracle.
pub struct Report {
    kind: String,
    oracle: Option<PathBuf>,
    files: usize,
    lines: usize,
    compared: usize,
    mismatches: Vec<String>,
    notes: Vec<String>,
}

impl Report {
    /// A report for `kind`, comparing with `<oracle>/<kind>/` when the
    /// oracle folder is known.
    pub fn new(kind: &str, dirs: &DataDirs) -> Self {
        Self {
            kind: kind.to_owned(),
            oracle: dirs.oracle.clone(),
            files: 0,
            lines: 0,
            compared: 0,
            mismatches: Vec::new(),
            notes: Vec::new(),
        }
    }

    /// Compares the files this dump covers with `<oracle>/<kind>/files.txt`.
    pub fn file_list(&mut self, files: &[String]) {
        self.files = files.len();
        let Some(oracle) = &self.oracle else {
            return;
        };
        let listed =
            fs::read_to_string(oracle.join(&self.kind).join("files.txt")).unwrap_or_default();
        let listed: Vec<&str> = listed.lines().collect();
        if listed != files.iter().map(String::as_str).collect::<Vec<_>>() {
            self.mismatches.push(format!(
                "files.txt: the oracle lists {} files, rust finds {}",
                listed.len(),
                files.len()
            ));
        }
    }

    /// Adds a line to the summary.
    pub fn note(&mut self, text: impl Into<String>) {
        self.notes.push(text.into());
    }

    /// Compares the lines rendered for `relative` with the oracle's.
    pub fn check(&mut self, relative: &str, lines: &[String]) {
        self.lines += lines.len();
        let Some(oracle) = &self.oracle else {
            return;
        };
        self.compared += 1;
        let path = oracle.join(&self.kind).join(format!("{relative}.jsonl"));
        let text = match fs::read(&path) {
            Ok(bytes) => String::from_utf8_lossy(&bytes).into_owned(),
            Err(error) => {
                self.mismatches
                    .push(format!("{relative}: no oracle dump ({error})"));
                return;
            }
        };
        let want: Vec<&str> = text.lines().collect();
        for (index, (want, got)) in want.iter().zip(lines).enumerate() {
            if want != got {
                let cut = |s: &str| s.chars().take(200).collect::<String>();
                self.mismatches.push(format!(
                    "{relative} line {}:\n  oracle: {}\n  rust:   {}",
                    index + 1,
                    cut(want),
                    cut(got)
                ));
                return;
            }
        }
        if want.len() != lines.len() {
            self.mismatches.push(format!(
                "{relative}: oracle has {} lines, rust {}",
                want.len(),
                lines.len()
            ));
        }
    }

    /// Compares the lines rendered for `relative` with the oracle's as
    /// [`Report::check`] does, except that the fields `tolerance.keys`
    /// names hold comma-separated `f32` bit patterns (eight hex digits
    /// each) compared within the tolerance (Tier B). Any NaN matches any
    /// NaN.
    pub fn check_within(&mut self, relative: &str, lines: &[String], tolerance: &Tolerance) {
        self.lines += lines.len();
        let Some(oracle) = &self.oracle else {
            return;
        };
        self.compared += 1;
        let path = oracle.join(&self.kind).join(format!("{relative}.jsonl"));
        let text = match fs::read(&path) {
            Ok(bytes) => String::from_utf8_lossy(&bytes).into_owned(),
            Err(error) => {
                self.mismatches
                    .push(format!("{relative}: no oracle dump ({error})"));
                return;
            }
        };
        let want: Vec<&str> = text.lines().collect();
        for (index, (want, got)) in want.iter().zip(lines).enumerate() {
            if let Err(why) = tolerance.compare_line(want, got) {
                let cut = |s: &str| s.chars().take(200).collect::<String>();
                self.mismatches.push(format!(
                    "{relative} line {}: {why}\n  oracle: {}\n  rust:   {}",
                    index + 1,
                    cut(want),
                    cut(got)
                ));
                return;
            }
        }
        if want.len() != lines.len() {
            self.mismatches.push(format!(
                "{relative}: oracle has {} lines, rust {}",
                want.len(),
                lines.len()
            ));
        }
    }

    /// Prints the summary, writes it to `<oracle>/rust-<kind>.txt`, and
    /// fails when any dump differs.
    pub fn finish(self) {
        let mut summary = String::new();
        let _ = writeln!(
            summary,
            "{} data dump: {} files, {} dump lines",
            self.kind, self.files, self.lines
        );
        for note in &self.notes {
            let _ = writeln!(summary, "{note}");
        }
        match &self.oracle {
            Some(_) => {
                let _ = writeln!(
                    summary,
                    "{} dumps compared with the oracle, {} differ",
                    self.compared,
                    self.mismatches.len()
                );
            }
            None => {
                let _ = writeln!(
                    summary,
                    "OUTPOST_ORACLE_DIR is not set: nothing was compared"
                );
            }
        }
        for mismatch in self.mismatches.iter().take(MAX_REPORTED) {
            let _ = writeln!(summary, "{mismatch}");
        }
        print!("{summary}");
        if let Some(oracle) = &self.oracle {
            let _ = fs::write(oracle.join(format!("rust-{}.txt", self.kind)), &summary);
        }
        assert!(
            self.mismatches.is_empty(),
            "{}: {} dumps differ from the oracle",
            self.kind,
            self.mismatches.len()
        );
    }
}

/// The bound of [`Report::check_within`]: a float field matches when it is
/// within `ulps` units in the last place, or within `absolute` of the
/// oracle's value (for results that cancel to near zero).
pub struct Tolerance {
    /// The fields that hold float lists.
    pub keys: &'static [&'static str],
    /// The distance in units in the last place.
    pub ulps: u32,
    /// The absolute distance.
    pub absolute: f32,
}

impl Tolerance {
    /// Whether two dump lines agree: the same fields in the same order,
    /// equal text except in the float fields, which are compared word by
    /// word.
    pub fn compare_line(&self, want: &str, got: &str) -> Result<(), String> {
        if want == got {
            return Ok(());
        }
        let (Some(want_pairs), Some(got_pairs)) = (pairs(want), pairs(got)) else {
            return Err("not a dump line".to_owned());
        };
        if want_pairs.len() != got_pairs.len() {
            return Err("field count".to_owned());
        }
        for ((wk, wv), (gk, gv)) in want_pairs.iter().zip(&got_pairs) {
            if wk != gk {
                return Err(format!("field {wk} / {gk}"));
            }
            if wv == gv {
                continue;
            }
            if !self.keys.contains(&wk.as_str()) {
                return Err(format!("{wk} differs"));
            }
            let w: Vec<&str> = wv.split(',').collect();
            let g: Vec<&str> = gv.split(',').collect();
            if w.len() != g.len() {
                return Err(format!("{wk} length"));
            }
            for (i, (a, b)) in w.iter().zip(&g).enumerate() {
                let (Some(a), Some(b)) = (float_bits(a), float_bits(b)) else {
                    return Err(format!("{wk} word {i} is not a float"));
                };
                if !self.close(a, b) {
                    return Err(format!(
                        "{wk} word {i}: oracle {:08x}, rust {:08x}",
                        a.to_bits(),
                        b.to_bits()
                    ));
                }
            }
        }
        Ok(())
    }

    fn close(&self, a: f32, b: f32) -> bool {
        if a.is_nan() || b.is_nan() {
            return a.is_nan() && b.is_nan();
        }
        if (f64::from(a) - f64::from(b)).abs() <= f64::from(self.absolute) {
            return true;
        }
        let ordered = |v: f32| {
            let magnitude = i64::from(v.to_bits() & 0x7FFF_FFFF);
            if v.is_sign_negative() {
                -magnitude
            } else {
                magnitude
            }
        };
        ordered(a).abs_diff(ordered(b)) <= u64::from(self.ulps)
    }
}

fn float_bits(text: &str) -> Option<f32> {
    if text.len() != 8 {
        return None;
    }
    u32::from_str_radix(text, 16).ok().map(f32::from_bits)
}

/// The `"key":"value"` pairs of a flat dump line, in order.
fn pairs(line: &str) -> Option<Vec<(String, String)>> {
    let inner = line.strip_prefix("{\"")?.strip_suffix("\"}")?;
    inner
        .split("\",\"")
        .map(|pair| {
            let (key, value) = pair.split_once("\":\"")?;
            Some((key.to_owned(), value.to_owned()))
        })
        .collect()
}
