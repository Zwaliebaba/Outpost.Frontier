//! `IVP_Calc_Next_PSI_Solver` and the anomaly limits
//! (`CppSource/IVP/ivp_physics/ivp_calc_next_psi_solver.cpp:118-290`,
//! `ivp_environment.cpp:770-810`).
//!
//! Not modelled: the car-wheel axis lock (`AxisLock`, never set by Common)
//! and the hull managers, which only schedule IVP's own collision events;
//! the port's contacts do not use them.

use super::core::{Core, FLAG_ROTATION_SIMPLE_MASK};
use super::math::{IvpQuat, IvpTime, add_multiple, e, quad_length, r};

/// `IVP_Event_Sim`: the PSI length and its reciprocal.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EventSim {
    /// `delta_time`.
    pub delta_time: f32,
    /// `i_delta_time`.
    pub i_delta_time: f32,
}

/// `IVP_Anomaly_Limits` with the values Common writes from `[PhySys]`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct AnomalyLimits {
    /// `MaxVelocity` (`ANOM_LIMITS_MAX_VELOCITY`).
    pub max_velocity: f32,
    /// `MaxAngularVelocityPerPsi` (`ANOM_LIMITS_MAX_ANGULAR_VELOCITY_PER_PSI`).
    pub max_angular_velocity_per_psi: f32,
}

impl Default for AnomalyLimits {
    /// The IVP constructor's values (`ivp_environment.cpp:735`).
    fn default() -> Self {
        Self {
            max_velocity: 2000.0,
            max_angular_velocity_per_psi: 1.570_796_4,
        }
    }
}

/// `IVP_Anomaly_Manager::MaxVelocityExceeded`: the speed scaled to 99 % of
/// the limit.
pub fn max_velocity_exceeded(limits: &AnomalyLimits, speed: &mut [f32; 3]) {
    let target = e(limits.max_velocity) * e(0.99f32);
    let length = quad_length(*speed).sqrt();
    let scale = target / length;
    *speed = speed.map(|c| r(scale * e(c)));
}

/// `IVP_Anomaly_Manager::MaxAngularVelocityExceeded`: the spin scaled to
/// 90 % of the per-second limit.
pub fn max_angular_velocity_exceeded(
    limits: &AnomalyLimits,
    env_inv_delta_time: f32,
    rot_speed: &mut [f32; 3],
) {
    let target = (e(env_inv_delta_time) * e(limits.max_angular_velocity_per_psi)) * e(0.9f32);
    let length = quad_length(*rot_speed).sqrt();
    let scale = target / length;
    *rot_speed = rot_speed.map(|c| r(scale * e(c)));
}

/// `calc_next_PSI_matrix` (`ivp_calc_next_psi_solver.cpp:118`): limits, the
/// rotation over the PSI, the position and orientation at its start and
/// the orientation at its end. `now` is the environment's current time
/// (the start of this PSI) and `env_inv_delta_time` its `inv_delta_time`.
pub fn calc_next_psi_matrix(
    core: &mut Core,
    event: EventSim,
    now: IvpTime,
    limits: &AnomalyLimits,
    env_inv_delta_time: f32,
) {
    if let Some(clip) = core.spin_clipping {
        for i in 0..3 {
            if core.rot_speed[i] > clip[i] {
                core.rot_speed[i] = clip[i];
            } else if -clip[i] > core.rot_speed[i] {
                core.rot_speed[i] = -clip[i];
            }
        }
    } else {
        let quad_rot = quad_length(core.rot_speed);
        let max_rot = e(env_inv_delta_time) * e(limits.max_angular_velocity_per_psi);
        if quad_rot > max_rot * max_rot {
            max_angular_velocity_exceeded(limits, env_inv_delta_time, &mut core.rot_speed);
        }
    }
    let max_velocity = e(limits.max_velocity);
    if !(max_velocity * max_velocity >= quad_length(core.speed)) {
        max_velocity_exceeded(limits, &mut core.speed);
    }

    core.current_speed = r(quad_length(core.speed).sqrt());
    core.i_delta_time = event.i_delta_time;

    let q_core_f_core = calc_rotation_matrix(core, event.delta_time);

    let since = now.since(core.time_of_last_psi);
    core.time_of_last_psi = now;
    let since_stored = r(since);
    core.pos_world_f_core_last_psi = add_multiple(
        core.pos_world_f_core_last_psi,
        core.delta_world_f_core_psis,
        since_stored,
    );
    core.delta_world_f_core_psis = core.speed;
    core.q_world_f_core_last_psi = core.q_world_f_core_next_psi;

    let a = core.q_world_f_core_next_psi;
    let l = q_core_f_core;
    let (ax, ay, az, aw) = (e(a.x), e(a.y), e(a.z), e(a.w));
    let (lx, ly, lz, lw) = (e(l.x), e(l.y), e(l.z), e(l.w));
    let new_y = ((ly * aw + lw * ay) + lx * az) - lz * ax;
    let new_z = r(((lz * aw + ly * ax) + lw * az) - lx * ay);
    let new_w = r(((lw * aw - lx * ax) - ly * ay) - lz * az);
    let new_x = ((lz * ay + lx * aw) + lw * ax) - ly * az;
    core.q_world_f_core_next_psi = IvpQuat::new(r(new_x), r(new_y), new_z, new_w);
    core.q_world_f_core_next_psi.fast_normize();

    calc_psi_rotation_axis(core, &q_core_f_core);
}

