//! Scenario comparison (decision O20, closed loop; record 0003 point 8):
//! the scenarios of `CppSource/Oracle/scenarios` run through the Rust
//! world on the original data in `outdir/`, and, with
//! `OUTPOST_ORACLE_DIR` set, their per-tick object states are compared
//! with the oracle's `tick.jsonl` (the first run found under
//! `<oracle dir>/<scenario>/`).
//!
//! The Gate G2 envelopes (record 0003 points 9 and 10, approved by the
//! owner on 2026-11-19) are judged over the seed ensemble in the
//! `ensemble` module. This single-run comparison reports the measured
//! differences (position, velocity, orientation, hit
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
//! The guided munitions (P2.5, the server's `Guided` controller) are
//! compared with the oracle's `guided` records of `tick.jsonl` and its
//! `guided` cores of `psi.jsonl`, each side's numbered by id (the launch
//! order): the launch tick, the tick it is spent, the speed then, and the
//! first bit difference of the trajectory and of the core.
//!
//! A fight is judged as `ram_ship` is (owner, 2026-11-18; record 0003
//! point 9): bit for bit up to its first contact or munition impact,
//! whichever comes first in the oracle ([`judge_fight`]). A difference
//! before that tick, or a first contact, hit or spent missile on the Rust
//! side in any other tick (owner, 2026-11-19: in the same tick, as
//! `ram_ship`'s contact), is a gross divergence; after it the outcome
//! (alive states), the trajectories, the collisions, the hits and the
//! missiles' ends are reported for the Gate G2 envelopes, because the
//! differences after a contact come from the accepted contact solver (D87,
//! decision O19) and the fuse's timing (D177).
//!
//! For every scenario, the AI ones too, the report lists the `rand()`
//! draws the Rust world makes while it loads the system and spawns the
//! objects beside the oracle's load-time draws (`load_rand.jsonl`).
//!
//! A ship with an `ai` state graph gets a behaviour manager
//! (`pub::AI::SubmitState`), and the `goto`, `face`, `idle` and `cancel`
//! commands are directives (`pub::AI::SubmitDirective`); `fly_to_point`
//! (goto) and `turn_to_heading` (face) are compared with the oracle like
//! the other scenarios, and the directives that name objects (`dock`,
//! `launch`, `tradelane`) take their ids from the Rust world. Scenarios
//! whose directives need a behaviour that is not ported yet, and those
//! marked `# rust: skip <reason>` (the server's and Content's parts the
//! Rust world does not have yet) or `# rust: server <reason>` (answers
//! only the server gives, which `scenario_hooks` runs), are skipped with a
//! note. The fights (`dogfight`, `skirmish`: AI ships of hostile
//! factions; `strafe`: an AI fighter and a hostile ship without one) take
//! each pilot's personality and the ships' attitudes from the oracle's
//! `ai.jsonl` (a ship without a manager the reverse attitude of the AI
//! ship's snapshot), and are judged to their first contact or munition
//! impact (see below; record 0003 point 9): each ship's exact span, the
//! first behaviour and target difference, the behaviour sequences, the
//! hits before the first divergence and the outcome are reported. A
//! summary goes to `rust-scenario-replay.txt` in the oracle folder.
//!
//! The scenario files, the run driver, the oracle's records and the
//! judges are in `game_sim::scenario` (decision O45); this test runs them
//! without a server (`NoServerHooks`) and adds the diagnostic reports.
//! `game-logic/tests/scenario_hooks.rs` runs the same scenarios through
//! the server's hooks.

#[path = "scenario_replay/ensemble.rs"]
mod ensemble;
mod support;

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

use game_sim::ai::state_graph::GraphType;
use game_sim::object::World;
use game_sim::object::world::{NoServerHooks, ShipSpawn, WorldConfig};
use game_sim::scenario::{
    self, AiInputs, Contact, EXACT_SCENARIOS, GROSS_POSITION, Hit, Run, Scenario, State, Unit,
    dist, first_event, first_event_of, fmt_contact, gun_text, judge_contact, judge_exact,
    judge_fight, needs_ai, needs_server, oracle_ai_inputs, oracle_run, parse, report_guided,
    report_object_errors, same_state, scenario_files, scenarios_dir, spawn_orientation,
    world_config,
};
use neuron_core::id::create_id;
use neuron_core::math::Vector;
use neuron_core::physics::StepMode;
use neuron_core::physics::replay::{CoreRecord, compare};
use neuron_core::rng::CrtRand;
use support::records::{Json, find_files, read_lines};

