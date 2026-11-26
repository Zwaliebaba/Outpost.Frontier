//! The preferences screen's lists on the original data (`outdir/`,
//! decision O15) and against the C++ client's `+oracle` file (P5.2,
//! decision O53).
//!
//! - `the_shipped_lists_load`: `keylist.ini` and `optlist.ini` with the
//!   shipped key map.
//! - `layout_lists_equal_the_cpp_client` (`OUTPOST_CLIENT_ORACLE_DIR`, a C++
//!   `+oracle` output folder): every `keylist`/`keyrow` and `optlist`/`opt`
//!   record of its `layout.jsonl` equals the port's, each load replayed at
//!   the frame of its C++ head record (the screen flow that decides the
//!   frame comes with the screen). A seed-mode option load is a fresh list,
//!   as `SeedGraphicsDefaults`'s bare screen is; the screen's own loads
//!   append, as the C++ does. Without the variable it prints a notice.

use std::path::PathBuf;

use game_app::keymap::{KEY_MAP_PATH, KeyMap};
use game_app::oracle::layout::{key_list_lines, opt_list_lines};
use game_app::preferences::{KEY_LIST_PATH, OPT_LIST_PATH, Preferences};
use neuron_core::vfs::Host;

fn data_dir() -> PathBuf {
    std::env::var_os("OUTPOST_DATA_DIR").map_or_else(
        || PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data"),
        PathBuf::from,
    )
}

/// A file under the data root, found case-insensitively; `rel` is relative
/// to the data root.
fn data_file(rel: &str) -> Option<Vec<u8>> {
    let path = Host::new(data_dir())
        .locate(rel.as_bytes())
        .ok()
        .flatten()?
        .path;
    std::fs::read(path).ok()
}

/// The client-relative paths of the lists, under the data root.
fn list_text(path: &str) -> Option<Vec<u8>> {
    data_file(path.strip_prefix("data/").unwrap_or(path))
}

fn keymap() -> KeyMap {
    let mut map = KeyMap::new();
    map.load_base(data_file(KEY_MAP_PATH).as_deref(), KEY_MAP_PATH);
    map
}

#[test]
fn the_shipped_lists_load() {
    let map = keymap();
    let mut p = Preferences::default();
    p.load_key_list(list_text(KEY_LIST_PATH).as_deref(), KEY_LIST_PATH, &map);
    let rows: usize = p.key_groups.iter().map(Vec::len).sum();
    assert!(p.load_opt_list(
        list_text(OPT_LIST_PATH).as_deref(),
        OPT_LIST_PATH,
        &mut |_, _| {}
    ));
    let options = p.interface.len() + p.audio.len() + p.video.len();
    println!(
        "key rows {rows}, option rows {options}, group names {:?}",
        p.group_name_ids
    );
    // The owner run of menu_walk.txt (reference-build.md section 8).
    assert_eq!((rows, options), (93, 49));
}

#[test]
fn layout_lists_equal_the_cpp_client() {
    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let text = std::fs::read_to_string(dir.join("layout.jsonl")).expect("layout.jsonl");
    let kinds = [
        "\"rec\":\"keylist\"",
        "\"rec\":\"keyrow\"",
        "\"rec\":\"optlist\"",
        "\"rec\":\"opt\"",
    ];
    let cpp: Vec<&str> = text
        .lines()
        .filter(|l| kinds.iter().any(|k| l.contains(k)))
        .collect();
    let map = keymap();
    let mut screen = Preferences::default();
    let mut port = Vec::new();
    for line in &cpp {
        let frame = || {
            let rest = &line[line.find("\"frame\":").map_or(0, |i| i + 8)..];
            rest.split(',')
                .next()
                .and_then(|f| f.parse::<u32>().ok())
                .unwrap_or(0)
        };
        if line.contains("\"rec\":\"keylist\"") {
            screen.load_key_list(list_text(KEY_LIST_PATH).as_deref(), KEY_LIST_PATH, &map);
            port.extend(key_list_lines(&screen, frame(), KEY_LIST_PATH));
        } else if line.contains("\"rec\":\"optlist\"") {
            let text = list_text(OPT_LIST_PATH);
            if line.contains("\"seed_mode\":1") {
                let mut bare = Preferences {
                    seed_mode: true,
                    ..Preferences::default()
                };
                bare.load_opt_list(text.as_deref(), OPT_LIST_PATH, &mut |_, _| {});
                port.extend(opt_list_lines(&bare, frame(), OPT_LIST_PATH));
            } else {
                screen.load_opt_list(text.as_deref(), OPT_LIST_PATH, &mut |_, _| {});
                port.extend(opt_list_lines(&screen, frame(), OPT_LIST_PATH));
            }
        }
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
    println!("layout.jsonl: {} list records equal", port.len());
}
