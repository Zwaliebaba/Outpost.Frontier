//! The scenario files (`CppSource/Oracle/scenarios/*.txt`, read as
//! `CppSource/Oracle/oracle_sim.cpp` reads them).

use std::fs;
use std::path::{Path, PathBuf};

use neuron_core::math::{Matrix, Vector};

use super::directives::directive_with;
use crate::ai::behaviors::create_behavior;
use crate::object::World;

/// One object of a scenario: a `ship` (archetype, loadout, position and
/// options) or a named `solar` of the system.
#[derive(Clone, Debug)]
pub struct ObjDesc {
    /// The scenario's name of the object.
    pub name: String,
    /// A ship (else a solar of the system).
    pub ship: bool,
    /// A ship's archetype nickname.
    pub archetype: String,
    /// A ship's loadout nickname.
    pub loadout: String,
    /// A solar's nickname.
    pub nickname: String,
    /// A ship's position.
    pub pos: Vector,
    /// A ship's orientation (`heading`).
    pub orient: Matrix,
    /// A ship's velocity (`vel`).
    pub vel: Vector,
    /// A ship's angular velocity (`angvel`).
    pub angvel: Vector,
    /// A ship's hit points (`hp`), -1 for the archetype's.
    pub hp: f32,
    /// The state graph (`ai NAME`), submitted with `pub::AI::SubmitState`.
    pub ai: Option<String>,
    /// The `pilot` option: the personality's `[Pilot]` nickname.
    pub pilot: Option<String>,
    /// The `faction` option: the reputation group of the ship's vibe.
    pub faction: Option<String>,
    /// The `escort` option: the state graph's escort variant.
    pub escort: bool,
    /// The `content` option (P3.6c): the ship is Content's NPC ship, made
    /// by the server (`FSystemInfo::CreateNpcShip`) and its manager's
    /// callbacks ([`super::driver::ScenarioServer::spawn_content_ship`]).
    pub content: bool,
    /// The `flee X Y Z DISTANCE` option of a `content` ship: its flee
    /// position (`FContentNpcShip::SetFleePosition`).
    pub flee: Option<(Vector, f32)>,
}

/// One command (`at TICK OBJECT VERB ARGS`; a tick range expands to one
/// command per tick).
#[derive(Clone, Debug)]
pub struct Cmd {
    /// The tick before whose step it applies.
    pub tick: u32,
    /// The scenario's name of the object.
    pub object: String,
    /// The verb and its arguments, lower case.
    pub args: Vec<String>,
}

/// A scenario file.
#[derive(Clone, Debug)]
pub struct Scenario {
    /// The file's stem.
    pub name: String,
    /// The system's nickname.
    pub system: String,
    /// The number of ticks.
    pub ticks: u32,
    /// The frame time, seconds.
    pub frame: f64,
    /// `stepping sp`: the physics steps in single-player mode.
    pub sp: bool,
    /// `collisions sp`: the server's collision filter takes its
    /// single-player branch (every pair collides).
    pub sp_collisions: bool,
    /// `comms off`: the oracle clamps Content's comm system (GCS), so its
    /// `rand()` sequence holds no comm rolls (owner, 2026-11-19). The Rust
    /// world alone has no comm system; a run through the server's hooks
    /// clamps Content's the same way (P3.6b).
    pub comms_off: bool,
    /// The file is marked `# ci: expect collision`: a contact scenario,
    /// judged to its first contact ([`judge_contact`](super::judge_contact)).
    pub contact: bool,
    /// The file is marked `# rust: skip <reason>`: the Rust world cannot
    /// run it yet.
    pub rust_skip: Option<String>,
    /// The file is marked `# rust: server <reason>`: the scenario needs
    /// the server's (or Content's) answers, so only a run through the
    /// server's hooks is judged ([`needs_server`]).
    pub rust_server: Option<String>,
    /// The `srand` seed (`seed`).
    pub seed: u32,
    /// The seed of the seeding point when it is not `seed`: Gate G2's seed
    /// ensemble (`Oracle.exe sim ... --seed S`; record 0003 point 9).
    pub reseed: Option<u32>,
    /// The objects, in file order.
    pub objects: Vec<ObjDesc>,
    /// The commands, in tick order.
    pub commands: Vec<Cmd>,
}

/// The repository's scenario folder, `CppSource/Oracle/scenarios`.
#[must_use]
pub fn scenarios_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../CppSource/Oracle/scenarios")
}

