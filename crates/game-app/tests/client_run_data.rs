//! The port's own client run against the C++ client's (decision O53 (6)):
//! `OUTPOST_CLIENT_ORACLE_PORT_DIR` names the output folder of the Rust
//! `OutpostFrontier.exe +oracle menu_walk.txt`, `OUTPOST_CLIENT_ORACLE_FRESH_DIR`
//! that of the C++ client's run of the same script on a fresh user folder,
//! as the port's run has (run w of the client-oracle job).
//!
//! What is compared is `game_app::oracle::compare`'s: in `session.jsonl`
//! the screen changes, the dumps, the waits and the script lines' results,
//! without the `start` record's paths, the screenshots' names (D1114) and
//! the intro base's `room` screen, which the port does not enter yet
//! (D1107: the line is removed from both files before they are compared);
//! in `tree.jsonl` every line of every dump without `rtti`, the
//! screenshots' names and the dump heads' `room` (D1107), and the Options
//! page's key names, gamma row and video mode text, which the port's
//! preferences window does not have yet (D1111, `compare::PORT_RUN`). Each
//! file's line gives the fields each mask hid, by divergence row. The text
//! windows' heights are compared: the port's run has the client's fonts.
//! `OUTPOST_CLIENT_ORACLE_MASK_HEIGHTS` set masks them, for a port run
//! without them.

use std::fs;
use std::path::{Path, PathBuf};

use game_app::oracle::compare;

fn lines(dir: &Path, file: &str) -> Vec<String> {
    let path = dir.join(file);
    fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("{}: {e}", path.display()))
        .lines()
        .map(String::from)
        .collect()
}

/// How many records are equal, and the first differences.
fn report(file: &str, port: &[String], cpp: &[String]) -> bool {
    let equal = port.iter().zip(cpp).filter(|(a, b)| a == b).count();
    if port == cpp {
        println!("{file}: {equal} records equal");
        return true;
    }
    println!(
        "{file}: {equal} of {} records equal at the same place ({} in the port's run)",
        cpp.len(),
        port.len()
    );
    for (i, (a, b)) in port
        .iter()
        .zip(cpp)
        .enumerate()
        .filter(|(_, (a, b))| a != b)
        .take(8)
    {
        println!("  record {i}:\n    port: {a}\n    C++:  {b}");
    }
    false
}

#[test]
fn the_ports_run_equals_the_cpp_clients() {
    let dirs = [
        "OUTPOST_CLIENT_ORACLE_PORT_DIR",
        "OUTPOST_CLIENT_ORACLE_FRESH_DIR",
    ]
    .map(|v| std::env::var_os(v).map(PathBuf::from));
    let [Some(port), Some(cpp)] = dirs else {
        println!(
            "notice: OUTPOST_CLIENT_ORACLE_PORT_DIR or OUTPOST_CLIENT_ORACLE_FRESH_DIR is not set; nothing was compared"
        );
        return;
    };
    let heights = std::env::var_os("OUTPOST_CLIENT_ORACLE_MASK_HEIGHTS").is_none();

    let compared = |file: &str, port: &compare::Masked, cpp: &compare::Masked| {
        // The masks' counts, per divergence row (decision O53 (4)).
        println!(
            "{file} masks: port {}; C++ {}",
            port.summary(),
            cpp.summary()
        );
        report(file, &port.lines, &cpp.lines)
    };
    let session = compared(
        "session.jsonl",
        &compare::session_records(&lines(&port, "session.jsonl")),
        &compare::session_records(&lines(&cpp, "session.jsonl")),
    );
    let tree = compared(
        "tree.jsonl",
        &compare::tree_records(&lines(&port, "tree.jsonl"), heights, compare::PORT_RUN),
        &compare::tree_records(&lines(&cpp, "tree.jsonl"), heights, compare::PORT_RUN),
    );
    assert!(
        session && tree,
        "the port's run differs from the C++ client's"
    );
}
