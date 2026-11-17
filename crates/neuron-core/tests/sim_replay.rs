//! Replay of the scenario oracle's per-PSI free-flight records (record
//! 0003 points 5 and 7; layout in the `psi.jsonl` section of
//! `design/sim-records.md`).
//!
//! `Oracle.exe sim` writes one `psi.jsonl` per run into its output folder
//! (with `--runs N`, one per `run_NN` folder). With `OUTPOST_ORACLE_DIR`
//! set to a folder above them, every record's PSI is run from its
//! recorded input state and compared with its recorded output, Tier B:
//! within `TOLERANCE` relative to the larger magnitude (at least
//! absolute), integers and flags exactly. Without the variable the test
//! prints a notice and passes. A summary is written to
//! `rust-sim-replay.txt` in that folder.

use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

use neuron_core::physics::replay::{PsiRecord, compare, replay};

/// The bound: a few float ulps around 1, from the trigonometry the port
/// takes from the Rust standard library (decision T11).
const TOLERANCE: f32 = 4.0e-6;
const MAX_REPORTED: usize = 40;

fn record_files(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            record_files(&path, out);
        } else if path.file_name().is_some_and(|n| n == "psi.jsonl") {
            out.push(path);
        }
    }
}

#[test]
fn free_flight_replay_matches_the_oracle() {
    let Some(oracle) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        println!("OUTPOST_ORACLE_DIR is not set: no free-flight records were replayed");
        return;
    };
    let mut files = Vec::new();
    record_files(&oracle, &mut files);
    files.sort();
    let mut summary = String::new();
    let (mut records, mut mismatched) = (0usize, 0usize);
    let mut reports = Vec::new();
    for file in &files {
        let text = fs::read_to_string(file).unwrap_or_else(|e| panic!("{}: {e}", file.display()));
        for (line_no, line) in text
            .lines()
            .enumerate()
            .filter(|(_, l)| !l.trim().is_empty())
        {
            let record = PsiRecord::parse(line)
                .unwrap_or_else(|e| panic!("{}:{}: {e}", file.display(), line_no + 1));
            records += 1;
            let diffs = compare(&record.output, &replay(&record), TOLERANCE);
            if !diffs.is_empty() {
                mismatched += 1;
                reports.push(format!(
                    "{}:{} tick {} psi {} {} (id {}): {}",
                    file.display(),
                    line_no + 1,
                    record.tick,
                    record.psi,
                    record.obj,
                    record.id,
                    diffs.join("; ")
                ));
            }
        }
    }
    let _ = writeln!(
        summary,
        "{} files, {records} records, {mismatched} outside the bound {TOLERANCE:e}",
        files.len()
    );
    for report in reports.iter().take(MAX_REPORTED) {
        let _ = writeln!(summary, "{report}");
    }
    print!("{summary}");
    let _ = fs::write(oracle.join("rust-sim-replay.txt"), &summary);
    assert_eq!(mismatched, 0, "{summary}");
}
