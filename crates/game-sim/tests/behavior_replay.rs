//! Behaviour-tick replay (record 0003 point 7): exploration pass.

mod support;

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::fs;

use support::records::{Json, find_files, oracle_dir};

fn short(line: &str, n: usize) -> String {
    if line.len() > n {
        format!("{}…", &line[..n])
    } else {
        line.to_owned()
    }
}

#[test]
fn behavior_ticks_against_the_oracle() {
    let Some(dir) = oracle_dir("behaviour ticks") else {
        return;
    };
    let mut summary = String::new();
    let mut seen = std::collections::BTreeSet::new();
    for file in find_files(&dir, "ai.jsonl") {
        let scenario = file
            .parent()
            .and_then(|p| p.parent())
            .map(|p| p.to_path_buf())
            .unwrap_or_default();
        if !seen.insert(scenario) {
            continue;
        }
        let text = fs::read_to_string(&file).unwrap_or_default();
        let _ = writeln!(summary, "== {}", file.display());
        let mut by_behavior: BTreeMap<i32, usize> = BTreeMap::new();
        let mut shown = 0;
        for line in text.lines().take(40) {
            let _ = writeln!(summary, "{}", short(line, 900));
        }
        for line in text.lines() {
            let Ok(j) = support::records::parse(line) else {
                continue;
            };
            let rec = j.get("rec").and_then(Json::as_str).unwrap_or("");
            if rec == "ai_in" {
                *by_behavior
                    .entry(j.get("behavior").and_then(Json::as_i32).unwrap_or(-9))
                    .or_default() += 1;
            }
            if rec == "ai_out" && shown < 120 {
                shown += 1;
                let rand = j
                    .get("rand")
                    .and_then(Json::as_array)
                    .map_or(0, <[Json]>::len);
                let mods: Vec<&str> = j
                    .get("rand_mod")
                    .and_then(Json::as_array)
                    .map(|a| a.iter().filter_map(Json::as_str).collect())
                    .unwrap_or_default();
                let _ = writeln!(
                    summary,
                    "seq {:?} tick {:?} beh {:?} dir {} timers {} rand {rand} {mods:?}",
                    j.get("seq").and_then(Json::as_u32),
                    j.get("tick").and_then(Json::as_u32),
                    j.get("behavior").and_then(Json::as_i32),
                    short(&format!("{:?}", j.get("directive")), 80),
                    short(&format!("{:?}", j.get("timers")), 200),
                );
            }
        }
        let _ = writeln!(summary, "ticks by behaviour: {by_behavior:?}");
        if let Some(calls) = file.parent().map(|p| p.join("calls.jsonl")) {
            let text = fs::read_to_string(&calls).unwrap_or_default();
            for line in text
                .lines()
                .filter(|l| l.contains("\"affect\"") || l.contains("behavior_in"))
                .take(80)
            {
                let _ = writeln!(summary, "{}", short(line, 400));
            }
        }
        if let Some(session) = file.parent().map(|p| p.join("session.jsonl")) {
            let text = fs::read_to_string(&session).unwrap_or_default();
            for line in text.lines().take(20) {
                let _ = writeln!(summary, "{}", short(line, 400));
            }
        }
        break_after_first(&mut summary);
    }
    print!("{summary}");
    let _ = fs::write(dir.join("rust-behavior-replay.txt"), &summary);
}

fn break_after_first(_s: &mut String) {}
