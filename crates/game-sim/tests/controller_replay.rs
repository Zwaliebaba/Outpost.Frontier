//! Controller replay (decision O20, Tier B; record 0003 point 7): for every
//! PSI of a ship in the scenario oracle's `psi.jsonl` records, the flight
//! controllers are rebuilt from the recorded inputs (`ctl`) and run for
//! that PSI, and the pushes they make are compared with the pushes the C++
//! controllers made (`pushes`), without drift from earlier PSIs.
//!
//! The controllers run in the order IVP calls them, the simulation unit's
//! controller buckets from the last (`IVP_Simulation_Unit_Process_One_Psi_Step`):
//! the order the record names (`ctl.order`, P2.5), else the order of a unit
//! whose buckets were never rebuilt, the last registered first: strafe,
//! nudge, steering, the thrusters from the last, then the engine. A
//! record's pushes must agree in kind and within `TOLERANCE`, relative to
//! the larger magnitude, push by push; when only the order differs it is
//! reported but passes for a record without `ctl.order` (the integrator's
//! order is checked by `neuron-core`'s `sim_replay`), and fails for one
//! with it.
//!
//! Inputs older harness builds did not record (`power`, `orient`,
//! `zone_drag`, the controller gates, the thrusters; added for P2.4 in
//! design/sim-records.md) fall back to: enough power, the core's matrix as
//! the orientation, no zone, every controller registered, no thrusters.
//!
//! `OUTPOST_ORACLE_DIR` names a folder above the runs; without it the test
//! prints a notice and passes. A summary goes to
//! `rust-controller-replay.txt` there.

mod support;

use std::fmt::Write as _;

use game_sim::data::constants::Constants;
use game_sim::equip::EquipEvent;
use game_sim::equip::flight::{
    EngineArch, EngineShipIo, EngineState, Nudge, PushLog, PushRecord, ShipFlightArch, Steering,
    Strafe, StrafeDir, Thruster, engine_controller, nudge_controller, steering_controller,
    strafe_controller, thruster_controller,
};
use game_sim::equip::{Equip, EquipKind, EquipManager};
use game_sim::object::ship::{ShipFlight, linear_drag};
use neuron_core::math::{Matrix, Vector};
use support::records::{Json, close, find_files, oracle_dir, read_lines};

/// Pure arithmetic in the reference's order: a few ulps.
const TOLERANCE: f32 = 1.0e-6;
const MAX_REPORTED: usize = 40;

fn f(j: &Json, key: &str) -> Option<f32> {
    j.get(key)?.as_f32()
}

fn v3(j: &Json, key: &str) -> Option<Vector> {
    let a = j.get(key)?.as_floats()?;
    Some(Vector::new(*a.first()?, *a.get(1)?, *a.get(2)?))
}

/// The orientation the controllers read: `ctl.orient`, else the core's
/// rows (`in.m`), which the sync copies into the object between PSIs.
fn orientation(ctl: &Json, record: &Json) -> Option<Matrix> {
    if let Some(m) = ctl.get("orient").and_then(Json::as_floats)
        && m.len() == 9
    {
        return Some(Matrix::from_words([
            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8],
        ]));
    }
    let m = record.get("in")?.get("m")?.as_floats()?;
    Some(Matrix::from_rows([
        [m[0], m[1], m[2]],
        [m[3], m[4], m[5]],
        [m[6], m[7], m[8]],
    ]))
}

/// One controller of a ship's unit, as `ctl.order` names it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Ctl {
    Strafe,
    Nudge,
    Steering,
    /// A thruster by its index in `ctl.thrusters`.
    Thruster(usize),
    Engine,
    /// The ship's own controller, IVP's standard controller or anything
    /// else: no push.
    Silent,
}

struct Rebuilt {
    flight: ShipFlight,
    engine: Option<(EngineState, bool)>,
    thrusters: Vec<(Thruster, bool)>,
    /// The run order (`ctl.order`), `None` in older records.
    order: Option<Vec<Ctl>>,
    power: f32,
    orientation: Matrix,
    zone_drag: Option<f32>,
}

