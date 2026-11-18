//! The numbers the scene-script parser registers as Lua globals
//! (`RegisterSceneScriptLuaConstants` in
//! `CppSource/SceneScript/SceneScriptLua.cpp`) are the port's
//! `SCENE_CONSTANTS` and the oracle's copy (`oracle_scene_script.cpp`), in
//! the same order. Removed with the C++ in P7.3 (P7.4).

use std::fs;
use std::path::Path;

use neuron_client::scene_script::SCENE_CONSTANTS;

fn cpp(relative: &str) -> Option<String> {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../CppSource")
        .join(relative);
    fs::read_to_string(path).ok()
}

/// The quoted strings of `text`, with the number that follows each, if any.
fn quoted(text: &str) -> Vec<(String, Option<f64>)> {
    let mut out = Vec::new();
    let mut rest = text;
    while let Some(start) = rest.find('"') {
        let after = &rest[start + 1..];
        let end = after.find('"').expect("closing quote");
        let name = after[..end].to_owned();
        rest = &after[end + 1..];
        let tail = rest.trim_start_matches([',', ' ', '\t']);
        let number: String = tail
            .chars()
            .take_while(|c| c.is_ascii_digit() || *c == '.' || *c == '-')
            .collect();
        out.push((name, number.parse().ok()));
    }
    out
}

fn between<'a>(text: &'a str, start: &str, end: &str) -> &'a str {
    let from = text
        .find(start)
        .unwrap_or_else(|| panic!("{start} not found"))
        + start.len();
    let to = text[from..]
        .find(end)
        .unwrap_or_else(|| panic!("{end} not found"))
        + from;
    &text[from..to]
}

#[test]
fn the_reference_registers_these_constants() {
    let Some(source) = cpp("SceneScript/SceneScriptLua.cpp") else {
        println!("CppSource is not present: nothing to compare");
        return;
    };
    let body = between(
        &source,
        "void RegisterSceneScriptLuaConstants()",
        "G_bLuaConstantsRegistered = true;",
    );
    let mut expected: Vec<(String, f64)> = Vec::new();
    // Entity types and axes by their index; events by their id.
    for (i, (name, _)) in quoted(between(body, "EntityNames[] = {", "};"))
        .into_iter()
        .enumerate()
    {
        expected.push((name, i as f64));
    }
    for (name, id) in quoted(between(body, "EventIds[] = {", "};")) {
        expected.push((name, id.expect("event id")));
    }
    for (i, (name, _)) in quoted(between(body, "AxisNames[] = {", "};"))
        .into_iter()
        .enumerate()
    {
        expected.push((name, i as f64));
    }
    let calls = &body[body.find("AxisCount; ++Index)").expect("axis loop")..];
    for line in calls.lines().filter(|l| l.contains("SetGlobalNumber(\"")) {
        let (name, value) = quoted(line).remove(0);
        expected.push((name, value.expect("constant value")));
    }
    let port: Vec<(String, f64)> = SCENE_CONSTANTS
        .iter()
        .map(|&(n, v)| (n.to_owned(), v))
        .collect();
    assert_eq!(port, expected, "SCENE_CONSTANTS");

    let oracle = cpp("Oracle/oracle_scene_script.cpp").expect("the oracle source");
    let copy: Vec<(String, f64)> =
        quoted(between(&oracle, "const FConstant Constants[] = {", "};"))
            .into_iter()
            .map(|(n, v)| (n, v.expect("oracle value")))
            .collect();
    assert_eq!(copy, expected, "oracle_scene_script.cpp");
}
