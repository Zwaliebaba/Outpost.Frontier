//! Scenario comparison (decision O20, closed loop; record 0003 point 8):
//! the scenarios of `CppSource/Oracle/scenarios` run through the Rust
//! world on the original data in `outdir/`, and, with
//! `OUTPOST_ORACLE_DIR` set, their per-tick object states are compared
//! with the oracle's `tick.jsonl` (the first run found under
//! `<oracle dir>/<scenario>/`).
//!
//! The envelopes are not approved yet (record 0003 point 10), so the test
//! reports the measured differences (position, velocity, orientation, hit
//! points, alive state) and fails only on gross divergence: a named
//! object more than `GROSS_POSITION` metres (or half the distance it
//! travelled in the oracle, if more) from the oracle's position, or alive
//! in one run and gone in the other at the last tick.
//!
//! Scenarios whose objects need the AI (an `ai` state graph, or the
//! `goto`, `face` and `dock` directives) are skipped with a note: the
//! behaviours are P2.5. A summary goes to `rust-scenario-replay.txt` in
//! the oracle folder.

mod support;

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

use game_sim::equip::flight::StrafeDir;
use game_sim::object::world::{ShipSpawn, WorldConfig};
use game_sim::object::{Command, ObjHandle, World};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::StepMode;
use support::records::{Json, find_files, read_lines};

/// The gross-divergence bound on position, metres.
const GROSS_POSITION: f64 = 500.0;

#[derive(Clone, Debug)]
struct ObjDesc {
    name: String,
    ship: bool,
    archetype: String,
    loadout: String,
    nickname: String,
    pos: Vector,
    orient: Matrix,
    vel: Vector,
    angvel: Vector,
    hp: f32,
    ai: bool,
}

#[derive(Clone, Debug)]
struct Cmd {
    tick: u32,
    object: String,
    args: Vec<String>,
}

#[derive(Clone, Debug)]
struct Scenario {
    name: String,
    system: String,
    ticks: u32,
    frame: f64,
    sp: bool,
    seed: u32,
    objects: Vec<ObjDesc>,
    commands: Vec<Cmd>,
}

fn scenarios_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../CppSource/Oracle/scenarios")
}

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

/// `HeadingMatrix`: a yaw of `degrees` about +Y.
fn heading(degrees: f64) -> Matrix {
    let r = degrees.to_radians();
    let (s, c) = (r.sin() as f32, r.cos() as f32);
    Matrix::from_rows([[c, 0.0, s], [0.0, 1.0, 0.0], [-s, 0.0, c]])
}

