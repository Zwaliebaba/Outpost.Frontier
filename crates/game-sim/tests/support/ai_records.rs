//! The behaviour-tick records of `Oracle.exe sim` (`design/sim-records.md`,
//! `ai.jsonl` and the `affect` records of `calls.jsonl`): one [`AiTick`]
//! per `IBehaviorManager::UpdateBehaviorTick`, with the manager fields on
//! entry and exit, the personality, the object snapshot, the queries and
//! the `rand()` values, and the commands the tick issued.

use std::collections::BTreeMap;
use std::path::Path;

use game_sim::ai::state_graph::{BEHAVIOR_COUNT, BehaviorScore};
use game_sim::ai::world::{ObjectView, StrafeDir};
use neuron_core::math::{Matrix, Vector};

use super::records::{Json, read_lines};

/// The manager fields of `ai_in` and `ai_out`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ManagerRecord {
    /// `behavior`.
    pub behavior: i32,
    /// `state_graph` (-1 none).
    pub state_graph: i32,
    /// `directive`.
    pub directive: [u32; 3],
    /// `maneuver_mask`.
    pub maneuver_mask: u32,
    /// `lock`.
    pub lock: bool,
    /// `dirty`.
    pub dirty: bool,
    /// `suppressed`.
    pub suppressed: bool,
    /// `timers`: A, B, C, attack override timer and period.
    pub timers: [f32; 5],
    /// `in_sights`.
    pub in_sights: u32,
    /// `f118`.
    pub f118: i32,
    /// `f11c`.
    pub f11c: i32,
    /// `f120`.
    pub f120: u32,
    /// `score_w` and `score_b`.
    pub scores: [BehaviorScore; BEHAVIOR_COUNT],
}

/// One object of a tick's snapshot.
#[derive(Clone, Debug, PartialEq)]
pub struct ObjRecord {
    /// The id.
    pub id: u32,
    /// The roles.
    pub roles: Vec<String>,
    /// The fields, `None` when the read faulted.
    pub view: Option<ObjectView>,
    /// The ship's attitude toward this object and the reverse.
    pub attitude: Option<(f32, f32)>,
}

/// One recorded query.
#[derive(Clone, Debug, PartialEq)]
pub struct QueryRecord {
    /// `ray`, `ray_object` or `sphere`.
    pub kind: String,
    /// The system (0 for `ray_object`).
    pub system: u32,
    /// The body `ray_object` tests.
    pub object: Option<u32>,
    /// The start (the centre for a sphere).
    pub from: Vector,
    /// The end.
    pub to: Vector,
    /// The radius (0 for rays).
    pub radius: f32,
    /// The hit limit.
    pub max: i32,
    /// The hits: the object id, the point and normal (rays).
    pub hits: Vec<(u32, Vector, Vector)>,
}

/// One behaviour tick.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct AiTick {
    /// The frame.
    pub tick: u32,
    /// The behaviour tick number.
    pub seq: u32,
    /// The object.
    pub id: u32,
    /// Its scenario name.
    pub obj: Option<String>,
    /// The tick's `dt`.
    pub dt: f32,
    /// The fields on entry.
    pub input: ManagerRecord,
    /// The target list.
    pub targets: Vec<u32>,
    /// The personality bytes, when written with this tick.
    pub personality: Option<Vec<u8>>,
    /// The snapshot.
    pub objects: Vec<ObjRecord>,
    /// The queries.
    pub queries: Vec<QueryRecord>,
    /// The fields on exit (`None` when the record is cut off).
    pub output: Option<ManagerRecord>,
    /// The `rand()` values the tick drew.
    pub rand: Vec<i32>,
    /// The module that drew each.
    pub rand_mod: Vec<String>,
}

/// One `IObjAffect` call (`affect` of `calls.jsonl`).
#[derive(Clone, Debug, PartialEq)]
pub struct Affect {
    /// The frame.
    pub tick: u32,
    /// The behaviour tick in progress (0 outside one).
    pub beh: u32,
    /// `behavior`, `scenario` or `other`.
    pub src: String,
    /// The ship.
    pub id: u32,
    /// The slot.
    pub slot: i32,
    /// The integer argument.
    pub i: i32,
    /// The float arguments.
    pub v: Option<Vec<f32>>,
}

