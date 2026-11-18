//! The behaviour-tick records of `Oracle.exe sim` (`design/sim-records.md`,
//! `ai.jsonl` and the `affect` records of `calls.jsonl`): one [`AiTick`]
//! per `IBehaviorManager::UpdateBehaviorTick`, with the manager fields on
//! entry and exit, the personality, the object snapshot, the queries and
//! the `rand()` values, and the commands the tick issued.

use std::collections::BTreeMap;
use std::path::Path;

use game_sim::ai::gun_groups::{CmDroppers, GunGroups, GunSlot, LineOfFireSlot};
use game_sim::ai::state_graph::{BEHAVIOR_COUNT, BehaviorScore};
use game_sim::ai::target_manager::TargetManager;
use game_sim::ai::timer_table::{SustainedThresholdTimer, TimerTable};
use game_sim::ai::world::{EngineStatus, ObjectView, StrafeDir};
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
    /// `pathfind` (records written before the field: `None`).
    pub pathfind: Option<PathfindRecord>,
    /// `thrust`: the thrust manager's flags (cruise engaged 1, cruise
    /// unobstructed 2, engine active 4, auto level 8, auto avoidance 16).
    pub thrust: Option<u32>,
    /// `flags_1c8`.
    pub flags_1c8: Option<u8>,
    /// `sys`: the system manager's combat state.
    pub sys: Option<SysRecord>,
    /// `face`: the face behaviour's fields while it is current.
    pub face: Option<FaceRecord>,
}

/// The system manager's combat state (`sys`), in the port's types. The
/// fields the record does not hold keep their defaults: the target
/// manager's enemy clamp, the gun groups' countermeasure `player` flag.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SysRecord {
    /// `targeting`.
    pub targeting: TargetManager,
    /// `timers`.
    pub timers: TimerTable,
    /// `guns`.
    pub guns: GunGroups,
}

/// The face behaviour's fields (`face`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct FaceRecord {
    /// +0x0C.
    pub mode: i32,
    /// +0x10.
    pub result: i32,
    /// +0x1C.
    pub dt: f32,
    /// +0x28.
    pub flags: u8,
    /// +0x29.
    pub flag29: u8,
    /// +0x2C.
    pub kind: i32,
    /// +0x30's id (0 none).
    pub watched: u32,
    /// +0x38.
    pub orient: Matrix,
    /// +0x5C.
    pub target: Vector,
    /// +0x1B4.
    pub flag_1b4: u8,
    /// +0x1B8 to +0x1C0.
    pub steer: Vector,
    /// +0x1C4.
    pub flag_1c4: u8,
    /// +0x1C5.
    pub flag_1c5: u8,
    /// +0x1C8.
    pub field_1c8: f32,
    /// +0x1CC.
    pub field_1cc: f32,
    /// +0x1D0.
    pub field_1d0: f32,
    /// +0x1D4.
    pub flag_1d4: u8,
    /// +0x1D8.
    pub field_1d8: f32,
}

/// One of the own ship's items (`ai_obj.equip`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct EquipRecord {
    /// The id.
    pub id: u16,
    /// The `find_equipment` mask that found it.
    pub type_mask: u32,
    /// `get_equipment_status`: the flags and `EquipFlags`, `None` when the
    /// call failed.
    pub status: Option<(u8, u32)>,
    /// `get_equipment_val` words: munition kind, auto turret, seeker,
    /// range, target id (`None` when the call failed).
    pub vals: [Option<u32>; 5],
    /// Barrel 0's position and direction (launchers).
    pub barrel: Option<(Vector, Vector)>,
    /// `is_pointing_at` at the gun angle, the wide and the missile cone.
    pub pointing: [bool; 3],
    /// `can_point_at(candidate position, 0.3)`: candidate id, answer.
    pub can_point: Vec<(u32, bool)>,
}

fn matrix(a: &[f32]) -> Option<Matrix> {
    (a.len() == 9)
        .then(|| Matrix::from_rows([[a[0], a[1], a[2]], [a[3], a[4], a[5]], [a[6], a[7], a[8]]]))
}

fn byte(j: &Json, key: &str) -> Option<u8> {
    u8::try_from(u(j, key)?).ok()
}

