//! The behaviour-tick records of `Oracle.exe sim` (`design/sim-records.md`,
//! `ai.jsonl` and the `affect` records of `calls.jsonl`): one [`AiTick`]
//! per `IBehaviorManager::UpdateBehaviorTick`, with the manager fields on
//! entry and exit, the personality, the object snapshot, the queries and
//! the `rand()` values, and the commands the tick issued.

use std::collections::BTreeMap;
use std::path::Path;

use crate::ai::behavior::{
    DirectiveOp, DockOp, DrasticEvadeOp, FaceOp, FollowOp, GotoOp, GuideOp, InstantTradeLaneOp,
    LaunchOp, TractorBeamOp, TrailOp,
};
use crate::ai::gun_groups::{CmDroppers, GunGroups, GunSlot, LineOfFireSlot};
use crate::ai::state_graph::{BEHAVIOR_COUNT, BehaviorScore};
use crate::ai::target_manager::TargetManager;
use crate::ai::timer_table::{SustainedThresholdTimer, TimerTable};
use crate::ai::world::{EngineStatus, ObjectView, StrafeDir};
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
    /// `beh7`, `beh8` or `beh9`: the dock, launch or trade-lane
    /// behaviour's fields while it is current.
    pub beh: Option<BehRecord>,
}

/// The own fields of the dock (7), launch (8) or instant-trade-lane (9)
/// behaviour (`beh7`, `beh8`, `beh9`: `i` and `v` in the order
/// `design/sim-records.md` lists).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct BehRecord {
    /// The behaviour.
    pub kind: i32,
    /// The integer fields.
    pub ints: Vec<i32>,
    /// The float fields.
    pub floats: Vec<f32>,
}

/// A world answer (`ai_answer`): `request`, `dock_hardpoints`,
/// `lane_start` or `go_tradelane`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct AnswerRecord {
    /// The frame.
    pub tick: u32,
    /// The behaviour tick it came in (0 outside one).
    pub seq: u32,
    /// The kind.
    pub kind: String,
    /// The object asked (the ship for `request`, `lane_start` and
    /// `go_tradelane`).
    pub id: u32,
    /// The integers.
    pub ints: Vec<i32>,
    /// The floats.
    pub floats: Vec<f32>,
}

/// A directive a behaviour manager heard (`ai_answer` of kind `directive`,
/// P3.6d): `pub::AI::SubmitDirective`'s AI branch, the op rebuilt from
/// its recorded words.
#[derive(Clone, Debug, PartialEq)]
pub struct RecordedDirective {
    /// The op.
    pub op: DirectiveOp,
    /// Its priority (`BaseOp +0x08`).
    pub priority: i32,
    /// What `SubmitDirective` returned.
    pub result: i32,
}