fn f(j: &Json, key: &str) -> Option<f32> {
    j.get(key)?.as_f32()
}

fn u(j: &Json, key: &str) -> Option<u32> {
    j.get(key)?.as_u32()
}

fn i(j: &Json, key: &str) -> Option<i32> {
    j.get(key)?.as_i32()
}

fn b(j: &Json, key: &str) -> Option<bool> {
    j.get(key)?.as_bool()
}

fn v3(j: &Json, key: &str) -> Option<Vector> {
    let a = j.get(key)?.as_floats()?;
    (a.len() == 3).then(|| Vector::new(a[0], a[1], a[2]))
}

fn opt_u(j: &Json, key: &str) -> Option<u32> {
    j.get(key).and_then(Json::as_u32)
}

fn manager(j: &Json) -> Option<ManagerRecord> {
    let dir = j.get("directive")?.as_array()?;
    let timers = j.get("timers")?.as_floats()?;
    let weights = j.get("score_w")?.as_floats()?;
    let behaviors: Vec<i32> = j
        .get("score_b")?
        .as_array()?
        .iter()
        .filter_map(Json::as_i32)
        .collect();
    let mut scores = [BehaviorScore::default(); BEHAVIOR_COUNT];
    for (k, s) in scores.iter_mut().enumerate() {
        s.weight = *weights.get(k)?;
        s.behavior = *behaviors.get(k)?;
    }
    Some(ManagerRecord {
        behavior: i(j, "behavior")?,
        state_graph: i(j, "state_graph")?,
        directive: [
            dir.first()?.as_u32()?,
            dir.get(1)?.as_u32()?,
            dir.get(2)?.as_u32()?,
        ],
        maneuver_mask: u(j, "maneuver_mask")?,
        lock: b(j, "lock")?,
        dirty: b(j, "dirty")?,
        suppressed: b(j, "suppressed")?,
        timers: [timers[0], timers[1], timers[2], timers[3], timers[4]],
        in_sights: u(j, "in_sights")?,
        f118: i(j, "f118")?,
        f11c: i(j, "f11c")?,
        f120: u(j, "f120")?,
        scores,
    })
}

/// The object fields of an `ai_obj` record as an [`ObjectView`].
#[must_use]
pub fn object_view(j: &Json) -> Option<ObjectView> {
    let orient = j.get("orient")?.as_floats()?;
    let (radius, center) = (f(j, "radius")?, v3(j, "center")?);
    let extents = j
        .get("extents")
        .and_then(Json::as_floats)
        .filter(|e| e.len() == 6)
        .map(|e| (Vector::new(e[0], e[1], e[2]), Vector::new(e[3], e[4], e[5])));
    let shield = j.get("shield").and_then(Json::as_array).and_then(|a| {
        Some((
            a.first()?.as_f32()?,
            a.get(1)?.as_f32()?,
            a.get(2)?.as_bool()?,
        ))
    });
    let cruise = j
        .get("cruise")
        .and_then(Json::as_array)
        .and_then(|a| Some((a.first()?.as_bool()?, a.get(1)?.as_bool()?)));
    Some(ObjectView {
        id: u(j, "id")?,
        class: u(j, "class")?,
        type_flags: opt_u(j, "type").unwrap_or(0),
        position: v3(j, "pos")?,
        orientation: Matrix::from_rows([
            [orient[0], orient[1], orient[2]],
            [orient[3], orient[4], orient[5]],
            [orient[6], orient[7], orient[8]],
        ]),
        velocity: v3(j, "vel")?,
        angular_velocity: v3(j, "angvel")?,
        radius,
        center,
        center_of_mass: center,
        extents,
        mass: f(j, "mass")?,
        hit_points: (f(j, "hp")?, f(j, "hp_max")?),
        shield,
        throttle: f(j, "throttle"),
        axis_throttle: v3(j, "axis"),
        nudge: v3(j, "nudge"),
        strafe: i(j, "strafe").map(StrafeDir::from_value),
        cruise,
        thrusters: None,
        target: opt_u(j, "target"),
        subtarget: opt_u(j, "subtarget").and_then(|s| u16::try_from(s).ok()),
        rank: opt_u(j, "rank"),
        affiliation: opt_u(j, "affiliation"),
        base: opt_u(j, "base"),
        dock_target: opt_u(j, "dock_target"),
        power: None,
        zone_props: None,
        formation_leader: opt_u(j, "formation_leader"),
        formation_offset: None,
        follow_leader: opt_u(j, "follow_leader"),
        follow_offset: None,
        formation_followers: j
            .get("followers")
            .and_then(Json::as_array)
            .map(|a| a.iter().filter_map(Json::as_u32).collect())
            .unwrap_or_default(),
        is_player: b(j, "player").unwrap_or(false),
        is_dying: b(j, "dying").unwrap_or(false),
        is_targetable: b(j, "targetable").unwrap_or(false),
        toughness: i(j, "toughness").unwrap_or(0),
        behavior_id: i(j, "behavior_id").unwrap_or(-1),
        max_bank_angle: 0.0,
        scanner_interference: 0.0,
        atmosphere_range: f(j, "atmosphere").unwrap_or(0.0),
        engine: None,
    })
}