fn face(j: &Json) -> Option<FaceRecord> {
    Some(FaceRecord {
        mode: i(j, "mode")?,
        result: i(j, "result")?,
        dt: f(j, "dt")?,
        flags: byte(j, "flags")?,
        flag29: byte(j, "flag29")?,
        kind: i(j, "kind")?,
        watched: u(j, "watched")?,
        orient: matrix(&j.get("orient")?.as_floats()?)?,
        target: v3(j, "target")?,
        flag_1b4: byte(j, "flag_1b4")?,
        steer: v3(j, "steer")?,
        flag_1c4: byte(j, "flag_1c4")?,
        flag_1c5: byte(j, "flag_1c5")?,
        field_1c8: f(j, "field_1c8")?,
        field_1cc: f(j, "field_1cc")?,
        field_1d0: f(j, "field_1d0")?,
        flag_1d4: byte(j, "flag_1d4")?,
        field_1d8: f(j, "field_1d8")?,
    })
}

fn gun_slot(j: &Json) -> Option<GunSlot> {
    let a = j.as_array()?;
    Some(GunSlot {
        id: u16::try_from(a.first()?.as_u32()?).ok()?,
        auto_turret: a.get(1)?.as_bool()?,
        seeker: u8::try_from(a.get(2)?.as_i32()?).ok()?,
        fire_tick: a.get(3)?.as_bool()?,
        kind: a.get(4)?.as_i32()?,
        burst_time: a.get(5)?.as_f32()?,
        no_burst_time: a.get(6)?.as_f32()?,
        fire_interval: a.get(7)?.as_f32()?,
        phase: a.get(8)?.as_f32()?,
    })
}

fn slots(j: &Json, key: &str) -> Option<Vec<GunSlot>> {
    j.get(key)?.as_array()?.iter().map(gun_slot).collect()
}

fn targeting(j: &Json) -> Option<TargetManager> {
    let anchor = v3(j, "anchor")?;
    Some(TargetManager {
        attack_state: i(j, "attack_state")?,
        aggression_ratio: f(j, "aggression")?,
        subtarget_distance_sq: f(j, "subtarget_dist_sq")?,
        subtarget_id: u16::try_from(u(j, "subtarget")?).ok()?,
        forced_targets: j
            .get("forced")?
            .as_array()?
            .iter()
            .filter_map(Json::as_u32)
            .collect(),
        anchor,
        committed: b(j, "committed")?,
        commit_timer: f(j, "commit_timer")?,
        selection_priority: i(j, "priority")?,
        candidates_a: i(j, "candidates_a")?,
        candidates_b: i(j, "candidates_b")?,
        selected: i(j, "selected")?,
        selected_is_player: b(j, "selected_is_player")?,
        targeted_enemy_count: i(j, "targeted_enemies")?,
        over_clamp_kept: b(j, "over_clamp_kept")?,
        over_clamp_dropped: b(j, "over_clamp_dropped")?,
        ..TargetManager::default()
    })
}

fn timer_table(j: &Json) -> Option<TimerTable> {
    let mut timers = [SustainedThresholdTimer::default(); 11];
    let rows = j.get("timers")?.as_array()?;
    for (t, row) in timers.iter_mut().zip(rows) {
        let a = row.as_array()?;
        *t = SustainedThresholdTimer {
            active: a.first()?.as_bool()?,
            threshold: a.get(1)?.as_f32()?,
            duration: a.get(2)?.as_f32()?,
            accumulator: a.get(3)?.as_f32()?,
        };
    }
    let scanner = u(j, "scanner")?;
    let best = u(j, "best_missile")?;
    Some(TimerTable {
        // `SUBOBJ_ID_NONE` and 0 are no scanner.
        scanner: u16::try_from(scanner)
            .ok()
            .filter(|&id| id != 0 && id != 0xffff),
        decay_rate: f(j, "decay_rate")?,
        level: f(j, "level")?,
        timers,
        decay_scale: f(j, "decay_scale")?,
        missiles: j
            .get("missiles")?
            .as_array()?
            .iter()
            .filter_map(Json::as_u32)
            .collect(),
        best_missile: (best != 0).then_some(best),
        best_time: f(j, "best_time")?,
    })
}

