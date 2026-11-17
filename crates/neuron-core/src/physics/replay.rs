//! Per-PSI free-flight records from the scenario oracle and their replay
//! (record 0003 point 5 and point 7; the layout is the `psi.jsonl`
//! section of `design/sim-records.md`).
//!
//! A record is one core in one PSI: the environment's time, PSI length
//! and anomaly limits, the core state when its simulation unit starts the
//! PSI, the pushes the game's force controllers made in it, whether the
//! movement check came to the unit, and the core state after
//! `calc_next_PSI_matrix`. [`replay`] runs that one PSI through the same
//! functions [`super::PhysicsWorld`] uses, so a replay test compares the
//! integrator with the oracle without drift from earlier PSIs.
//!
//! The line format is the oracle's (`oracle_json.h`): one JSON object per
//! line, integers as numbers, each `f32` as a string of 8 hex digits of
//! its bits, several floats as an array of those, nested objects for the
//! two core states.

mod json;

use self::json::Json;
use super::ivp::core::Core;
use super::ivp::math::{IvpMatrix, IvpQuat, IvpTime};
use super::ivp::psi::{self, AnomalyLimits};
use super::ivp::{MOTION_IDLE_TIME, PsiClock, unit_psi_begin, unit_psi_end};

/// One push a force controller made (`push_core_linear`,
/// `push_core_angular_ws`, `push_core_angular_cs`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Push {
    /// `linear`: `push_core_linear(direction, magnitude)`.
    Linear([f32; 3], f32),
    /// `angular_ws`: `push_core_angular_ws(axis, magnitude)`.
    AngularWs([f32; 3], f32),
    /// `angular_cs`: `push_core_angular_cs(axis, magnitude)`.
    AngularCs([f32; 3], f32),
}

/// A core's recorded state and its simulation unit's flag word.
#[derive(Clone, Debug, PartialEq)]
pub struct CoreRecord {
    /// The core.
    pub core: Core,
    /// The unit's flags.
    pub unit_state: u32,
}

/// One core in one PSI.
#[derive(Clone, Debug, PartialEq)]
pub struct PsiRecord {
    /// The tick (server frame) the PSI ran in.
    pub tick: u32,
    /// The PSI counter in the run, from 1.
    pub psi: u32,
    /// The scenario's name for the object.
    pub obj: String,
    /// The game's object id.
    pub id: u32,
    /// The environment's `current_time` at the PSI.
    pub time: IvpTime,
    /// The environment's `delta_time` (the PSI length).
    pub delta_time: f32,
    /// The environment's `inv_delta_time`.
    pub inv_delta_time: f32,
    /// The anomaly limits.
    pub limits: AnomalyLimits,
    /// The state when the unit starts the PSI.
    pub input: CoreRecord,
    /// The force controllers' pushes, in call order.
    pub pushes: Vec<Push>,
    /// Whether the movement check came to this unit in this PSI.
    pub motion_check: bool,
    /// The state after `calc_next_PSI_matrix`.
    pub output: CoreRecord,
}

/// Why a record line does not parse.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RecordError(pub String);

impl core::fmt::Display for RecordError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for RecordError {}

fn err(text: impl Into<String>) -> RecordError {
    RecordError(text.into())
}

fn three(v: &[f32]) -> [f32; 3] {
    [v[0], v[1], v[2]]
}

/// The float fields of a core state and their float counts, in the order
/// the oracle writes them (after `flags`, `movement_state`, `unit_state`;
/// `spin_clip` comes between `angular_limit` and `time_of_last_psi`).
const CORE_FIELDS: &[(&str, usize)] = &[
    ("upper_limit_radius", 1),
    ("max_surface_deviation", 1),
    ("mass", 1),
    ("inv_mass", 1),
    ("inertia", 3),
    ("inv_inertia", 3),
    ("rot_speed_damp", 3),
    ("speed_damp", 1),
    ("angular_limit", 1),
    ("time_of_last_psi", 2),
    ("i_delta_time", 1),
    ("rot_speed_change", 3),
    ("speed_change", 3),
    ("rot_speed", 3),
    ("speed", 3),
    ("pos", 3),
    ("delta_world", 3),
    ("q", 4),
    ("q_next", 4),
    ("m", 12),
    ("rot_axis", 3),
    ("current_speed", 1),
    ("abs_omega", 1),
    ("surface_rot_speed", 1),
    ("time_of_last_motion", 2),
    ("time_of_last_long_motion", 2),
    ("q_last_motion", 4),
    ("q_last_long_motion", 4),
    ("pos_last_motion", 3),
    ("pos_last_long_motion", 3),
];

