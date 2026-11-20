//! The `account` suite: `tests/fixtures/oracle/account.list` through
//! `neuron_server::account`, rendered as `tests/vectors/account.jsonl`.
//! `Oracle.exe neuron-server` writes the same file from `Server.dll`
//! (`CppSource/Oracle/oracle_bus.cpp`, `RunAccountSuite`).

use std::fs;
use std::path::Path;

use neuron_server::account::{account_crc, hashed_name, name_file_key};

fn unhex(text: &str) -> Vec<u8> {
    if text == "." {
        return Vec::new();
    }
    (0..text.len() / 2)
        .map(|i| u8::from_str_radix(&text[2 * i..2 * i + 2], 16).expect("hex"))
        .collect()
}

#[test]
fn account_vectors() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests");
    let list = fs::read_to_string(root.join("fixtures/oracle/account.list")).expect("account.list");
    let mut lines = Vec::new();
    for line in list.lines().filter(|l| !l.starts_with('#')) {
        let a: Vec<&str> = line.split_whitespace().collect();
        match a.as_slice() {
            [] => {}
            ["crc", field] => {
                let crc = account_crc(&unhex(field));
                lines.push(format!("{{\"in\":\"{field}\",\"crc\":\"{crc:08x}\"}}"));
            }
            ["name", field] => {
                let bytes = unhex(field);
                let name: Vec<u16> = bytes
                    .as_chunks::<2>()
                    .0
                    .iter()
                    .map(|&p| u16::from_le_bytes(p))
                    .collect();
                lines.push(format!(
                    "{{\"in\":\"{field}\",\"dir\":\"{}\",\"key\":\"{:04x}\"}}",
                    hashed_name(&name),
                    name_file_key()
                ));
            }
            _ => panic!("account.list: bad line {line}"),
        }
    }
    let path = root.join("vectors/account.jsonl");
    let rendered: String = lines.iter().map(|l| format!("{l}\n")).collect();
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        fs::write(&path, rendered).expect("write account.jsonl");
        return;
    }
    let committed = fs::read_to_string(&path).expect("account.jsonl");
    let committed: Vec<&str> = committed.lines().collect();
    for (index, (want, got)) in committed.iter().zip(&lines).enumerate() {
        assert_eq!(*want, got, "account.jsonl line {}", index + 1);
    }
    assert_eq!(committed.len(), lines.len(), "account.jsonl: line count");
}
