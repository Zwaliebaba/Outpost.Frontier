//! Per-PSI free-flight records from the scenario oracle and their replay
//! (record 0003 point 5 and point 7; the layout is in
//! `design/sim-records.md`).
//!
//! A record is one core in one PSI: the environment's time and PSI length,
//! the core state when its simulation unit starts the PSI, the pushes the
//! game's force controllers made in it, and the core state after
//! `calc_next_PSI_matrix`. [`replay`] runs that one PSI through the same
//! functions [`super::PhysicsWorld`] uses, so a replay test compares the
//! integrator with the oracle without drift from earlier PSIs.
//!
//! The line format is the oracle's: one flat JSON object per line, every
//! value a string, `f32` values as 8 hex digits of their bits, several
//! floats concatenated.

use super::ivp::core::Core;
use super::ivp::math::{IvpMatrix, IvpQuat, IvpTime};
use super::ivp::psi::{self, AnomalyLimits};
use super::ivp::{MOTION_IDLE_TIME, PsiClock, unit_psi_begin, unit_psi_end};

/// One push a force controller made (`push_core_linear`,
/// `push_core_angular_ws`, `push_core_angular_cs`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Push {
    /// `L`: `push_core_linear(direction, magnitude)`.
    Linear([f32; 3], f32),
    /// `A`: `push_core_angular_ws(axis, magnitude)`.
    AngularWs([f32; 3], f32),
    /// `C`: `push_core_angular_cs(axis, magnitude)`.
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
    /// The PSI counter in the run.
    pub psi: u64,
    /// The object id.
    pub object: u32,
    /// The environment's `current_time` at the PSI.
    pub time: IvpTime,
    /// The environment's `delta_time` (the PSI length).
    pub delta_time: f32,
    /// The environment's `inv_delta_time`.
    pub inv_delta_time: f32,
    /// Whether the movement check came to this unit in this PSI.
    pub motion_check: bool,
    /// The anomaly limits.
    pub limits: AnomalyLimits,
    /// The state when the unit starts the PSI.
    pub input: CoreRecord,
    /// The force controllers' pushes, in call order.
    pub pushes: Vec<Push>,
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

/// The fields of a flat JSON object whose values are all strings without
/// escapes, in order.
fn fields(line: &str) -> Result<Vec<(&str, &str)>, RecordError> {
    let body = line
        .trim()
        .strip_prefix('{')
        .and_then(|l| l.strip_suffix('}'))
        .ok_or_else(|| err("not a JSON object"))?;
    let mut out = Vec::new();
    let mut rest = body;
    while !rest.is_empty() {
        let rest_key = rest.strip_prefix('"').ok_or_else(|| err("key"))?;
        let (key, after) = rest_key.split_once('"').ok_or_else(|| err("key end"))?;
        let after = after.strip_prefix(":\"").ok_or_else(|| err("colon"))?;
        let (value, after) = after.split_once('"').ok_or_else(|| err("value end"))?;
        out.push((key, value));
        rest = after.strip_prefix(',').unwrap_or(after);
    }
    Ok(out)
}

fn floats(text: &str, count: usize) -> Result<Vec<f32>, RecordError> {
    if text.len() != count * 8 {
        return Err(err(format!("{text}: expected {count} floats")));
    }
    (0..count)
        .map(|i| {
            u32::from_str_radix(&text[i * 8..i * 8 + 8], 16)
                .map(f32::from_bits)
                .map_err(|e| err(format!("{text}: {e}")))
        })
        .collect()
}

fn hex_floats(values: &[f32]) -> String {
    values
        .iter()
        .map(|v| format!("{:08x}", v.to_bits()))
        .collect()
}

fn three(v: &[f32]) -> [f32; 3] {
    [v[0], v[1], v[2]]
}

/// The names and float counts of a core's fields in a record, after the
/// `in_` or `out_` prefix.
const CORE_FIELDS: &[(&str, usize)] = &[
    ("ulr", 1),
    ("msd", 1),
    ("mass", 1),
    ("inv_mass", 1),
    ("inertia", 3),
    ("inv_inertia", 3),
    ("rot_damp", 3),
    ("speed_damp", 1),
    ("angular_limit", 1),
    ("tlp", 2),
    ("idt", 1),
    ("rot_speed_change", 3),
    ("speed_change", 3),
    ("rot_speed", 3),
    ("speed", 3),
    ("pos", 3),
    ("delta", 3),
    ("q_last", 4),
    ("q_next", 4),
    ("m", 12),
    ("axis", 3),
    ("current_speed", 1),
    ("abs_omega", 1),
    ("surface_rot_speed", 1),
    ("tlm", 2),
    ("tllm", 2),
    ("q_lm", 4),
    ("q_llm", 4),
    ("pos_lm", 3),
    ("pos_llm", 3),
];

