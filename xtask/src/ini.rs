//! `cargo xtask ini`: a readable listing of game INI files,
//! for reading facts from the original installation (design/facts.md).
//! It uses the ported reader, so it shows what the game reads.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use neuron_core::ini::IniReader;

pub const USAGE: &str = "  ini <path>... [--grep <text>]
                 list the sections and values of INI files;
                 a folder is searched for .ini files. With --grep, only the
                 values whose section, name or parameters contain the text
                 (any case) are shown, each with its file and section";

/// Bytes shown one character per byte, so nothing is lost or replaced.
fn show(bytes: &[u8]) -> String {
    bytes.iter().map(|&b| char::from(b)).collect()
}

fn collect(path: &Path, out: &mut Vec<PathBuf>) {
    if path.is_dir() {
        let mut entries: Vec<PathBuf> = fs::read_dir(path)
            .map(|dir| dir.flatten().map(|e| e.path()).collect())
            .unwrap_or_default();
        entries.sort();
        for entry in entries {
            if entry.is_dir() {
                collect(&entry, out);
            } else if entry
                .extension()
                .is_some_and(|ext| ext.eq_ignore_ascii_case("ini"))
            {
                out.push(entry);
            }
        }
    } else {
        out.push(path.to_path_buf());
    }
}

fn list(path: &Path, grep: Option<&str>) -> bool {
    let name = path.display().to_string();
    let Ok(contents) = fs::read(path) else {
        eprintln!("{name}: cannot read");
        return false;
    };
    let mut reader = IniReader::new();
    if !reader.open(name.as_bytes(), contents) {
        eprintln!("{name}: cannot open (empty file)");
        return false;
    }
    if grep.is_none() {
        println!("== {name}");
    }
    while reader.read_header() {
        let header = show(&reader.header());
        if grep.is_none() {
            println!("[{header}]");
        }
        while reader.read_value() {
            let value_name = show(&reader.name());
            let params = show(&reader.value_string());
            match grep {
                None => println!("{value_name} = {params}"),
                Some(text) => {
                    let haystack = format!("{header}\n{value_name}\n{params}").to_lowercase();
                    if haystack.contains(&text.to_lowercase()) {
                        println!("{name} [{header}] {value_name} = {params}");
                    }
                }
            }
        }
    }
    reader.take_reports();
    true
}

pub fn run(args: &[String]) -> ExitCode {
    let mut grep = None;
    let mut paths = Vec::new();
    let mut rest = args.iter();
    while let Some(arg) = rest.next() {
        if arg == "--grep" {
            match rest.next() {
                Some(text) => grep = Some(text.as_str()),
                None => {
                    eprintln!("ini: --grep needs a text");
                    return ExitCode::from(2);
                }
            }
        } else {
            paths.push(PathBuf::from(arg));
        }
    }
    if paths.is_empty() {
        eprintln!("usage: cargo xtask{USAGE}");
        return ExitCode::from(2);
    }
    let mut files = Vec::new();
    for path in &paths {
        collect(path, &mut files);
    }
    let mut ok = true;
    for file in &files {
        ok &= list(file, grep);
    }
    if ok {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}