/// The field `CORE_FIELDS` lists before `spin_clip`.
const BEFORE_SPIN_CLIP: &str = "angular_limit";

fn core_floats(core: &Core, name: &str) -> Vec<f32> {
    let q = |q: &IvpQuat| q.to_array().to_vec();
    let t = |t: &IvpTime| vec![t.base, t.offset];
    match name {
        "upper_limit_radius" => vec![core.upper_limit_radius],
        "max_surface_deviation" => vec![core.max_surface_deviation],
        "mass" => vec![core.mass],
        "inv_mass" => vec![core.inv_mass],
        "inertia" => core.rot_inertia.to_vec(),
        "inv_inertia" => core.inv_rot_inertia.to_vec(),
        "rot_speed_damp" => core.rot_speed_damp_factor.to_vec(),
        "speed_damp" => vec![core.speed_damp_factor],
        "angular_limit" => vec![core.d_angular_limit_factor],
        "time_of_last_psi" => t(&core.time_of_last_psi),
        "i_delta_time" => vec![core.i_delta_time],
        "rot_speed_change" => core.rot_speed_change.to_vec(),
        "speed_change" => core.speed_change.to_vec(),
        "rot_speed" => core.rot_speed.to_vec(),
        "speed" => core.speed.to_vec(),
        "pos" => core.pos_world_f_core_last_psi.to_vec(),
        "delta_world" => core.delta_world_f_core_psis.to_vec(),
        "q" => q(&core.q_world_f_core_last_psi),
        "q_next" => q(&core.q_world_f_core_next_psi),
        "m" => {
            let m = &core.m_world_f_core_last_psi;
            m.rows
                .iter()
                .flatten()
                .chain(m.vv.iter())
                .copied()
                .collect()
        }
        "rot_axis" => core.rotation_axis_ws.to_vec(),
        "current_speed" => vec![core.current_speed],
        "abs_omega" => vec![core.abs_omega],
        "surface_rot_speed" => vec![core.max_surface_rot_speed],
        "time_of_last_motion" => t(&core.time_of_last_motion),
        "time_of_last_long_motion" => t(&core.time_of_last_long_motion),
        "q_last_motion" => q(&core.q_last_motion),
        "q_last_long_motion" => q(&core.q_last_long_motion),
        "pos_last_motion" => core.pos_last_motion.to_vec(),
        "pos_last_long_motion" => core.pos_last_long_motion.to_vec(),
        _ => Vec::new(),
    }
}

fn set_core_floats(core: &mut Core, name: &str, v: &[f32]) {
    let q = |v: &[f32]| IvpQuat::new(v[0], v[1], v[2], v[3]);
    let t = |v: &[f32]| IvpTime::new(v[0], v[1]);
    match name {
        "upper_limit_radius" => core.upper_limit_radius = v[0],
        "max_surface_deviation" => core.max_surface_deviation = v[0],
        "mass" => core.mass = v[0],
        "inv_mass" => core.inv_mass = v[0],
        "inertia" => core.rot_inertia = three(v),
        "inv_inertia" => core.inv_rot_inertia = three(v),
        "rot_speed_damp" => core.rot_speed_damp_factor = three(v),
        "speed_damp" => core.speed_damp_factor = v[0],
        "angular_limit" => core.d_angular_limit_factor = v[0],
        "time_of_last_psi" => core.time_of_last_psi = t(v),
        "i_delta_time" => core.i_delta_time = v[0],
        "rot_speed_change" => core.rot_speed_change = three(v),
        "speed_change" => core.speed_change = three(v),
        "rot_speed" => core.rot_speed = three(v),
        "speed" => core.speed = three(v),
        "pos" => core.pos_world_f_core_last_psi = three(v),
        "delta_world" => core.delta_world_f_core_psis = three(v),
        "q" => core.q_world_f_core_last_psi = q(v),
        "q_next" => core.q_world_f_core_next_psi = q(v),
        "m" => {
            core.m_world_f_core_last_psi = IvpMatrix {
                rows: [three(&v[0..3]), three(&v[3..6]), three(&v[6..9])],
                vv: three(&v[9..12]),
            }
        }
        "rot_axis" => core.rotation_axis_ws = three(v),
        "current_speed" => core.current_speed = v[0],
        "abs_omega" => core.abs_omega = v[0],
        "surface_rot_speed" => core.max_surface_rot_speed = v[0],
        "time_of_last_motion" => core.time_of_last_motion = t(v),
        "time_of_last_long_motion" => core.time_of_last_long_motion = t(v),
        "q_last_motion" => core.q_last_motion = q(v),
        "q_last_long_motion" => core.q_last_long_motion = q(v),
        "pos_last_motion" => core.pos_last_motion = three(v),
        "pos_last_long_motion" => core.pos_last_long_motion = three(v),
        _ => {}
    }
}

