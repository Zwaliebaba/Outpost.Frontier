//! The asteroid field walk and the docking points against the scenario
//! oracle (`Oracle.exe sim`'s `asteroids.jsonl` and `dock.jsonl`,
//! design/sim-records.md), replayed through the Rust world from the
//! oracle's own inputs, so that a difference in flight does not hide one in
//! the walk.
//!
//! * **Asteroid fields.** The scenario's system loads its fields
//!   ([`World::load_asteroid_fields`]); each tick the walk runs around the
//!   scenario ships where the oracle's `tick.jsonl` had them after the tick
//!   before (the server walks the ships after their update and before the
//!   physics moves them), with the frame time
//!   ([`World::update_asteroid_fields_with`]). Every cube the oracle made is
//!   compared when it is made (id, centre, snap rotation, emptiness, and its
//!   rocks: object ids, rock ids, positions, orientations, flags), and the
//!   cube states and timers of every field after every tick. Ids, states,
//!   timers and positions are Tier A; the rotations went through sines and
//!   cosines (Tier B, within a bound).
//! * **Docking points.** The docking server is P3.3, so the oracle's own
//!   calls drive the points: each `CEqObj::activate` it recorded is made in
//!   the same tick, before or after the object's update as the record order
//!   says, and the object is updated (kept updated, the server's `ListD4`)
//!   in exactly the ticks the oracle ran `update_docking_animations`. Each
//!   point the oracle processed is compared after the tick: its active flag,
//!   the playback's time and duration, and the last time it kept.
//!
//! A scenario without the files, or whose files are empty, is listed and
//! skipped. A summary goes to `rust-field-replay.txt` in the oracle folder.

mod support;

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

use game_sim::object::World;
use game_sim::object::field::CubeState;
use game_sim::object::world::WorldConfig;
use neuron_core::id::create_id;
use neuron_core::math::Vector;
use neuron_core::physics::StepMode;
use support::records::{Json, close, find_files, oracle_dir, read_lines};

/// The bound on the floats that went through sines and cosines.
const TIER_B: f32 = 1e-5;

struct Scenario {
    name: String,
    system: String,
    ticks: u32,
    frame: f64,
    sp: bool,
    seed: u32,
    ships: Vec<String>,
    solars: Vec<(String, String)>,
}

fn scenarios() -> Vec<Scenario> {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../CppSource/Oracle/scenarios");
    let mut files: Vec<PathBuf> = fs::read_dir(dir)
        .map(|d| {
            d.flatten()
                .map(|e| e.path())
                .filter(|p| p.extension().is_some_and(|x| x == "txt"))
                .collect()
        })
        .unwrap_or_default();
    files.sort();
    files.iter().filter_map(|p| parse(p)).collect()
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
        ships: Vec::new(),
        solars: Vec::new(),
    };
    for line in text.lines() {
        let line = line.split('#').next().unwrap_or("").trim();
        let t: Vec<&str> = line
            .split(|c: char| c.is_whitespace() || c == ',')
            .filter(|w| !w.is_empty())
            .collect();
        let Some(first) = t.first() else {
            continue;
        };
        match first.to_ascii_lowercase().as_str() {
            "system" => s.system = (*t.get(1)?).to_owned(),
            "ticks" => s.ticks = t.get(1)?.parse().ok()?,
            "frame" => s.frame = t.get(1)?.parse().ok()?,
            "stepping" => s.sp = t.get(1)?.eq_ignore_ascii_case("sp"),
            "seed" => s.seed = t.get(1)?.parse().ok()?,
            "ship" => s.ships.push((*t.get(1)?).to_owned()),
            "solar" => s
                .solars
                .push(((*t.get(1)?).to_owned(), (*t.get(2)?).to_owned())),
            _ => {}
        }
    }
    Some(s)
}

/// The first run's file of a scenario.
fn first_run(dir: &Path, scenario: &str, file: &str) -> Option<PathBuf> {
    find_files(&dir.join(scenario), file).into_iter().min()
}