fn parse(path: &Path) -> Option<Scenario> {
    let text = fs::read_to_string(path).ok()?;
    let mut s = Scenario {
        name: path.file_stem()?.to_string_lossy().into_owned(),
        system: String::new(),
        ticks: 0,
        frame: 1.0 / 30.0,
        sp: false,
        seed: 0x4f52_4143,
        objects: Vec::new(),
        commands: Vec::new(),
    };
    for line in text.lines() {
        let line = line.split('#').next().unwrap_or("").trim();
        if line.is_empty() {
            continue;
        }
        let t: Vec<String> = line.split_whitespace().map(str::to_owned).collect();
        match t[0].to_ascii_lowercase().as_str() {
            "system" => s.system = t.get(1)?.clone(),
            "ticks" => s.ticks = t.get(1)?.parse().ok()?,
            "frame" => s.frame = num(t.get(1)?)?,
            "stepping" => s.sp = t.get(1)?.eq_ignore_ascii_case("sp"),
            "seed" => s.seed = t.get(1)?.parse().ok()?,
            "ship" | "solar" => {
                let ship = t[0].eq_ignore_ascii_case("ship");
                let mut o = ObjDesc {
                    name: t.get(1)?.clone(),
                    ship,
                    archetype: if ship {
                        t.get(2)?.clone()
                    } else {
                        String::new()
                    },
                    loadout: if ship {
                        t.get(3)?.clone()
                    } else {
                        String::new()
                    },
                    nickname: if ship {
                        String::new()
                    } else {
                        t.get(2)?.clone()
                    },
                    pos: if ship { vec3(&t, 4)? } else { Vector::ZERO },
                    orient: Matrix::IDENTITY,
                    vel: Vector::ZERO,
                    angvel: Vector::ZERO,
                    hp: -1.0,
                    ai: false,
                };
                let mut at = if ship { 7 } else { 3 };
                while at < t.len() {
                    match t[at].to_ascii_lowercase().as_str() {
                        "psi" => at += 1,
                        "heading" => {
                            o.orient = heading(num(t.get(at + 1)?)?);
                            at += 2;
                        }
                        "vel" => {
                            o.vel = vec3(&t, at + 1)?;
                            at += 4;
                        }
                        "angvel" => {
                            o.angvel = vec3(&t, at + 1)?;
                            at += 4;
                        }
                        "ai" => {
                            o.ai = true;
                            at += 2;
                        }
                        "hp" => {
                            o.hp = num(t.get(at + 1)?)? as f32;
                            at += 2;
                        }
                        _ => at += 1,
                    }
                }
                s.objects.push(o);
            }
            "at" => {
                let range = t.get(1)?;
                let (span, step) = match range.split_once('/') {
                    Some((a, b)) => (a, b.parse().ok()?),
                    None => (range.as_str(), 1u32),
                };
                let (first, last) = match span.split_once("..") {
                    Some((a, b)) => (a.parse().ok()?, b.parse().ok()?),
                    None => (span.parse().ok()?, span.parse().ok()?),
                };
                let mut tick: u32 = first;
                while tick <= last {
                    s.commands.push(Cmd {
                        tick,
                        object: t.get(2)?.clone(),
                        args: t[3..].iter().map(|a| a.to_ascii_lowercase()).collect(),
                    });
                    tick += step;
                }
            }
            _ => {}
        }
    }
    s.commands.sort_by_key(|c| c.tick);
    Some(s)
}

fn needs_ai(s: &Scenario) -> Option<String> {
    if let Some(o) = s.objects.iter().find(|o| o.ai) {
        return Some(format!("{} has a state graph", o.name));
    }
    s.commands
        .iter()
        .find(|c| matches!(c.args[0].as_str(), "goto" | "face" | "dock"))
        .map(|c| format!("the {} directive", c.args[0]))
}

#[derive(Clone, Copy, Debug, Default)]
struct State {
    alive: bool,
    pos: Vector,
    vel: Vector,
    orient: [f32; 9],
    hp: f32,
}