fn gun_groups(j: &Json) -> Option<GunGroups> {
    let params = j.get("params")?.as_floats()?;
    let p = |k: usize| params.get(k).copied();
    let mut line_of_fire = [LineOfFireSlot::default(); 16];
    for (slot, row) in line_of_fire
        .iter_mut()
        .zip(j.get("line_of_fire")?.as_array()?)
    {
        let a = row.as_array()?;
        *slot = LineOfFireSlot {
            in_use: a.first()?.as_bool()?,
            target_id: a.get(1)?.as_u32()?,
            hardpoint: u16::try_from(a.get(2)?.as_u32()?).ok()?,
            age: a.get(3)?.as_f32()?,
            refresh_timer: a.get(4)?.as_f32()?,
            needs_refresh: a.get(5)?.as_bool()?,
            blocked: a.get(6)?.as_bool()?,
        };
    }
    let cm = j.get("cm")?.as_array()?;
    let target = u(j, "target")?;
    Some(GunGroups {
        last_gun_fire_result: i(j, "last_result")?,
        target: (target != 0).then_some(target),
        hold_mask: u(j, "hold")?,
        aim_offset: v3(j, "aim_offset")?,
        aim_refresh_timer: f(j, "aim_timer")?,
        cruise_held_time: f(j, "cruise_held")?,
        scan_timer: f(j, "scan_timer")?,
        scan_period: f(j, "scan_period")?,
        guns: slots(j, "slots")?,
        mine_droppers: slots(j, "mines")?,
        gun_fire_interval_time: p(0)?,
        gun_fire_interval_variance: p(1)?,
        auto_turret_interval_time: p(2)?,
        gun_fire_burst_interval_time: p(3)?,
        gun_fire_burst_interval_variance: p(4)?,
        gun_fire_no_burst_interval_time: p(5)?,
        auto_turret_burst_interval_time: p(6)?,
        auto_turret_no_burst_interval_time: p(7)?,
        auto_turret_burst_interval_variance: p(8)?,
        missile_launch_interval_time: p(9)?,
        missile_launch_interval_variance: p(10)?,
        missile_launch_cone_angle: p(11)?,
        missile_launch_range: p(12)?,
        missile_launch_allow_out_of_range: p(13)?,
        mine_launch_interval: p(14)?,
        mine_launch_cone_angle: p(15)?,
        // p(16) is `Field_0x107C`, which the port does not keep.
        gun_fire_accuracy_cone_angle: p(17)?,
        gun_range: p(18)?,
        fire_all_guns: b(j, "fire_all")?,
        wide_cone_angle: f(j, "wide_cone")?,
        enabled: b(j, "enabled")?,
        line_of_fire,
        cm: CmDroppers {
            timer: cm.first()?.as_f32()?,
            active: cm.get(1)?.as_bool()?,
            ids: cm
                .get(2)?
                .as_array()?
                .iter()
                .filter_map(|v| u16::try_from(v.as_u32()?).ok())
                .collect(),
            player: false,
        },
    })
}

fn sys(j: &Json) -> Option<SysRecord> {
    Some(SysRecord {
        targeting: targeting(j.get("targeting")?)?,
        timers: timer_table(j.get("timers")?)?,
        guns: gun_groups(j.get("guns")?)?,
    })
}

fn equip(j: &Json) -> Option<EquipRecord> {
    let status = j.get("status").and_then(Json::as_array).and_then(|a| {
        Some((
            u8::try_from(a.first()?.as_u32()?).ok()?,
            a.get(1)?.as_u32()?,
        ))
    });
    let val = |k: &str| j.get(k).and_then(Json::as_u32);
    let pointing = j.get("pointing")?.as_array()?;
    Some(EquipRecord {
        id: u16::try_from(u(j, "id")?).ok()?,
        type_mask: u(j, "type")?,
        status,
        vals: [
            val("munition"),
            val("auto_turret"),
            val("seeker"),
            val("range"),
            val("target"),
        ],
        barrel: j
            .get("barrel")
            .and_then(Json::as_floats)
            .filter(|a| a.len() == 6)
            .map(|a| (Vector::new(a[0], a[1], a[2]), Vector::new(a[3], a[4], a[5]))),
        pointing: [
            pointing.first()?.as_bool()?,
            pointing.get(1)?.as_bool()?,
            pointing.get(2)?.as_bool()?,
        ],
        can_point: j
            .get("can_point")
            .and_then(Json::as_array)
            .map(|a| {
                a.iter()
                    .filter_map(|c| {
                        let c = c.as_array()?;
                        Some((c.first()?.as_u32()?, c.get(1)?.as_bool()?))
                    })
                    .collect()
            })
            .unwrap_or_default(),
    })
}

