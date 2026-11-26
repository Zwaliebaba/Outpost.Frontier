//! The key map on the original data (`outdir/`, decision O15) and against
//! the C++ client's `+oracle` file (P5.2, decision O53 (2) and (4)).
//!
//! - `the_shipped_key_map_loads`: `interface/KeyMap.ini` of `outdir/data`
//!   gives the owner run's figures (122 commands, 5 groups).
//! - `keymap_jsonl_equals_the_cpp_client` (`OUTPOST_CLIENT_ORACLE_DIR`, a
//!   C++ `+oracle` output folder of `menu_walk.txt` run without a
//!   `UserKeyMap.ini`): the port's `keymap.jsonl` equals that run's line for
//!   line. Without the variable it prints a notice and compares nothing.

use std::path::PathBuf;

use game_app::keymap::{KEY_MAP_PATH, KeyMap, KeyMapFiles};
use game_app::oracle::keymap::phase_lines;
use neuron_core::vfs::Host;

fn data_dir() -> PathBuf {
    std::env::var_os("OUTPOST_DATA_DIR").map_or_else(
        || PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data"),
        PathBuf::from,
    )
}

/// The data root's `interface/KeyMap.ini`, found case-insensitively as on
/// Windows; no player file.
struct Shipped;

impl KeyMapFiles for Shipped {
    fn base(&mut self) -> (Option<Vec<u8>>, String) {
        let dir = data_dir();
        let path = Host::new(&dir)
            .locate(KEY_MAP_PATH.as_bytes())
            .ok()
            .flatten()
            .map(|e| e.path);
        let text = path.as_ref().and_then(|p| std::fs::read(p).ok());
        (text, KEY_MAP_PATH.to_string())
    }
    fn user(&mut self) -> Option<Vec<u8>> {
        None
    }
}

/// The lines `EnsureKeyMapLoaded` makes the oracle write, at frame 0.
fn port_lines() -> (KeyMap, Vec<String>) {
    let mut map = KeyMap::new();
    let mut lines = Vec::new();
    map.ensure_loaded(&mut Shipped, &mut |m, phase, result| {
        lines.extend(phase_lines(m, 0, phase, result));
    });
    (map, lines)
}

#[test]
fn the_shipped_key_map_loads() {
    let (map, lines) = port_lines();
    assert_eq!((map.commands.len(), map.groups.len()), (122, 5));
    assert_eq!(lines.len(), 258);
    assert!(lines[0].starts_with(
        r#"{"frame":0,"rec":"load","phase":"base","result":5,"cmds":122,"groups":5}"#
    ));
}

#[test]
fn keymap_jsonl_equals_the_cpp_client() {
    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let text = std::fs::read_to_string(dir.join("keymap.jsonl")).expect("keymap.jsonl");
    let cpp: Vec<&str> = text.lines().collect();
    let (_, port) = port_lines();
    let first = cpp.iter().zip(&port).position(|(c, p)| *c != p.as_str());
    if let Some(i) = first {
        panic!(
            "line {} differs:\n  C++:  {}\n  port: {}",
            i + 1,
            cpp[i],
            port[i]
        );
    }
    assert_eq!(cpp.len(), port.len(), "the files have different lengths");
    println!("keymap.jsonl: {} lines equal", port.len());
}
