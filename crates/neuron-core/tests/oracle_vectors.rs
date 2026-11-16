//! Oracle vectors for work packages P1.1 and P1.2.
//!
//! Each suite reads its inputs from `tests/fixtures/oracle/` and renders
//! one JSON object per line. The committed files in `tests/vectors/` are
//! the output of the C++ oracle harness (`CppSource/Oracle`); CI's oracle
//! job fails unless the harness reproduces them byte for byte. The Rust
//! port must produce the same text.
//!
//! Set `OUTPOST_BLESS_VECTORS=1` to rewrite the files from the Rust port.
//! Do that only to add a suite; the oracle job then decides whether the
//! new file is right.

use std::fs;
use std::path::{Path, PathBuf};

mod support;

use support::{bits32, dump_engine, dump_reader, hex};

use neuron_core::hash::{continued_name_crc32, continued_name_crc32_byte, name_crc32, stricmp};
use neuron_core::id::{create_id, make_id, make_location_id};
use neuron_core::ini::{EngineIni, IniReader, is_number, parse_number, string_index, value_num};
use neuron_core::msvcrt::{atof, atoi, format_g, wtoi};
use neuron_core::num::{round_even_to_i32, trunc_to_i32, trunc_to_i32_via_i64, trunc_to_i64};
use neuron_core::rng::CrtRand;

const CRC_CONTINUE_SEED: u32 = 0x5A5A_5A5A;
const LOCATION_BASE: u32 = 0x8ABC_DEF0;

fn fixtures() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/oracle")
}

fn read_fixture(relative: &str) -> Vec<u8> {
    let path = fixtures().join(relative);
    fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

/// The non-comment lines of an input list. Empty lines are inputs too.
fn input_lines(relative: &str) -> Vec<String> {
    let text = String::from_utf8(read_fixture(relative)).unwrap_or_default();
    text.lines()
        .filter(|l| !l.starts_with('#'))
        .map(str::to_owned)
        .collect()
}

fn unhex(text: &str) -> Vec<u8> {
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&text[i..i + 2], 16).unwrap_or_else(|e| panic!("{text}: {e}")))
        .collect()
}

fn sign(ordering: core::cmp::Ordering) -> i32 {
    ordering as i32
}

fn bits64(value: i64) -> u64 {
    u64::from_ne_bytes(value.to_ne_bytes())
}

/// Compares `lines` with the committed suite, or writes it when blessing.
fn check_suite(name: &str, lines: &[String]) {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(name);
    let rendered: String = lines.iter().map(|l| format!("{l}\n")).collect();
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        fs::write(&path, rendered).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
        return;
    }
    let committed = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let committed: Vec<&str> = committed.lines().collect();
    for (index, (want, got)) in committed.iter().zip(lines).enumerate() {
        assert_eq!(*want, got, "{name} line {}", index + 1);
    }
    assert_eq!(committed.len(), lines.len(), "{name}: line count");
}

#[test]
fn names() {
    let lines: Vec<String> = input_lines("names.hex")
        .iter()
        .map(|line| {
            let name = unhex(line);
            let first = name.first().copied().unwrap_or(0);
            format!(
                "{{\"in\":\"{line}\",\"crc\":\"{:08x}\",\"crc_cont\":\"{:08x}\",\"crc_byte\":\"{:08x}\",\"create_id\":\"{:08x}\",\"make_id\":\"{:04x}\",\"location_id\":\"{:08x}\"}}",
                name_crc32(&name),
                continued_name_crc32(CRC_CONTINUE_SEED, &name),
                continued_name_crc32_byte(CRC_CONTINUE_SEED, first),
                create_id(&name),
                make_id(&name),
                make_location_id(LOCATION_BASE, &name),
            )
        })
        .collect();
    check_suite("names.jsonl", &lines);
}

#[test]
fn stricmp_pairs() {
    let lines: Vec<String> = input_lines("stricmp.hex")
        .iter()
        .map(|line| {
            let (a, b) = line.split_once(':').unwrap_or((line, ""));
            let result = sign(stricmp(&unhex(a), &unhex(b)));
            format!("{{\"a\":\"{a}\",\"b\":\"{b}\",\"stricmp\":\"{result}\",\"crt\":\"{result}\"}}")
        })
        .collect();
    check_suite("stricmp.jsonl", &lines);
}

