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
//! A scenario with `collisions sp` (`ram_ship`) runs without the server's
//! collision filter, as the oracle's does; its collision events
//! (`GameObject::on_collision`: the relative speed and the
//! `CalcCollisionDamage` result) are paired with the oracle's
//! `events.jsonl` and `calls.jsonl`, and collision events on one side only
//! are a gross divergence.
//!
//! For every scenario, the AI ones too, the report lists the `rand()`
//! draws the Rust world makes while it loads the system and spawns the
//! objects beside the oracle's load-time draws (`load_rand.jsonl`).
//!
//! A ship with an `ai` state graph gets a behaviour manager
//! (`pub::AI::SubmitState`), and the `goto`, `face`, `idle` and `cancel`
//! commands are directives (`pub::AI::SubmitDirective`); `fly_to_point`
//! (goto) and `turn_to_heading` (face) are compared with the oracle like
//! the other scenarios. Scenarios whose directives need a behaviour that
//! is not ported yet, and the `dock` directive, are skipped with a note
//! (P2.5). The fights (`dogfight`, `skirmish`: AI ships of hostile
//! factions; `strafe`: an AI fighter and a hostile ship without one) take
//! each pilot's personality and the ships' attitudes from the oracle's
//! `ai.jsonl` (a ship without a manager the reverse attitude of the AI
//! ship's snapshot), and are reported as chaotic contact (record
//! 0003 point 9): each ship's exact span, the first behaviour and target
//! difference, the behaviour sequences, the hits before the first
//! divergence and the outcome. A summary goes to `rust-scenario-replay.txt`
//! in the oracle folder.

mod support;

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

use game_sim::ai::behavior::DirectiveOp;
use game_sim::ai::behaviors::create_behavior;
use game_sim::ai::personality::Personality;
use game_sim::ai::state_graph::GraphType;
use game_sim::equip::flight::StrafeDir;
use game_sim::equip::{EquipKind, SUBOBJ_ID_ROOT, types};
use game_sim::object::world::{ShipSpawn, WorldConfig};
use game_sim::object::{Command, DamageList, ObjHandle, World, WorldEvent};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::StepMode;
use neuron_core::physics::replay::{CoreRecord, compare};
use neuron_core::rng::CrtRand;
use support::directives::directive;
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

/// Collision events listed per scenario.
const REPORT_COLLISIONS: usize = 20;

/// The furthest the load-time draw count is searched for.
const LOAD_DRAWS_SEARCH: usize = 2_000_000;

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
    /// The state graph (`ai NAME`), submitted with `pub::AI::SubmitState`.
    ai: Option<String>,
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
    /// `collisions sp`: the server's collision filter takes its
    /// single-player branch (every pair collides).
    sp_collisions: bool,
    /// The file is marked `# ci: expect collision`: a contact scenario,
    /// judged to its first contact ([`judge_contact`]).
    contact: bool,
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
        sp_collisions: false,
        contact: text
            .lines()
            .any(|l| l.trim_start().starts_with("# ci: expect collision")),
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
            "collisions" => s.sp_collisions = t.get(1)?.eq_ignore_ascii_case("sp"),
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