fn world_for(s: &Scenario) -> Option<World> {
    support::world::world(WorldConfig {
        single_player: false,
        step_mode: if s.sp {
            StepMode::SinglePlayer
        } else {
            StepMode::Multiplayer
        },
        server_physics: true,
        seed: s.seed,
    })
}

fn u32_of(j: &Json, key: &str) -> u32 {
    j.get(key).and_then(Json::as_u32).unwrap_or(u32::MAX)
}

fn vec_of(j: &Json, key: &str) -> Option<Vector> {
    let f = j.get(key)?.as_floats()?;
    (f.len() == 3).then(|| Vector::new(f[0], f[1], f[2]))
}

/// The scenario ships' positions and negated forward axes by tick, in the
/// scenario's (spawn, list) order; a ship that is gone is left out.
fn observers(tick_file: &Path, ships: &[String]) -> BTreeMap<u32, Vec<(Vector, Vector)>> {
    let mut by_tick: BTreeMap<u32, BTreeMap<usize, (Vector, Vector)>> = BTreeMap::new();
    for j in read_lines(tick_file) {
        if j.get("rec").and_then(Json::as_str) != Some("object") {
            continue;
        }
        let Some(index) = j
            .get("obj")
            .and_then(Json::as_str)
            .and_then(|n| ships.iter().position(|s| s == n))
        else {
            continue;
        };
        if j.get("alive").and_then(Json::as_bool) != Some(true) {
            continue;
        }
        let (Some(pos), Some(orient)) =
            (vec_of(&j, "pos"), j.get("orient").and_then(Json::as_floats))
        else {
            continue;
        };
        if orient.len() != 9 {
            continue;
        }
        let dir = Vector::new(-orient[2], -orient[5], -orient[8]);
        by_tick
            .entry(u32_of(&j, "tick"))
            .or_default()
            .insert(index, (pos, dir));
    }
    by_tick
        .into_iter()
        .map(|(t, m)| (t, m.into_values().collect()))
        .collect()
}

/// One field's cubes as `(id, state, timer bits)`, in id order.
fn rust_cubes(world: &World, system: u32, field: usize) -> Vec<(u32, u32, u32)> {
    world
        .asteroid_fields(system)
        .get(field)
        .map(|f| {
            f.cubes
                .values()
                .map(|c| (c.id, c.state.value(), c.state_timer.to_bits()))
                .collect()
        })
        .unwrap_or_default()
}

