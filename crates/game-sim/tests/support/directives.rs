//! The directives of the scenario commands (`SubmitScenarioDirective` of
//! `CppSource/Oracle/oracle_sim.cpp`), shared by `scenario_replay` (which
//! submits them to the Rust world) and `behavior_replay` (which submits
//! them to its managers between the recorded behaviour ticks, as the
//! harness does before the frame's step).

use game_sim::ai::behavior::{DirectiveOp, FaceOp, FollowOp, GotoOp, GuideOp};
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
/// first) submit, as [`directive_with`], with every object named 0 (enough
/// to tell the behaviour it runs).
#[must_use]
pub fn directive(a: &[String]) -> Option<DirectiveOp> {
    directive_with(a, &|_| Some(0))
}

/// The follow op of `follow`'s options (`distance d`, `offset x y z`,
/// `range r`) or `guide`'s (`distance d`, `offset x y z`); `range` is the
/// guide's arrival range, returned apart.
fn follow_options(a: &[String], at: usize, op: &mut FollowOp) -> Option<Option<f32>> {
    let mut range = None;
    let mut at = at;
    while at < a.len() {
        match a[at].as_str() {
            "distance" => {
                op.distance = num(a.get(at + 1)?)? as f32;
                at += 2;
            }
            "offset" => {
                op.offset = vec3(a, at + 1)?;
                at += 4;
            }
            "range" => {
                range = Some(num(a.get(at + 1)?)? as f32);
                at += 2;
            }
            _ => return None,
        }
    }
    Some(range)
}

/// The directive a scenario command's arguments (lower case, the verb
/// first) submit: `goto x y z [range r] [cruise]` (a point, range 100, no
/// cruise unless asked), `face x y z` (a point), `follow <ship> [distance
/// d] [offset x y z] [range r]`, `guide <ship> x y z [range r] [distance
/// d] [offset x y z]` (`oracle_sim.cpp`'s ops: the guide's goto part as
/// `goto`'s, the guided ship in its `field_5c` and as the follow target),
/// `idle`, `cancel`; `None` for any other command. `id` names the objects.
#[must_use]
pub fn directive_with(a: &[String], id: &dyn Fn(&str) -> Option<u32>) -> Option<DirectiveOp> {
    match a.first()?.as_str() {
        "follow" => {
            let mut op = FollowOp {
                target: id(a.get(1)?)?,
                ..FollowOp::default()
            };
            if let Some(range) = follow_options(a, 2, &mut op)? {
                op.range = range;
            }
            Some(DirectiveOp::Follow(op))
        }
        "guide" => {
            let guided = id(a.get(1)?)?;
            let mut op = GuideOp {
                goto: GotoOp {
                    kind: 1,
                    destination: vec3(a, 2)?,
                    range: 100.0,
                    field_58: 0,
                    cruise: 0,
                    no_cruise: 1,
                    field_5c: guided,
                    ..GotoOp::default()
                },
                follow: FollowOp {
                    target: guided,
                    ..FollowOp::default()
                },
                ..GuideOp::default()
            };
            if let Some(range) = follow_options(a, 5, &mut op.follow)? {
                op.goto.range = range;
            }
            Some(DirectiveOp::Guide(Box::new(op)))
        }
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