fn run(s: &Scenario) -> Option<BTreeMap<String, Vec<State>>> {
    let mut world = support::world::world(WorldConfig {
        single_player: false,
        step_mode: if s.sp {
            StepMode::SinglePlayer
        } else {
            StepMode::Multiplayer
        },
        server_physics: true,
        seed: s.seed,
    })?;
    let system = create_id(s.system.as_bytes());
    world.load_system(system);
    let mut handles: BTreeMap<String, Option<ObjHandle>> = BTreeMap::new();
    // Only the solars the scenario names: the oracle's server loads the
    // whole system, but nothing else is near the scenario objects, and the
    // contact pass over every solar is slow in a debug build.
    let named: Vec<u32> = s
        .objects
        .iter()
        .filter(|o| !o.ship)
        .map(|o| create_id(o.nickname.as_bytes()))
        .collect();
    if !named.is_empty() {
        world.load_system_solars_where(system, |n| named.contains(&n));
    }
    for o in &s.objects {
        let h = if o.ship {
            let mut spawn = ShipSpawn::new(
                create_id(o.archetype.as_bytes()),
                create_id(o.loadout.as_bytes()),
                o.pos,
                o.orient,
            );
            spawn.velocity = o.vel;
            spawn.angular_velocity = o.angvel;
            spawn.hit_pts = o.hp;
            world.spawn_ship(system, &spawn).ok()
        } else {
            world.find(create_id(o.nickname.as_bytes()))
        };
        handles.insert(o.name.clone(), h);
    }
    let mut out: BTreeMap<String, Vec<State>> = BTreeMap::new();
    let record = |world: &World,
                  out: &mut BTreeMap<String, Vec<State>>,
                  handles: &BTreeMap<String, Option<ObjHandle>>| {
        for (name, h) in handles {
            let st = h.and_then(|h| world.inspect(h)).filter(|i| !i.dead).map_or(
                State::default(),
                |i| State {
                    alive: true,
                    pos: i.position,
                    vel: i.velocity,
                    orient: i.orientation.to_words(),
                    hp: i.hit_pts,
                },
            );
            out.entry(name.clone()).or_default().push(st);
        }
    };
    record(&world, &mut out, &handles);
    let mut next = 0;
    for tick in 1..=s.ticks {
        while next < s.commands.len() && s.commands[next].tick == tick {
            let c = &s.commands[next];
            next += 1;
            let Some(Some(h)) = handles.get(&c.object).copied() else {
                continue;
            };
            let a = &c.args;
            let command = match a[0].as_str() {
                "throttle" => a
                    .get(1)
                    .and_then(|v| num(v))
                    .map(|v| Command::SetThrottle(v as f32)),
                "axis" => vec3(a, 1).map(Command::SetAxisThrottle),
                "nudge" => vec3(a, 1).map(Command::SetNudgeVec),
                "strafe" => {
                    let names = ["none", "left", "right", "up", "down"];
                    names
                        .iter()
                        .position(|n| a.get(1).is_some_and(|x| x == n))
                        .map(|i| Command::SetStrafeDir(StrafeDir::from_value(i as i32)))
                }
                "fire" => {
                    let target = match a.get(1) {
                        Some(t) => handles
                            .get(t)
                            .copied()
                            .flatten()
                            .and_then(|h| world.inspect(h))
                            .map(|i| i.position),
                        None => world.inspect(h).map(|i| {
                            let m = i.orientation.m;
                            Vector::new(
                                (f64::from(i.position.x) - 1000.0 * f64::from(m[0][2])) as f32,
                                (f64::from(i.position.y) - 1000.0 * f64::from(m[1][2])) as f32,
                                (f64::from(i.position.z) - 1000.0 * f64::from(m[2][2])) as f32,
                            )
                        }),
                    };
                    target.map(|t| Command::Fire {
                        guns: world.unguided_guns(h),
                        target: t,
                    })
                }
                _ => None,
            };
            if let Some(command) = command {
                world.command(h, command);
            }
        }
        world.step(system, s.frame as f32);
        let _ = world.take_events();
        record(&world, &mut out, &handles);
    }
    Some(out)
}

fn oracle_ticks(dir: &Path, scenario: &str) -> Option<BTreeMap<String, Vec<State>>> {
    let base = dir.join(scenario);
    let file = find_files(&base, "tick.jsonl").into_iter().next()?;
    let mut out: BTreeMap<String, BTreeMap<u32, State>> = BTreeMap::new();
    for r in read_lines(&file) {
        if r.get("rec").and_then(Json::as_str) != Some("object") {
            continue;
        }
        let (Some(name), Some(tick)) = (
            r.get("obj").and_then(Json::as_str),
            r.get("tick").and_then(Json::as_u32),
        ) else {
            continue;
        };
        let alive = r.get("alive").and_then(Json::as_bool).unwrap_or(false);
        let v = |k: &str| {
            r.get(k)
                .and_then(Json::as_floats)
                .filter(|a| a.len() >= 3)
                .map_or(Vector::ZERO, |a| Vector::new(a[0], a[1], a[2]))
        };
        let orient = r
            .get("orient")
            .and_then(Json::as_floats)
            .and_then(|a| <[f32; 9]>::try_from(a).ok())
            .unwrap_or_default();
        out.entry(name.to_owned()).or_default().insert(
            tick,
            State {
                alive,
                pos: v("pos"),
                vel: v("vel"),
                orient,
                hp: r.get("hp").and_then(Json::as_f32).unwrap_or(0.0),
            },
        );
    }
    Some(
        out.into_iter()
            .map(|(k, m)| (k, m.into_values().collect()))
            .collect(),
    )
}