/// `calc_rotation_matrix` (`ivp_calc_next_psi_solver.cpp:290`): the
/// rotation over one PSI in core space. A core whose inertia is not
/// isotropic integrates the torque-free precession in sub-steps.
#[must_use]
pub fn calc_rotation_matrix(core: &mut Core, delta_time: f32) -> IvpQuat {
    if core.flags & FLAG_ROTATION_SIMPLE_MASK != 0 {
        return IvpQuat::rot_vector(core.rot_speed, delta_time);
    }
    let dt = e(delta_time);
    let quad_rot_step = quad_length(core.rot_speed) * dt * dt;
    let mut sub_dt = delta_time;
    let mut step_count = 1i32;
    if quad_rot_step > e(f32::from_bits(0x3ce3_8e3a)) {
        step_count =
            crate::num::trunc_to_i32((quad_rot_step * e(f32::from_bits(0x430f_ffff))).sqrt()) + 1;
        sub_dt = r(dt / f64::from(step_count));
    }
    let mut q = IvpQuat::fast_rot_vector(core.rot_speed, sub_dt);
    let inertia = core.rot_inertia;
    let inv = core.inv_rot_inertia;
    let coeff_x = r((e(inertia[1]) - e(inertia[2])) * e(inv[0]));
    let coeff_y = r((e(inertia[2]) - e(inertia[0])) * e(inv[1]));
    let coeff_z = r((e(inertia[0]) - e(inertia[1])) * e(inv[2]));
    let mut step = 1;
    loop {
        let w = core.rot_speed;
        let dx = e(coeff_x) * e(w[1]) * e(w[2]);
        let dy = r(e(coeff_y) * e(w[0]) * e(w[2]));
        let dz = r(e(w[1]) * e(w[0]) * e(coeff_z));
        core.rot_speed[0] = r(dx * e(sub_dt) + e(w[0]));
        core.rot_speed[1] = r(e(dy) * e(sub_dt) + e(w[1]));
        core.rot_speed[2] = r(e(dz) * e(sub_dt) + e(w[2]));
        if step >= step_count {
            return q;
        }
        let s = IvpQuat::fast_rot_vector(core.rot_speed, sub_dt);
        step += 1;
        let (sx, sy, sz, sw) = (e(s.x), e(s.y), e(s.z), e(s.w));
        let (ox, oy, oz, ow) = (e(q.x), e(q.y), e(q.z), e(q.w));
        let new_y = ((sy * ow + sw * oy) + sz * ox) - sx * oz;
        let new_z = r(((sz * ow + sx * oy) + sw * oz) - sy * ox);
        let new_w = r(((sw * ow - sx * ox) - sy * oy) - sz * oz);
        let new_x = ((sx * ow + sw * ox) + sy * oz) - sz * oy;
        q = IvpQuat::new(r(new_x), r(new_y), new_z, new_w);
    }
}

/// `calc_psi_rotation_axis` (`ivp_calc_next_psi_solver.cpp:430`): the
/// angular speed, its world axis and the fastest surface rotation speed.
pub fn calc_psi_rotation_axis(core: &mut Core, q_core_f_core: &IvpQuat) {
    let (qx, qy, qz) = (e(q_core_f_core.x), e(q_core_f_core.y), e(q_core_f_core.z));
    let quad_len = r((qx * qx + qy * qy) + qz * qz);
    if quad_len > f32::from_bits(0x2edb_e6ff) {
        let inv_length = 1.0 / e(quad_len).sqrt();
        let length = inv_length * e(quad_len);
        let length2 = length * length;
        let length3 = length2 * length;
        let length5 = length3 * length2;
        let series =
            length5 * e(f32::from_bits(0x3ece_eb70)) + length3 * e(f32::from_bits(0x3e2a_aaab));
        let abs_omega = (length + series) * e(core.i_delta_time);
        core.abs_omega = r(abs_omega + abs_omega);
        let m = &core.m_world_f_core_last_psi.rows;
        let row0 = (e(m[0][2]) * qz + e(m[0][1]) * qy) + e(m[0][0]) * qx;
        let row1 = r((e(m[1][0]) * qx + e(m[1][2]) * qz) + e(m[1][1]) * qy);
        let row2 = r((e(m[2][0]) * qx + e(m[2][2]) * qz) + e(m[2][1]) * qy);
        core.rotation_axis_ws = [
            r(row0 * inv_length),
            r(e(row1) * inv_length),
            r(e(row2) * inv_length),
        ];
    } else {
        core.abs_omega = 0.0;
        core.rotation_axis_ws = [1.0, 0.0, 0.0];
    }
    core.max_surface_rot_speed = r(e(core.abs_omega) * e(core.max_surface_deviation));
}

