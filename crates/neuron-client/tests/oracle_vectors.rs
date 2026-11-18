//! The committed oracle vectors of this crate (`tests/vectors/`), rendered
//! through the port. `Oracle.exe neuron-client` writes the same files from
//! the C++ reference (`CppSource/Oracle/oracle_scene_script.cpp`).

mod support;

use neuron_client::scene_script::Vm;
use neuron_client::text::rdl::markup;

use support::{check_suite, input_lines, scene_script, text, unhex};

/// P4.14: the Lua state after each buffer of a case, one fresh state per
/// case (`tests/fixtures/oracle/scene_script.list`).
#[test]
fn scene_script() {
    let mut lines = Vec::new();
    for (case, line) in input_lines("scene_script.list").iter().enumerate() {
        let mut vm = Vm::new();
        for (buffer, text) in line.split(' ').enumerate() {
            let status = scene_script::status(&mut vm, &unhex(text));
            let head =
                format!("{{\"case\":\"{case}\",\"buffer\":\"{buffer}\",\"status\":\"{status}\"}}");
            lines.extend(scene_script::render(&vm, head));
        }
    }
    check_suite("scene_script.jsonl", &lines);
}

/// P4.13, P4.14: `MsvcrtFormatLuaNumber` and `MsvcrtNumericVsnprintf`
/// (`tests/fixtures/oracle/number_text.list`).
#[test]
fn number_text() {
    let lines: Vec<String> = input_lines("number_text.list")
        .iter()
        .map(|line| text::number_line(line))
        .collect();
    check_suite("number_text.jsonl", &lines);
}

/// P4.12: `XMLReader::read_buffer` into an empty list
/// (`tests/fixtures/oracle/rdl_markup.list`).
#[test]
fn rdl_markup() {
    let lines: Vec<String> = input_lines("rdl_markup.list")
        .iter()
        .enumerate()
        .map(|(case, line)| text::markup_line(case, line, markup::read))
        .collect();
    check_suite("rdl_markup.jsonl", &lines);
}