fn rebuild(ctl: &Json, record: &Json) -> Option<Rebuilt> {
    let arch = ctl.get("arch").filter(|a| !a.is_null())?;
    let angular = v3(arch, "angular_drag")?;
    let torque = v3(arch, "steering_torque")?;
    let flight_arch = ShipFlightArch {
        linear_drag: f(arch, "linear_drag")?,
        angular_drag: angular.to_words(),
        steering_torque: torque.to_words(),
        nudge_force: f(arch, "nudge_force")?,
        strafe_force: f(arch, "strafe_force")?,
        strafe_power_usage: f(arch, "strafe_power_usage")?,
    };
    let gate = |key: &str| ctl.get(key).and_then(Json::as_bool).unwrap_or(true);
    let flight = ShipFlight {
        arch: flight_arch,
        steering: Steering {
            controller_enabled: gate("steering_enabled"),
            active: ctl.get("steering_active")?.as_bool()?,
            axis_throttle: v3(ctl, "axis")?,
            wake_requested: false,
        },
        nudge: Nudge {
            controller_enabled: gate("nudge_enabled"),
            active: ctl.get("nudge_active")?.as_bool()?,
            current: v3(ctl, "nudge")?,
            wake_requested: false,
        },
        strafe: Strafe {
            controller_enabled: gate("strafe_enabled"),
            current: StrafeDir::from_value(ctl.get("strafe")?.as_i32()?),
            wake_requested: false,
        },
        throttle: f(ctl, "throttle")?,
        thrust_power: f(ctl, "thrust_power")?,
        max_thrust_power: f(ctl, "max_thrust_power")?,
        angular_drag_factor: f(ctl, "angular_drag_factor")?,
    };
    let engine = match ctl.get("engine") {
        Some(e) if !e.is_null() => {
            let arch = EngineArch {
                max_force: f(e, "max_force")?,
                linear_drag: f(e, "linear_drag")?,
                power_usage: f(e, "power_usage")?,
                cruise_power_usage: f(e, "cruise_power_usage")?,
                cruise_charge_time: f(e, "cruise_charge_time")?,
                indestructible: true,
                reverse_fraction: f(e, "reverse_fraction")?,
            };
            let mut state = EngineState::new(arch);
            state.cruise_active = e.get("cruise")?.as_bool()?;
            state.cruise_charge_elapsed = f(e, "cruise_charge_elapsed")?;
            state.cruise_charge_rate = f(e, "cruise_charge_rate")?;
            state.last_wake_throttle = f(e, "last_wake_throttle")?;
            state.controller_enabled = e.get("controller").and_then(Json::as_bool).unwrap_or(true);
            let functioning = e.get("functioning").and_then(Json::as_bool).unwrap_or(true);
            Some((state, functioning))
        }
        _ => None,
    };
    let thruster_records = ctl
        .get("thrusters")
        .and_then(Json::as_array)
        .unwrap_or_default();
    let thruster_ids: Vec<Option<u32>> = thruster_records
        .iter()
        .map(|t| t.get("id").and_then(Json::as_u32))
        .collect();
    let order = ctl.get("order").and_then(Json::as_array).map(|a| {
        a.iter()
            .map(|n| match n.as_str().unwrap_or("") {
                "strafe" => Ctl::Strafe,
                "nudge" => Ctl::Nudge,
                "steering" => Ctl::Steering,
                "engine" => Ctl::Engine,
                other => other
                    .strip_prefix("thruster ")
                    .and_then(|id| id.parse::<u32>().ok())
                    .and_then(|id| thruster_ids.iter().position(|t| *t == Some(id)))
                    .map_or(Ctl::Silent, Ctl::Thruster),
            })
            .collect()
    });
    let thrusters = thruster_records
        .iter()
        .filter_map(|t| {
            let thruster = Thruster {
                power_usage: f(t, "power_usage")?,
                max_force: f(t, "max_force")?,
                controller_enabled: t.get("controller").and_then(Json::as_bool).unwrap_or(true),
                could_thrust: false,
                wake_requested: false,
            };
            Some((
                thruster,
                t.get("functioning")
                    .and_then(Json::as_bool)
                    .unwrap_or(false),
            ))
        })
        .collect();
    Some(Rebuilt {
        flight,
        engine,
        thrusters,
        order,
        power: f(ctl, "power").unwrap_or(f32::MAX),
        orientation: orientation(ctl, record)?,
        zone_drag: ctl.get("zone_drag").and_then(Json::as_f32),
    })
}