/// Why a scenario cannot run yet: a directive whose behaviour is not
/// ported (the AI ships themselves run: they get a behaviour manager), or
/// the dock directive (which needs the station's Content side).
fn needs_ai(s: &Scenario) -> Option<String> {
    for c in &s.commands {
        if c.args[0] == "dock" {
            return Some("the dock directive".to_owned());
        }
        if let Some(op) = directive(&c.args)
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
    angvel: Vector,
    /// A ship's commanded throttle and axis throttle (`None` for others).
    throttle: Option<f32>,
    axis: Option<Vector>,
    hp: f32,
    /// `None` when the record does not carry it (an older oracle record,
    /// or an object without equipment).
    power: Option<f32>,
    /// `None` without a shield, or when the record does not carry it.
    shield: Option<ShieldState>,
    /// The current behaviour (`CurrentBehaviorIndex`), -1 without a
    /// manager.
    behavior: i32,
    /// The target's scenario name as an id (`create_id`), 0 for none.
    target: u32,
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

/// One collision event: the reporting object, the other, the relative
/// speed (`None` where the record does not carry it) and the damage.
#[derive(Clone, Debug)]
struct Contact {
    tick: u32,
    a: String,
    b: String,
    speed: Option<f32>,
    damage: f32,
}

/// What a run produced: the per-tick states, the munition hits and the
/// collisions.
#[derive(Default)]
struct Run {
    states: BTreeMap<String, Vec<State>>,
    hits: Vec<Hit>,
    contacts: Vec<Contact>,
    /// The behaviour managers' clocks after construction (the phases the
    /// constructors draw), for comparison with the first `ai_in` record.
    managers: Vec<String>,
    /// The first manager's pathfinder phases (its first three draws).
    pathfind_phases: Option<[f32; 3]>,
    /// The first behaviour the port does not have that an AI ship ran, and
    /// the tick (the state graph can pick one without a directive).
    unported: Option<(i32, u32)>,
    /// The shots fired (`WorldEvent::Fired`).
    fired: usize,
    /// Per object and tick, the body's core after the frame (diagnostic).
    cores: BTreeMap<String, Vec<String>>,
    /// The simulation units of the first [`UNIT_PSIS`] PSIs (`units.jsonl`).
    units: Vec<Vec<Unit>>,
    /// Each unit's core at the start and the end of its PSI, for the first
    /// PSIs (`unit_cores.jsonl`).
    unit_cores: Vec<Vec<neuron_core::physics::world::UnitCore>>,
}

/// A core's state as the bit diagnostic prints it.
fn fmt_core(c: &neuron_core::physics::ivp::core::Core) -> String {
    let h = |v: &[f32]| {
        v.iter()
            .map(|x| format!("{:08x}", x.to_bits()))
            .collect::<Vec<_>>()
            .join(" ")
    };
    let q = |q: &neuron_core::physics::ivp::math::IvpQuat| h(&[q.x, q.y, q.z, q.w]);
    let t = |t: &neuron_core::physics::ivp::math::IvpTime| h(&[t.base, t.offset]);
    format!(
        "flags {:x} movement {:x} rot_speed [{}] q_next [{}] q_last_motion [{}] pos_last_motion [{}] time_of_last_motion [{}] time_of_last_long_motion [{}] time_of_last_psi [{}]",
        c.flags,
        c.movement_state,
        h(&c.rot_speed),
        q(&c.q_world_f_core_next_psi),
        q(&c.q_last_motion),
        h(&c.pos_last_motion),
        t(&c.time_of_last_motion),
        t(&c.time_of_last_long_motion),
        t(&c.time_of_last_psi),
    )
}

fn record_cores(
    world: &World,
    out: &mut BTreeMap<String, Vec<String>>,
    handles: &BTreeMap<String, Option<ObjHandle>>,
) {
    for (name, h) in handles {
        let text = h
            .and_then(|h| world.get(h))
            .and_then(|o| {
                let body = o.physical.body?;
                world.physics(o.physical.system)?.core(body).map(fmt_core)
            })
            .unwrap_or_else(|| "-".to_owned());
        out.entry(name.clone()).or_default().push(text);
    }
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

/// The index in the CRT sequence of `seed` at which three draws give the
/// pathfinder's phases (`rand() / 32768 * period` for the periods 1, 0.5
/// and 0.1, as `PathfindManager`'s constructor stores them).
fn phase_index(seed: u32, phases: [f32; 3]) -> Option<usize> {
    let phase = |r: i32, p: f32| (f64::from(r) * (1.0 / 32768.0) * f64::from(p)) as f32;
    let mut rng = CrtRand::with_seed(seed);
    let mut window: Vec<i32> = (0..3).map(|_| rng.rand()).collect();
    for i in 0..1_000_000 {
        if phase(window[0], 1.0).to_bits() == phases[0].to_bits()
            && phase(window[1], 0.5).to_bits() == phases[1].to_bits()
            && phase(window[2], 0.1).to_bits() == phases[2].to_bits()
        {
            return Some(i);
        }
        window.remove(0);
        window.push(rng.rand());
    }
    None
}

/// The pathfinder phases of the first AI ship in the oracle's first
/// `ai.jsonl` of a scenario (its first `ai_in` record).
fn oracle_phases(dir: &Path, scenario: &str) -> Option<[f32; 3]> {
    let files = find_files(&dir.join(scenario), "ai.jsonl");
    let first = files.into_iter().min()?;
    let ticks = support::ai_records::read_ai_ticks(&first);
    ticks.first()?.input.pathfind.as_ref().map(|p| p.timers)
}

/// What the Rust world cannot load for the AI ships and the harness takes
/// from the oracle's records: each pilot's personality (Content's
/// `[Pilot]` loader is P3.6) and the ships' attitudes toward each other
/// (the reputation groups are not ported).
#[derive(Clone, Debug, Default)]
struct AiInputs {
    personalities: BTreeMap<String, Personality>,
    attitudes: BTreeMap<(String, String), f32>,
}

/// The [`AiInputs`] of a scenario from its first `ai.jsonl` under
/// `<oracle dir>/<scenario>/`: the first personality of each ship and its
/// first attitude toward each other scenario ship.
fn oracle_ai_inputs(dir: &Path, scenario: &str) -> AiInputs {
    let mut out = AiInputs::default();
    let Some(first) = find_files(&dir.join(scenario), "ai.jsonl")
        .into_iter()
        .min()
    else {
        return out;
    };
    let ticks = support::ai_records::read_ai_ticks(&first);
    let ai_ships: BTreeMap<u32, String> = ticks
        .iter()
        .filter_map(|t| Some((t.id, t.obj.clone()?)))
        .collect();
    // Every scenario ship by its id, those without a behaviour manager too
    // (`strafe`'s target), from the same run's `tick.jsonl`.
    let mut names = ai_ships.clone();
    if let Some(tick_file) = first.parent().map(|d| d.join("tick.jsonl")) {
        for j in read_lines(&tick_file) {
            if j.get("rec").and_then(Json::as_str) != Some("object") {
                continue;
            }
            if let (Some(id), Some(obj)) = (
                j.get("id").and_then(Json::as_u32),
                j.get("obj").and_then(Json::as_str),
            ) {
                names.entry(id).or_insert_with(|| obj.to_owned());
            }
        }
    }
    for t in &ticks {
        let Some(name) = &t.obj else {
            continue;
        };
        if let Some(p) = t.personality.as_deref().and_then(Personality::from_bytes) {
            out.personalities.entry(name.clone()).or_insert(p);
        }
        for o in &t.objects {
            if let (Some(other), Some((toward, reverse))) = (names.get(&o.id), o.attitude) {
                out.attitudes
                    .entry((name.clone(), other.clone()))
                    .or_insert(toward);
                // A ship without a manager records no attitude of its own:
                // it takes the reverse one the AI ship's snapshot reads.
                if !ai_ships.contains_key(&o.id) {
                    out.attitudes
                        .entry((other.clone(), name.clone()))
                        .or_insert(reverse);
                }
            }
        }
    }
    out
}

fn run(s: &Scenario, ai: &AiInputs) -> Option<Run> {
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
    let mut managers = Vec::new();
    let mut pathfind_phases = None;
    // The solars the scenario names, and every solar whose body or parts
    // are simulation units: the spinning ones and those with free (`!fxd`)
    // parts, which `BuildIVP_Real` makes bodies of their own held by
    // golems. The oracle's `StarSystem::load` builds them all, and each
    // unit takes its turn at the environment's movement-check countdown
    // (that decides when a scenario ship's motion is classified), so the
    // Rust world steps the oracle's units in the oracle's order. The other
    // solars are fixed and far from the scenario objects, and the contact
    // pass over every solar is slow in a debug build.
    let named: Vec<u32> = s
        .objects
        .iter()
        .filter(|o| !o.ship)
        .map(|o| create_id(o.nickname.as_bytes()))
        .collect();
    for sp in world.system_solar_spawns(system) {
        if named.contains(&sp.nickname)
            || sp.angular_velocity != Vector::ZERO
            || world.solar_has_free_parts(sp.archetype)
        {
            let _ = world.spawn_solar(system, &sp);
        }
    }
    world.set_unit_trace(system, true);
    if s.sp_collisions {
        world.set_server_collision_filter(system, false);
    }
    // The seeding point (owner, 2026-11-18): the oracle's harness calls
    // `srand(seed)` again once the system and its population have loaded.
    world.rng().seed(s.seed);
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
            let h = world.spawn_ship(system, &spawn).ok();
            if let (Some(h), Some(graph)) = (h, &o.ai) {
                let id = world
                    .data()
                    .state_graphs
                    .find(graph.as_bytes(), GraphType::Leader);
                world.submit_state(h, id, ai.personalities.get(&o.name).copied());
                if let Some(m) = world.behavior_manager(h) {
                    let pf = &m.state.pathfind;
                    pathfind_phases.get_or_insert([pf.timer_1e0, pf.timer_1e8, pf.timer_20c]);
                    managers.push(format!(
                        "{}: manager timers {:08x?} pathfind {:08x?}",
                        o.name,
                        m.state.timers().map(f32::to_bits),
                        [pf.timer_1e0, pf.timer_1e8, pf.timer_20c].map(f32::to_bits)
                    ));
                }
            }
            h
        } else {
            world.find(create_id(o.nickname.as_bytes()))
        };
        handles.insert(o.name.clone(), h);
    }
    for ((from, to), value) in &ai.attitudes {
        if let (Some(Some(a)), Some(Some(b))) = (handles.get(from), handles.get(to)) {
            world.set_attitude(*a, *b, *value);
        }
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
    let mut out = Run {
        managers,
        pathfind_phases,
        ..Run::default()
    };
    let record = |world: &World,
                  out: &mut BTreeMap<String, Vec<State>>,
                  handles: &BTreeMap<String, Option<ObjHandle>>| {
        let target_of = |h: ObjHandle| {
            let id = world.ai_view(h)?.target?;
            names.get(&id).map(|n| create_id(n.as_bytes()))
        };
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
                        angvel: i.angular_velocity,
                        throttle: i.ship.as_ref().map(|s| s.throttle),
                        axis: i.ship.as_ref().map(|s| s.axis_throttle),
                        hp: i.hit_pts,
                        power,
                        shield,
                        behavior: world.behavior_manager(h).map_or(-1, |m| m.state.behavior),
                        target: target_of(h).unwrap_or(0),
                    }
                });
            out.entry(name.clone()).or_default().push(st);
        }
    };
    record(&world, &mut out.states, &handles);
    record_cores(&world, &mut out.cores, &handles);
    let mut next = 0;
    for tick in 1..=s.ticks {
        while next < s.commands.len() && s.commands[next].tick == tick {
            let c = &s.commands[next];
            next += 1;
            let Some(Some(h)) = handles.get(&c.object).copied() else {
                continue;
            };
            let a = &c.args;
            if let Some(op) = directive(a) {
                // A current directive blocks a new one, so each scenario
                // directive cancels the one before.
                if !matches!(op, DirectiveOp::Cancel) {
                    world.submit_directive(h, &DirectiveOp::Cancel);
                }
                world.submit_directive(h, &op);
                continue;
            }
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
        if out.unported.is_none() {
            out.unported = handles.values().flatten().find_map(|&h| {
                let m = world.behavior_manager(h)?;
                let b = m.state.behavior;
                (b != -1 && !m.is_ported(b)).then_some((b, tick))
            });
        }
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
                WorldEvent::Fired { .. } => out.fired += 1,
                WorldEvent::Collision {
                    a,
                    b,
                    speed,
                    damage,
                } => out.contacts.push(Contact {
                    tick,
                    a: name_of(a),
                    b: name_of(b),
                    speed: Some(speed),
                    damage,
                }),
                _ => {}
            }
        }
        record(&world, &mut out.states, &handles);
        record_cores(&world, &mut out.cores, &handles);
        // The units of the PSIs this frame ran, named while their objects
        // still exist.
        if out.units.len() < UNIT_PSIS {
            let trace = world.unit_trace(system);
            for psi in trace
                .into_iter()
                .skip(out.units.len())
                .take(UNIT_PSIS - out.units.len())
            {
                out.units.push(
                    psi.into_iter()
                        .map(|(id, class, check)| Unit { id, class, check })
                        .collect(),
                );
            }
        }
    }
    out.unit_cores = world.unit_core_trace(system);
    Some(out)
}