fn dist(a: Vector, b: Vector) -> f64 {
    let d = [
        f64::from(a.x) - f64::from(b.x),
        f64::from(a.y) - f64::from(b.y),
        f64::from(a.z) - f64::from(b.z),
    ];
    (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt()
}

#[test]
fn scenarios_against_the_oracle() {
    if support::world::game_data().is_none() {
        println!("outdir/ is missing: no scenario was run");
        return;
    }
    let oracle = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from);
    let mut summary = String::new();
    let mut gross = Vec::new();
    let mut files: Vec<PathBuf> = fs::read_dir(scenarios_dir())
        .map(|d| {
            d.flatten()
                .map(|e| e.path())
                .filter(|p| p.extension().is_some_and(|x| x == "txt"))
                .collect()
        })
        .unwrap_or_default();
    files.sort();
    for path in files {
        let Some(s) = parse(&path) else {
            let _ = writeln!(summary, "{}: does not parse", path.display());
            continue;
        };
        if let Some(why) = needs_ai(&s) {
            let _ = writeln!(summary, "{}: skipped, {why} (AI is P2.5)", s.name);
            continue;
        }
        let Some(ours) = run(&s) else {
            let _ = writeln!(summary, "{}: did not run", s.name);
            continue;
        };
        for (name, states) in &ours {
            if let Some(last) = states.last() {
                let _ = writeln!(
                    summary,
                    "{}: {name} after {} ticks: alive {} pos ({:.3}, {:.3}, {:.3}) speed {:.3} hp {}",
                    s.name,
                    states.len() - 1,
                    last.alive,
                    last.pos.x,
                    last.pos.y,
                    last.pos.z,
                    dist(last.vel, Vector::ZERO),
                    last.hp
                );
            }
        }
        let Some(dir) = &oracle else {
            continue;
        };
        let Some(theirs) = oracle_ticks(dir, &s.name) else {
            let _ = writeln!(summary, "{}: no oracle tick.jsonl", s.name);
            continue;
        };
        for (name, ours_states) in &ours {
            let Some(their_states) = theirs.get(name) else {
                continue;
            };
            let n = ours_states.len().min(their_states.len());
            let (mut max_pos, mut max_vel, mut max_orient, mut alive_mismatch) =
                (0.0f64, 0.0f64, 0.0f32, 0usize);
            let mut max_tick = 0;
            for t in 0..n {
                let (a, b) = (&ours_states[t], &their_states[t]);
                if a.alive != b.alive {
                    alive_mismatch += 1;
                    continue;
                }
                if !a.alive {
                    continue;
                }
                let p = dist(a.pos, b.pos);
                if p > max_pos {
                    max_pos = p;
                    max_tick = t;
                }
                max_vel = max_vel.max(dist(a.vel, b.vel));
                for i in 0..9 {
                    max_orient = max_orient.max((a.orient[i] - b.orient[i]).abs());
                }
            }
            let travelled = dist(their_states[0].pos, their_states[n - 1].pos);
            let bound = GROSS_POSITION.max(0.5 * travelled);
            let (fa, fb) = (&ours_states[n - 1], &their_states[n - 1]);
            let _ = writeln!(
                summary,
                "{}: {name}: max position error {max_pos:.4} m (tick {max_tick}), velocity {max_vel:.4} m/s, orientation {max_orient:.6}, alive mismatches {alive_mismatch}, final hp {} vs {}",
                s.name, fa.hp, fb.hp
            );
            if max_pos > bound || fa.alive != fb.alive {
                gross.push(format!(
                    "{}: {name} (position {max_pos:.1} m > {bound:.1} m or alive {} vs {})",
                    s.name, fa.alive, fb.alive
                ));
            }
        }
    }
    print!("{summary}");
    if let Some(dir) = &oracle {
        let _ = fs::write(dir.join("rust-scenario-replay.txt"), &summary);
    }
    assert!(gross.is_empty(), "gross divergence: {gross:?}\n{summary}");
}