fn write_floats(out: &mut String, name: &str, values: &[f32]) {
    json::key(out, name);
    if values.len() == 1 {
        out.push_str(&json::float(values[0]));
    } else {
        out.push_str(&json::floats(values));
    }
}

fn write_core(out: &mut String, name: &str, record: &CoreRecord) {
    let core = &record.core;
    json::key(out, name);
    out.push('{');
    json::key(out, "flags");
    out.push_str(&core.flags.to_string());
    json::key(out, "movement_state");
    out.push_str(&core.movement_state.to_string());
    json::key(out, "unit_state");
    out.push_str(&record.unit_state.to_string());
    for (field, _) in CORE_FIELDS {
        write_floats(out, field, &core_floats(core, field));
        if *field == BEFORE_SPIN_CLIP {
            json::key(out, "spin_clip");
            match core.spin_clipping {
                Some(clip) => out.push_str(&json::floats(&clip)),
                None => out.push_str("null"),
            }
        }
    }
    out.push('}');
}

fn read_core(value: &Json) -> Result<CoreRecord, RecordError> {
    let mut core = Core {
        flags: value.get("flags")?.as_u32("flags")?,
        movement_state: value.get("movement_state")?.as_u32("movement_state")?,
        ..Core::default()
    };
    for (name, count) in CORE_FIELDS {
        let field = value.get(name)?;
        let v = if *count == 1 {
            vec![field.as_f32(name)?]
        } else {
            field.as_floats(name, *count)?
        };
        set_core_floats(&mut core, name, &v);
    }
    core.spin_clipping = match value.get("spin_clip")? {
        Json::Null => None,
        clip => Some(three(&clip.as_floats("spin_clip", 3)?)),
    };
    Ok(CoreRecord {
        core,
        unit_state: value.get("unit_state")?.as_u32("unit_state")?,
    })
}

fn read_push(value: &Json) -> Result<Push, RecordError> {
    let items = value.as_array("push")?;
    let [kind, x, y, z, magnitude] = items else {
        return Err(err(format!("push: {} elements, expected 5", items.len())));
    };
    let dir = [x.as_f32("push")?, y.as_f32("push")?, z.as_f32("push")?];
    let magnitude = magnitude.as_f32("push")?;
    Ok(match kind.as_str("push kind")? {
        "linear" => Push::Linear(dir, magnitude),
        "angular_ws" => Push::AngularWs(dir, magnitude),
        "angular_cs" => Push::AngularCs(dir, magnitude),
        other => return Err(err(format!("push kind {other}"))),
    })
}

impl PsiRecord {
    /// The record as one line, as `Oracle.exe sim` writes it (without the
    /// optional controller inputs).
    #[must_use]
    pub fn to_line(&self) -> String {
        let mut out = String::from("{");
        json::key(&mut out, "rec");
        out.push_str("\"psi\"");
        json::key(&mut out, "tick");
        out.push_str(&self.tick.to_string());
        json::key(&mut out, "psi");
        out.push_str(&self.psi.to_string());
        json::key(&mut out, "obj");
        out.push_str(&json::string(&self.obj));
        json::key(&mut out, "id");
        out.push_str(&self.id.to_string());
        write_floats(&mut out, "dt", &[self.delta_time]);
        write_floats(&mut out, "idt", &[self.inv_delta_time]);
        write_floats(&mut out, "time", &[self.time.base, self.time.offset]);
        write_floats(
            &mut out,
            "limits",
            &[
                self.limits.max_velocity,
                self.limits.max_angular_velocity_per_psi,
            ],
        );
        write_core(&mut out, "in", &self.input);
        json::key(&mut out, "pushes");
        let pushes: Vec<String> = self
            .pushes
            .iter()
            .map(|p| {
                let (kind, v, m) = match p {
                    Push::Linear(v, m) => ("linear", v, m),
                    Push::AngularWs(v, m) => ("angular_ws", v, m),
                    Push::AngularCs(v, m) => ("angular_cs", v, m),
                };
                let values: Vec<String> = [v[0], v[1], v[2], *m]
                    .iter()
                    .map(|f| json::float(*f))
                    .collect();
                format!("[\"{kind}\",{}]", values.join(","))
            })
            .collect();
        out.push_str(&format!("[{}]", pushes.join(",")));
        json::key(&mut out, "check");
        out.push_str(if self.motion_check { "true" } else { "false" });
        write_core(&mut out, "out", &self.output);
        out.push('}');
        out
    }

