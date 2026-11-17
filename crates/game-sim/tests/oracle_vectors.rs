//! Oracle vectors of work package P2.1: asteroid ids and cube orientations,
//! the string cache and the faction registry. The inputs are under
//! `tests/fixtures/oracle/`, written by `make_game_sim_fixtures.py`; the C++
//! side is `CppSource/Oracle/oracle_game_suites.cpp`, run as
//! `Oracle.exe game-sim <fixtures> <output>`.
//!
//! `CAsteroidCube::compute_asteroid_id` is private in the C++ and has no
//! export the harness can call; its unit tests are in the crate.
//!
//! Every suite is Tier A and compared as text except `asteroid_orient`,
//! whose matrix went through sine and cosine and is compared within a
//! bound (the draws and the empty/filled answer are exact).

mod support;

use std::collections::HashMap;
use std::fs;
use std::path::Path;

use game_sim::data::Reports;
use game_sim::data::asteroid::{
    AxisRotations, closest_cube_coord, compute_cube_id, compute_cube_orientation,
};
use game_sim::data::reputation::{Relation, Reputation};
use game_sim::data::string_cache::{CacheString, StringCache};
use neuron_core::ini::IniReader;
use neuron_core::math::Matrix;
use neuron_core::vfs::Host;

use support::data::Tolerance;
use support::render::Line;
use support::{check_suite, f32_hex, f32_text, fixtures, hex, input_lines, read_fixture};

fn unhex(text: &str) -> Vec<u8> {
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&text[i..i + 2], 16).unwrap_or_else(|e| panic!("{text}: {e}")))
        .collect()
}

#[test]
fn asteroid_ids() {
    let mut lines = Vec::new();
    for line in input_lines("asteroid.list") {
        let v: Vec<f32> = line.split(' ').map(f32_hex).collect();
        let pos = [v[0], v[1], v[2]];
        lines.push(
            Line::new()
                .add("pos", line.replace(' ', ","))
                .add("cube", format!("{:08x}", compute_cube_id(pos)))
                .done(),
        );
    }
    for coord in [
        -1000,
        -300,
        -256,
        -1,
        0,
        1,
        255,
        256,
        300,
        1000,
        i32::MAX,
        i32::MIN,
    ] {
        for size in [256, 1000, 1, -256] {
            lines.push(
                Line::new()
                    .add("coord", coord.to_string())
                    .add("size", size.to_string())
                    .add(
                        "closest",
                        closest_cube_coord(coord, size, size / 2).to_string(),
                    )
                    .done(),
            );
        }
    }
    check_suite("asteroid_ids.jsonl", &lines);
}

/// Tier B: the matrix within 4 ulps or 1e-6 (entries that cancel to near
/// zero).
#[test]
fn asteroid_orient() {
    let mut lines = Vec::new();
    for line in input_lines("orient.list") {
        let parts: Vec<&str> = line.split(' ').collect();
        let seed: u32 = parts[0].parse().unwrap_or_else(|e| panic!("{line}: {e}"));
        let freq = f32_hex(parts[1]);
        let mut rotations = AxisRotations::default();
        for (i, word) in parts[2..14].iter().enumerate() {
            rotations.angles[i / 4][i % 4] = f32_hex(word);
        }
        let mut m = Matrix::IDENTITY;
        let filled = compute_cube_orientation(seed, &mut m, &rotations, freq);
        let words: Vec<String> = m.to_words().iter().map(|&w| f32_text(w)).collect();
        lines.push(
            Line::new()
                .add("seed", seed.to_string())
                .add("freq", parts[1])
                .add("filled", u8::from(filled).to_string())
                .add("m", words.join(","))
                .done(),
        );
    }
    let name = "asteroid_orient.jsonl";
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(name);
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        check_suite(name, &lines);
        return;
    }
    let tolerance = Tolerance {
        keys: &["m"],
        ulps: 4,
        absolute: 1.0e-6,
    };
    let committed = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let committed: Vec<&str> = committed.lines().collect();
    assert_eq!(committed.len(), lines.len(), "{name}: line count");
    for (index, (want, got)) in committed.iter().zip(&lines).enumerate() {
        if let Err(why) = tolerance.compare_line(want, got) {
            panic!(
                "{name} line {}: {why}\n  oracle: {want}\n  rust:   {got}",
                index + 1
            );
        }
    }
}