/// The oracle's unit cores (`unit_cores.jsonl` beside the first
/// `units.jsonl`): per PSI, the cores in run order, the start state only in
/// the first PSI.
/// One unit of `unit_cores.jsonl`: the start (first PSI only) and end
/// states, and for a free part its object's frame and its golem's target
/// and current target rotations (the raw line, for the report).
type OracleUnitCore = (Option<CoreRecord>, CoreRecord, String);

fn oracle_unit_cores(dir: &Path, scenario: &str) -> Vec<Vec<OracleUnitCore>> {
    let Some(file) = find_files(&dir.join(scenario), "unit_cores.jsonl")
        .into_iter()
        .min()
    else {
        return Vec::new();
    };
    let Ok(text) = fs::read_to_string(&file) else {
        return Vec::new();
    };
    let mut out: Vec<Vec<OracleUnitCore>> = Vec::new();
    for line in text.lines() {
        let Ok(r) = support::records::parse(line) else {
            continue;
        };
        let psi = r.get("psi").and_then(Json::as_u32).unwrap_or(0) as usize;
        let Ok(o) = CoreRecord::parse_field(line, "out") else {
            continue;
        };
        let i = CoreRecord::parse_field(line, "in").ok();
        if psi == 0 {
            continue;
        }
        while out.len() < psi {
            out.push(Vec::new());
        }
        let floats = |k: &str| {
            r.get(k)
                .and_then(Json::as_floats)
                .map(|v| {
                    v.iter()
                        .map(|x| format!("{:08x}", x.to_bits()))
                        .collect::<Vec<_>>()
                        .join(" ")
                })
                .unwrap_or_default()
        };
        let extra = format!(
            "orient [{}] pos [{}] golem_q [{}] golem_current_q [{}]",
            floats("orient"),
            floats("pos"),
            floats("golem_q"),
            floats("golem_current_q")
        );
        out[psi - 1].push((i, o, extra));
    }
    out
}