fn core_floats(core: &Core, name: &str) -> Vec<f32> {
    let q = |q: &IvpQuat| q.to_array().to_vec();
    let t = |t: &IvpTime| vec![t.base, t.offset];
    match name {
        "ulr" => vec![core.upper_limit_radius],
        "msd" => vec![core.max_surface_deviation],
        "mass" => vec![core.mass],
        "inv_mass" => vec![core.inv_mass],
        "inertia" => core.rot_inertia.to_vec(),
        "inv_inertia" => core.inv_rot_inertia.to_vec(),
        "rot_damp" => core.rot_speed_damp_factor.to_vec(),
        "speed_damp" => vec![core.speed_damp_factor],
        "angular_limit" => vec![core.d_angular_limit_factor],
        "tlp" => t(&core.time_of_last_psi),
        "idt" => vec![core.i_delta_time],
        "rot_speed_change" => core.rot_speed_change.to_vec(),
        "speed_change" => core.speed_change.to_vec(),
        "rot_speed" => core.rot_speed.to_vec(),
        "speed" => core.speed.to_vec(),
        "pos" => core.pos_world_f_core_last_psi.to_vec(),
        "delta" => core.delta_world_f_core_psis.to_vec(),
        "q_last" => q(&core.q_world_f_core_last_psi),
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
        "axis" => core.rotation_axis_ws.to_vec(),
        "current_speed" => vec![core.current_speed],
        "abs_omega" => vec![core.abs_omega],
        "surface_rot_speed" => vec![core.max_surface_rot_speed],
        "tlm" => t(&core.time_of_last_motion),
        "tllm" => t(&core.time_of_last_long_motion),
        "q_lm" => q(&core.q_last_motion),
        "q_llm" => q(&core.q_last_long_motion),
        "pos_lm" => core.pos_last_motion.to_vec(),
        "pos_llm" => core.pos_last_long_motion.to_vec(),
        _ => Vec::new(),
    }
}

fn set_core_floats(core: &mut Core, name: &str, v: &[f32]) {
    let q = |v: &[f32]| IvpQuat::new(v[0], v[1], v[2], v[3]);
    let t = |v: &[f32]| IvpTime::new(v[0], v[1]);
    match name {
        "ulr" => core.upper_limit_radius = v[0],
        "msd" => core.max_surface_deviation = v[0],
        "mass" => core.mass = v[0],
        "inv_mass" => core.inv_mass = v[0],
        "inertia" => core.rot_inertia = three(v),
        "inv_inertia" => core.inv_rot_inertia = three(v),
        "rot_damp" => core.rot_speed_damp_factor = three(v),
        "speed_damp" => core.speed_damp_factor = v[0],
        "angular_limit" => core.d_angular_limit_factor = v[0],
        "tlp" => core.time_of_last_psi = t(v),
        "idt" => core.i_delta_time = v[0],
        "rot_speed_change" => core.rot_speed_change = three(v),
        "speed_change" => core.speed_change = three(v),
        "rot_speed" => core.rot_speed = three(v),
        "speed" => core.speed = three(v),
        "pos" => core.pos_world_f_core_last_psi = three(v),
        "delta" => core.delta_world_f_core_psis = three(v),
        "q_last" => core.q_world_f_core_last_psi = q(v),
        "q_next" => core.q_world_f_core_next_psi = q(v),
        "m" => {
            core.m_world_f_core_last_psi = IvpMatrix {
                rows: [three(&v[0..3]), three(&v[3..6]), three(&v[6..9])],
                vv: three(&v[9..12]),
            }
        }
        "axis" => core.rotation_axis_ws = three(v),
        "current_speed" => core.current_speed = v[0],
        "abs_omega" => core.abs_omega = v[0],
        "surface_rot_speed" => core.max_surface_rot_speed = v[0],
        "tlm" => core.time_of_last_motion = t(v),
        "tllm" => core.time_of_last_long_motion = t(v),
        "q_lm" => core.q_last_motion = q(v),
        "q_llm" => core.q_last_long_motion = q(v),
        "pos_lm" => core.pos_last_motion = three(v),
        "pos_llm" => core.pos_last_long_motion = three(v),
        _ => {}
    }
}

fn write_core(out: &mut String, prefix: &str, record: &CoreRecord) {
    let core = &record.core;
    out.push_str(&format!(
        ",\"{prefix}flags\":\"{:08x}\",\"{prefix}movement\":\"{:08x}\",\"{prefix}unit\":\"{:08x}\"",
        core.flags, core.movement_state, record.unit_state
    ));
    for (name, _) in CORE_FIELDS {
        out.push_str(&format!(
            ",\"{prefix}{name}\":\"{}\"",
            hex_floats(&core_floats(core, name))
        ));
    }
    let clip = core
        .spin_clipping
        .map_or_else(|| "-".to_owned(), |c| hex_floats(&c));
    out.push_str(&format!(",\"{prefix}spin_clip\":\"{clip}\""));
}

