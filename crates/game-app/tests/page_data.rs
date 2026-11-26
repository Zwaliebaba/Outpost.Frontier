//! The page descriptors' `layout.jsonl` records on the original data
//! (`outdir/`, decision O15) and against the C++ client's `+oracle` file
//! (P5.2, decision O53).
//!
//! - `the_char_select_page_loads`: `INTERFACE/INTRO/page_charselectsingle.page`,
//!   the page `menu_walk.txt` opens, gives its 15 widgets.
//! - `page_records_equal_the_cpp_client` (`OUTPOST_CLIENT_ORACLE_DIR`): every
//!   `page` and `widget` record of the C++ run's `layout.jsonl` equals the
//!   port's for the file it names, with the C++ record's frame and path (the
//!   full path of the machine that ran it). Without the variable it prints a
//!   notice.

use std::path::PathBuf;

use game_app::oracle::layout::page_lines;
use neuron_client::ui::page::PageDesc;
use neuron_core::vfs::Host;

fn data_dir() -> PathBuf {
    std::env::var_os("OUTPOST_DATA_DIR").map_or_else(
        || PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data"),
        PathBuf::from,
    )
}

fn data_file(rel: &[u8]) -> Option<Vec<u8>> {
    let path = Host::new(data_dir()).locate(rel).ok().flatten()?.path;
    std::fs::read(path).ok()
}

/// The JSON string after `"key":` in a record, unescaped as `FOracleJson`
/// escapes (`\\`, `\"`, `\uXXXX` for bytes outside printable ASCII).
fn string_field(line: &str, key: &str) -> Option<Vec<u8>> {
    let start = line.find(&format!("\"{key}\":\""))? + key.len() + 4;
    let bytes = line.as_bytes();
    let mut out = Vec::new();
    let mut i = start;
    while i < bytes.len() {
        match bytes[i] {
            b'"' => return Some(out),
            b'\\' if bytes.get(i + 1) == Some(&b'u') => {
                let hex = std::str::from_utf8(bytes.get(i + 2..i + 6)?).ok()?;
                out.push(u8::try_from(u32::from_str_radix(hex, 16).ok()?).ok()?);
                i += 6;
            }
            b'\\' => {
                out.push(*bytes.get(i + 1)?);
                i += 2;
            }
            b => {
                out.push(b);
                i += 1;
            }
        }
    }
    None
}

fn frame(line: &str) -> u32 {
    let rest = &line[line.find("\"frame\":").map_or(0, |i| i + 8)..];
    rest.split(',')
        .next()
        .and_then(|f| f.parse().ok())
        .unwrap_or(0)
}

/// The data-relative part of a path the C++ names (after its last
/// `data\` or `data/`, any case).
fn data_relative(path: &[u8]) -> Vec<u8> {
    let lower: Vec<u8> = path.iter().map(u8::to_ascii_lowercase).collect();
    let cut = lower
        .windows(5)
        .rposition(|w| w == b"data\\" || w == b"data/")
        .map_or(0, |i| i + 5);
    path[cut..].to_vec()
}

#[test]
fn the_char_select_page_loads() {
    let text = data_file(b"INTERFACE/INTRO/page_charselectsingle.page").expect("the page");
    let desc = PageDesc::load(&text);
    assert!(desc.ok);
    assert_eq!(desc.entries.len(), 15);
    let lines = page_lines(&desc, 0, b"page_charselectsingle.page");
    assert_eq!(lines.len(), 16);
    assert!(
        lines[1].contains(r#""type":"UITextMsgButton","name":"LoadCharacter""#),
        "{}",
        lines[1]
    );
}

#[test]
fn page_records_equal_the_cpp_client() {
    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let text = std::fs::read_to_string(dir.join("layout.jsonl")).expect("layout.jsonl");
    let cpp: Vec<&str> = text
        .lines()
        .filter(|l| l.contains("\"rec\":\"page\"") || l.contains("\"rec\":\"widget\""))
        .collect();
    let mut port = Vec::new();
    for line in cpp.iter().filter(|l| l.contains("\"rec\":\"page\"")) {
        let path = string_field(line, "path").expect("a path");
        let rel = data_relative(&path);
        let desc = data_file(&rel)
            .map(|t| PageDesc::load(&t))
            .unwrap_or_default();
        port.extend(page_lines(&desc, frame(line), &path));
    }
    if let Some(i) = cpp.iter().zip(&port).position(|(c, p)| *c != p.as_str()) {
        panic!(
            "record {} differs:\n  C++:  {}\n  port: {}",
            i + 1,
            cpp[i],
            port[i]
        );
    }
    assert_eq!(cpp.len(), port.len(), "the record counts differ");
    println!("layout.jsonl: {} page records equal", port.len());
}