fn query(j: &Json) -> Option<QueryRecord> {
    let hits = j
        .get("hits")?
        .as_array()?
        .iter()
        .filter_map(|h| {
            let a = h.as_array()?;
            let id = a.first()?.as_u32()?;
            let point = a.get(1).and_then(Json::as_floats);
            let normal = a.get(2).and_then(Json::as_floats);
            let vec = |v: Option<Vec<f32>>| {
                v.filter(|v| v.len() == 3)
                    .map_or(Vector::ZERO, |v| Vector::new(v[0], v[1], v[2]))
            };
            Some((id, vec(point), vec(normal)))
        })
        .collect();
    Some(QueryRecord {
        kind: j.get("kind")?.as_str()?.to_owned(),
        system: u(j, "system").unwrap_or(0),
        object: opt_u(j, "object"),
        from: v3(j, "from")?,
        to: v3(j, "to")?,
        radius: f(j, "radius").unwrap_or(0.0),
        max: i(j, "max").unwrap_or(0),
        hits,
    })
}

fn unhex(text: &str) -> Vec<u8> {
    (0..text.len() / 2)
        .filter_map(|k| u8::from_str_radix(&text[2 * k..2 * k + 2], 16).ok())
        .collect()
}

/// The behaviour ticks of an `ai.jsonl`, in record order.
#[must_use]
pub fn read_ai_ticks(path: &Path) -> Vec<AiTick> {
    let mut out: Vec<AiTick> = Vec::new();
    for j in read_lines(path) {
        let rec = j.get("rec").and_then(Json::as_str).unwrap_or("");
        let seq = u(&j, "seq").unwrap_or(0);
        match rec {
            "ai_in" => {
                let Some(input) = manager(&j) else {
                    continue;
                };
                out.push(AiTick {
                    tick: u(&j, "tick").unwrap_or(0),
                    seq,
                    id: u(&j, "id").unwrap_or(0),
                    obj: j.get("obj").and_then(Json::as_str).map(str::to_owned),
                    dt: f(&j, "dt").unwrap_or(0.0),
                    input,
                    targets: j
                        .get("targets")
                        .and_then(Json::as_array)
                        .map(|a| a.iter().filter_map(Json::as_u32).collect())
                        .unwrap_or_default(),
                    ..AiTick::default()
                });
            }
            _ => {
                let Some(t) = out.last_mut().filter(|t| t.seq == seq) else {
                    continue;
                };
                match rec {
                    "personality" => {
                        t.personality = j.get("bytes").and_then(Json::as_str).map(unhex);
                    }
                    "ai_obj" => t.objects.push(ObjRecord {
                        id: u(&j, "id").unwrap_or(0),
                        roles: j
                            .get("roles")
                            .and_then(Json::as_array)
                            .map(|a| {
                                a.iter()
                                    .filter_map(|r| r.as_str().map(str::to_owned))
                                    .collect()
                            })
                            .unwrap_or_default(),
                        view: if j.get("fault").is_some() {
                            None
                        } else {
                            object_view(&j)
                        },
                        attitude: j
                            .get("attitude")
                            .and_then(Json::as_floats)
                            .filter(|a| a.len() == 2)
                            .map(|a| (a[0], a[1])),
                    }),
                    "ai_query" => {
                        if let Some(q) = query(&j) {
                            t.queries.push(q);
                        }
                    }
                    "ai_out" => {
                        t.output = manager(&j);
                        t.rand = j
                            .get("rand")
                            .and_then(Json::as_array)
                            .map(|a| a.iter().filter_map(Json::as_i32).collect())
                            .unwrap_or_default();
                        t.rand_mod = j
                            .get("rand_mod")
                            .and_then(Json::as_array)
                            .map(|a| {
                                a.iter()
                                    .filter_map(|m| m.as_str().map(str::to_owned))
                                    .collect()
                            })
                            .unwrap_or_default();
                    }
                    _ => {}
                }
            }
        }
    }
    out
}

