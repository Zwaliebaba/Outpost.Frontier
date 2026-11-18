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
//! The report also shows what a divergence needs to be explained: for each
//! compared object the first tick at which its position, hit points or
//! shield leave a small bound, with both sides' values around it; each
//! munition hit (the sub-object hit, the hull and shield before and after)
//! paired by tick and order with the oracle's `calls.jsonl`; and for each
//! ship and named solar the closest approach on both sides, the Rust
//! collision events and the oracle's `events.jsonl` collisions.
//!
//! Scenarios whose objects need the AI (an `ai` state graph, or the
//! `goto`, `face` and `dock` directives) are skipped with a note: the
//! behaviours are P2.5. So is a scenario whose AI ships have a `faction`
//! (the dogfight): its pilots need the combat behaviours. A summary goes
//! to `rust-scenario-replay.txt` in the oracle folder.

mod support;

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

use game_sim::equip::flight::StrafeDir;
use game_sim::equip::{EquipKind, SUBOBJ_ID_ROOT, types};
use game_sim::object::world::{ShipSpawn, WorldConfig};
use game_sim::object::{Command, DamageList, ObjHandle, World, WorldEvent};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::StepMode;
use support::records::{Json, find_files, read_lines};

/// The gross-divergence bound on position, metres.
const GROSS_POSITION: f64 = 500.0;

/// The report's divergence bounds: position (m), hit points and shield
/// hit points (absolute).
const REPORT_POSITION: f64 = 0.01;
const REPORT_HIT_PTS: f32 = 1e-3;

/// Ticks shown on each side of a first divergence.
const REPORT_WINDOW: usize = 2;

/// Munition hits listed per scenario.
const REPORT_HITS: usize = 40;

/// Collision events listed per scenario and side.
const REPORT_COLLISIONS: usize = 10;

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
    /// The `pilot` option: the personality's `[Pilot]` nickname.
    pilot: Option<String>,
    /// The `faction` option: the reputation group of the ship's vibe.
    faction: Option<String>,
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
                    pilot: None,
                    faction: None,
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
                        "pilot" => {
                            o.pilot = Some(t.get(at + 1)?.clone());
                            at += 2;
                        }
                        "faction" => {
                            o.faction = Some(t.get(at + 1)?.clone());
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
    // A hostile AI ship (the dogfight): its pilot fights.
    let fighters: Vec<String> = s
        .objects
        .iter()
        .filter(|o| o.ai && o.faction.is_some())
        .map(|o| match &o.pilot {
            Some(pilot) => format!("{} ({pilot})", o.name),
            None => o.name.clone(),
        })
        .collect();
    if !fighters.is_empty() {
        return Some(format!(
            "{}: the pilot needs combat behaviours",
            fighters.join(" and ")
        ));
    }
    if let Some(o) = s.objects.iter().find(|o| o.ai) {
        return Some(format!("{} has a state graph", o.name));
    }
    s.commands
        .iter()
        .find(|c| matches!(c.args[0].as_str(), "goto" | "face" | "dock"))
        .map(|c| format!("the {} directive", c.args[0]))
}

/// An equipped object's first shield.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct ShieldState {
    id: u16,
    hp: f32,
    max: f32,
    online: bool,
}

#[derive(Clone, Copy, Debug, Default)]
struct State {
    alive: bool,
    pos: Vector,
    vel: Vector,
    orient: [f32; 9],
    hp: f32,
    /// `None` when the record does not carry it (an older oracle record,
    /// or an object without equipment).
    power: Option<f32>,
    /// `None` without a shield, or when the record does not carry it.
    shield: Option<ShieldState>,
}

/// One munition hit: the target, the sub-object, the hull and shield
/// before and after.
#[derive(Clone, Debug)]
struct Hit {
    tick: u32,
    target: String,
    sub: u16,
    /// The oracle's `sub_kind`; on the Rust side from the shield's id.
    kind: String,
    hull: (f32, f32),
    shield: Option<(f32, f32)>,
    entries: usize,
}

/// One collision event.
#[derive(Clone, Debug)]
struct Contact {
    tick: u32,
    a: String,
    b: String,
    damage: f32,
}

/// What a run produced: the per-tick states, the munition hits and the
/// collisions.
#[derive(Default)]
struct Run {
    states: BTreeMap<String, Vec<State>>,
    hits: Vec<Hit>,
    contacts: Vec<Contact>,
}

