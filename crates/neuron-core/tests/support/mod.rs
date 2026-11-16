//! Dump formats shared by the oracle tests. The C++ harness
//! (`CppSource/Oracle/oracle_main.cpp`) writes the same lines.

use std::fmt::Write as _;

use neuron_core::ini::{EngineIni, EngineIniError, IniReader};

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
/// parameter past the last; only text readers may do that, since the C++
/// shows a message box for BINI.
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