#[test]
fn string_cache() {
    let mut cache = StringCache::new();
    let mut first: HashMap<CacheString, usize> = HashMap::new();
    let mut lines = Vec::new();
    for (index, line) in input_lines("strings.list").iter().enumerate() {
        let (op, arg) = line.split_once(' ').unwrap_or((line.as_str(), ""));
        let result = match op {
            "i" => Some(cache.alloc(&unhex(arg), false)),
            "t" => Some(cache.alloc(&unhex(arg), true)),
            "bay" => Some(cache.cargo_bay()),
            _ => {
                cache.shutdown();
                None
            }
        };
        let mut out = Line::new();
        out.add("op", op).add("in", arg);
        if let Some(handle) = result {
            let seen = if op == "bay" {
                first
                    .get(&handle)
                    .map_or(-1, |&i| i64::try_from(i).unwrap_or(-1))
            } else {
                i64::try_from(*first.entry(handle).or_insert(index)).unwrap_or(-1)
            };
            out.add("out", hex(cache.get(handle)))
                .add("first", seen.to_string());
        }
        lines.push(out.done());
    }
    check_suite("string_cache.jsonl", &lines);
}

fn name_arg(text: &str) -> Vec<u8> {
    if text == "-" { Vec::new() } else { unhex(text) }
}

fn feeling(rep: &Reputation, from: u32, to: u32) -> String {
    let (result, value) = rep.feelings_towards(from, to);
    format!("{result},{}", value.map_or("-".to_owned(), f32_text))
}

#[test]
fn reputation() {
    let source = Host::new(fixtures());
    let mut rep = Reputation::default();
    let mut lines = Vec::new();
    let started = rep.startup(&source, b"groups.ini");
    lines.push(
        Line::new()
            .add("startup", u8::from(started).to_string())
            .add("count", rep.group_count().to_string())
            .done(),
    );
    let ids: Vec<u32> = rep.groups().map(|(id, _)| id).collect();
    for &id in &ids {
        lines.push(
            Line::new()
                .add("group", format!("{id:04x}"))
                .add("nickname", hex(rep.get_nickname(id).unwrap_or_default()))
                .add("name", rep.get_name(id).to_string())
                .add("info", rep.get_info_card(id).to_string())
                .add("short", rep.get_short_name(id).to_string())
                .done(),
        );
    }
    let mut reader = IniReader::new();
    assert!(reader.open(b"feelings.ini", read_fixture("feelings.ini")));
    let mut reports = Reports::default();
    rep.load_feelings(&mut reader, &mut reports);
    for &from in &ids {
        for &to in &ids {
            lines.push(
                Line::new()
                    .add("from", format!("{from:04x}"))
                    .add("to", format!("{to:04x}"))
                    .add("feel", feeling(&rep, from, to))
                    .done(),
            );
        }
    }
    for line in input_lines("reputation.list") {
        let parts: Vec<&str> = line.split(' ').collect();
        let mut out = Line::new();
        out.add("op", parts[0]);
        match parts[0] {
            "id" => {
                out.add("group", format!("{:08x}", rep.get_id(&name_arg(parts[1]))));
            }
            "feel" => {
                let from = rep.get_id(&name_arg(parts[1]));
                let to = rep.get_id(&name_arg(parts[2]));
                out.add("feel", feeling(&rep, from, to));
            }
            "affect" => {
                let from = rep.get_id(&name_arg(parts[1]));
                let to = rep.get_id(&name_arg(parts[2]));
                let result = rep.affect_relation(from, to, f32_hex(parts[3]));
                out.add("result", result.to_string())
                    .add("feel", feeling(&rep, from, to));
            }
            "story" => {
                let id = game_sim::data::reputation::group_id(&name_arg(parts[1]));
                out.add(
                    "story",
                    u8::from(Reputation::is_story_faction(id)).to_string(),
                );
            }
            _ => {
                let relations: Vec<Relation> = parts[2..]
                    .iter()
                    .map(|pair| {
                        let (name, value) = pair.split_once(':').unwrap_or((pair, "0"));
                        Relation {
                            group_id: game_sim::data::reputation::group_id(&name_arg(name)),
                            feeling: f32_hex(value),
                        }
                    })
                    .collect();
                let name = name_arg(parts[1]);
                rep.set(&name, &relations);
                let id = game_sim::data::reputation::group_id(&name);
                let feelings: Vec<String> = rep
                    .groups()
                    .map(|(to, _)| format!("{to:04x}:{}", feeling(&rep, id, to)))
                    .collect();
                out.add("group", format!("{id:04x}"))
                    .add("nickname", hex(rep.get_nickname(id).unwrap_or_default()))
                    .add("feelings", feelings.join(";"));
            }
        }
        lines.push(out.done());
    }
    check_suite("reputation.jsonl", &lines);
}