fn shield_of(world: &World, h: ObjHandle) -> (Option<f32>, Option<ShieldState>) {
    let Some(eq) = world.get(h).and_then(|o| o.as_equipped()) else {
        return (None, None);
    };
    let shield = eq
        .equipment
        .first(types::SHIELD)
        .and_then(|e| match &e.kind {
            EquipKind::Shield(s) => Some(ShieldState {
                id: e.id,
                hp: s.hit_pts,
                max: s.max_hit_pts,
                online: s.online,
            }),
            _ => None,
        });
    (Some(eq.power), shield)
}

/// The Rust side of a hit: the target's hull and shield as the world had
/// them before the hit (so shield regeneration earlier in the frame is in
/// "before", as in the oracle's pre-hit state), then the damage list's
/// entries applied, in order.
fn rust_hit(
    tick: u32,
    target: &str,
    sub: u16,
    list: &DamageList,
    before: (f32, Option<(u16, f32)>),
    running: &mut State,
) -> Hit {
    running.hp = before.0;
    if let (Some(sh), Some((id, hp))) = (running.shield.as_mut(), before.1)
        && sh.id == id
    {
        sh.hp = hp;
    }
    let hull_before = running.hp;
    let shield_before = running.shield;
    for e in &list.entries {
        if e.sub_obj_id == SUBOBJ_ID_ROOT {
            running.hp = e.hit_pts;
        } else if let Some(sh) = running.shield.as_mut()
            && sh.id == e.sub_obj_id
        {
            sh.hp = e.hit_pts.min(sh.max);
        }
    }
    let kind = match shield_before {
        Some(sh) if sh.id == sub => "shield",
        _ if sub == SUBOBJ_ID_ROOT => "root",
        _ => "other",
    };
    Hit {
        tick,
        target: target.to_owned(),
        sub,
        kind: kind.to_owned(),
        hull: (hull_before, running.hp),
        shield: shield_before.zip(running.shield).map(|(a, b)| (a.hp, b.hp)),
        entries: list.entries.len(),
    }
}

fn run(s: &Scenario) -> Option<Run> {
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
    let names: BTreeMap<u32, String> = handles
        .iter()
        .filter_map(|(n, h)| {
            h.and_then(|h| world.get(h))
                .map(|o| (o.simple.id, n.clone()))
        })
        .filter(|(id, _)| *id != 0)
        .collect();
    let name_of = |id: u32| names.get(&id).cloned().unwrap_or_else(|| format!("#{id}"));
    let mut out = Run::default();
    let record = |world: &World,
                  out: &mut BTreeMap<String, Vec<State>>,
                  handles: &BTreeMap<String, Option<ObjHandle>>| {
        for (name, h) in handles {
            let st = h
                .and_then(|h| world.inspect(h).map(|i| (h, i)))
                .filter(|(_, i)| !i.dead)
                .map_or(State::default(), |(h, i)| {
                    let (power, shield) = shield_of(world, h);
                    State {
                        alive: true,
                        pos: i.position,
                        vel: i.velocity,
                        orient: i.orientation.to_words(),
                        hp: i.hit_pts,
                        power,
                        shield,
                    }
                });
            out.entry(name.clone()).or_default().push(st);
        }
    };
    record(&world, &mut out.states, &handles);
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
        let mut running: BTreeMap<String, State> = out
            .states
            .iter()
            .filter_map(|(n, v)| v.last().map(|st| (n.clone(), *st)))
            .collect();
        for ev in world.take_events() {
            match ev {
                WorldEvent::MunitionHit {
                    target,
                    sub,
                    damage,
                    hull_before,
                    shield_before,
                    ..
                } => {
                    let name = name_of(target);
                    let st = running.entry(name.clone()).or_default();
                    let before = (hull_before, shield_before);
                    out.hits
                        .push(rust_hit(tick, &name, sub, &damage, before, st));
                }
                WorldEvent::Collision { a, b, damage, .. } => out.contacts.push(Contact {
                    tick,
                    a: name_of(a),
                    b: name_of(b),
                    damage,
                }),
                _ => {}
            }
        }
        record(&world, &mut out.states, &handles);
    }
    Some(out)
}