/// A scenario's run without a server: the world over `outdir/`
/// ([`support::world::world`]) and the library's driver with
/// [`NoServerHooks`]. `None` when the game data is missing.
fn run(s: &Scenario, ai: &AiInputs) -> Option<Run> {
    let world = support::world::world(world_config(s))?;
    Some(scenario::run(world, s, ai, &mut NoServerHooks))
}

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

/// The first tick whose turret joints differ from the oracle's
/// (`tick.jsonl` `guns`), per ship (diagnostic).
fn report_guns(summary: &mut String, dir: &Path, scenario: &str, ours: &Run) {
    let Some(file) = find_files(&dir.join(scenario), "tick.jsonl")
        .into_iter()
        .next()
    else {
        return;
    };
    let mut theirs: BTreeMap<String, Vec<(u32, String)>> = BTreeMap::new();
    let records = read_lines(&file);
    // The lock targets by the scenario's names (the two worlds number their
    // objects differently).
    let oracle_names: BTreeMap<u32, String> = records
        .iter()
        .filter(|r| r.get("rec").and_then(Json::as_str) == Some("object"))
        .filter_map(|r| Some((r.get("id")?.as_u32()?, r.get("obj")?.as_str()?.to_owned())))
        .collect();
    for r in &records {
        if r.get("rec").and_then(Json::as_str) != Some("object") {
            continue;
        }
        let Some(guns) = r.get("guns").and_then(Json::as_array) else {
            continue;
        };
        let (Some(name), Some(tick)) = (
            r.get("obj").and_then(Json::as_str),
            r.get("tick").and_then(Json::as_u32),
        ) else {
            continue;
        };
        let text = guns
            .iter()
            .filter_map(|g| {
                let a = g.as_array()?;
                let id = a.first()?.as_u32()?;
                let v: Vec<f32> = a.get(1..6)?.iter().filter_map(Json::as_f32).collect();
                let int = |i: usize| a.get(i).and_then(Json::as_u32).unwrap_or(0);
                let lock = match int(8) {
                    0 => "-".to_owned(),
                    id => oracle_names
                        .get(&id)
                        .cloned()
                        .unwrap_or_else(|| format!("#{id}")),
                };
                let extra = format!("b{}{} t{lock}", int(6), int(7));
                Some(gun_text(id, <[f32; 5]>::try_from(v).ok()?, &extra))
            })
            .collect::<Vec<_>>()
            .join(" ");
        theirs
            .entry(name.to_owned())
            .or_default()
            .push((tick, text));
    }
    for (name, list) in &theirs {
        let Some(mine) = ours.guns.get(name) else {
            continue;
        };
        // The Rust side records the guns after each tick from tick 1 (its
        // first entry is tick 1's), the oracle from tick 0.
        let rust_at = |t: u32| t.checked_sub(1).and_then(|k| mine.get(k as usize));
        match list
            .iter()
            .find(|(t, text)| rust_at(*t).is_some_and(|m| m != text))
        {
            None => {
                let _ = writeln!(
                    summary,
                    "{scenario}: {name}: turret joints equal the oracle's on every tick"
                );
            }
            Some((t, text)) => {
                let _ = writeln!(
                    summary,
                    "{scenario}: {name}: turret joints first differ at tick {t}: rust {} | oracle {text}",
                    rust_at(*t).cloned().unwrap_or_default()
                );
            }
        }
    }
}