/// The environment values [`set_transformation`] reads.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct BeamClock {
    /// `current_time`.
    pub current_time: IvpTime,
    /// `time_of_last_psi`.
    pub time_of_last_psi: IvpTime,
    /// `delta_time`.
    pub delta_time: f32,
    /// `inv_delta_time`.
    pub inv_delta_time: f32,
}

/// `set_transformation` (`ivp_calc_next_psi_solver.cpp:460`): puts the
/// core at a new pose and stops it (`beam_object_to_new_position`).
pub fn set_transformation(
    core: &mut Core,
    rotation: IvpQuat,
    position: [f32; 3],
    optimize_for_repeated_calls: bool,
    clock: &BeamClock,
) {
    let mut since_last_psi = r(clock.current_time.since(core.time_of_last_psi));
    if clock.delta_time > since_last_psi {
        since_last_psi = clock.delta_time;
        core.i_delta_time = clock.inv_delta_time;
    } else {
        core.i_delta_time = r(1.0 / e(since_last_psi));
    }
    let _ = since_last_psi;
    core.q_world_f_core_next_psi = rotation;
    let mut q_delta =
        IvpQuat::inv_mult(&core.q_world_f_core_last_psi, &core.q_world_f_core_next_psi);
    core.q_world_f_core_last_psi = rotation;
    core.time_of_last_psi = clock.time_of_last_psi;
    let moved = [
        r(e(position[0]) - e(core.pos_world_f_core_last_psi[0])),
        r(e(position[1]) - e(core.pos_world_f_core_last_psi[1])),
        r(e(position[2]) - e(core.pos_world_f_core_last_psi[2])),
    ];
    let distance = r(quad_length(moved).sqrt());
    core.pos_world_f_core_last_psi = position;
    core.delta_world_f_core_psis = [0.0; 3];
    core.speed = [0.0; 3];
    core.m_world_f_core_last_psi.vv = position;
    rotation.set_matrix(&mut core.m_world_f_core_last_psi);
    if optimize_for_repeated_calls {
        core.current_speed = r(e(distance) * e(core.i_delta_time));
    } else {
        core.current_speed = 0.0;
        q_delta = IvpQuat::IDENTITY;
    }
    calc_psi_rotation_axis(core, &q_delta);
    core.current_speed = 0.0;
    core.abs_omega = 0.0;
    core.max_surface_rot_speed = 0.0;
}

#[cfg(test)]
mod tests {
    use super::*;

    fn core() -> Core {
        let mut core = Core {
            movement_state: 1,
            mass: 10.0,
            rot_inertia: [1.0, 2.0, 3.0],
            upper_limit_radius: 2.0,
            max_surface_deviation: 2.5,
            ..Core::default()
        };
        core.recompute_inverse_inertia();
        core
    }

    const EVENT: EventSim = EventSim {
        delta_time: 0.1,
        i_delta_time: 10.0,
    };

    #[test]
    fn velocity_limit_scales_to_ninety_nine_percent() {
        let limits = AnomalyLimits {
            max_velocity: 100.0,
            ..AnomalyLimits::default()
        };
        let mut c = core();
        c.speed = [300.0, 0.0, 400.0];
        calc_next_psi_matrix(&mut c, EVENT, IvpTime::default(), &limits, 10.0);
        assert!((c.speed[0] - 59.4).abs() < 1e-4 && (c.speed[2] - 79.2).abs() < 1e-4);
        assert_eq!(c.delta_world_f_core_psis, c.speed);
        assert!((c.current_speed - 99.0).abs() < 1e-4);
    }