/// The scenario files (`*.txt`) of [`scenarios_dir`], sorted.
#[must_use]
pub fn scenario_files() -> Vec<PathBuf> {
    let mut files: Vec<PathBuf> = fs::read_dir(scenarios_dir())
        .map(|d| {
            d.flatten()
                .map(|e| e.path())
                .filter(|p| p.extension().is_some_and(|x| x == "txt"))
                .collect()
        })
        .unwrap_or_default();
    files.sort();
    files
}

pub(super) fn num(t: &str) -> Option<f64> {
    t.parse().ok()
}

pub(super) fn vec3(t: &[String], at: usize) -> Option<Vector> {
    Some(Vector::new(
        num(t.get(at)?)? as f32,
        num(t.get(at + 1)?)? as f32,
        num(t.get(at + 2)?)? as f32,
    ))
}

/// The orientation `pub::SpaceObj::Create` hands the ship it makes: the
/// request carries the math engine's `MatrixToQuaternion` of the info's
/// matrix, and the ship's parameters take `QuaternionToMatrix` of it
/// (`server_pub.cpp:2474`, `server_classes.cpp:7057`).
#[must_use]
pub fn spawn_orientation(m: &Matrix) -> Matrix {
    neuron_core::math::quaternion_to_matrix(&neuron_core::math::matrix_to_quaternion(m))
}

/// `HeadingMatrix`: a yaw of `degrees` about +Y.
fn heading(degrees: f64) -> Matrix {
    let r = degrees * std::f64::consts::PI / 180.0;
    let (s, c) = (r.sin() as f32, r.cos() as f32);
    Matrix::from_rows([[c, 0.0, s], [0.0, 1.0, 0.0], [-s, 0.0, c]])
}

/// Reads a scenario file; `None` when it cannot be read or a line does
/// not parse.
#[must_use]
pub fn parse(path: &Path) -> Option<Scenario> {
    let text = fs::read_to_string(path).ok()?;
    let mut s = Scenario {
        name: path.file_stem()?.to_string_lossy().into_owned(),
        system: String::new(),
        ticks: 0,
        frame: 1.0 / 30.0,
        sp: false,
        sp_collisions: false,
        comms_off: false,
        contact: text
            .lines()
            .any(|l| l.trim_start().starts_with("# ci: expect collision")),
        rust_skip: text.lines().find_map(|l| {
            l.trim_start()
                .strip_prefix("# rust: skip ")
                .map(|r| r.trim().to_owned())
        }),
        rust_server: text.lines().find_map(|l| {
            l.trim_start()
                .strip_prefix("# rust: server ")
                .map(|r| r.trim().to_owned())
        }),
        seed: 0x4f52_4143,
        reseed: None,
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
            "collisions" => s.sp_collisions = t.get(1)?.eq_ignore_ascii_case("sp"),
            "comms" => s.comms_off = t.get(1)?.eq_ignore_ascii_case("off"),
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
                    ai: None,
                    pilot: None,
                    faction: None,
                    escort: false,
                    content: false,
                    flee: None,
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
                            o.ai = Some(t.get(at + 1)?.clone());
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
                        "escort" => {
                            o.escort = true;
                            at += 1;
                        }
                        "content" => {
                            o.content = true;
                            at += 1;
                        }
                        "flee" => {
                            let point = vec3(&t, at + 1)?;
                            o.flee = Some((point, num(t.get(at + 4)?)? as f32));
                            at += 5;
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

/// Why a scenario is judged only through the server's hooks: the file is
/// marked `# rust: server <reason>` (the world alone would wait for an
/// answer nobody gives). `scenario_replay` still runs one without
/// Content's ships without a server, reported and not judged, for its
/// first divergence (owner, 2026-11-25).
#[must_use]
pub fn needs_server(s: &Scenario) -> Option<String> {
    s.rust_server.clone()
}

/// Why a scenario cannot run yet: the file says so (`# rust: skip`), or
/// a directive whose behaviour is not ported (the AI ships themselves
/// run: they get a behaviour manager).
#[must_use]
pub fn needs_ai(s: &Scenario) -> Option<String> {
    if let Some(why) = &s.rust_skip {
        return Some(why.clone());
    }
    for c in &s.commands {
        if let Some(op) = directive_with(&c.args, &|_| Some(1))
            && let Some(id) = op.behavior()
            && !create_behavior::<World>(id).ported()
        {
            return Some(format!(
                "the {} directive (behaviour {id} is not ported)",
                c.args[0]
            ));
        }
    }
    None
}
