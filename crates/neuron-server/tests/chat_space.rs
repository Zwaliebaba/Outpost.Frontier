//! The `chat_space` suite: the port's `C1_SPACE` table above U+00FF,
//! rendered as `tests/vectors/chat_space.jsonl`. `Oracle.exe neuron-server`
//! writes the same file from Windows' `GetStringTypeW`
//! (`CppSource/Oracle/oracle_bus.cpp`, `RunChatSpaceSuite`).

use std::fs;
use std::path::Path;

use neuron_server::chat_space::{WIDE_SPACES, is_chat_space};

#[test]
fn chat_space_vectors() {
    let lines: String = WIDE_SPACES
        .iter()
        .map(|u| format!("{{\"u\":\"{u:04x}\"}}\n"))
        .collect();
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/vectors/chat_space.jsonl");
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        fs::write(&path, &lines).expect("bless");
    }
    let want = fs::read_to_string(&path).expect("chat_space.jsonl");
    assert_eq!(lines, want);
}

#[test]
fn the_class_holds_the_ascii_spaces_and_the_table() {
    for unit in [0x09, 0x0a, 0x0d, 0x20, 0x3000] {
        assert!(is_chat_space(unit), "{unit:#x}");
    }
    for unit in [0x08, 0x0e, 0x21, 0x85, 0xa0, 0x41] {
        assert!(!is_chat_space(unit), "{unit:#x}");
    }
}