/// The differences of a new cube from the oracle's `cube` record.
fn compare_cube(world: &World, system: u32, rec: &Json) -> Vec<String> {
    let mut out = Vec::new();
    let field = u32_of(rec, "field") as usize;
    let id = u32_of(rec, "id");
    let Some(cube) = world
        .asteroid_fields(system)
        .get(field)
        .and_then(|f| f.cubes.get(&id))
    else {
        out.push(format!("field {field} cube {id:08x} is missing"));
        return out;
    };
    if cube.state != CubeState::Active {
        out.push(format!("cube {id:08x} is {:?}, not new", cube.state));
    }
    if vec_of(rec, "pos") != Some(cube.pos) {
        out.push(format!("cube {id:08x} centre {:?}", cube.pos));
    }
    let orient = rec
        .get("orient")
        .and_then(Json::as_floats)
        .unwrap_or_default();
    let ours = cube.orient.to_words();
    if orient.len() != 9 || orient.iter().zip(ours).any(|(a, b)| !close(*a, b, TIER_B)) {
        out.push(format!(
            "cube {id:08x} rotation {ours:?} against {orient:?}"
        ));
    }
    if rec.get("empty").and_then(Json::as_bool) != Some(cube.empty) {
        out.push(format!("cube {id:08x} empty {}", cube.empty));
    }
    let rocks = rec.get("rocks").and_then(Json::as_array).unwrap_or(&[]);
    if rocks.len() != cube.rocks.len() || rocks.len() != cube.objects.len() {
        out.push(format!(
            "cube {id:08x}: {} rocks ({} objects) against {}",
            cube.rocks.len(),
            cube.objects.len(),
            rocks.len()
        ));
        return out;
    }
    for (i, (theirs, ours)) in rocks.iter().zip(&cube.rocks).enumerate() {
        let obj = world.get(cube.objects[i]);
        let same_ids = u32_of(theirs, "obj_id") == ours.id
            && u32_of(theirs, "asteroid_id") == ours.asteroid_id
            && obj.is_some_and(|o| o.simple.id == ours.id);
        let flags = theirs.get("mine").and_then(Json::as_bool) == Some(ours.mine)
            && theirs.get("instant_kill").and_then(Json::as_bool) == Some(ours.instant_kill);
        let pos_ok = vec_of(theirs, "pos").is_some_and(|p| {
            close(p.x, ours.position.x, TIER_B)
                && close(p.y, ours.position.y, TIER_B)
                && close(p.z, ours.position.z, TIER_B)
        });
        let rot = theirs
            .get("orient")
            .and_then(Json::as_floats)
            .unwrap_or_default();
        let rot_ok = rot.len() == 9
            && rot
                .iter()
                .zip(ours.orientation.to_words())
                .all(|(a, b)| close(*a, b, TIER_B));
        let engine_ok = obj.is_some_and(|o| o.engine.position == ours.position);
        if !(same_ids && flags && pos_ok && rot_ok && engine_ok) {
            out.push(format!(
                "cube {id:08x} rock {i}: ids {same_ids}, flags {flags}, position {pos_ok}, \
                 rotation {rot_ok}, object at its position {engine_ok}: {ours:?} against {theirs:?}"
            ));
        }
    }
    out
}

#[test]
fn asteroid_fields_against_the_oracle() {
    let Some(dir) = oracle_dir("asteroid fields") else {
        return;
    };
    if support::world::game_data().is_none() {
        println!("outdir/ is missing: no asteroid field was replayed");
        return;
    }
    let mut summary = String::new();
    let mut failures = Vec::new();
    for s in scenarios() {
        let Some(file) = first_run(&dir, &s.name, "asteroids.jsonl") else {
            let _ = writeln!(summary, "{}: no asteroids.jsonl", s.name);
            continue;
        };
        let records = read_lines(&file);
        if records.is_empty() {
            let _ = writeln!(summary, "{}: no cube in the oracle", s.name);
            continue;
        }
        let Some(mut world) = world_for(&s) else {
            continue;
        };
        let system = create_id(s.system.as_bytes());
        world.load_system(system);
        let fields = world.load_asteroid_fields(system);
        let obs = observers(&file.with_file_name("tick.jsonl"), &s.ships);
        let mut by_tick: BTreeMap<u32, Vec<&Json>> = BTreeMap::new();
        for r in &records {
            by_tick.entry(u32_of(r, "tick")).or_default().push(r);
        }
        let mut theirs: BTreeMap<usize, Vec<(u32, u32, u32)>> = BTreeMap::new();
        let (mut cubes_made, mut ticks_same, mut first_diff) = (0usize, 0u32, None);
        let mut diffs = Vec::new();
        for tick in 1..=s.ticks {
            let here = obs.get(&(tick - 1)).cloned().unwrap_or_default();
            world.update_asteroid_fields_with(system, &here, s.frame as f32);
            world.reap();
            for r in by_tick.get(&tick).into_iter().flatten() {
                match r.get("rec").and_then(Json::as_str) {
                    Some("cube") => {
                        cubes_made += 1;
                        for d in compare_cube(&world, system, r) {
                            diffs.push(format!("tick {tick}: {d}"));
                        }
                    }
                    Some("cubes") => {
                        let list = r
                            .get("cubes")
                            .and_then(Json::as_array)
                            .unwrap_or(&[])
                            .iter()
                            .map(|c| {
                                (
                                    u32_of(c, "id"),
                                    c.get("state")
                                        .and_then(Json::as_i32)
                                        .map_or(u32::MAX, i32::cast_unsigned),
                                    c.get("timer")
                                        .and_then(Json::as_f32)
                                        .map_or(u32::MAX, f32::to_bits),
                                )
                            })
                            .collect();
                        theirs.insert(u32_of(r, "field") as usize, list);
                    }
                    _ => {}
                }
            }
            let mut same = true;
            for field in 0..fields.max(theirs.len()) {
                let ours = rust_cubes(&world, system, field);
                let want = theirs.get(&field).cloned().unwrap_or_default();
                if ours != want {
                    same = false;
                    if first_diff.is_none() {
                        first_diff = Some(tick);
                        diffs.push(format!(
                            "tick {tick} field {field}: rust {ours:x?} oracle {want:x?}"
                        ));
                    }
                }
            }
            if same {
                ticks_same += 1;
            }
        }
        let line = format!(
            "{}: {fields} fields, {cubes_made} cubes made in the oracle, cube states equal on \
             {ticks_same} of {} ticks, first difference {first_diff:?}, {} differences",
            s.name,
            s.ticks,
            diffs.len()
        );
        println!("{line}");
        let _ = writeln!(summary, "{line}");
        for d in diffs.iter().take(20) {
            println!("    {d}");
            let _ = writeln!(summary, "    {d}");
        }
        if !diffs.is_empty() {
            failures.push(s.name.clone());
        }
    }
    println!("{summary}");
    let _ = fs::write(dir.join("rust-field-replay.txt"), &summary);
    assert!(failures.is_empty(), "asteroid fields differ: {failures:?}");
}