/// The oracle's run of a scenario: the first `tick.jsonl` under
/// `<oracle dir>/<scenario>/`, and the `events.jsonl` and `calls.jsonl`
/// beside it.
fn oracle_run(dir: &Path, scenario: &str) -> Option<Run> {
    let base = dir.join(scenario);
    let file = find_files(&base, "tick.jsonl").into_iter().next()?;
    let folder = file.parent().unwrap_or(&base);
    let mut run = Run {
        states: oracle_ticks(&file),
        ..Run::default()
    };
    let calls = folder.join("calls.jsonl");
    if calls.is_file() {
        run.hits = oracle_hits(&calls);
    }
    let events = folder.join("events.jsonl");
    if events.is_file() {
        for r in read_lines(&events) {
            if r.get("rec").and_then(Json::as_str) != Some("collision") {
                continue;
            }
            run.contacts.push(Contact {
                tick: r.get("tick").and_then(Json::as_u32).unwrap_or(0),
                a: record_name(&r, "a", "a_obj"),
                b: record_name(&r, "b", "b_obj"),
                damage: r.get("damage").and_then(Json::as_f32).unwrap_or(0.0),
            });
        }
    }
    Some(run)
}

/// A record's scenario name for an object, or `#<id>`.
fn record_name(r: &Json, id: &str, obj: &str) -> String {
    r.get(obj).and_then(Json::as_str).map_or_else(
        || format!("#{}", r.get(id).and_then(Json::as_u32).unwrap_or(0)),
        str::to_owned,
    )
}

/// The oracle's munition hits (`calls.jsonl` `munition_hit` with its
/// `state`): the hull and the hit sub-object before, the entries after.
fn oracle_hits(file: &Path) -> Vec<Hit> {
    let mut out = Vec::new();
    for r in read_lines(file) {
        if r.get("rec").and_then(Json::as_str) != Some("munition_hit") {
            continue;
        }
        let sub = r
            .get("sub")
            .and_then(Json::as_u32)
            .and_then(|v| u16::try_from(v).ok())
            .unwrap_or(0);
        let state = r.get("state");
        let st = |k: &str| state.and_then(|s| s.get(k));
        let kind = st("sub_kind")
            .and_then(Json::as_str)
            .unwrap_or("?")
            .to_owned();
        let hull_before = st("hp").and_then(Json::as_f32).unwrap_or(f32::NAN);
        let sub_before = st("sub_hp").and_then(Json::as_f32);
        let entries: Vec<(u16, f32)> = r
            .get("damage")
            .and_then(|d| d.get("entries"))
            .and_then(Json::as_array)
            .map(|a| {
                a.iter()
                    .filter_map(|e| {
                        let e = e.as_array()?;
                        let id = u16::try_from(e.first()?.as_u32()?).ok()?;
                        Some((id, e.get(1)?.as_f32()?))
                    })
                    .collect()
            })
            .unwrap_or_default();
        let after = |id: u16| entries.iter().rev().find(|(e, _)| *e == id).map(|e| e.1);
        let hull_after = after(SUBOBJ_ID_ROOT).unwrap_or(hull_before);
        let shield = if kind == "shield" {
            sub_before.map(|b| (b, after(sub).unwrap_or(b)))
        } else {
            None
        };
        out.push(Hit {
            tick: r.get("tick").and_then(Json::as_u32).unwrap_or(0),
            target: record_name(&r, "target", "target_obj"),
            sub,
            kind,
            hull: (hull_before, hull_after),
            shield,
            entries: entries.len(),
        });
    }
    out
}

fn oracle_ticks(file: &Path) -> BTreeMap<String, Vec<State>> {
    let mut out: BTreeMap<String, BTreeMap<u32, State>> = BTreeMap::new();
    for r in read_lines(file) {
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
        let shield = r
            .get("shield")
            .filter(|v| !v.is_null())
            .map(|v| ShieldState {
                id: v
                    .get("id")
                    .and_then(Json::as_u32)
                    .and_then(|i| u16::try_from(i).ok())
                    .unwrap_or(0),
                hp: v.get("hp").and_then(Json::as_f32).unwrap_or(f32::NAN),
                max: v.get("hp_max").and_then(Json::as_f32).unwrap_or(f32::NAN),
                online: v.get("online").and_then(Json::as_bool).unwrap_or(false),
            });
        out.entry(name.to_owned()).or_default().insert(
            tick,
            State {
                alive,
                pos: v("pos"),
                vel: v("vel"),
                orient,
                hp: r.get("hp").and_then(Json::as_f32).unwrap_or(0.0),
                power: r.get("power").and_then(Json::as_f32),
                shield,
            },
        );
    }
    out.into_iter()
        .map(|(k, m)| (k, m.into_values().collect()))
        .collect()
}