#[test]
fn numbers() {
    let lines: Vec<String> = input_lines("numbers.hex")
        .iter()
        .map(|line| {
            let text = unhex(line);
            let parsed = parse_number(&text);
            format!(
                "{{\"in\":\"{line}\",\"atoi\":\"{:08x}\",\"atof\":\"{:016x}\",\"parse\":\"{:016x}\",\"parse_dumps\":\"{}\",\"is_number\":\"{}\"}}",
                bits32(atoi(&text)),
                atof(&text).to_bits(),
                parsed.value.to_bits(),
                u8::from(parsed.illegal_digit),
                u8::from(is_number(&text)),
            )
        })
        .collect();
    check_suite("numbers.jsonl", &lines);
}

#[test]
fn fields() {
    let lines: Vec<String> = input_lines("fields.hex")
        .iter()
        .map(|line| {
            let text = unhex(line);
            format!(
                "{{\"in\":\"{line}\",\"f0\":\"{}\",\"f1\":\"{}\",\"f2\":\"{}\",\"f3\":\"{}\",\"value_num1\":\"{:016x}\"}}",
                hex(string_index(&text, 0)),
                hex(string_index(&text, 1)),
                hex(string_index(&text, 2)),
                hex(string_index(&text, 3)),
                value_num(&text, 1).to_bits(),
            )
        })
        .collect();
    check_suite("fields.jsonl", &lines);
}

#[test]
fn wide() {
    let lines: Vec<String> = input_lines("wide.hex")
        .iter()
        .map(|line| {
            let units: Vec<u16> = (0..line.len())
                .step_by(4)
                .map(|i| u16::from_str_radix(&line[i..i + 4], 16).unwrap_or_default())
                .collect();
            format!(
                "{{\"in\":\"{line}\",\"wtoi\":\"{:08x}\"}}",
                bits32(wtoi(&units))
            )
        })
        .collect();
    check_suite("wide.jsonl", &lines);
}

#[test]
fn doubles() {
    let lines: Vec<String> = input_lines("doubles.hex")
        .iter()
        .map(|line| {
            let value = f64::from_bits(u64::from_str_radix(line, 16).unwrap_or_default());
            format!(
                "{{\"bits\":\"{line}\",\"g1\":\"{}\",\"g6\":\"{}\",\"g9\":\"{}\",\"g17\":\"{}\",\"trunc32\":\"{:08x}\",\"trunc64\":\"{:016x}\",\"via64\":\"{:08x}\",\"round32\":\"{:08x}\"}}",
                format_g(value, 1),
                format_g(value, 6),
                format_g(value, 9),
                format_g(value, 17),
                bits32(trunc_to_i32(value)),
                bits64(trunc_to_i64(value)),
                bits32(trunc_to_i32_via_i64(value)),
                bits32(round_even_to_i32(value)),
            )
        })
        .collect();
    check_suite("doubles.jsonl", &lines);
}

#[test]
fn rand() {
    let lines: Vec<String> = input_lines("rand.hex")
        .iter()
        .map(|line| {
            let mut rand = CrtRand::with_seed(u32::from_str_radix(line, 16).unwrap_or_default());
            let values: Vec<String> = (0..8).map(|_| rand.rand().to_string()).collect();
            format!("{{\"seed\":\"{line}\",\"rand\":\"{}\"}}", values.join(","))
        })
        .collect();
    check_suite("rand.jsonl", &lines);
}

#[test]
fn engine_ini() {
    let mut lines = Vec::new();
    for file in input_lines("engine.list") {
        let source = read_fixture(&file);
        let mut loader = |path: &[u8]| {
            let relative = String::from_utf8_lossy(path).replace('\\', "/");
            fs::read(fixtures().join(relative)).ok()
        };
        let mut ini = EngineIni::new();
        let result = ini.load_named(file.as_bytes(), &source, &mut loader);
        dump_engine(&file, &ini, result, &mut lines);
    }
    check_suite("engine_ini.jsonl", &lines);
}

#[test]
fn ini_reader() {
    let mut lines = Vec::new();
    for file in input_lines("reader.list") {
        let contents = read_fixture(&file);
        for mode in ["file", "memory"] {
            let mut reader = IniReader::new();
            let open = if mode == "file" {
                reader.open(file.as_bytes(), contents.clone())
            } else {
                reader.open_memory(&contents)
            };
            lines.push(format!(
                "{{\"file\":\"{file}\",\"mode\":\"{mode}\",\"open\":\"{}\"}}",
                u8::from(open)
            ));
            if open {
                dump_reader(&mut reader, mode == "memory", &mut lines);
            }
        }
    }
    check_suite("ini_reader.jsonl", &lines);
}