    #[test]
    fn angular_limit_and_spin_clipping() {
        let limits = AnomalyLimits::default();
        let mut c = core();
        c.flags |= 0x40;
        c.rot_speed = [100.0, 0.0, 0.0];
        calc_next_psi_matrix(&mut c, EVENT, IvpTime::default(), &limits, 10.0);
        assert!((c.rot_speed[0] - 10.0 * 1.570_796_4 * 0.9).abs() < 1e-3);
        let mut clipped = core();
        clipped.flags |= 0x40;
        clipped.spin_clipping = Some([1.0, 1.0, 1.0]);
        clipped.rot_speed = [5.0, -5.0, 0.5];
        calc_next_psi_matrix(&mut clipped, EVENT, IvpTime::default(), &limits, 10.0);
        assert_eq!(clipped.rot_speed, [1.0, -1.0, 0.5]);
    }

    #[test]
    fn position_and_orientation_advance() {
        let limits = AnomalyLimits::default();
        let mut c = core();
        c.flags |= 0x40;
        c.time_of_last_psi = IvpTime::new(0.0, 0.0);
        c.delta_world_f_core_psis = [5.0, 0.0, 0.0];
        c.speed = [6.0, 0.0, 0.0];
        c.rot_speed = [0.0, 0.0, 1.0];
        calc_next_psi_matrix(&mut c, EVENT, IvpTime::new(0.0, 0.1), &limits, 10.0);
        assert_eq!(c.pos_world_f_core_last_psi, [0.5, 0.0, 0.0]);
        assert_eq!(c.time_of_last_psi, IvpTime::new(0.0, 0.1));
        assert_eq!(c.q_world_f_core_last_psi, IvpQuat::IDENTITY);
        assert!((c.q_world_f_core_next_psi.z - 0.05f32.sin()).abs() < 1e-6);
        assert!((c.abs_omega - 1.0).abs() < 1e-3, "{}", c.abs_omega);
        assert_eq!(c.rotation_axis_ws, [0.0, 0.0, 1.0]);
        assert!((c.max_surface_rot_speed - 2.5).abs() < 3e-3);
    }

    #[test]
    fn precession_conserves_rough_energy() {
        let mut c = core();
        c.rot_speed = [3.0, 4.0, 5.0];
        let energy = |w: [f32; 3]| {
            f64::from(w[0]).powi(2) + 2.0 * f64::from(w[1]).powi(2) + 3.0 * f64::from(w[2]).powi(2)
        };
        let before = energy(c.rot_speed);
        let q = calc_rotation_matrix(&mut c, 0.1);
        assert!((energy(c.rot_speed) - before).abs() / before < 0.2);
        let len = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
        assert!((len - 1.0).abs() < 1e-2);
        let mut small = core();
        small.rot_speed = [0.1, 0.0, 0.0];
        let q = calc_rotation_matrix(&mut small, 0.1);
        assert_eq!(small.rot_speed, [0.1, 0.0, 0.0]);
        assert!((q.x - 0.005).abs() < 1e-7);
    }

    #[test]
    fn rotation_axis_of_no_rotation() {
        let mut c = core();
        c.abs_omega = 5.0;
        calc_psi_rotation_axis(&mut c, &IvpQuat::IDENTITY);
        assert_eq!(c.abs_omega, 0.0);
        assert_eq!(c.rotation_axis_ws, [1.0, 0.0, 0.0]);
        assert_eq!(c.max_surface_rot_speed, 0.0);
    }

    #[test]
    fn beaming_stops_the_core() {
        let mut c = core();
        c.speed = [1.0, 2.0, 3.0];
        c.time_of_last_psi = IvpTime::new(0.0, 0.0);
        let clock = BeamClock {
            current_time: IvpTime::new(0.0, 0.05),
            time_of_last_psi: IvpTime::new(0.0, 0.0),
            delta_time: 0.1,
            inv_delta_time: 10.0,
        };
        let rotation = IvpQuat::rot_vector([0.0, 1.0, 0.0], 1.0);
        set_transformation(&mut c, rotation, [7.0, 8.0, 9.0], true, &clock);
        assert_eq!(c.speed, [0.0; 3]);
        assert_eq!(c.pos_world_f_core_last_psi, [7.0, 8.0, 9.0]);
        assert_eq!(c.m_world_f_core_last_psi.vv, [7.0, 8.0, 9.0]);
        assert_eq!(c.q_world_f_core_last_psi, rotation);
        assert_eq!(c.q_world_f_core_next_psi, rotation);
        assert_eq!(c.i_delta_time, 10.0);
        assert_eq!(c.current_speed, 0.0);
        assert_eq!(c.abs_omega, 0.0);
        let long_clock = BeamClock {
            current_time: IvpTime::new(1.0, 0.0),
            ..clock
        };
        set_transformation(&mut c, rotation, [7.0, 8.0, 9.0], false, &long_clock);
        assert_eq!(c.i_delta_time, 1.0);
    }
}