/// Runs the controllers in IVP's order.
fn run(mut r: Rebuilt, consts: &Constants, dt: f32) -> PushLog {
    let mut log = PushLog::default();
    let order = r.order.take().unwrap_or_else(|| {
        let mut order = vec![Ctl::Strafe, Ctl::Nudge, Ctl::Steering];
        order.extend((0..r.thrusters.len()).rev().map(Ctl::Thruster));
        order.push(Ctl::Engine);
        order
    });
    for c in order {
        run_one(&mut r, c, consts, dt, &mut log);
    }
    log
}

/// Runs one controller when it is registered.
fn run_one(r: &mut Rebuilt, c: Ctl, consts: &Constants, dt: f32, log: &mut PushLog) {
    let o = r.orientation;
    let arch = r.flight.arch;
    match c {
        Ctl::Strafe if r.flight.strafe.controller_enabled => strafe_controller(
            &mut r.flight.strafe,
            &arch,
            &o,
            &mut r.flight.thrust_power,
            dt,
            log,
        ),
        Ctl::Nudge if r.flight.nudge.controller_enabled => {
            nudge_controller(&r.flight.nudge, &arch, &o, dt, log);
        }
        Ctl::Steering if r.flight.steering.controller_enabled => {
            steering_controller(&r.flight.steering, &arch, &o, dt, log);
        }
        Ctl::Thruster(i) => {
            let mut events: Vec<EquipEvent> = Vec::new();
            if let Some((t, functioning)) = r.thrusters.get_mut(i)
                && t.controller_enabled
            {
                thruster_controller(
                    t,
                    *functioning,
                    &o,
                    &mut r.flight.thrust_power,
                    dt,
                    &mut events,
                    log,
                );
            }
        }
        Ctl::Engine => run_engine(r, consts, dt, log),
        _ => {}
    }
}

/// The engine controller with the ship's `get_linear_drag`.
fn run_engine(r: &mut Rebuilt, consts: &Constants, dt: f32, log: &mut PushLog) {
    let o = r.orientation;
    if let Some((mut engine, functioning)) = r.engine.take()
        && engine.controller_enabled
    {
        // The ship's `get_linear_drag` with this engine as its first,
        // active one.
        let mut list = EquipManager::default();
        let mut item = Equip::new(
            0x22,
            &game_sim::data::archetype::equipment::Equipment::new(0xC),
            game_sim::equip::types::ENGINE,
            EquipKind::Engine(engine.clone()),
            false,
        );
        item.active = true;
        list.add(item);
        let drag = linear_drag(&r.flight, &list, r.zone_drag, &consts.engine);
        let io = EngineShipIo {
            throttle: r.flight.throttle,
            orientation: &o,
            linear_drag: drag,
        };
        engine_controller(
            &mut engine,
            functioning,
            io,
            &mut r.power,
            &consts.engine,
            dt,
            log,
        );
    }
}

fn recorded(record: &Json) -> Option<Vec<(String, [f32; 4])>> {
    record
        .get("pushes")?
        .as_array()?
        .iter()
        .map(|p| {
            let a = p.as_array()?;
            let kind = a.first()?.as_str()?.to_owned();
            let v: Option<Vec<f32>> = a[1..].iter().map(Json::as_f32).collect();
            let v = v?;
            Some((kind, [*v.first()?, *v.get(1)?, *v.get(2)?, *v.get(3)?]))
        })
        .collect()
}

fn as_rows(log: &PushLog) -> Vec<(String, [f32; 4])> {
    log.0
        .iter()
        .map(|p| match *p {
            PushRecord::Linear(v, m) => ("linear".to_owned(), [v.x, v.y, v.z, m]),
            PushRecord::AngularWs(v, m) => ("angular_ws".to_owned(), [v.x, v.y, v.z, m]),
        })
        .collect()
}

fn same(a: &(String, [f32; 4]), b: &(String, [f32; 4])) -> bool {
    a.0 == b.0
        && a.1
            .iter()
            .zip(b.1.iter())
            .all(|(x, y)| close(*x, *y, TOLERANCE))
}