/// The op of a `directive` answer: `i` is the result, then the op's
/// dwords from `+0x04` (the op type, the priority, the op's fields from
/// `+0x0c`). `None` for another kind or too few words.
#[must_use]
pub fn recorded_directive(a: &AnswerRecord) -> Option<RecordedDirective> {
    if a.kind != "directive" || a.ints.len() < 3 {
        return None;
    }
    let result = a.ints[0];
    let op_type = a.ints[1];
    let priority = a.ints[2];
    // The dword at op offset `o` (from +0x04 on).
    let word = |o: usize| -> u32 {
        a.ints
            .get(1 + (o - 4) / 4)
            .map_or(0, |&v| u32::from_ne_bytes(v.to_ne_bytes()))
    };
    let int = |o: usize| i32::from_ne_bytes(word(o).to_ne_bytes());
    let float = |o: usize| f32::from_bits(word(o));
    let byte = |o: usize| word(o & !3).to_le_bytes()[o & 3];
    let vector = |o: usize| Vector::new(float(o), float(o + 4), float(o + 8));
    let goto = |b: usize| GotoOp {
        kind: int(b + 0x0c),
        destination: vector(b + 0x10),
        field_1c: int(b + 0x1c),
        words_20: core::array::from_fn(|k| word(b + 0x20 + 4 * k)),
        range: float(b + 0x50),
        field_54: float(b + 0x54),
        field_58: byte(b + 0x58),
        field_59: byte(b + 0x59),
        cruise: byte(b + 0x5a),
        no_cruise: byte(b + 0x5b),
        field_5c: word(b + 0x5c),
        field_60: float(b + 0x60),
        field_64: float(b + 0x64),
        field_68: word(b + 0x68),
        field_6c: byte(b + 0x6c),
    };
    let follow = |b: usize| FollowOp {
        target: word(b + 0x0c),
        distance: float(b + 0x10),
        offset: vector(b + 0x14),
        range: float(b + 0x20),
    };
    let row = |o: usize| [float(o), float(o + 4), float(o + 8)];
    let (op, size) = match op_type {
        3 => (DirectiveOp::Goto(goto(0)), 0x70),
        4 => (DirectiveOp::Follow(follow(0)), 0x24),
        5 => (
            DirectiveOp::Trail(TrailOp {
                target: word(0x0c),
                distance: float(0x10),
                field_14: float(0x14),
                field_18: float(0x18),
            }),
            0x1c,
        ),
        7 => (
            DirectiveOp::Face(FaceOp {
                mode: int(0x0c),
                target: word(0x10),
                point: vector(0x14),
                orientation: Matrix::from_rows([row(0x20), row(0x2c), row(0x38)]),
                field_44: byte(0x44),
            }),
            0x48,
        ),
        9 => (
            DirectiveOp::TractorBeam(TractorBeamOp {
                target: word(0x0c),
                field_10: byte(0x10),
            }),
            0x14,
        ),
        10 => (DirectiveOp::Evade(word(0x0c)), 0x10),
        11 => (
            DirectiveOp::DrasticEvade(DrasticEvadeOp {
                threat: word(0x0c),
                words_10: [word(0x10), word(0x14), word(0x18)],
                duration: float(0x1c),
                words_20: [word(0x20), word(0x24), word(0x28), word(0x2c)],
            }),
            0x30,
        ),
        12 => (DirectiveOp::Delay(float(0x0c)), 0x10),
        13 => (
            DirectiveOp::Dock(DockOp {
                dock_object: word(0x0c),
                field_10: word(0x10),
                field_14: byte(0x14),
                port: int(0x18),
                field_1c: word(0x1c),
                field_20: float(0x20),
                field_24: float(0x24),
                field_28: word(0x28),
            }),
            0x2c,
        ),
        14 => (
            DirectiveOp::Launch(LaunchOp {
                launch_object: word(0x0c),
                index: int(0x10),
                terminal: int(0x14),
            }),
            0x18,
        ),
        15 => (
            DirectiveOp::InstantTradeLane(InstantTradeLaneOp {
                entrance: word(0x0c),
                exit: word(0x10),
                speed: float(0x14),
            }),
            0x18,
        ),
        17 => (
            DirectiveOp::Guide(Box::new(GuideOp {
                goto: goto(0x0c),
                follow: follow(0x7c),
                field_a0: byte(0xa0),
                field_a4: float(0xa4),
                field_a8: float(0xa8),
                field_ac: int(0xac),
                field_b0: float(0xb0),
                field_b4: float(0xb4),
                field_b8: float(0xb8),
            })),
            0xbc,
        ),
        20 => (DirectiveOp::Idle, 0x0c),
        21 => (DirectiveOp::Cancel, 0x0c),
        other => (DirectiveOp::Other(other), 0x0c),
    };
    // The words from +0x04: the result's slot first.
    (a.ints.len() > (size - 4) / 4).then_some(RecordedDirective {
        op,
        priority,
        result,
    })
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
    /// `get_tgt_lead_fire_pos(id)`: the gun's lead on its target.
    pub lead: Option<Vector>,
    /// `is_pointing_at` at the gun angle, the wide and the missile cone.
    pub pointing: [bool; 3],
    /// `can_point_at(candidate position, 0.3)`: candidate id, answer.
    pub can_point: Vec<(u32, bool)>,
    /// `turret`: what `CEGun::CanPointAt` reads (an auto turret): the yaw
    /// and pitch joints (bound, limited, min, max), the origin, the
    /// turret frame and the connector's orientation (29 floats).
    pub turret: Option<Vec<f32>>,
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
        lead: v3(j, "lead"),
        pointing: [
            pointing.first()?.as_bool()?,
            pointing.get(1)?.as_bool()?,
            pointing.get(2)?.as_bool()?,
        ],
        turret: j
            .get("turret")
            .and_then(Json::as_floats)
            .filter(|a| a.len() == 29),
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
    /// `locks` (P2.5 dock): `Flag245`, `Field_24C`, `Field_254`, the
    /// flags `SetCruiseLock` writes (`None` in older records).
    pub locks: Option<[u8; 3]>,
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
        locks: j.get("locks").and_then(Json::as_array).and_then(|a| {
            let byte = |k: usize| a.get(k)?.as_u32().and_then(|v| u8::try_from(v).ok());
            Some([byte(0)?, byte(1)?, byte(2)?])
        }),
    })
}