    /// Parses one line. Fields the replay does not use (such as the
    /// controller inputs `ctl`) are ignored.
    pub fn parse(line: &str) -> Result<Self, RecordError> {
        let value = json::parse(line)?;
        let rec = value.get("rec")?.as_str("rec")?;
        if rec != "psi" {
            return Err(err(format!("rec {rec}, expected psi")));
        }
        let time = value.get("time")?.as_floats("time", 2)?;
        let limits = value.get("limits")?.as_floats("limits", 2)?;
        let pushes = value
            .get("pushes")?
            .as_array("pushes")?
            .iter()
            .map(read_push)
            .collect::<Result<Vec<_>, _>>()?;
        Ok(Self {
            tick: value.get("tick")?.as_u32("tick")?,
            psi: value.get("psi")?.as_u32("psi")?,
            obj: value.get("obj")?.as_str("obj")?.to_owned(),
            id: value.get("id")?.as_u32("id")?,
            time: IvpTime::new(time[0], time[1]),
            delta_time: value.get("dt")?.as_f32("dt")?,
            inv_delta_time: value.get("idt")?.as_f32("idt")?,
            limits: AnomalyLimits {
                max_velocity: limits[0],
                max_angular_velocity_per_psi: limits[1],
            },
            input: read_core(value.get("in")?)?,
            pushes,
            motion_check: value.get("check")?.as_bool("check")?,
            output: read_core(value.get("out")?)?,
        })
    }
}

/// Runs the record's PSI on its input state: the core step, the pushes,
/// the standard controller, the movement check and `calc_next_PSI_matrix`.
#[must_use]
pub fn replay(record: &PsiRecord) -> CoreRecord {
    let mut core = record.input.core.clone();
    let mut unit_state = record.input.unit_state;
    let now = record.time;
    unit_psi_begin(&mut core, &mut unit_state, now);
    for push in &record.pushes {
        match *push {
            Push::Linear(v, m) => core.push_core_linear(v, m),
            Push::AngularWs(v, m) => core.push_core_angular_ws(v, m),
            Push::AngularCs(v, m) => core.push_core_angular_cs(v, m),
        }
    }
    core.gravity_controller_step(record.delta_time, [0.0; 3]);
    unit_psi_end(
        &mut core,
        &mut unit_state,
        now,
        record.motion_check,
        MOTION_IDLE_TIME,
    );
    let mut clock = PsiClock::new(record.delta_time);
    clock.delta_time = record.delta_time;
    clock.inv_delta_time = record.inv_delta_time;
    let commit = clock.commit_event_sim();
    psi::calc_next_psi_matrix(
        &mut core,
        commit,
        now,
        &record.limits,
        record.inv_delta_time,
    );
    CoreRecord { core, unit_state }
}