fn fmt_state(st: &State) -> String {
    if !st.alive {
        return "gone".to_owned();
    }
    let shield = st.shield.map_or_else(
        || "-".to_owned(),
        |sh| {
            format!(
                "{}/{}{}",
                sh.hp,
                sh.max,
                if sh.online { "" } else { " off" }
            )
        },
    );
    format!(
        "pos ({:.3}, {:.3}, {:.3}) speed {:.3} hp {} shield {shield} power {}",
        st.pos.x,
        st.pos.y,
        st.pos.z,
        dist(st.vel, Vector::ZERO),
        st.hp,
        st.power.map_or_else(|| "-".to_owned(), |p| p.to_string())
    )
}

/// Whether a metric of two states differs beyond the report's bound.
type Differs = fn(&State, &State) -> bool;

/// The report's metrics. The shield is compared only where the oracle's
/// tick record carries it.
const METRICS: [(&str, Differs); 4] = [
    ("alive", |a, b| a.alive != b.alive),
    ("position", |a, b| {
        a.alive && b.alive && dist(a.pos, b.pos) > REPORT_POSITION
    }),
    ("hp", |a, b| {
        a.alive && b.alive && (a.hp - b.hp).abs() > REPORT_HIT_PTS
    }),
    ("shield", |a, b| match (a.shield, b.shield) {
        _ if !(a.alive && b.alive) => false,
        (_, None) => false,
        (Some(x), Some(y)) => (x.hp - y.hp).abs() > REPORT_HIT_PTS || x.online != y.online,
        (None, Some(_)) => true,
    }),
];

/// The first tick at which each metric of an object leaves its bound, with
/// both sides' states around it.
fn first_divergences(
    summary: &mut String,
    scenario: &str,
    name: &str,
    ours: &[State],
    theirs: &[State],
) {
    let n = ours.len().min(theirs.len());
    for (metric, differs) in METRICS {
        let Some(first) = (0..n).find(|&t| differs(&ours[t], &theirs[t])) else {
            continue;
        };
        let _ = writeln!(
            summary,
            "{scenario}: {name}: {metric} first diverges at tick {first}"
        );
        for t in first.saturating_sub(REPORT_WINDOW)..(first + REPORT_WINDOW + 1).min(n) {
            let _ = writeln!(summary, "    tick {t} rust:   {}", fmt_state(&ours[t]));
            let _ = writeln!(summary, "    tick {t} oracle: {}", fmt_state(&theirs[t]));
        }
    }
    if theirs
        .first()
        .is_some_and(|b| b.alive && b.power.is_none() && ours[0].power.is_some())
    {
        let _ = writeln!(
            summary,
            "{scenario}: {name}: the oracle's tick records carry no shield or power (an older harness)"
        );
    }
}

fn fmt_hit(h: &Hit) -> String {
    let shield = h
        .shield
        .map_or_else(|| "-".to_owned(), |(a, b)| format!("{a} -> {b}"));
    format!(
        "sub {:#06x} ({}) hull {} -> {} shield {shield} entries {}",
        h.sub, h.kind, h.hull.0, h.hull.1, h.entries
    )
}