/// The first unit core that differs from the oracle's (record 0003's bit
/// comparison), PSI by PSI in run order, for the PSIs both recorded.
fn report_unit_cores(
    summary: &mut String,
    dir: &Path,
    scenario: &str,
    ours: &Run,
    theirs_units: &[Vec<Unit>],
) {
    let theirs = oracle_unit_cores(dir, scenario);
    if theirs.is_empty() {
        return;
    }
    for (p, (a, b)) in ours.unit_cores.iter().zip(&theirs).enumerate() {
        for (i, ((ain, aout, agolem), (bin, bout, bextra))) in a.iter().zip(b).enumerate() {
            let unit = theirs_units
                .get(p)
                .and_then(|u| u.get(i))
                .map_or_else(|| "?".to_owned(), |u| format!("{}:{:x}", u.id, u.class));
            if let Some(bin) = bin {
                let d = compare(bin, ain, 0.0);
                if !d.is_empty() {
                    let _ = writeln!(
                        summary,
                        "{scenario}: unit cores: psi {} unit {i} ({unit}) starts differently, oracle != rust: {}",
                        p + 1,
                        d.iter().take(10).cloned().collect::<Vec<_>>().join("; ")
                    );
                    return;
                }
            }
            let d = compare(bout, aout, 0.0);
            if !d.is_empty() {
                let _ = writeln!(
                    summary,
                    "{scenario}: unit cores: psi {} unit {i} ({unit}) first differs at the end of its PSI, oracle != rust: {}",
                    p + 1,
                    d.iter().take(12).cloned().collect::<Vec<_>>().join("; ")
                );
                let h = |v: &[f32]| {
                    v.iter()
                        .map(|x| format!("{:08x}", x.to_bits()))
                        .collect::<Vec<_>>()
                        .join(" ")
                };
                let q = |c: &CoreRecord| {
                    let q = c.core.q_world_f_core_next_psi;
                    h(&[q.x, q.y, q.z, q.w])
                };
                let _ = writeln!(
                    summary,
                    "    oracle: {bextra}; core q_next at the start [{}]",
                    bin.as_ref().map(q).unwrap_or_default()
                );
                let _ = writeln!(
                    summary,
                    "    rust: golem target and current [{}]; core q_next at the start [{}]",
                    agolem.map(|g| h(&g)).unwrap_or_default(),
                    q(ain)
                );
                return;
            }
        }
        if a.len() != b.len() {
            let _ = writeln!(
                summary,
                "{scenario}: unit cores: psi {} rust {} units, oracle {}",
                p + 1,
                a.len(),
                b.len()
            );
            return;
        }
    }
    let _ = writeln!(
        summary,
        "{scenario}: unit cores: every core of the first {} PSIs equals the oracle's bit for bit",
        theirs.len().min(ours.unit_cores.len())
    );
}

/// The PSIs `units.jsonl` records (`kUnitPsis`, oracle_sim.cpp).
const UNIT_PSIS: usize = 120;

/// A simulation unit of a PSI: its (first) core's object as `units.jsonl`
/// names it, and whether the movement check came to it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Unit {
    id: u32,
    class: u32,
    check: bool,
}

/// The oracle's simulation units per PSI from the first `units.jsonl`
/// under `<oracle dir>/<scenario>/`, and whether any unit held more than
/// one core.
fn oracle_units(dir: &Path, scenario: &str) -> Option<(Vec<Vec<Unit>>, bool)> {
    let file = find_files(&dir.join(scenario), "units.jsonl")
        .into_iter()
        .min()?;
    let mut multi = false;
    let mut out = Vec::new();
    for r in read_lines(&file) {
        let units = r.get("units").and_then(Json::as_array).unwrap_or_default();
        let psi: Vec<Unit> = units
            .iter()
            .map(|u| {
                let a = u.as_array().unwrap_or_default();
                let words: Vec<u32> = a.iter().filter_map(Json::as_u32).collect();
                multi |= words.len() > 2;
                Unit {
                    id: words.first().copied().unwrap_or(0),
                    class: words.get(1).copied().unwrap_or(0),
                    check: a.last().and_then(Json::as_bool) == Some(true),
                }
            })
            .collect();
        out.push(psi);
    }
    Some((out, multi))
}

/// A PSI's units, runs of equal ones folded (`0:1x48`), the unit the
/// movement check came to marked with `*`.
fn fmt_units(units: &[Unit]) -> String {
    let mut out: Vec<String> = Vec::new();
    let mut i = 0;
    while i < units.len() {
        let u = units[i];
        let mut j = i + 1;
        while j < units.len() && units[j] == u && !u.check {
            j += 1;
        }
        let mark = if u.check { "*" } else { "" };
        if j - i > 1 {
            out.push(format!("{}:{:x}x{}", u.id, u.class, j - i));
        } else {
            out.push(format!("{}:{:x}{mark}", u.id, u.class));
        }
        i = j;
    }
    out.join(" ")
}