/// One object of a tick's snapshot.
#[derive(Clone, Debug, PartialEq)]
pub struct ObjRecord {
    /// The id.
    pub id: u32,
    /// The scenario name (`obj`), `None` for an object the scenario does
    /// not name.
    pub name: Option<String>,
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
    /// `hpmount`: the own ship's `get_hardpoint("hpmount")` (position and
    /// orientation; `None` when it failed or for other objects).
    pub hpmount: Option<(Vector, Matrix)>,
    /// `lane` (P3.6d): `is_using_tradelane` (`None` when it failed and for
    /// older records).
    pub using_tradelane: Option<bool>,
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
    /// The world answers given during the tick.
    pub answers: Vec<AnswerRecord>,
    /// The world answers given outside a behaviour tick (seq 0) since the
    /// previous tick of any object, in record order.
    pub before: Vec<AnswerRecord>,
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
        beh: [(7, "beh7"), (8, "beh8"), (9, "beh9")]
            .into_iter()
            .find_map(|(kind, key)| {
                let b = j.get(key)?;
                Some(BehRecord {
                    kind,
                    ints: b
                        .get("i")?
                        .as_array()?
                        .iter()
                        .filter_map(Json::as_i32)
                        .collect(),
                    floats: b.get("v").and_then(Json::as_floats).unwrap_or_default(),
                })
            }),
    })
}

fn answer(j: &Json) -> AnswerRecord {
    AnswerRecord {
        tick: u(j, "tick").unwrap_or(0),
        seq: u(j, "seq").unwrap_or(0),
        kind: j
            .get("kind")
            .and_then(Json::as_str)
            .unwrap_or("")
            .to_owned(),
        id: u(j, "id").unwrap_or(0),
        ints: j
            .get("i")
            .and_then(Json::as_array)
            .map(|a| a.iter().filter_map(Json::as_i32).collect())
            .unwrap_or_default(),
        floats: j.get("v").and_then(Json::as_floats).unwrap_or_default(),
    }
}

