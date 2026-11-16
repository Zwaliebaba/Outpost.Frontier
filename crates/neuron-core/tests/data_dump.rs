//! The INI readers on the original game data (work package P1.2, Gates G0
//! and G1).
//!
//! `OUTPOST_DATA_DIR` names a folder that holds `DATA` and `EXE`, made from
//! the master copy in `outdir/` (design/reference-build.md section 7).
//! The test reads every `.ini` file under them with [`IniReader`], and every
//! `.ini` file directly in `EXE` with the engine parser, and renders the
//! same dumps as `Oracle.exe dump`. With `OUTPOST_ORACLE_DIR` set to that
//! program's output, every dump must equal the oracle's line for line; the
//! summary is also written to `rust-result.txt` there.
//!
//! Without `OUTPOST_DATA_DIR` the test prints a notice and passes. Nothing
//! it reads or writes is committed. Run it with `--release --nocapture`:
//! the tree is large.

use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

mod support;

use neuron_core::ini::{EngineIni, IniReader};
use support::{dump_engine, dump_reader};

const TREES: [&str; 2] = ["DATA", "EXE"];
const MAX_REPORTED: usize = 25;

/// `.ini` files under `root/tree`, as `/`-separated relative paths. The
/// rule matches the harness: a name longer than four bytes ending in
/// `.ini` in any case.
fn list_ini_files(root: &Path, relative: &str, out: &mut Vec<String>) {
    let Ok(entries) = fs::read_dir(root.join(relative)) else {
        return;
    };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        let child = format!("{relative}/{name}");
        let Ok(kind) = entry.file_type() else {
            continue;
        };
        if kind.is_dir() {
            list_ini_files(root, &child, out);
        } else if name.len() > 4 && name[name.len() - 4..].eq_ignore_ascii_case(".ini") {
            out.push(child);
        }
    }
}

fn is_engine_file(relative: &str) -> bool {
    relative
        .strip_prefix("EXE/")
        .is_some_and(|rest| !rest.contains('/'))
}

fn reader_dump(root: &Path, relative: &str, stats: &mut Stats) -> Vec<String> {
    let contents = fs::read(root.join(relative)).unwrap_or_default();
    let mut reader = IniReader::new();
    let open = reader.open(relative.as_bytes(), contents);
    let mut lines = vec![format!(
        "{{\"file\":\"{relative}\",\"open\":\"{}\"}}",
        u8::from(open)
    )];
    if open {
        stats.opened += 1;
        dump_reader(&mut reader, false, &mut lines);
    } else {
        stats.not_opened.push(relative.to_owned());
    }
    lines
}

fn engine_dump(root: &Path, relative: &str) -> Vec<String> {
    let source = fs::read(root.join(relative)).unwrap_or_default();
    let mut loader = |path: &[u8]| {
        let path = String::from_utf8_lossy(path).replace('\\', "/");
        fs::read(root.join(path)).ok()
    };
    let mut ini = EngineIni::new();
    let result = ini.load_named(relative.as_bytes(), &source, &mut loader);
    let mut lines = Vec::new();
    dump_engine(relative, &ini, result, &mut lines);
    lines
}

#[derive(Default)]
struct Stats {
    opened: usize,
    engine: usize,
    not_opened: Vec<String>,
    lines: usize,
    mismatches: Vec<String>,
    compared: usize,
}

/// The first difference between `lines` and the oracle's file, if any.
fn compare(oracle: &Path, relative: &str, lines: &[String]) -> Option<String> {
    let text = match fs::read(oracle) {
        Ok(bytes) => String::from_utf8_lossy(&bytes).into_owned(),
        Err(error) => return Some(format!("{relative}: no oracle dump ({error})")),
    };
    let want: Vec<&str> = text.lines().collect();
    for (index, (want, got)) in want.iter().zip(lines).enumerate() {
        if want != got {
            let cut = |s: &str| s.chars().take(160).collect::<String>();
            return Some(format!(
                "{relative} line {}:\n  oracle: {}\n  rust:   {}",
                index + 1,
                cut(want),
                cut(got)
            ));
        }
    }
    (want.len() != lines.len()).then(|| {
        format!(
            "{relative}: oracle has {} lines, rust {}",
            want.len(),
            lines.len()
        )
    })
}

fn oracle_path(oracle: &Path, kind: &str, relative: &str) -> PathBuf {
    oracle.join(kind).join(format!("{relative}.jsonl"))
}

#[test]
fn original_data() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_DATA_DIR is not set; the original-data dump was skipped");
        return;
    };
    let oracle = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from);

    let mut files = Vec::new();
    for tree in TREES {
        list_ini_files(&root, tree, &mut files);
    }
    files.sort();
    assert!(!files.is_empty(), "no .ini files under {}", root.display());

    let mut stats = Stats::default();
    if let Some(oracle) = &oracle {
        let listed = fs::read_to_string(oracle.join("files.txt")).unwrap_or_default();
        let listed: Vec<&str> = listed.lines().collect();
        if listed != files.iter().map(String::as_str).collect::<Vec<_>>() {
            stats.mismatches.push(format!(
                "files.txt: the oracle lists {} files, rust finds {}",
                listed.len(),
                files.len()
            ));
        }
    }

    for relative in &files {
        let mut dumps = vec![("reader", reader_dump(&root, relative, &mut stats))];
        if is_engine_file(relative) {
            stats.engine += 1;
            dumps.push(("engine", engine_dump(&root, relative)));
        }
        for (kind, lines) in dumps {
            stats.lines += lines.len();
            if let Some(oracle) = &oracle {
                stats.compared += 1;
                if let Some(mismatch) =
                    compare(&oracle_path(oracle, kind, relative), relative, &lines)
                {
                    stats.mismatches.push(format!("[{kind}] {mismatch}"));
                }
            }
        }
    }

    let mut summary = String::new();
    let _ = writeln!(summary, "original-data dump of {}", root.display());
    let _ = writeln!(
        summary,
        "{} files: {} opened, {} not opened; {} also through the engine parser; {} dump lines",
        files.len(),
        stats.opened,
        stats.not_opened.len(),
        stats.engine,
        stats.lines
    );
    for name in &stats.not_opened {
        let _ = writeln!(summary, "not opened (empty or unreadable): {name}");
    }
    match &oracle {
        Some(_) => {
            let _ = writeln!(
                summary,
                "{} dumps compared with the oracle, {} differ",
                stats.compared,
                stats.mismatches.len()
            );
        }
        None => {
            let _ = writeln!(
                summary,
                "OUTPOST_ORACLE_DIR is not set: nothing was compared"
            );
        }
    }
    for mismatch in stats.mismatches.iter().take(MAX_REPORTED) {
        let _ = writeln!(summary, "{mismatch}");
    }
    print!("{summary}");
    if let Some(oracle) = &oracle {
        let _ = fs::write(oracle.join("rust-result.txt"), &summary);
    }
    assert!(
        stats.mismatches.is_empty(),
        "{} dumps differ from the oracle",
        stats.mismatches.len()
    );
}
