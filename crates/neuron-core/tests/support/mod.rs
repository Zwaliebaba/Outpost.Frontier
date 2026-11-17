//! Helpers and dump formats shared by the oracle tests. The C++ harness
//! (`CppSource/Oracle/`) writes the same lines.
//!
//! Each test target uses part of this module.
#![allow(dead_code)]

pub mod data;

use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

use neuron_core::ini::{EngineIni, EngineIniError, IniReader};

/// The synthetic oracle inputs, `tests/fixtures/oracle/`.
pub fn fixtures() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/oracle")
}

/// The bytes of a file under [`fixtures`].
pub fn read_fixture(relative: &str) -> Vec<u8> {
    let path = fixtures().join(relative);
    fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

/// The non-comment lines of an input list. Empty lines are inputs too.
pub fn input_lines(relative: &str) -> Vec<String> {
    let text = String::from_utf8(read_fixture(relative)).unwrap_or_default();
    text.lines()
        .filter(|l| !l.starts_with('#'))
        .map(str::to_owned)
        .collect()
}

/// The bytes a lower- or upper-case hex string spells.
pub fn unhex(text: &str) -> Vec<u8> {
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&text[i..i + 2], 16).unwrap_or_else(|e| panic!("{text}: {e}")))
        .collect()
}

/// Compares `lines` with the committed suite `tests/vectors/<name>`, or
/// writes it when `OUTPOST_BLESS_VECTORS` is set.
pub fn check_suite(name: &str, lines: &[String]) {
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

/// Lower-case hex of `bytes`.
pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut out, b| {
        let _ = write!(out, "{b:02x}");
        out
    })
}

/// The bits of an `i32`.
pub fn bits32(value: i32) -> u32 {
    u32::from_ne_bytes(value.to_ne_bytes())
}

/// The engine parser's result and database for `file`.
pub fn dump_engine(
    file: &str,
    ini: &EngineIni,
    result: Result<(), EngineIniError>,
    lines: &mut Vec<String>,
) {
    let result = match result {
        Ok(()) => "ok",
        Err(EngineIniError::IncludeWithoutName) => "generic",
        Err(EngineIniError::Breakpoint) => "breakpoint",
        Err(EngineIniError::KeyBeforeSection) => "fault",
        Err(EngineIniError::IncludeTooDeep) => "too_deep",
    };
    lines.push(format!("{{\"file\":\"{file}\",\"result\":\"{result}\"}}"));
    for section in ini.sections() {
        lines.push(format!("{{\"section\":\"{}\"}}", hex(&section.name)));
        let Some(first) = ini.section(&section.name) else {
            continue;
        };
        for line in (0..)
            .map_while(|i| first.line(i))
            .take_while(|l| !l.is_empty())
        {
            let key = line
                .windows(3)
                .position(|w| w == b" = ")
                .map_or(&line[..], |end| &line[..end]);
            let value = first.value(key).unwrap_or_default();
            lines.push(format!(
                "{{\"line\":\"{}\",\"value\":\"{}\"}}",
                hex(&line),
                hex(value)
            ));
        }
    }
}

/// Every section and value an open reader yields, then a `find_header`
/// of each section name in upper case. `extra_param` also reads one
/// parameter past the last.
pub fn dump_reader(reader: &mut IniReader, extra_param: bool, lines: &mut Vec<String>) {
    let mut headers = Vec::new();
    while reader.read_header() {
        let header = reader.header();
        lines.push(format!("{{\"header\":\"{}\"}}", hex(&header)));
        headers.push(header);
        while reader.read_value() {
            let name = reader.name();
            let line = reader.line();
            let count = reader.num_parameters();
            let whole = reader.value_string();
            let wide: String = reader
                .value_wide()
                .iter()
                .map(|u| format!("{u:04x}"))
                .collect();
            let line_num = reader.line_num();
            let tell = reader.tell();
            let end = reader.is_end();
            let dumps = reader.take_reports().len();
            lines.push(format!(
                "{{\"name\":\"{}\",\"line\":\"{}\",\"n\":\"{count}\",\"str\":\"{}\",\"wide\":\"{wide}\",\"line_num\":\"{line_num}\",\"tell\":\"{tell}\",\"end\":\"{}\",\"dumps\":\"{dumps}\"}}",
                hex(&name),
                hex(&line),
                hex(&whole),
                u8::from(end),
            ));
            let limit = if extra_param { count + 1 } else { count };
            for index in 0..limit {
                let s = reader.value_string_at(index);
                let i = reader.value_int(index);
                let f = reader.value_float(index);
                let b = reader.value_bool(index);
                let e = reader.is_value_empty(index);
                let dumps = reader.take_reports().len();
                lines.push(format!(
                    "{{\"param\":\"{index}\",\"s\":\"{}\",\"i\":\"{:08x}\",\"f\":\"{:016x}\",\"b\":\"{}\",\"e\":\"{}\",\"dumps\":\"{dumps}\"}}",
                    hex(&s),
                    bits32(i),
                    f.to_bits(),
                    u8::from(b),
                    u8::from(e),
                ));
            }
        }
    }
    for header in headers {
        let query = header.to_ascii_uppercase();
        let found = reader.find_header(&query);
        let first = if found && reader.read_value() {
            reader.name()
        } else {
            Vec::new()
        };
        lines.push(format!(
            "{{\"find\":\"{}\",\"found\":\"{}\",\"first\":\"{}\"}}",
            hex(&query),
            u8::from(found),
            hex(&first)
        ));
    }
    reader.take_reports();
}