fn read_core(map: &[(&str, &str)], prefix: &str) -> Result<CoreRecord, RecordError> {
    let get = |name: &str| {
        let key = format!("{prefix}{name}");
        map.iter()
            .find(|(k, _)| *k == key)
            .map(|(_, v)| *v)
            .ok_or_else(|| err(format!("missing {key}")))
    };
    let hex_u32 = |name: &str| -> Result<u32, RecordError> {
        u32::from_str_radix(get(name)?, 16).map_err(|e| err(format!("{prefix}{name}: {e}")))
    };
    let mut core = Core {
        flags: hex_u32("flags")?,
        movement_state: hex_u32("movement")?,
        ..Core::default()
    };
    for (name, count) in CORE_FIELDS {
        let v = floats(get(name)?, *count)?;
        set_core_floats(&mut core, name, &v);
    }
    let clip = get("spin_clip")?;
    core.spin_clipping = if clip == "-" {
        None
    } else {
        Some(three(&floats(clip, 3)?))
    };
    Ok(CoreRecord {
        core,
        unit_state: hex_u32("unit")?,
    })
}

impl PsiRecord {
    /// The record as one line.
    #[must_use]
    pub fn to_line(&self) -> String {
        let mut out = format!(
            "{{\"psi\":\"{}\",\"object\":\"{:08x}\",\"time\":\"{}\",\"dt\":\"{}\",\"idt\":\"{}\",\"check\":\"{}\",\"limits\":\"{}\"",
            self.psi,
            self.object,
            hex_floats(&[self.time.base, self.time.offset]),
            hex_floats(&[self.delta_time]),
            hex_floats(&[self.inv_delta_time]),
            u8::from(self.motion_check),
            hex_floats(&[
                self.limits.max_velocity,
                self.limits.max_angular_velocity_per_psi
            ]),
        );
        write_core(&mut out, "in_", &self.input);
        let pushes: Vec<String> = self
            .pushes
            .iter()
            .map(|p| {
                let (tag, v, m) = match p {
                    Push::Linear(v, m) => ('L', v, m),
                    Push::AngularWs(v, m) => ('A', v, m),
                    Push::AngularCs(v, m) => ('C', v, m),
                };
                format!("{tag}{}", hex_floats(&[v[0], v[1], v[2], *m]))
            })
            .collect();
        out.push_str(&format!(",\"pushes\":\"{}\"", pushes.join(";")));
        write_core(&mut out, "out_", &self.output);
        out.push('}');
        out
    }

    /// Parses one line. Unknown keys (such as the controller inputs kept
    /// for P2.4) are ignored.
    pub fn parse(line: &str) -> Result<Self, RecordError> {
        let map = fields(line)?;
        let get = |key: &str| {
            map.iter()
                .find(|(k, _)| *k == key)
                .map(|(_, v)| *v)
                .ok_or_else(|| err(format!("missing {key}")))
        };
        let time = floats(get("time")?, 2)?;
        let limits = floats(get("limits")?, 2)?;
        let pushes_text = get("pushes")?;
        let mut pushes = Vec::new();
        for entry in pushes_text.split(';').filter(|e| !e.is_empty()) {
            let (tag, rest) = entry.split_at(1);
            let v = floats(rest, 4)?;
            let dir = three(&v);
            pushes.push(match tag {
                "L" => Push::Linear(dir, v[3]),
                "A" => Push::AngularWs(dir, v[3]),
                "C" => Push::AngularCs(dir, v[3]),
                other => return Err(err(format!("push kind {other}"))),
            });
        }
        Ok(Self {
            psi: get("psi")?.parse().map_err(|e| err(format!("psi: {e}")))?,
            object: u32::from_str_radix(get("object")?, 16)
                .map_err(|e| err(format!("object: {e}")))?,
            time: IvpTime::new(time[0], time[1]),
            delta_time: floats(get("dt")?, 1)?[0],
            inv_delta_time: floats(get("idt")?, 1)?[0],
            motion_check: get("check")? == "1",
            limits: AnomalyLimits {
                max_velocity: limits[0],
                max_angular_velocity_per_psi: limits[1],
            },
            input: read_core(&map, "in_")?,
            pushes,
            output: read_core(&map, "out_")?,
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
            "movement {:x} != {:x}",
            expected.core.movement_state, actual.core.movement_state
        ));
    }
    if expected.unit_state != actual.unit_state {
        out.push(format!(
            "unit {:x} != {:x}",
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
            psi: 6,
            object: 0x1234_5678,
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
        assert!(line.starts_with("{\"psi\":\"6\",\"object\":\"12345678\""));
        let mut with_extra = line.clone();
        with_extra.insert_str(1, "\"controllers\":\"x\",");
        assert_eq!(PsiRecord::parse(&with_extra).unwrap(), record);
        assert!(PsiRecord::parse("[]").is_err());
        assert!(PsiRecord::parse("{\"psi\":\"1\"}").is_err());
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