/// The pathfinder fields of `ai_in` and `ai_out` (`pathfind`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PathfindRecord {
    /// `Timer1E0`, `Timer1E8`, `Timer20C`.
    pub timers: [f32; 3],
    /// `Period1E4`, `Period1EC`, `Period210`.
    pub periods: [f32; 3],
    /// `Field_214`.
    pub record: i32,
    /// `Field_22C`.
    pub goal: Vector,
    /// `Field_23C`.
    pub cap: f32,
    /// `Field_240`.
    pub kind: i32,
    /// `CachedTargetPosition`.
    pub target: Vector,
    /// `Field_064`.
    pub target_id: u32,
    /// `Reaction.State`.
    pub reaction: i32,
    /// `Avoidance.Active`.
    pub probe_active: bool,
    /// `Avoidance.HasContact`.
    pub probe_contact: bool,
}

fn pathfind(j: &Json) -> Option<PathfindRecord> {
    let timers = j.get("timers")?.as_floats()?;
    let periods = j.get("periods")?.as_floats()?;
    Some(PathfindRecord {
        timers: [*timers.first()?, *timers.get(1)?, *timers.get(2)?],
        periods: [*periods.first()?, *periods.get(1)?, *periods.get(2)?],
        record: i(j, "record")?,
        goal: v3(j, "goal")?,
        cap: f(j, "cap")?,
        kind: i(j, "kind")?,
        target: v3(j, "target")?,
        target_id: u(j, "target_id")?,
        reaction: i(j, "reaction")?,
        probe_active: b(j, "probe_active")?,
        probe_contact: b(j, "probe_contact")?,
    })
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
    /// `targeted_enemies` (ships).
    pub targeted_enemies: Option<i32>,
    /// `lead`: the own ship's `CShip::get_tgt_lead_fire_pos` (`Some(None)`
    /// when it answered false; `None` for other objects and older records).
    pub lead: Option<Option<Vector>>,
    /// `equip`: the own ship's items (`None` for other objects and older
    /// records).
    pub equip: Option<Vec<EquipRecord>>,
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
    /// The instrumented calls around this one: 0 for a call the caller
    /// made, more for one an `IObjAffect` body made (0 in records written
    /// before the field).
    pub depth: u32,
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
        pathfind: j.get("pathfind").and_then(pathfind),
        thrust: u(j, "thrust"),
        flags_1c8: u(j, "flags_1c8").and_then(|v| u8::try_from(v).ok()),
        sys: j.get("sys").and_then(sys),
        face: j.get("face").and_then(face),
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
        thrusters: b(j, "thrusters"),
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
        max_bank_angle: f(j, "max_bank").unwrap_or(0.0),
        scanner_interference: 0.0,
        atmosphere_range: f(j, "atmosphere").unwrap_or(0.0),
        engine: j.get("engine").and_then(Json::as_array).and_then(|a| {
            Some(EngineStatus {
                id: u16::try_from(a.first()?.as_u32()?).ok()?,
                active: a.get(1)?.as_bool()?,
                destroyed: a.get(2)?.as_bool()?,
            })
        }),
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
                        targeted_enemies: i(&j, "targeted_enemies"),
                        lead: j.get("equip").map(|_| v3(&j, "lead")),
                        equip: j
                            .get("equip")
                            .and_then(Json::as_array)
                            .map(|a| a.iter().filter_map(equip).collect()),
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
            depth: u(&j, "depth").unwrap_or(0),
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
