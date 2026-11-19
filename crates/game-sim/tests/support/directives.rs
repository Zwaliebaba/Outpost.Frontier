//! The directives of the scenario commands (`SubmitScenarioDirective` of
//! `CppSource/Oracle/oracle_sim.cpp`), shared by `scenario_replay` (which
//! submits them to the Rust world) and `behavior_replay` (which submits
//! them to its managers between the recorded behaviour ticks, as the
//! harness does before the frame's step).

use game_sim::ai::behavior::{DirectiveOp, DockOp, FaceOp, GotoOp, InstantTradeLaneOp, LaunchOp};
use neuron_core::math::Vector;

fn num(t: &str) -> Option<f64> {
    t.parse().ok()
}

fn vec3(t: &[String], at: usize) -> Option<Vector> {
    Some(Vector::new(
        num(t.get(at)?)? as f32,
        num(t.get(at + 1)?)? as f32,
        num(t.get(at + 2)?)? as f32,
    ))
}

/// The directive a scenario command's arguments (lower case, the verb
/// first) submit: `goto x y z [range r] [cruise]` (a point, range 100, no
/// cruise unless asked), `face x y z` (a point), `idle`, `cancel`; `None`
/// for any other command.
#[must_use]
pub fn directive(a: &[String]) -> Option<DirectiveOp> {
    match a.first()?.as_str() {
        "goto" => {
            let mut op = GotoOp {
                kind: 1,
                destination: vec3(a, 1)?,
                range: 100.0,
                field_58: 0,
                cruise: 0,
                no_cruise: 1,
                ..GotoOp::default()
            };
            let mut at = 4;
            while at < a.len() {
                match a[at].as_str() {
                    "range" => {
                        op.range = num(a.get(at + 1)?)? as f32;
                        at += 2;
                    }
                    "cruise" => {
                        op.cruise = 1;
                        op.no_cruise = 0;
                        at += 1;
                    }
                    _ => return None,
                }
            }
            Some(DirectiveOp::Goto(op))
        }
        "face" if a.len() == 4 => Some(DirectiveOp::Face(FaceOp {
            mode: 2,
            point: vec3(a, 1)?,
            ..FaceOp::default()
        })),
        "idle" if a.len() == 1 => Some(DirectiveOp::Idle),
        "cancel" if a.len() == 1 => Some(DirectiveOp::Cancel),
        _ => None,
    }
}

/// As [`directive`], and the directives that name objects, whose ids
/// `resolve` gives for the scenario's names: `dock <target>`
/// (`DirectiveDockOp` with its defaults), `launch <target> <index>
/// <terminal>` and `tradelane <entrance> <exit|none> [speed]`.
#[must_use]
pub fn directive_with(a: &[String], resolve: &dyn Fn(&str) -> Option<u32>) -> Option<DirectiveOp> {
    match a.first()?.as_str() {
        "dock" if a.len() == 2 => Some(DirectiveOp::Dock(DockOp {
            dock_object: resolve(&a[1])?,
            ..DockOp::default()
        })),
        "launch" if a.len() == 4 => Some(DirectiveOp::Launch(LaunchOp {
            launch_object: resolve(&a[1])?,
            index: num(&a[2])? as i32,
            terminal: num(&a[3])? as i32,
        })),
        "tradelane" if a.len() == 3 || a.len() == 4 => {
            Some(DirectiveOp::InstantTradeLane(InstantTradeLaneOp {
                entrance: resolve(&a[1])?,
                exit: if a[2] == "none" { 0 } else { resolve(&a[2])? },
                speed: a.get(3).and_then(|t| num(t)).map_or(0.0, |v| v as f32),
            }))
        }
        _ => directive(a),
    }
}

/// The `at` commands of a scenario file: tick, object name, arguments
/// (lower case), in tick order (`at FIRST..LAST/STEP` expanded).
#[must_use]
pub fn commands(text: &str) -> Vec<(u32, String, Vec<String>)> {
    let mut out = Vec::new();
    for line in text.lines() {
        let t: Vec<&str> = line.split_whitespace().collect();
        if t.first() != Some(&"at") || t.len() < 4 {
            continue;
        }
        let (span, step) = match t[1].split_once('/') {
            Some((a, b)) => (a, b.parse().unwrap_or(1u32)),
            None => (t[1], 1u32),
        };
        let (first, last) = match span.split_once("..") {
            Some((a, b)) => (a.parse().unwrap_or(0u32), b.parse().unwrap_or(0u32)),
            None => {
                let v = span.parse().unwrap_or(0u32);
                (v, v)
            }
        };
        let mut tick = first;
        while tick <= last {
            out.push((
                tick,
                t[2].to_owned(),
                t[3..].iter().map(|a| a.to_ascii_lowercase()).collect(),
            ));
            tick += step.max(1);
        }
    }
    out.sort_by_key(|c| c.0);
    out
}
