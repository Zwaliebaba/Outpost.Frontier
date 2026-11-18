//! The committed oracle vectors of this crate (`tests/vectors/`), rendered
//! through the port. `Oracle.exe neuron-client` writes the same files from
//! the C++ reference (`CppSource/Oracle/oracle_scene_script.cpp`).

mod support;

use neuron_client::scene_script::Vm;

use support::{check_suite, input_lines, scene_script, unhex};

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