#[test]
fn docking_points_against_the_oracle() {
    let Some(dir) = oracle_dir("docking points") else {
        return;
    };
    if support::world::game_data().is_none() {
        println!("outdir/ is missing: no docking point was replayed");
        return;
    }
    let mut summary = String::new();
    let mut failures = Vec::new();
    for s in scenarios() {
        let Some(file) = first_run(&dir, &s.name, "dock.jsonl") else {
            let _ = writeln!(summary, "{}: no dock.jsonl", s.name);
            continue;
        };
        let records = read_lines(&file);
        if records.is_empty() {
            let _ = writeln!(summary, "{}: no docking point in the oracle", s.name);
            continue;
        }
        let Some(mut world) = world_for(&s) else {
            continue;
        };
        let system = create_id(s.system.as_bytes());
        world.load_system(system);
        let named: Vec<u32> = s
            .solars
            .iter()
            .map(|(_, n)| create_id(n.as_bytes()))
            .collect();
        world.load_system_solars_where(system, |sp| named.contains(&sp.nickname));
        let mut by_tick: BTreeMap<u32, Vec<&Json>> = BTreeMap::new();
        for r in &records {
            by_tick.entry(u32_of(r, "tick")).or_default().push(r);
        }
        let (mut compared, mut updates, mut activations) = (0usize, 0usize, 0usize);
        let mut diffs = Vec::new();
        let mut kept: Vec<game_sim::object::ObjHandle> = Vec::new();
        let last_tick = by_tick
            .keys()
            .next_back()
            .copied()
            .unwrap_or(0)
            .min(s.ticks);
        for tick in 1..=last_tick {
            let recs = by_tick.get(&tick).cloned().unwrap_or_default();
            // The objects updated in this tick, and where each update
            // starts in the record order.
            let mut first_update: BTreeMap<u32, u32> = BTreeMap::new();
            for r in &recs {
                if r.get("rec").and_then(Json::as_str) == Some("dock_anim") {
                    first_update
                        .entry(u32_of(r, "obj_id"))
                        .or_insert(u32_of(r, "seq"));
                }
            }
            let mut unknown = false;
            let handle = |w: &World, id: u32| w.find(id);
            for h in kept.drain(..) {
                world.set_always_update(h, false);
            }
            for &id in first_update.keys() {
                match handle(&world, id) {
                    Some(h) => {
                        world.set_always_update(h, true);
                        kept.push(h);
                    }
                    None => unknown = true,
                }
            }
            let mut after = Vec::new();
            for r in &recs {
                if r.get("rec").and_then(Json::as_str) != Some("dock_activate") {
                    continue;
                }
                let id = u32_of(r, "obj_id");
                let late = first_update
                    .get(&id)
                    .is_some_and(|&seq| u32_of(r, "seq") > seq);
                if late {
                    after.push(*r);
                } else {
                    activations += 1;
                    let Some(h) = handle(&world, id) else {
                        unknown = true;
                        continue;
                    };
                    let on = r.get("on").and_then(Json::as_bool) == Some(true);
                    let result = world.activate_docking_point(h, on, u32_of(r, "index") as usize);
                    let want = r.get("result").and_then(Json::as_i32).unwrap_or(-1);
                    let got = match result {
                        game_sim::object::world::ActivateResult::Failed => 0,
                        game_sim::object::world::ActivateResult::Changed => 1,
                        game_sim::object::world::ActivateResult::NoChange => 2,
                    };
                    if got != want {
                        diffs.push(format!(
                            "tick {tick}: activate answers {got}, oracle {want}"
                        ));
                    }
                }
            }
            if unknown {
                diffs.push(format!(
                    "tick {tick}: an object of the records is not in the world"
                ));
            }
            world.step(system, s.frame as f32);
            world.take_events();
            for r in after {
                activations += 1;
                if let Some(h) = handle(&world, u32_of(r, "obj_id")) {
                    let on = r.get("on").and_then(Json::as_bool) == Some(true);
                    world.activate_docking_point(h, on, u32_of(r, "index") as usize);
                }
            }
            for r in &recs {
                if r.get("rec").and_then(Json::as_str) != Some("dock_anim") {
                    continue;
                }
                updates += 1;
                let Some(h) = handle(&world, u32_of(r, "obj_id")) else {
                    continue;
                };
                let index = u32_of(r, "index") as usize;
                let point = world
                    .get(h)
                    .and_then(|o| o.as_equipped())
                    .and_then(|eq| eq.docking_points.get(index))
                    .cloned();
                let Some(point) = point else {
                    diffs.push(format!("tick {tick}: no docking point {index}"));
                    continue;
                };
                compared += 1;
                let f = |k: &str| r.get(k).and_then(Json::as_f32).map(f32::to_bits);
                let (time, duration) = point.playback.map_or((None, None), |p| {
                    (Some(p.time.to_bits()), Some(p.duration.to_bits()))
                });
                let ok = r.get("active").and_then(Json::as_bool) == Some(point.active)
                    && f("time") == time
                    && f("duration") == duration
                    && f("time") == Some(point.last_duration.to_bits());
                if !ok {
                    diffs.push(format!(
                        "tick {tick} point {index}: rust active {} time {:?} duration {:?} last {} \
                         against {:?}",
                        point.active,
                        point.playback.map(|p| p.time),
                        point.playback.map(|p| p.duration),
                        point.last_duration,
                        r
                    ));
                }
            }
        }
        let line = format!(
            "{}: {activations} activations, {updates} point updates, {compared} compared, {} \
             differences",
            s.name,
            diffs.len()
        );
        println!("{line}");
        let _ = writeln!(summary, "{line}");
        for d in diffs.iter().take(20) {
            println!("    {d}");
            let _ = writeln!(summary, "    {d}");
        }
        if !diffs.is_empty() {
            failures.push(s.name.clone());
        }
    }
    println!("{summary}");
    let _ = fs::write(dir.join("rust-dock-replay.txt"), &summary);
    assert!(failures.is_empty(), "docking points differ: {failures:?}");
}