/// The behaviour commands of a `calls.jsonl` (`affect` with `src`
/// `behavior`), by behaviour tick.
#[must_use]
pub fn read_affects(path: &Path) -> BTreeMap<u32, Vec<Affect>> {
    let mut out: BTreeMap<u32, Vec<Affect>> = BTreeMap::new();
    if !path.exists() {
        return out;
    }
    for j in read_lines(path) {
        if j.get("rec").and_then(Json::as_str) != Some("affect") {
            continue;
        }
        let a = Affect {
            tick: u(&j, "tick").unwrap_or(0),
            beh: u(&j, "beh").unwrap_or(0),
            src: j.get("src").and_then(Json::as_str).unwrap_or("").to_owned(),
            id: u(&j, "id").unwrap_or(0),
            slot: i(&j, "slot").unwrap_or(-1),
            i: i(&j, "i").unwrap_or(0),
            v: j.get("v").and_then(Json::as_floats),
        };
        if a.src == "behavior" && a.beh != 0 {
            out.entry(a.beh).or_default().push(a);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_a_tick() {
        let dir = std::env::temp_dir().join(format!("ai-records-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        let path = dir.join("ai.jsonl");
        let zeros = vec!["\"00000000\""; 21].join(",");
        let ints = vec!["0"; 21].join(",");
        let fields = format!(
            "\"behavior\":6,\"state_graph\":2,\"directive\":[0,0,15],\"maneuver_mask\":0,\"lock\":false,\"dirty\":false,\"suppressed\":false,\"timers\":[\"3f800000\",\"00000000\",\"00000000\",\"00000000\",\"3f800000\"],\"in_sights\":0,\"f118\":2,\"f11c\":-1,\"f120\":6,\"score_w\":[{zeros}],\"score_b\":[{ints}]"
        );
        let text = format!(
            "{{\"rec\":\"ai_in\",\"tick\":3,\"seq\":3,\"id\":5,\"obj\":\"pilot\",\"dt\":\"3d088889\",{fields},\"targets\":[]}}\n{{\"rec\":\"ai_out\",\"tick\":3,\"seq\":3,\"id\":5,\"obj\":\"pilot\",{fields},\"rand\":[7],\"rand_mod\":[\"Common.dll\"]}}\n"
        );
        std::fs::write(&path, text).unwrap_or_default();
        let ticks = read_ai_ticks(&path);
        let _ = std::fs::remove_dir_all(&dir);
        assert_eq!(ticks.len(), 1);
        assert_eq!(ticks[0].input.behavior, 6);
        assert_eq!(ticks[0].input.directive, [0, 0, 15]);
        assert_eq!(ticks[0].input.timers[0], 1.0);
        assert_eq!(ticks[0].rand, vec![7]);
        assert!(ticks[0].output.is_some());
    }
}