/// The simulation units of both sides, PSI by PSI: the first PSI where the
/// list (objects, order, movement check) differs, or that they agree.
fn report_units(summary: &mut String, dir: &Path, scenario: &str, ours: &Run) {
    let Some((theirs, multi)) = oracle_units(dir, scenario) else {
        return;
    };
    // The two sides number the scenario ships differently (the oracle's
    // load makes objects the Rust world does not): each ship's id becomes
    // its place in the spawn order, which is the order of the ids.
    let by_spawn = |v: &[Vec<Unit>]| -> Vec<Vec<Unit>> {
        let mut ships: Vec<u32> = v
            .iter()
            .flatten()
            .filter(|u| u.class == 0x503)
            .map(|u| u.id)
            .collect();
        ships.sort_unstable();
        ships.dedup();
        v.iter()
            .map(|psi| {
                psi.iter()
                    .map(|u| match ships.iter().position(|s| *s == u.id) {
                        Some(k) if u.class == 0x503 => Unit {
                            id: k as u32 + 1,
                            ..*u
                        },
                        _ => *u,
                    })
                    .collect()
            })
            .collect()
    };
    let theirs = by_spawn(&theirs);
    report_unit_cores(summary, dir, scenario, ours, &theirs);
    let ours_units = by_spawn(&ours.units);
    let ours = &Run {
        units: ours_units,
        ..Run::default()
    };
    if multi {
        let _ = writeln!(
            summary,
            "{scenario}: units: a unit of the oracle holds more than one core (compared by its first)"
        );
    }
    let n = theirs.len().min(ours.units.len());
    let count = |v: &[Vec<Unit>], p: usize| v.get(p).map_or(0, Vec::len);
    let _ = writeln!(
        summary,
        "{scenario}: units at PSI 1, 40, 120: rust {} {} {}, oracle {} {} {}",
        count(&ours.units, 0),
        count(&ours.units, 39),
        count(&ours.units, 119),
        count(&theirs, 0),
        count(&theirs, 39),
        count(&theirs, 119)
    );
    match (0..n).find(|&p| ours.units[p] != theirs[p]) {
        None => {
            let _ = writeln!(
                summary,
                "{scenario}: units: every unit of the {n} recorded PSIs equals the oracle's (objects, order, movement check)"
            );
        }
        Some(p) => {
            let _ = writeln!(
                summary,
                "{scenario}: units first differ at PSI {} (of {n}):",
                p + 1
            );
            for q in p.saturating_sub(1)..(p + 2).min(n) {
                let _ = writeln!(
                    summary,
                    "    psi {} rust {}: {}",
                    q + 1,
                    ours.units[q].len(),
                    fmt_units(&ours.units[q])
                );
                let _ = writeln!(
                    summary,
                    "    psi {} oracle {}: {}",
                    q + 1,
                    theirs[q].len(),
                    fmt_units(&theirs[q])
                );
            }
        }
    }
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
                speed: None,
                damage: r.get("damage").and_then(Json::as_f32).unwrap_or(0.0),
            });
        }
    }
    // The relative speeds of the same contacts, from their
    // `CalcCollisionDamage` calls (one per event, in the same order).
    if calls.is_file() {
        let speeds: Vec<f32> = read_lines(&calls)
            .iter()
            .filter(|r| r.get("rec").and_then(Json::as_str) == Some("collision_damage"))
            .map(|r| r.get("speed").and_then(Json::as_f32).unwrap_or(f32::NAN))
            .collect();
        if speeds.len() == run.contacts.len() {
            for (c, v) in run.contacts.iter_mut().zip(speeds) {
                c.speed = Some(v);
            }
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
                angvel: v("angvel"),
                throttle: r.get("throttle").and_then(Json::as_f32),
                axis: r
                    .get("axis")
                    .and_then(Json::as_floats)
                    .filter(|a| a.len() >= 3)
                    .map(|a| Vector::new(a[0], a[1], a[2])),
                hp: r.get("hp").and_then(Json::as_f32).unwrap_or(0.0),
                power: r.get("power").and_then(Json::as_f32),
                shield,
                behavior: r.get("behavior").and_then(Json::as_i32).unwrap_or(-1),
                target: r
                    .get("target_obj")
                    .and_then(Json::as_str)
                    .map_or(0, |n| create_id(n.as_bytes())),
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
        "pos ({:.3}, {:.3}, {:.3}) vel ({:.3}, {:.3}, {:.3}) fwd ({:.3}, {:.3}, {:.3}) hp {} shield {shield} power {}",
        st.pos.x,
        st.pos.y,
        st.pos.z,
        st.vel.x,
        st.vel.y,
        st.vel.z,
        -st.orient[2],
        -st.orient[5],
        -st.orient[8],
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

/// The comparison of a fight (record 0003 point 9): the first tick at
/// which each ship's position (0.01 m), behaviour and target leave the
/// oracle's, the munition hits up to the first divergence, and the
/// outcome (alive, hull).
fn report_combat(summary: &mut String, scenario: &str, ours: &Run, theirs: &Run) {
    let mut first_any = usize::MAX;
    for (name, a) in &ours.states {
        let Some(b) = theirs.states.get(name) else {
            continue;
        };
        let n = a.len().min(b.len());
        let first = |f: &dyn Fn(&State, &State) -> bool| (0..n).find(|&t| f(&a[t], &b[t]));
        let pos =
            first(&|x, y| x.alive != y.alive || (x.alive && dist(x.pos, y.pos) > REPORT_POSITION));
        let beh = first(&|x, y| x.alive && y.alive && x.behavior != y.behavior);
        let tgt = first(&|x, y| x.alive && y.alive && x.target != y.target);
        let shown = |v: Option<usize>| v.map_or_else(|| "never".to_owned(), |t| t.to_string());
        let changes = |v: &[State]| {
            let mut seq: Vec<(usize, i32)> = Vec::new();
            for (t, st) in v.iter().enumerate() {
                if seq.last().is_none_or(|l| l.1 != st.behavior) {
                    seq.push((t, st.behavior));
                }
            }
            seq
        };
        let _ = writeln!(
            summary,
            "{scenario}: {name}: exact (position within {REPORT_POSITION} m) until tick {}; behaviour first differs at {}, target at {}",
            pos.map_or(n, |t| t.saturating_sub(1)),
            shown(beh),
            shown(tgt)
        );
        let _ = writeln!(
            summary,
            "{scenario}: {name}: behaviours rust {:?}",
            changes(&a[..n])
        );
        let _ = writeln!(
            summary,
            "{scenario}: {name}: behaviours oracle {:?}",
            changes(&b[..n])
        );
        first_divergences(summary, scenario, name, &a[..n], &b[..n]);
        let (fa, fb) = (&a[n - 1], &b[n - 1]);
        let _ = writeln!(
            summary,
            "{scenario}: {name}: outcome rust alive {} hull {} oracle alive {} hull {}",
            fa.alive, fa.hp, fb.alive, fb.hp
        );
        first_any = first_any.min(pos.unwrap_or(usize::MAX));
    }
    let tick_limit = u32::try_from(first_any).unwrap_or(u32::MAX);
    let before = |h: &[Hit]| h.iter().filter(|x| x.tick < tick_limit).count();
    let _ = writeln!(
        summary,
        "{scenario}: shots rust {}; munition hits rust {} oracle {}; before the first divergence (tick {}) rust {} oracle {}",
        ours.fired,
        ours.hits.len(),
        theirs.hits.len(),
        if first_any == usize::MAX {
            "none".to_owned()
        } else {
            first_any.to_string()
        },
        before(&ours.hits),
        before(&theirs.hits)
    );
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
    }
    report_collisions(summary, &s.name, &ours.contacts, &theirs.contacts);
}

fn fmt_contact(c: &Contact) -> String {
    let speed = c.speed.map_or_else(|| "-".to_owned(), |v| v.to_string());
    format!("against {} speed {speed} damage {}", c.b, c.damage)
}

/// The collision events of both sides (`GameObject::on_collision` with
/// `CalcCollisionDamage`), paired by tick, reporting object and order.
/// Returns the number of pairs that differ.
fn report_collisions(
    summary: &mut String,
    scenario: &str,
    ours: &[Contact],
    theirs: &[Contact],
) -> usize {
    type Pair<'a> = (Vec<&'a Contact>, Vec<&'a Contact>);
    let mut groups: BTreeMap<(u32, &str), Pair<'_>> = BTreeMap::new();
    for c in ours {
        groups.entry((c.tick, &c.a)).or_default().0.push(c);
    }
    for c in theirs {
        groups.entry((c.tick, &c.a)).or_default().1.push(c);
    }
    let mut lines = Vec::new();
    let mut differing = 0;
    for ((tick, a), (x, y)) in &groups {
        for i in 0..x.len().max(y.len()) {
            let side = |v: &[&Contact]| {
                v.get(i)
                    .map_or_else(|| "none".to_owned(), |c| fmt_contact(c))
            };
            let (p, q) = (side(x), side(y));
            if p == q {
                lines.push(format!("    tick {tick} {a} #{}: both {p}", i + 1));
            } else {
                differing += 1;
                lines.push(format!(
                    "    tick {tick} {a} #{}: rust {p} | oracle {q}",
                    i + 1
                ));
            }
        }
    }
    if lines.is_empty() {
        return 0;
    }
    let _ = writeln!(
        summary,
        "{scenario}: collision events that differ: {differing} of {}",
        lines.len()
    );
    for line in lines.iter().take(REPORT_COLLISIONS) {
        let _ = writeln!(summary, "{line}");
    }
    if lines.len() > REPORT_COLLISIONS {
        let _ = writeln!(summary, "    ... (the first {REPORT_COLLISIONS} listed)");
    }
    differing
}

/// The first tick at which any recorded field of an object differs from
/// the oracle's at all (bit patterns), with the fields that differ and
/// both sides' values in hex, and how many ticks differ.
fn first_bit_difference(
    summary: &mut String,
    scenario: &str,
    name: &str,
    ours: &[State],
    theirs: &[State],
) -> Option<usize> {
    let hex3 = |v: Vector| {
        format!(
            "[{:08x} {:08x} {:08x}]",
            v.x.to_bits(),
            v.y.to_bits(),
            v.z.to_bits()
        )
    };
    let fields = |a: &State, b: &State| -> Vec<String> {
        let mut out = Vec::new();
        if a.alive != b.alive {
            out.push(format!("alive {} | {}", a.alive, b.alive));
            return out;
        }
        if !a.alive {
            return out;
        }
        let bits = |v: Vector| [v.x.to_bits(), v.y.to_bits(), v.z.to_bits()];
        for (field, x, y) in [
            ("pos", a.pos, b.pos),
            ("vel", a.vel, b.vel),
            ("angvel", a.angvel, b.angvel),
        ] {
            if bits(x) != bits(y) {
                out.push(format!("{field} {} | {}", hex3(x), hex3(y)));
            }
        }
        for i in 0..9 {
            if a.orient[i].to_bits() != b.orient[i].to_bits() {
                out.push(format!(
                    "orient[{i}] {:08x} ({}) | {:08x} ({})",
                    a.orient[i].to_bits(),
                    a.orient[i],
                    b.orient[i].to_bits(),
                    b.orient[i]
                ));
            }
        }
        if a.hp.to_bits() != b.hp.to_bits() {
            out.push(format!("hp {} | {}", a.hp, b.hp));
        }
        if let (Some(x), Some(y)) = (a.throttle, b.throttle)
            && x.to_bits() != y.to_bits()
        {
            out.push(format!("throttle {x} | {y}"));
        }
        if let (Some(x), Some(y)) = (a.axis, b.axis)
            && bits(x) != bits(y)
        {
            out.push(format!("axis {} | {}", hex3(x), hex3(y)));
        }
        out
    };
    let n = ours.len().min(theirs.len());
    let differing: Vec<usize> = (0..n)
        .filter(|&t| !fields(&ours[t], &theirs[t]).is_empty())
        .collect();
    let Some(&first) = differing.first() else {
        let _ = writeln!(
            summary,
            "{scenario}: {name}: every recorded field equals the oracle bit for bit"
        );
        return None;
    };
    let _ = writeln!(
        summary,
        "{scenario}: {name}: first bit difference at tick {first} ({} of {n} ticks differ), rust | oracle:",
        differing.len()
    );
    for f in fields(&ours[first], &theirs[first]) {
        let _ = writeln!(summary, "    {f}");
    }
    // The commands of the ticks around it, both sides (axis throttle bits).
    for t in first.saturating_sub(3)..(first + 2).min(n) {
        let ax = |st: &State| st.axis.map_or_else(|| "-".to_owned(), hex3);
        let _ = writeln!(
            summary,
            "    tick {t}: axis {} | {}, angvel {} | {}",
            ax(&ours[t]),
            ax(&theirs[t]),
            hex3(ours[t].angvel),
            hex3(theirs[t].angvel)
        );
    }
    Some(first)
}

/// Around a first bit difference: the Rust core after each frame and the
/// oracle's PSI records of the object (`psi.jsonl` beside `tick.jsonl`).
fn report_cores(
    summary: &mut String,
    dir: &Path,
    scenario: &str,
    name: &str,
    first: usize,
    ours: &Run,
) {
    let range = first.saturating_sub(2)..first + 2;
    if let Some(cores) = ours.cores.get(name) {
        for t in first.saturating_sub(16)..first + 2 {
            if let Some(c) = cores.get(t) {
                let _ = writeln!(summary, "    rust core after tick {t}: {c}");
            }
        }
    }
    // The simulation units of each PSI (`units.jsonl`): how many share the
    // movement-check countdown and which one it came to.
    if let Some(file) = find_files(&dir.join(scenario), "units.jsonl")
        .into_iter()
        .min()
    {
        for r in read_lines(&file) {
            let psi = r.get("psi").and_then(Json::as_u32).unwrap_or(0) as usize;
            if psi > first + 2 {
                break;
            }
            let units = r.get("units").and_then(Json::as_array).unwrap_or_default();
            let ids = |u: &Json| {
                u.as_array().map_or_else(String::new, |a| {
                    a.iter()
                        .filter_map(Json::as_u32)
                        .collect::<Vec<_>>()
                        .chunks(2)
                        .map(|c| format!("{}:{:x}", c[0], c.get(1).copied().unwrap_or(0)))
                        .collect::<Vec<_>>()
                        .join("+")
                })
            };
            let checked: Vec<String> = units
                .iter()
                .filter(|u| {
                    u.as_array().and_then(|a| a.last()).and_then(Json::as_bool) == Some(true)
                })
                .map(ids)
                .collect();
            let all: Vec<String> = units.iter().map(ids).collect();
            let _ = writeln!(
                summary,
                "    oracle units psi {psi}: {} [{}], check to {:?}",
                units.len(),
                all.join(" "),
                checked
            );
        }
    }
    let Some(file) = find_files(&dir.join(scenario), "psi.jsonl")
        .into_iter()
        .min()
    else {
        return;
    };
    let h = |r: &Json, k: &str| {
        r.get(k)
            .and_then(Json::as_floats)
            .map(|v| {
                v.iter()
                    .map(|x| format!("{:08x}", x.to_bits()))
                    .collect::<Vec<_>>()
                    .join(" ")
            })
            .unwrap_or_default()
    };
    for r in read_lines(&file) {
        let tick = r.get("tick").and_then(Json::as_u32).unwrap_or(0) as usize;
        let check = r.get("check").and_then(Json::as_bool).unwrap_or(false);
        if !(range.contains(&tick) || (check && tick < first))
            || r.get("obj").and_then(Json::as_str) != Some(name)
        {
            continue;
        }
        let (Some(i), Some(o)) = (r.get("in"), r.get("out")) else {
            continue;
        };
        let ctl = r.get("ctl");
        let _ = writeln!(
            summary,
            "    oracle psi {} tick {tick}: in flags {:x} movement {:x} rot_speed [{}] q_next [{}] q_last_motion [{}] pos_last_motion [{}] time_of_last_motion [{}] time_of_last_long_motion [{}] time_of_last_psi [{}] | out movement {:x} q_last_motion [{}] time_of_last_motion [{}] | pushes {} check {} steering {:?} drag {:?}",
            r.get("psi").and_then(Json::as_u32).unwrap_or(0),
            i.get("flags").and_then(Json::as_u32).unwrap_or(0),
            i.get("movement_state").and_then(Json::as_u32).unwrap_or(0),
            h(i, "rot_speed"),
            h(i, "q_next"),
            h(i, "q_last_motion"),
            h(i, "pos_last_motion"),
            h(i, "time_of_last_motion"),
            h(i, "time_of_last_long_motion"),
            h(i, "time_of_last_psi"),
            o.get("movement_state").and_then(Json::as_u32).unwrap_or(0),
            h(o, "q_last_motion"),
            h(o, "time_of_last_motion"),
            r.get("pushes")
                .and_then(Json::as_array)
                .map_or(0, |a| a.len()),
            check,
            ctl.and_then(|c| c.get("steering_active"))
                .and_then(Json::as_bool),
            ctl.and_then(|c| c.get("angular_drag_factor"))
                .and_then(Json::as_f32),
        );
    }
}

/// Whether two states are the same, bit for bit where it counts: alive,
/// position, velocity, orientation, hit points and the shield's hit points.
fn same_state(a: &State, b: &State) -> bool {
    let bits = |v: Vector| [v.x.to_bits(), v.y.to_bits(), v.z.to_bits()];
    a.alive == b.alive
        && (!a.alive
            || (bits(a.pos) == bits(b.pos)
                && bits(a.vel) == bits(b.vel)
                && a.orient.map(f32::to_bits) == b.orient.map(f32::to_bits)
                && a.hp.to_bits() == b.hp.to_bits()
                && match (a.shield, b.shield) {
                    (Some(x), Some(y)) => x.hp.to_bits() == y.hp.to_bits(),
                    (_, None) => true,
                    (None, Some(_)) => false,
                }))
}

/// A contact scenario (`ram_ship`, Gate G2's collide check; owner,
/// 2026-11-18, record 0003 point 9): the run is judged to its first
/// contact. Every object must equal the oracle exactly before the tick of
/// the oracle's first collision event, the Rust world's first collision
/// event must come in the same tick, and at the last tick every object
/// must be alive or gone as in the oracle. The first contact's damage and
/// the trajectories after it are reported, not judged (their envelopes
/// wait for the owner's G2 numbers).
fn judge_contact(
    summary: &mut String,
    gross: &mut Vec<String>,
    s: &Scenario,
    ours: &Run,
    theirs: &Run,
) {
    let Some(first) = theirs.contacts.iter().map(|c| c.tick).min() else {
        gross.push(format!("{}: the oracle records no contact", s.name));
        return;
    };
    let rust_first = ours.contacts.iter().map(|c| c.tick).min();
    let mut pre = None;
    for (name, a) in &ours.states {
        let Some(b) = theirs.states.get(name) else {
            continue;
        };
        let n = a.len().min(b.len()).min(first as usize);
        if let Some(t) = (0..n).find(|&t| !same_state(&a[t], &b[t])) {
            pre = Some(pre.map_or(t, |p: usize| p.min(t)));
            gross.push(format!(
                "{}: {name} differs from the oracle at tick {t}, before the first contact (tick {first})",
                s.name
            ));
        }
        if let (Some(x), Some(y)) = (a.last(), b.last())
            && x.alive != y.alive
        {
            gross.push(format!(
                "{}: {name} outcome: alive {} vs the oracle's {}",
                s.name, x.alive, y.alive
            ));
        }
    }
    if rust_first != Some(first) {
        gross.push(format!(
            "{}: first contact at tick {} vs the oracle's {first}",
            s.name,
            rust_first.map_or_else(|| "none".to_owned(), |t| t.to_string())
        ));
    }
    let _ = writeln!(
        summary,
        "{}: contact scenario: first contact rust tick {}, oracle tick {first}; before it {}",
        s.name,
        rust_first.map_or_else(|| "none".to_owned(), |t| t.to_string()),
        pre.map_or_else(
            || "every object equals the oracle exactly".to_owned(),
            |t| format!("the first difference is at tick {t}")
        )
    );
    for c in theirs.contacts.iter().filter(|c| c.tick == first) {
        let rust = ours
            .contacts
            .iter()
            .find(|r| r.tick == first && r.a == c.a)
            .map_or_else(|| "none".to_owned(), fmt_contact);
        let _ = writeln!(
            summary,
            "{}: first-contact damage to {}: rust {rust} | oracle {}",
            s.name,
            c.b,
            fmt_contact(c)
        );
    }
}

/// The number of `rand()` draws a generator seeded with `seed` made to
/// reach `now`, `None` beyond `LOAD_DRAWS_SEARCH`.
fn draws_since(seed: u32, now: &CrtRand) -> Option<usize> {
    let mut r = CrtRand::with_seed(seed);
    for i in 0..=LOAD_DRAWS_SEARCH {
        if r == *now {
            return Some(i);
        }
        r.rand();
    }
    None
}

/// The Rust world's draws while it loads the scenario's system (every
/// solar, as `StarSystem::load` builds them) and, after seeding again as
/// the harness does, spawns each object, by the oracle's phases
/// (`load_rand.jsonl`): `load`, then `spawn <name>`.
fn rust_load_draws(s: &Scenario) -> Option<Vec<(String, usize)>> {
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
    let mut out = Vec::new();
    let mut last = 0;
    let mark =
        |world: &mut World, last: &mut usize, phase: String, out: &mut Vec<(String, usize)>| {
            let now = draws_since(s.seed, world.rng()).unwrap_or(usize::MAX);
            out.push((phase, now.saturating_sub(*last)));
            *last = now;
        };
    world.load_system_solars(system);
    mark(&mut world, &mut last, "load".to_owned(), &mut out);
    // The seeding point after the load, as in the harness: the spawns'
    // draws count from the seed again.
    world.rng().seed(s.seed);
    last = 0;
    for o in &s.objects {
        if o.ship {
            let mut spawn = ShipSpawn::new(
                create_id(o.archetype.as_bytes()),
                create_id(o.loadout.as_bytes()),
                o.pos,
                o.orient,
            );
            spawn.velocity = o.vel;
            spawn.angular_velocity = o.angvel;
            spawn.hit_pts = o.hp;
            let h = world.spawn_ship(system, &spawn).ok();
            if let (Some(h), Some(graph)) = (h, &o.ai) {
                let id = world
                    .data()
                    .state_graphs
                    .find(graph.as_bytes(), GraphType::Leader);
                world.submit_state(h, id, None);
            }
        }
        mark(&mut world, &mut last, format!("spawn {}", o.name), &mut out);
    }
    Some(out)
}

/// The oracle's load-time draws by phase (`load_rand.jsonl` `phase`
/// records of the first run): the phase and its draw count.
fn oracle_load_draws(dir: &Path, scenario: &str) -> Option<Vec<(String, usize)>> {
    let file = find_files(&dir.join(scenario), "load_rand.jsonl")
        .into_iter()
        .min()?;
    let phases: Vec<(String, usize)> = read_lines(&file)
        .iter()
        .filter(|r| r.get("rec").and_then(Json::as_str) == Some("phase"))
        .filter_map(|r| {
            Some((
                r.get("phase")?.as_str()?.to_owned(),
                r.get("count")?.as_u32()? as usize,
            ))
        })
        .collect();
    (!phases.is_empty()).then_some(phases)
}

/// The load-time draws (owner, 2026-11-18): the Rust world's `rand()`
/// draws while it loads the system and spawns the objects, beside the
/// oracle's by phase. A phase the Rust world has no counterpart for (the
/// controller, `after_load`, `hooks`) is listed with the Rust count 0.
fn report_load_draws(summary: &mut String, s: &Scenario, oracle: Option<&Path>) {
    let Some(ours) = rust_load_draws(s) else {
        return;
    };
    let theirs = oracle.and_then(|d| oracle_load_draws(d, &s.name));
    let total: usize = ours.iter().map(|p| p.1).sum();
    let mut line = format!("{}: load-time draws: rust {total}", s.name);
    if let Some(theirs) = &theirs {
        let before_spawn: usize = theirs
            .iter()
            .take_while(|p| !p.0.starts_with("spawn "))
            .map(|p| p.1)
            .sum();
        let all: usize = theirs.iter().map(|p| p.1).sum();
        let _ = write!(
            line,
            ", oracle {all} ({before_spawn} before the first spawn)"
        );
    }
    let _ = writeln!(summary, "{line}");
    let rust_of = |phase: &str| ours.iter().find(|p| p.0 == phase).map_or(0, |p| p.1);
    match &theirs {
        Some(theirs) => {
            for (phase, n) in theirs {
                let _ = writeln!(summary, "    {phase}: rust {}, oracle {n}", rust_of(phase));
            }
        }
        None => {
            for (phase, n) in &ours {
                let _ = writeln!(summary, "    {phase}: rust {n}");
            }
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
        report_load_draws(&mut summary, &s, oracle.as_deref());
        if let Some(why) = needs_ai(&s) {
            let _ = writeln!(summary, "{}: skipped, {why} (AI is P2.5)", s.name);
            continue;
        }
        let ai = oracle
            .as_ref()
            .map(|d| oracle_ai_inputs(d, &s.name))
            .unwrap_or_default();
        let Some(our_run) = run(&s, &ai) else {
            let _ = writeln!(summary, "{}: did not run", s.name);
            continue;
        };
        if let Some((b, tick)) = our_run.unported {
            let _ = writeln!(
                summary,
                "{}: skipped, an AI ship runs behaviour {b} from tick {tick}, which is not ported (AI is P2.5)",
                s.name
            );
            continue;
        }
        let ours = &our_run.states;
        for m in &our_run.managers {
            let _ = writeln!(summary, "{}: {m}", s.name);
        }
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
            // A fight (the ships' attitudes come from the oracle) is
            // chaotic contact (record 0003 point 9): judged by its outcome,
            // which stays strict (owner, 2026-11-18).
            let fight = !ai.attitudes.is_empty();
            if !s.contact && ((!fight && max_pos > bound) || fa.alive != fb.alive) {
                gross.push(format!(
                    "{}: {name} (position {max_pos:.1} m > {bound:.1} m or alive {} vs {})",
                    s.name, fa.alive, fb.alive
                ));
            }
            first_divergences(&mut summary, &s.name, name, ours_states, their_states);
            if let Some(first) =
                first_bit_difference(&mut summary, &s.name, name, ours_states, their_states)
            {
                report_cores(&mut summary, dir, &s.name, name, first, &our_run);
            }
        }
        // The AI's phases come from the CRT sequence: with both sides
        // seeded after the load, the first AI ship's pathfinder phases sit
        // at the same index.
        if let (Some(rust), Some(oracle_p)) = (our_run.pathfind_phases, oracle_phases(dir, &s.name))
            && let (Some(i_rust), Some(i_oracle)) =
                (phase_index(s.seed, rust), phase_index(s.seed, oracle_p))
        {
            let _ = writeln!(
                summary,
                "{}: the AI's first draws are at index {i_oracle} of the oracle's rand() sequence and {i_rust} of the Rust world's (both from the seeding after the load)",
                s.name
            );
        }
        if !ai.attitudes.is_empty() {
            report_combat(&mut summary, &s.name, &our_run, &their_run);
        }
        report_hits(&mut summary, &s.name, &our_run.hits, &their_run.hits);
        report_units(&mut summary, dir, &s.name, &our_run);
        report_contacts(&mut summary, &s, &our_run, &their_run);
        if s.contact {
            judge_contact(&mut summary, &mut gross, &s, &our_run, &their_run);
        } else if our_run.contacts.is_empty() != their_run.contacts.is_empty() {
            gross.push(format!(
                "{}: collision events rust {} vs oracle {}",
                s.name,
                our_run.contacts.len(),
                their_run.contacts.len()
            ));
        }
    }
    print!("{summary}");
    if let Some(dir) = &oracle {
        let _ = fs::write(dir.join("rust-scenario-replay.txt"), &summary);
    }
    assert!(gross.is_empty(), "gross divergence: {gross:?}\n{summary}");
}
