//! The oracle's records of a scenario run (`design/sim-records.md`), each
//! from the first run found under `<oracle dir>/<scenario>/`.

use std::collections::BTreeMap;
use std::fs;
use std::path::Path;

use neuron_core::id::create_id;
use neuron_core::math::Vector;
use neuron_core::physics::replay::CoreRecord;

use super::ai_records::read_ai_ticks;
use super::driver::{Contact, GuidedState, Hit, Run, ShieldState, State};
use super::records::{Json, find_files, read_lines};
use crate::ai::personality::Personality;
use crate::equip::SUBOBJ_ID_ROOT;

/// What the AI ships take from the oracle's records: each pilot's
/// personality (the world alone has no `[Pilot]` loader, which is
/// Content's), and the ships' first attitudes toward each other, which
/// the world answers from their vibes (P3.11b) and the judges only set
/// beside the world's ([`super::judge::report_attitudes`]) and take as the
/// mark of a fight.
#[derive(Clone, Debug, Default)]
pub struct AiInputs {
    /// Each AI ship's personality, by scenario name.
    pub personalities: BTreeMap<String, Personality>,
    /// Each ship's first attitude toward another in the oracle, by their
    /// scenario names.
    pub attitudes: BTreeMap<(String, String), f32>,
}

/// The [`AiInputs`] of a scenario from its first `ai.jsonl` under
/// `<oracle dir>/<scenario>/`: the first personality of each ship and its
/// first attitude toward each other scenario ship.
#[must_use]
pub fn oracle_ai_inputs(dir: &Path, scenario: &str) -> AiInputs {
    let mut out = AiInputs::default();
    let Some(first) = find_files(&dir.join(scenario), "ai.jsonl")
        .into_iter()
        .min()
    else {
        return out;
    };
    let ticks = read_ai_ticks(&first);
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

/// The oracle's `guided` records (`tick.jsonl`) by id.
#[must_use]
pub fn oracle_guided(dir: &Path, scenario: &str) -> BTreeMap<u32, Vec<GuidedState>> {
    let mut out: BTreeMap<u32, Vec<GuidedState>> = BTreeMap::new();
    let Some(file) = find_files(&dir.join(scenario), "tick.jsonl")
        .into_iter()
        .next()
    else {
        return out;
    };
    for r in read_lines(&file) {
        if r.get("rec").and_then(Json::as_str) != Some("guided") {
            continue;
        }
        let (Some(id), Some(tick)) = (
            r.get("id").and_then(Json::as_u32),
            r.get("tick").and_then(Json::as_u32),
        ) else {
            continue;
        };
        let v = |k: &str| {
            r.get(k)
                .and_then(Json::as_floats)
                .filter(|a| a.len() >= 3)
                .map_or(Vector::ZERO, |a| Vector::new(a[0], a[1], a[2]))
        };
        out.entry(id).or_default().push(GuidedState {
            tick,
            alive: r.get("alive").and_then(Json::as_bool).unwrap_or(false),
            pos: v("pos"),
            vel: v("vel"),
            orient: r
                .get("orient")
                .and_then(Json::as_floats)
                .and_then(|a| <[f32; 9]>::try_from(a).ok())
                .unwrap_or_default(),
            angvel: v("angvel"),
            lifetime: r.get("lifetime").and_then(Json::as_f32).unwrap_or(0.0),
            seeker_elapsed: r
                .get("seeker_elapsed")
                .and_then(Json::as_f32)
                .unwrap_or(0.0),
            target: r
                .get("target_obj")
                .and_then(Json::as_str)
                .map(str::to_owned)
                .or_else(|| {
                    r.get("target")
                        .and_then(Json::as_u32)
                        .map(|t| format!("#{t}"))
                })
                .unwrap_or_default(),
        });
    }
    out
}

/// The oracle's `guided` cores (`psi.jsonl`) by id: (tick, `out`).
#[must_use]
pub fn oracle_guided_cores(dir: &Path, scenario: &str) -> BTreeMap<u32, Vec<(u32, CoreRecord)>> {
    let mut out: BTreeMap<u32, Vec<(u32, CoreRecord)>> = BTreeMap::new();
    let Some(file) = find_files(&dir.join(scenario), "psi.jsonl")
        .into_iter()
        .min()
    else {
        return out;
    };
    let Ok(text) = fs::read_to_string(&file) else {
        return out;
    };
    for line in text.lines() {
        if !line.contains("\"obj\":\"guided\"") {
            continue;
        }
        let Ok(record) = neuron_core::physics::replay::PsiRecord::parse(line) else {
            continue;
        };
        out.entry(record.id)
            .or_default()
            .push((record.tick, record.output));
    }
    out
}

/// The oracle's run of a scenario: the first `tick.jsonl` under
/// `<oracle dir>/<scenario>/`, and the `events.jsonl` and `calls.jsonl`
/// beside it.
#[must_use]
pub fn oracle_run(dir: &Path, scenario: &str) -> Option<Run> {
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
#[must_use]
pub fn oracle_hits(file: &Path) -> Vec<Hit> {
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

/// The oracle's object states (`tick.jsonl` `object` records), each
/// object's in tick order.
#[must_use]
pub fn oracle_ticks(file: &Path) -> BTreeMap<String, Vec<State>> {
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
                nudge: r
                    .get("nudge")
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
    // An object first recorded after tick 0 (P3.6d: a ship the population
    // spawned during the run) is not alive before it.
    out.into_iter()
        .map(|(k, m)| {
            let first = m.keys().next().copied().unwrap_or(0) as usize;
            let mut states = vec![State::default(); first];
            states.extend(m.into_values());
            (k, states)
        })
        .collect()
}