#[test]
fn controllers_replay_the_oracle() {
    let Some(dir) = oracle_dir("controller records") else {
        return;
    };
    let consts = Constants::default();
    let mut summary = String::new();
    let (mut records, mut checked, mut reordered, mut mismatched) =
        (0usize, 0usize, 0usize, 0usize);
    let mut reports = Vec::new();
    for file in find_files(&dir, "psi.jsonl") {
        for record in read_lines(&file) {
            records += 1;
            let Some(ctl) = record.get("ctl") else {
                continue;
            };
            let dt = f(&record, "dt").unwrap_or(0.0);
            let Some(rebuilt) = rebuild(ctl, &record) else {
                reports.push(format!(
                    "{}: tick {:?}: ctl incomplete",
                    file.display(),
                    record.get("tick")
                ));
                mismatched += 1;
                continue;
            };
            checked += 1;
            let has_order = rebuilt.order.is_some();
            let ours = as_rows(&run(rebuilt, &consts, dt));
            let theirs = recorded(&record).unwrap_or_default();
            let ordered =
                ours.len() == theirs.len() && ours.iter().zip(&theirs).all(|(a, b)| same(a, b));
            if ordered {
                continue;
            }
            let mut left = theirs.clone();
            let all = ours.len() == theirs.len()
                && ours.iter().all(|a| {
                    left.iter()
                        .position(|b| same(a, b))
                        .map(|i| left.remove(i))
                        .is_some()
                });
            // With the recorded order the pushes must come in it.
            if all && !has_order {
                reordered += 1;
                continue;
            }
            mismatched += 1;
            if reports.len() < MAX_REPORTED {
                reports.push(format!(
                    "{} tick {:?} psi {:?} {:?}: ours {ours:?} oracle {theirs:?}",
                    file.display(),
                    record.get("tick").and_then(Json::as_u32),
                    record.get("psi").and_then(Json::as_u32),
                    record.get("obj").and_then(Json::as_str),
                ));
            }
        }
    }
    let _ = writeln!(
        summary,
        "controller replay: {records} PSI records, {checked} with controller inputs, {reordered} in another order, {mismatched} mismatched (tolerance {TOLERANCE:e})"
    );
    for r in &reports {
        let _ = writeln!(summary, "{r}");
    }
    print!("{summary}");
    let _ = std::fs::write(dir.join("rust-controller-replay.txt"), &summary);
    assert_eq!(mismatched, 0, "{summary}");
}

#[test]
fn rebuild_and_run_a_synthetic_record() {
    // A record in the harness's layout: full throttle, a yaw, no power
    // given (enough is assumed), the orientation from the core's matrix.
    let line = concat!(
        r#"{"rec":"psi","tick":1,"psi":1,"obj":"pilot","id":1,"dt":"3d088889","idt":"41700000","#,
        r#""time":["00000000","00000000"],"limits":["00000000","00000000"],"#,
        r#""ctl":{"throttle":"3f800000","axis":["00000000","3f000000","00000000"],"steering_active":true,"#,
        r#""nudge":["00000000","00000000","00000000"],"nudge_active":true,"strafe":0,"#,
        r#""thrust_power":"00000000","max_thrust_power":"00000000","angular_drag_factor":"3f800000","#,
        r#""engine":{"cruise":false,"cruise_charge_elapsed":"00000000","cruise_charge_rate":"00000000","#,
        r#""last_wake_throttle":"3f800000","max_force":"47000000","linear_drag":"43c80000","power_usage":"00000000","#,
        r#""cruise_power_usage":"00000000","cruise_charge_time":"40a00000","reverse_fraction":"3e19999a"},"#,
        r#""arch":{"linear_drag":"3f800000","angular_drag":["45bb8000","45bb8000","45bb8000"],"#,
        r#""steering_torque":["46ea6000","46ea6000","46ea6000"],"nudge_force":"00000000","strafe_force":"00000000","strafe_power_usage":"00000000"}},"#,
        r#""in":{"m":["3f800000","00000000","00000000","00000000","3f800000","00000000","00000000","00000000","3f800000","00000000","00000000","00000000"]},"#,
        r#""pushes":[]}"#
    );
    let record = support::records::parse(line).unwrap();
    let ctl = record.get("ctl").unwrap();
    let r = rebuild(ctl, &record).unwrap();
    let log = run(r, &Constants::default(), f(&record, "dt").unwrap());
    let rows = as_rows(&log);
    assert_eq!(rows.len(), 2);
    assert_eq!(rows[0].0, "angular_ws");
    assert_eq!(rows[1].0, "linear");
    // The engine pushes along +Z (the ship's Z column) by −force·dt.
    assert_eq!(&rows[1].1[..3], &[0.0, 0.0, 1.0]);
    assert!(rows[1].1[3] < 0.0);
}