/// The names `ai_obj` records give object ids (the scenario's names),
/// and those of the run's `tick.jsonl` `object` records beside it (every
/// scenario object, near or not).
#[must_use]
pub fn read_names(path: &Path) -> BTreeMap<String, u32> {
    let mut out = BTreeMap::new();
    let ticks = path.with_file_name("tick.jsonl");
    let mut lines = read_lines(path);
    if ticks.exists() {
        lines.extend(
            read_lines(&ticks)
                .into_iter()
                .filter(|j| j.get("rec").and_then(Json::as_str) == Some("object"))
                .take_while(|j| u(j, "tick") == Some(0)),
        );
    }
    for j in lines {
        if !matches!(
            j.get("rec").and_then(Json::as_str),
            Some("ai_obj" | "object")
        ) {
            continue;
        }
        if let (Some(name), Some(id)) = (j.get("obj").and_then(Json::as_str), u(&j, "id")) {
            out.entry(name.to_owned()).or_insert(id);
        }
    }
    out
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
        formation_offset: v3(j, "formation_offset"),
        follow_leader: opt_u(j, "follow_leader"),
        follow_offset: v3(j, "follow_offset"),
        formation_followers: j
            .get("followers")
            .and_then(Json::as_array)
            .map(|a| a.iter().filter_map(Json::as_u32).collect())
            .unwrap_or_default(),
        is_player: b(j, "player").unwrap_or(false),
        group_id: 0,
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
    let mut between: Vec<AnswerRecord> = Vec::new();
    for j in read_lines(path) {
        let rec = j.get("rec").and_then(Json::as_str).unwrap_or("");
        let seq = u(&j, "seq").unwrap_or(0);
        if rec == "ai_answer" && seq == 0 {
            between.push(answer(&j));
            continue;
        }
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
                    before: std::mem::take(&mut between),
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
                        name: j.get("obj").and_then(Json::as_str).map(str::to_owned),
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
                        hpmount: j
                            .get("hpmount")
                            .and_then(Json::as_floats)
                            .filter(|m| m.len() == 12)
                            .and_then(|m| {
                                Some((Vector::new(m[0], m[1], m[2]), matrix(m.get(3..12)?)?))
                            }),
                        using_tradelane: j.get("lane").and_then(Json::as_bool),
                    }),
                    "ai_answer" => t.answers.push(answer(&j)),
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

    /// The words of a recorded op: the result, then the dwords from +0x04.
    fn words(result: i32, dwords: &[u32]) -> AnswerRecord {
        AnswerRecord {
            kind: "directive".to_owned(),
            ints: std::iter::once(result)
                .chain(dwords.iter().map(|&w| i32::from_ne_bytes(w.to_ne_bytes())))
                .collect(),
            ..AnswerRecord::default()
        }
    }

    #[test]
    fn rebuilds_a_recorded_directive() {
        // `DirectiveDockOp` (0x2c): type 13, priority 1, the dock object,
        // +0x10, the byte +0x14, the port, +0x1c, 200, 500, +0x28.
        let dock = words(
            1,
            &[
                13,
                1,
                0x1234,
                0,
                0x0000_0001,
                u32::MAX,
                0,
                0x4348_0000,
                0x43fa_0000,
                0,
            ],
        );
        let d = recorded_directive(&dock).expect("dock");
        assert_eq!((d.priority, d.result), (1, 1));
        assert_eq!(
            d.op,
            DirectiveOp::Dock(DockOp {
                dock_object: 0x1234,
                field_14: 1,
                ..DockOp::default()
            })
        );
        // A default `DirectiveGotoOp` (0x70) to a point.
        let mut goto = vec![3, 2, 1, 0x4100_0000, 0, 0x4120_0000, 0];
        goto.extend([0; 12]);
        goto.extend([
            0x43fa_0000,
            0xbf80_0000,
            0x0000_0100,
            0,
            0x4348_0000,
            0x43fa_0000,
            0,
            0,
        ]);
        let d = recorded_directive(&words(1, &goto)).expect("goto");
        assert_eq!(
            d.op,
            DirectiveOp::Goto(GotoOp {
                kind: 1,
                destination: Vector::new(8.0, 0.0, 10.0),
                ..GotoOp::default()
            })
        );
        // Too few words, or another kind.
        assert!(recorded_directive(&words(1, &[13, 1, 0x1234])).is_none());
        let mut request = dock;
        request.kind = "request".to_owned();
        assert!(recorded_directive(&request).is_none());
        assert_eq!(
            recorded_directive(&words(1, &[21, 0])).map(|d| d.op),
            Some(DirectiveOp::Cancel)
        );
    }
}