/// The munition hits of both sides, paired by tick, target and order.
fn report_hits(summary: &mut String, scenario: &str, ours: &[Hit], theirs: &[Hit]) {
    if ours.is_empty() && theirs.is_empty() {
        return;
    }
    let _ = writeln!(
        summary,
        "{scenario}: munition hits: rust {}, oracle {}",
        ours.len(),
        theirs.len()
    );
    type Pair<'a> = (Vec<&'a Hit>, Vec<&'a Hit>);
    let mut groups: BTreeMap<(u32, &str), Pair<'_>> = BTreeMap::new();
    for h in ours {
        groups.entry((h.tick, &h.target)).or_default().0.push(h);
    }
    for h in theirs {
        groups.entry((h.tick, &h.target)).or_default().1.push(h);
    }
    let mut lines = Vec::new();
    let mut differing = 0;
    for ((tick, target), (a, b)) in &groups {
        for i in 0..a.len().max(b.len()) {
            let side = |v: &[&Hit]| v.get(i).map_or_else(|| "none".to_owned(), |h| fmt_hit(h));
            let (x, y) = (side(a), side(b));
            let line = if x == y {
                format!("    tick {tick} {target} #{}: both {x}", i + 1)
            } else {
                differing += 1;
                format!("    tick {tick} {target} #{}: rust {x} | oracle {y}", i + 1)
            };
            lines.push(line);
        }
    }
    let _ = writeln!(
        summary,
        "{scenario}: munition hits that differ: {differing} of {}",
        lines.len()
    );
    for line in lines.iter().take(REPORT_HITS) {
        let _ = writeln!(summary, "{line}");
    }
    if lines.len() > REPORT_HITS {
        let _ = writeln!(summary, "    ... (the first {REPORT_HITS} listed)");
        if let Some(first) = lines
            .iter()
            .skip(REPORT_HITS)
            .find(|l| l.contains(" | oracle "))
        {
            let _ = writeln!(summary, "    first differing hit after them:\n{first}");
        }
    }
}

/// The closest approach of two objects' centres over the ticks both are
/// alive in, and its tick.
fn closest(states: &BTreeMap<String, Vec<State>>, a: &str, b: &str) -> Option<(f64, usize)> {
    let (a, b) = (states.get(a)?, states.get(b)?);
    (0..a.len().min(b.len()))
        .filter(|&t| a[t].alive && b[t].alive)
        .map(|t| (dist(a[t].pos, b[t].pos), t))
        .min_by(|x, y| x.0.total_cmp(&y.0))
}

/// Each ship's closest approach to each named solar on both sides, and the
/// collision events of both sides.
fn report_contacts(summary: &mut String, s: &Scenario, ours: &Run, theirs: &Run) {
    let fmt = |c: Option<(f64, usize)>| {
        c.map_or_else(
            || "no records".to_owned(),
            |(d, t)| format!("{d:.3} m at tick {t}"),
        )
    };
    for ship in s.objects.iter().filter(|o| o.ship) {
        for solar in s.objects.iter().filter(|o| !o.ship) {
            let first = ours
                .contacts
                .iter()
                .find(|c| c.a == ship.name || c.b == ship.name)
                .map_or_else(|| "none".to_owned(), |c| format!("tick {}", c.tick));
            let _ = writeln!(
                summary,
                "{}: {} to {} (centres): closest rust {}, oracle {}; first rust contact {first}",
                s.name,
                ship.name,
                solar.name,
                fmt(closest(&ours.states, &ship.name, &solar.name)),
                fmt(closest(&theirs.states, &ship.name, &solar.name)),
            );
        }
    }
    for (side, run) in [("rust", ours), ("oracle", theirs)] {
        let _ = writeln!(
            summary,
            "{}: {side} collision events: {}",
            s.name,
            run.contacts.len()
        );
        for c in run.contacts.iter().take(REPORT_COLLISIONS) {
            let _ = writeln!(
                summary,
                "    tick {} {} against {} damage {}",
                c.tick, c.a, c.b, c.damage
            );
        }
    }
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
        let Some(our_run) = run(&s) else {
            let _ = writeln!(summary, "{}: did not run", s.name);
            continue;
        };
        let ours = &our_run.states;
        for (name, states) in ours {
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
        let Some(their_run) = oracle_run(dir, &s.name) else {
            let _ = writeln!(summary, "{}: no oracle tick.jsonl", s.name);
            continue;
        };
        let theirs = &their_run.states;
        for (name, ours_states) in ours {
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
            first_divergences(&mut summary, &s.name, name, ours_states, their_states);
        }
        report_hits(&mut summary, &s.name, &our_run.hits, &their_run.hits);
        report_contacts(&mut summary, &s, &our_run, &their_run);
    }
    print!("{summary}");
    if let Some(dir) = &oracle {
        let _ = fs::write(dir.join("rust-scenario-replay.txt"), &summary);
    }
    assert!(gross.is_empty(), "gross divergence: {gross:?}\n{summary}");
}