/// The Rust world's turret probes against the oracle's own-ship item
/// records (`ai_obj.equip`: barrel 0 and `can_point`), each `ai_in` of
/// tick `t` against the Rust world after tick `t - 1` and after tick `t`
/// (diagnostic).
fn report_turret_probes(summary: &mut String, dir: &Path, scenario: &str, ours: &Run) {
    let Some(first) = find_files(&dir.join(scenario), "ai.jsonl")
        .into_iter()
        .min()
    else {
        return;
    };
    let ticks = support::ai_records::read_ai_ticks(&first);
    for (label, shift) in [("before", 1_u32), ("after", 0_u32)] {
        let mut compared = 0_usize;
        let mut differ = 0_usize;
        let mut first_diff: Option<String> = None;
        for t in &ticks {
            let Some(ship) = t.obj.as_deref() else {
                continue;
            };
            let Some(own) = t.objects.iter().find(|o| o.id == t.id) else {
                continue;
            };
            let Some(equip) = &own.equip else {
                continue;
            };
            let Some(mine) = t
                .tick
                .checked_sub(shift)
                .and_then(|k| ours.probes.get(&k))
                .and_then(|m| m.get(ship))
            else {
                continue;
            };
            let names: BTreeMap<u32, String> = t
                .objects
                .iter()
                .filter_map(|o| Some((o.id, o.name.clone()?)))
                .collect();
            for e in equip.iter().filter(|e| e.type_mask == 0x80) {
                let Some(p) = mine.iter().find(|p| p.gun == e.id) else {
                    continue;
                };
                let mut notes = Vec::new();
                if let (Some((theirs, _)), Some(rust)) = (e.barrel, p.barrel)
                    && [theirs.x, theirs.y, theirs.z].map(f32::to_bits)
                        != [rust.x, rust.y, rust.z].map(f32::to_bits)
                {
                    notes.push(format!("barrel rust {rust:?} oracle {theirs:?}"));
                }
                // The limited flags (index 1 and 5) are the oracle's only.
                if let (Some(theirs), Some(rust)) = (&e.turret, &p.turret) {
                    let differ: Vec<usize> = (0..29)
                        .filter(|&k| k != 1 && k != 5)
                        .filter(|&k| theirs[k].to_bits() != rust[k].to_bits())
                        .collect();
                    if !differ.is_empty() {
                        notes.push(format!(
                            "turret words {differ:?} rust {rust:?} oracle {theirs:?}"
                        ));
                    }
                }
                for (id, answer) in &e.can_point {
                    let Some(n) = names.get(id) else {
                        continue;
                    };
                    match p.can_point.get(n) {
                        Some(r) if r != answer => {
                            notes.push(format!("can_point {n} rust {r} oracle {answer}"));
                        }
                        _ => {}
                    }
                }
                compared += 1;
                if !notes.is_empty() {
                    differ += 1;
                    if first_diff.is_none() {
                        first_diff = Some(format!(
                            "tick {} {ship} gun {}: {}",
                            t.tick,
                            e.id,
                            notes.join("; ")
                        ));
                    }
                }
            }
        }
        if compared > 0 {
            let _ = writeln!(
                summary,
                "{scenario}: turret probes ({label} the tick): {compared} compared, {differ} differ{}",
                first_diff.map_or_else(String::new, |d| format!("; first: {d}"))
            );
        }
    }
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
    // load makes objects the Rust world does not): each ship's or
    // munition's id becomes its place in the order of the ids of its class
    // (the spawn order).
    let by_spawn = |v: &[Vec<Unit>]| -> Vec<Vec<Unit>> {
        let numbered = |class: u32| {
            let mut ids: Vec<u32> = v
                .iter()
                .flatten()
                .filter(|u| u.class == class)
                .map(|u| u.id)
                .collect();
            ids.sort_unstable();
            ids.dedup();
            ids
        };
        let (ships, guided) = (numbered(0x503), numbered(0xa7));
        v.iter()
            .map(|psi| {
                psi.iter()
                    .map(|u| {
                        let list = match u.class {
                            0x503 => &ships,
                            0xa7 => &guided,
                            _ => return *u,
                        };
                        match list.iter().position(|s| *s == u.id) {
                            Some(k) => Unit {
                                id: k as u32 + 1,
                                ..*u
                            },
                            None => *u,
                        }
                    })
                    .collect()
            })
            .collect()
    };
    let theirs = by_spawn(&theirs);
    // The guided munitions as simulation units: the PSI each first runs.
    let launches = |v: &[Vec<Unit>]| {
        let mut seen: Vec<(u32, usize)> = Vec::new();
        for (p, psi) in v.iter().enumerate() {
            for u in psi {
                if u.class == 0xa7 && !seen.iter().any(|(id, _)| *id == u.id) {
                    seen.push((u.id, p + 1));
                }
            }
        }
        seen.iter().map(|(_, p)| *p).collect::<Vec<_>>()
    };
    let _ = writeln!(
        summary,
        "{scenario}: guided munitions first stepped at PSI rust {:?} oracle {:?} (of {}); rust launches (tick, shooter, gun) {:?}",
        launches(&ours.units),
        launches(&theirs),
        theirs.len(),
        ours.missiles.iter().take(8).collect::<Vec<_>>()
    );
    // Around the oracle's first guided munition: the fire calls of both
    // sides (the oracle's `affect` records of `calls.jsonl`, slots 13 and 19).
    if let Some(&first) = launches(&theirs).first() {
        let to = u32::try_from(first).unwrap_or(0) + 1;
        let from = 0;
        for (t, who, r) in ours.fires.iter().filter(|f| (from..=to).contains(&f.0)) {
            let _ = writeln!(summary, "    rust fire tick {t} {who}: {r}");
        }
        if let Some(file) = find_files(&dir.join(scenario), "calls.jsonl")
            .into_iter()
            .min()
        {
            for r in read_lines(&file) {
                if r.get("rec").and_then(Json::as_str) != Some("affect") {
                    continue;
                }
                let tick = r.get("tick").and_then(Json::as_u32).unwrap_or(0);
                let slot = r.get("slot").and_then(Json::as_i32).unwrap_or(-1);
                if !(from..=to).contains(&tick) || !matches!(slot, 13 | 19) {
                    continue;
                }
                let _ = writeln!(
                    summary,
                    "    oracle affect tick {tick} {} slot {slot} ({}): i {} depth {} src {}",
                    r.get("obj").and_then(Json::as_str).unwrap_or("?"),
                    r.get("m").and_then(Json::as_str).unwrap_or("?"),
                    r.get("i").and_then(Json::as_i32).unwrap_or(-1),
                    r.get("depth").and_then(Json::as_i32).unwrap_or(-1),
                    r.get("src").and_then(Json::as_str).unwrap_or("?")
                );
            }
        }
    }
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
        if let (Some(x), Some(y)) = (a.nudge, b.nudge)
            && bits(x) != bits(y)
        {
            out.push(format!("nudge {} | {}", hex3(x), hex3(y)));
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
        let nu = |st: &State| st.nudge.map_or_else(|| "-".to_owned(), hex3);
        let _ = writeln!(
            summary,
            "    tick {t}: nudge {} | {}, axis {} | {}, angvel {} | {}",
            nu(&ours[t]),
            nu(&theirs[t]),
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
                spawn_orientation(&o.orient),
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

/// The bullet rays IVP's ray solver does not let hit an object the Rust
/// world's pose crosses: those that started inside the object, and those
/// whose OV sphere test (the core at the start of its PSI) culled it.
fn report_rays(summary: &mut String, scenario: &str, ours: &Run) {
    for (what, list) in [
        ("started inside an object", &ours.starts_inside),
        (
            "crossed an object its OV sphere test culled",
            &ours.ov_culled,
        ),
    ] {
        if list.is_empty() {
            continue;
        }
        let shown: Vec<String> = list
            .iter()
            .take(8)
            .map(|(t, n)| format!("tick {t} {n}"))
            .collect();
        let _ = writeln!(
            summary,
            "{scenario}: bullet rays that {what} (no hit, as in IVP): {}; {}",
            list.len(),
            shown.join(", ")
        );
    }
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
    for path in scenario_files() {
        let Some(s) = parse(&path) else {
            let _ = writeln!(summary, "{}: does not parse", path.display());
            continue;
        };
        report_load_draws(&mut summary, &s, oracle.as_deref());
        if let Some(why) = needs_ai(&s) {
            let _ = writeln!(summary, "{}: skipped, {why} (AI is P2.5)", s.name);
            continue;
        }
        if let Some(why) = needs_server(&s) {
            let _ = writeln!(
                summary,
                "{}: skipped without a server, {why} (game-logic's scenario_hooks runs it)",
                s.name
            );
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
            // The missiles fly before it.
            if let Some(dir) = &oracle {
                report_guided(&mut summary, &mut gross, dir, &s.name, &our_run, u32::MAX);
            }
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
            // The gross bound (not for a contact scenario or a fight; the
            // library's `judge` makes the same judgements in one call).
            let fight = !ai.attitudes.is_empty();
            report_object_errors(
                &mut summary,
                &mut gross,
                &s,
                name,
                ours_states,
                their_states,
                fight,
            );

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
        report_rays(&mut summary, &s.name, &our_run);
        report_units(&mut summary, dir, &s.name, &our_run);
        let fight = !ai.attitudes.is_empty();
        let cutoff = if fight {
            first_event(dir, &s.name, &their_run).unwrap_or(u32::MAX)
        } else {
            u32::MAX
        };
        report_guided(&mut summary, &mut gross, dir, &s.name, &our_run, cutoff);
        report_guns(&mut summary, dir, &s.name, &our_run);
        report_turret_probes(&mut summary, dir, &s.name, &our_run);
        report_contacts(&mut summary, &s, &our_run, &their_run);
        if EXACT_SCENARIOS.contains(&s.name.as_str()) {
            judge_exact(&mut summary, &mut gross, &s, &our_run, &their_run);
        }
        if s.contact {
            judge_contact(&mut summary, &mut gross, &s, &our_run, &their_run);
        } else if fight {
            judge_fight(&mut summary, &mut gross, dir, &s, &our_run, &their_run);
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