/// The fields where `actual` differs from `expected`: integers exactly,
/// floats by more than `tolerance` relative to the larger magnitude (and
/// at least `tolerance` absolute).
#[must_use]
pub fn compare(expected: &CoreRecord, actual: &CoreRecord, tolerance: f32) -> Vec<String> {
    let mut out = Vec::new();
    if expected.core.flags != actual.core.flags {
        out.push(format!(
            "flags {:x} != {:x}",
            expected.core.flags, actual.core.flags
        ));
    }
    if expected.core.movement_state != actual.core.movement_state {
        out.push(format!(
            "movement_state {:x} != {:x}",
            expected.core.movement_state, actual.core.movement_state
        ));
    }
    if expected.unit_state != actual.unit_state {
        out.push(format!(
            "unit_state {:x} != {:x}",
            expected.unit_state, actual.unit_state
        ));
    }
    for (name, _) in CORE_FIELDS {
        let (a, b) = (
            core_floats(&expected.core, name),
            core_floats(&actual.core, name),
        );
        for (i, (x, y)) in a.iter().zip(&b).enumerate() {
            if x.to_bits() == y.to_bits() {
                continue;
            }
            let scale = x.abs().max(y.abs()).max(1.0);
            if !((x - y).abs() <= tolerance * scale) {
                out.push(format!("{name}[{i}] {x:e} != {y:e}"));
            }
        }
    }
    let clip_bits = |c: Option<[f32; 3]>| c.map(|c| c.map(f32::to_bits));
    if clip_bits(expected.core.spin_clipping) != clip_bits(actual.core.spin_clipping) {
        out.push(format!(
            "spin_clip {:?} != {:?}",
            expected.core.spin_clipping, actual.core.spin_clipping
        ));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> PsiRecord {
        let mut core = Core {
            movement_state: 1,
            mass: 100.0,
            rot_inertia: [10.0, 20.0, 30.0],
            upper_limit_radius: 5.0,
            max_surface_deviation: 5.5,
            speed_damp_factor: 0.5,
            rot_speed_damp_factor: [0.2; 3],
            speed: [10.0, 0.0, 0.0],
            delta_world_f_core_psis: [10.0, 0.0, 0.0],
            rot_speed: [0.0, 0.1, 0.0],
            time_of_last_psi: IvpTime::new(0.0, 0.5),
            i_delta_time: 10.0,
            ..Core::default()
        };
        core.recompute_inverse_inertia();
        let input = CoreRecord {
            core,
            unit_state: 1,
        };
        let mut record = PsiRecord {
            tick: 3,
            psi: 6,
            obj: "ship \"a\" \u{e9}".to_owned(),
            id: 0x1234_5678,
            time: IvpTime::new(0.0, 0.6),
            delta_time: 0.1,
            inv_delta_time: 10.0,
            motion_check: false,
            limits: AnomalyLimits {
                max_velocity: 1000.0,
                max_angular_velocity_per_psi: 0.942_477_9,
            },
            input: input.clone(),
            pushes: vec![
                Push::Linear([1.0, 0.0, 0.0], 200.0),
                Push::AngularWs([0.0, 0.0, 1.0], 3.0),
                Push::AngularCs([1.0, 0.0, 0.0], 0.5),
            ],
            output: input,
        };
        record.output = replay(&record);
        record
    }

    #[test]
    fn lines_round_trip() {
        let record = sample();
        let line = record.to_line();
        let parsed = PsiRecord::parse(&line).unwrap();
        assert_eq!(parsed, record);
        assert!(line.starts_with(
            "{\"rec\":\"psi\",\"tick\":3,\"psi\":6,\"obj\":\"ship \\\"a\\\" \\u00e9\",\"id\":305419896,\"dt\":\"3dcccccd\""
        ));
        assert!(line.contains(
            "\"pushes\":[[\"linear\",\"3f800000\",\"00000000\",\"00000000\",\"43480000\"],"
        ));
        assert!(line.contains("\",\"spin_clip\":null,\"time_of_last_psi\":["));
        assert!(line.contains("\"check\":false,\"out\":{\"flags\":0,\"movement_state\":1,"));
        // The harness's controller inputs and any other extra field are ignored.
        let mut with_extra = line.clone();
        with_extra.insert_str(
            line.find(",\"in\":").unwrap(),
            ",\"ctl\":{\"throttle\":\"3f800000\",\"engine\":null,\"axis\":[]}",
        );
        assert_eq!(PsiRecord::parse(&with_extra).unwrap(), record);
        let mut clipped = record.clone();
        clipped.output.core.spin_clipping = Some([1.0, 2.0, 3.0]);
        let clipped_line = clipped.to_line();
        assert!(clipped_line.contains("\"spin_clip\":[\"3f800000\",\"40000000\",\"40400000\"]"));
        assert_eq!(PsiRecord::parse(&clipped_line).unwrap(), clipped);
        assert_eq!(compare(&record.output, &clipped.output, 1.0).len(), 1);
        assert!(PsiRecord::parse("[]").is_err());
        assert!(PsiRecord::parse("{\"rec\":\"psi\",\"psi\":1}").is_err());
        assert!(PsiRecord::parse(&line.replace("\"rec\":\"psi\"", "\"rec\":\"frame\"")).is_err());
        assert!(PsiRecord::parse(&line.replace("[\"linear\",", "[\"twist\",")).is_err());
    }

    #[test]
    fn replay_is_the_world_sequence() {
        let record = sample();
        let out = &record.output.core;
        // 10 m/s damped linearly by 0.5 * 0.1, then the 2 m/s push committed.
        assert!(
            (out.speed[0] - (10.0 * 0.95 + 2.0)).abs() < 1e-5,
            "{:?}",
            out.speed
        );
        assert_eq!(out.time_of_last_psi, record.time);
        // The position moves by the old delta over the 0.1 s since the last PSI.
        assert!((out.pos_world_f_core_last_psi[0] - 1.0).abs() < 1e-6);
        assert!(compare(&record.output, &replay(&record), 0.0).is_empty());
        let mut off = record.output.clone();
        off.core.speed[1] = 1.0;
        off.unit_state = 9;
        let diffs = compare(&off, &replay(&record), 1e-6);
        assert_eq!(diffs.len(), 2, "{diffs:?}");
    }
}
