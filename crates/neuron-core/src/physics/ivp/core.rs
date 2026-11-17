//! `IVP_Core`: the rigid-body state one or more real objects share
//! (`CppSource/IVP/ivp_physics/ivp_core.h`, `ivp_core.cpp`), with the
//! methods free flight uses.
//!
//! Field names follow the Ipion SDK; where the reconstruction named a field
//! differently its name is given in the field's documentation.

use super::math::{IvpMatrix, IvpQuat, IvpTime, e, r};

/// `IVP_Core::Flags` bit: the inertia is isotropic (`RecomputeInverseInertia`
/// sets it), so the PSI rotation skips the precession sub-steps.
pub const FLAG_ISOTROPIC: u32 = 0x40;
/// `IVP_Core::Flags` bits 0x80 and 0x40 tested together by
/// `calc_rotation_matrix`.
pub const FLAG_ROTATION_SIMPLE_MASK: u32 = 0xC0;
/// `IVP_Core::Flags` bits that mark a fixed (unmovable) core.
pub const FLAG_FIXED_MASK: u32 = 0x0C;
/// `IVP_Core::Flags` bits 8 and 9: the gravity controller skips the core.
pub const FLAG_SKIP_GRAVITY_MASK: u32 = 0x300;

/// The low byte of `MovementState` while a core sleeps.
pub const MOVEMENT_FROZEN: u32 = 8;

/// The rigid-body state of a core. Vectors are `[x, y, z]`.
#[derive(Clone, Debug, PartialEq)]
pub struct Core {
    /// +0x00 `flags`.
    pub flags: u32,
    /// +0x04 `upper_limit_radius`.
    pub upper_limit_radius: f32,
    /// +0x08 `max_surface_deviation` (the reconstruction's `SpeedLimit`).
    pub max_surface_deviation: f32,
    /// +0x14 `rot_inertia`.
    pub rot_inertia: [f32; 3],
    /// +0x20 `mass`.
    pub mass: f32,
    /// +0x24 `rot_speed_damp_factor` (`RotSpeedDamp`).
    pub rot_speed_damp_factor: [f32; 3],
    /// +0x34 `inv_rot_inertia`.
    pub inv_rot_inertia: [f32; 3],
    /// +0x40 `inv_object_mass`.
    pub inv_mass: f32,
    /// +0x44 `speed_damp_factor` (`SpeedDamp`).
    pub speed_damp_factor: f32,
    /// +0x48 `0.5 / upper_limit_radius`.
    pub d_angular_limit_factor: f32,
    /// +0x4c `spin_clipping`. Common never sets it; kept for completeness.
    pub spin_clipping: Option<[f32; 3]>,
    /// +0x60 `movement_state`: the low byte is 1 to 3 while moving and 8
    /// while frozen.
    pub movement_state: u32,
    /// +0x68 `time_of_last_psi`.
    pub time_of_last_psi: IvpTime,
    /// +0x70 `i_delta_time`.
    pub i_delta_time: f32,
    /// +0x74 `rot_speed_change`: pending angular impulses (object space).
    pub rot_speed_change: [f32; 3],
    /// +0x84 `speed_change`: pending linear impulses (world space).
    pub speed_change: [f32; 3],
    /// +0x94 `rot_speed`, object space.
    pub rot_speed: [f32; 3],
    /// +0xa4 `speed`, world space.
    pub speed: [f32; 3],
    /// +0xb4 `pos_world_f_core_last_psi`.
    pub pos_world_f_core_last_psi: [f32; 3],
    /// +0xc4 `delta_world_f_core_psis`: the velocity the position is
    /// extrapolated with until the next PSI.
    pub delta_world_f_core_psis: [f32; 3],
    /// +0xd4 `q_world_f_core_last_psi`.
    pub q_world_f_core_last_psi: IvpQuat,
    /// +0xe4 `q_world_f_core_next_psi`.
    pub q_world_f_core_next_psi: IvpQuat,
    /// +0xf4 `m_world_f_core_last_psi`.
    pub m_world_f_core_last_psi: IvpMatrix,
    /// +0x134 `rotation_axis_world_space` (the reconstruction's `RotAxisOS`).
    pub rotation_axis_ws: [f32; 3],
    /// +0x144 `current_speed` (the reconstruction's `Margin`).
    pub current_speed: f32,
    /// +0x148 `abs_omega` (the reconstruction's `MaxSurfaceDeviation`).
    pub abs_omega: f32,
    /// +0x14c `max_surface_rot_speed` (the reconstruction's `BoundingRadius`).
    pub max_surface_rot_speed: f32,
    /// +0x160 `time_of_last_motion`.
    pub time_of_last_motion: IvpTime,
    /// +0x168 `time_of_last_long_motion`.
    pub time_of_last_long_motion: IvpTime,
    /// +0x170 `q_last_motion`.
    pub q_last_motion: IvpQuat,
    /// +0x180 `q_last_long_motion`.
    pub q_last_long_motion: IvpQuat,
    /// +0x190 `pos_last_motion`.
    pub pos_last_motion: [f32; 3],
    /// +0x1a0 `pos_last_long_motion`.
    pub pos_last_long_motion: [f32; 3],
}

impl Default for Core {
    fn default() -> Self {
        Self {
            flags: 0,
            upper_limit_radius: 0.0,
            max_surface_deviation: 0.0,
            rot_inertia: [1.0; 3],
            mass: 1.0,
            rot_speed_damp_factor: [0.0; 3],
            inv_rot_inertia: [1.0; 3],
            inv_mass: 1.0,
            speed_damp_factor: 0.0,
            d_angular_limit_factor: 0.0,
            spin_clipping: None,
            movement_state: MOVEMENT_FROZEN,
            time_of_last_psi: IvpTime::default(),
            i_delta_time: 0.0,
            rot_speed_change: [0.0; 3],
            speed_change: [0.0; 3],
            rot_speed: [0.0; 3],
            speed: [0.0; 3],
            pos_world_f_core_last_psi: [0.0; 3],
            delta_world_f_core_psis: [0.0; 3],
            q_world_f_core_last_psi: IvpQuat::IDENTITY,
            q_world_f_core_next_psi: IvpQuat::IDENTITY,
            m_world_f_core_last_psi: IvpMatrix::IDENTITY,
            rotation_axis_ws: [1.0, 0.0, 0.0],
            current_speed: 0.0,
            abs_omega: 0.0,
            max_surface_rot_speed: 0.0,
            time_of_last_motion: IvpTime::default(),
            time_of_last_long_motion: IvpTime::default(),
            q_last_motion: IvpQuat::IDENTITY,
            q_last_long_motion: IvpQuat::IDENTITY,
            pos_last_motion: [0.0; 3],
            pos_last_long_motion: [0.0; 3],
        }
    }
}

impl Core {
    /// The low byte of `movement_state` as the signed byte the reference
    /// compares (`SHL 24; CMP`).
    #[must_use]
    pub fn movement_byte(&self) -> i8 {
        (self.movement_state & 0xFF) as u8 as i8
    }

    /// Whether the core is asleep (movement byte 8 or above, signed).
    #[must_use]
    pub fn is_frozen(&self) -> bool {
        self.movement_byte() >= 8
    }

    /// `RecomputeInverseInertia` (`ivp_core.cpp:1254`).
    pub fn recompute_inverse_inertia(&mut self) {
        let inv_izz = 1.0 / e(self.rot_inertia[2]);
        let inv_iyy = 1.0 / e(self.rot_inertia[1]);
        let inv_ixx = 1.0 / e(self.rot_inertia[0]);
        self.inv_rot_inertia = [r(inv_ixx), r(inv_iyy), r(inv_izz)];
        self.inv_mass = r(1.0 / e(self.mass));
        let inv_ixx_f = e(self.inv_rot_inertia[0]);
        let d1 = inv_iyy - inv_izz;
        let d2 = inv_izz - inv_ixx_f;
        let d3 = r(inv_ixx_f - inv_iyy);
        let spread = (d2 * d2 + e(d3) * e(d3)) + d1 * d1;
        let magnitude = (inv_ixx_f * inv_ixx_f + inv_iyy * inv_iyy) + inv_izz * inv_izz;
        if magnitude * e(0.010_000_001_f32) > spread {
            self.flags = (self.flags & 0xffff_ff7f) | 0x40;
        } else {
            self.flags &= 0xffff_ff3f;
        }
        self.d_angular_limit_factor = r(0.5 / e(self.upper_limit_radius));
    }

    /// `set_mass`: the inertia scales with the mass.
    pub fn set_mass(&mut self, new_mass: f32) {
        let ratio = e(new_mass) / e(self.mass);
        self.mass = new_mass;
        self.rot_inertia = self.rot_inertia.map(|c| r(ratio * e(c)));
        self.recompute_inverse_inertia();
    }

    /// `set_rotation_inertia`.
    pub fn set_rotation_inertia(&mut self, inertia: [f32; 3]) {
        self.rot_inertia = inertia;
        self.recompute_inverse_inertia();
    }

    /// `push_core_linear` (`ivp_core.cpp:1310`): `speed_change +=
    /// direction * (inv_mass * magnitude)`.
    pub fn push_core_linear(&mut self, direction: [f32; 3], magnitude: f32) {
        let k = e(self.inv_mass) * e(magnitude);
        let new_y = r(k * e(direction[1]) + e(self.speed_change[1]));
        let new_z = r(k * e(direction[2]) + e(self.speed_change[2]));
        let new_x = r(k * e(direction[0]) + e(self.speed_change[0]));
        self.speed_change = [new_x, new_y, new_z];
    }

    /// `push_core_angular_ws` (`ivp_core.cpp:1326`): the axis rotated into
    /// core space, scaled by the inverse inertia.
    pub fn push_core_angular_ws(&mut self, axis_ws: [f32; 3], magnitude: f32) {
        let local = self.m_world_f_core_last_psi.vimult(axis_ws);
        let scaled_x = e(local[0]) * e(self.inv_rot_inertia[0]);
        let scaled_y = r(e(local[1]) * e(self.inv_rot_inertia[1]));
        let scaled_z = r(e(local[2]) * e(self.inv_rot_inertia[2]));
        self.accumulate_rot_change(scaled_x, scaled_y, scaled_z, magnitude);
    }

    /// `push_core_angular_cs`: as [`Core::push_core_angular_ws`] without the
    /// rotation.
    pub fn push_core_angular_cs(&mut self, axis_cs: [f32; 3], magnitude: f32) {
        let scaled_x = e(self.inv_rot_inertia[0]) * e(axis_cs[0]);
        let scaled_y = r(e(self.inv_rot_inertia[1]) * e(axis_cs[1]));
        let scaled_z = r(e(self.inv_rot_inertia[2]) * e(axis_cs[2]));
        self.accumulate_rot_change(scaled_x, scaled_y, scaled_z, magnitude);
    }

    fn accumulate_rot_change(&mut self, sx: f64, sy: f32, sz: f32, magnitude: f32) {
        let m = e(magnitude);
        self.rot_speed_change[0] = r(sx * m + e(self.rot_speed_change[0]));
        self.rot_speed_change[1] = r(e(sy) * m + e(self.rot_speed_change[1]));
        self.rot_speed_change[2] = r(e(sz) * m + e(self.rot_speed_change[2]));
    }

    /// `async_add_speed_object_ws` without the wake-up (the world wakes
    /// the core): `speed_change += speed`, stores z, x, y.
    pub fn add_speed_change(&mut self, speed_ws: [f32; 3]) {
        let y = speed_ws[1] + self.speed_change[1];
        let z = speed_ws[2] + self.speed_change[2];
        let x = self.speed_change[0] + speed_ws[0];
        self.speed_change = [x, y, z];
    }

    /// `async_add_rot_speed_object_cs` without the wake-up.
    pub fn add_rot_speed_change(&mut self, rot_speed_cs: [f32; 3]) {
        let y = rot_speed_cs[1] + self.rot_speed_change[1];
        let z = rot_speed_cs[2] + self.rot_speed_change[2];
        let x = self.rot_speed_change[0] + rot_speed_cs[0];
        self.rot_speed_change = [x, y, z];
    }

    /// `calc_impulse_deltas` then `apply_impulse_at_point`: a world impulse
    /// at a point given in core space, accumulated as pending changes.
    pub fn apply_impulse_at_point(
        &mut self,
        lever_arm_cs: [f32; 3],
        impulse_cs: [f32; 3],
        impulse_ws: [f32; 3],
    ) {
        let (l, p) = (lever_arm_cs, impulse_cs);
        let cross_x = e(p[2]) * e(l[1]) - e(p[1]) * e(l[2]);
        let cross_y = e(p[0]) * e(l[2]) - e(p[2]) * e(l[0]);
        let cross_z = r(e(p[1]) * e(l[0]) - e(p[0]) * e(l[1]));
        let angular_y = r(cross_y * e(self.inv_rot_inertia[1]));
        let angular_z = cross_z * self.inv_rot_inertia[2];
        let angular_x = r(cross_x * e(self.inv_rot_inertia[0]));
        let linear = impulse_ws.map(|c| c * self.inv_mass);
        self.rot_speed_change[0] += angular_x;
        self.rot_speed_change[1] += angular_y;
        self.rot_speed_change[2] += angular_z;
        self.speed_change[0] += linear[0];
        self.speed_change[1] += linear[1];
        self.speed_change[2] += linear[2];
    }

    /// `commit_speed_changes`: the pending changes join the velocities.
    pub fn commit_speed_changes(&mut self) {
        for i in 0..3 {
            self.rot_speed[i] += self.rot_speed_change[i];
        }
        for i in 0..3 {
            self.speed[i] += self.speed_change[i];
        }
        self.rot_speed_change = [0.0; 3];
        self.speed_change = [0.0; 3];
    }

    /// `apply_velocity_damping` (`damp_object`, `ivp_core.cpp:1466`).
    pub fn apply_velocity_damping(&mut self, delta_time: f32, rot_damp: [f32; 3], speed_damp: f32) {
        let dt = e(delta_time);
        let dtx = dt * e(rot_damp[0]);
        let dty_ext = dt * e(rot_damp[1]);
        let dty = r(dty_ext);
        let dtz_ext = dt * e(rot_damp[2]);
        let dtz = r(dtz_ext);
        let quad = (dtz_ext * dtz_ext + dty_ext * dty_ext) + dtx * dtx;
        let (fx, fy, fz) = if !(quad >= 0.5) {
            (1.0 - dtx, r(1.0 - e(dty)), r(1.0 - e(dtz)))
        } else {
            ((-dtx).exp(), r((-e(dty)).exp()), r((-e(dtz)).exp()))
        };
        let s = dt * e(speed_damp);
        let fs = if !(s >= e(0.9f32)) {
            1.0 - s
        } else {
            (-s).exp()
        };
        self.rot_speed[2] = r(e(fz) * e(self.rot_speed[2]));
        self.rot_speed[0] = r(fx * e(self.rot_speed[0]));
        self.rot_speed[1] = r(e(fy) * e(self.rot_speed[1]));
        self.speed = self.speed.map(|c| r(fs * e(c)));
    }

    /// `damp_velocities` (`ivp_core.cpp:1510`): a slow core (movement byte
    /// 2 or more) is damped 0.1 harder on every axis.
    pub fn damp_velocities(&mut self, delta_time: f32) {
        if self.movement_byte() >= 2 {
            let rot = self.rot_speed_damp_factor.map(|c| c + 0.1);
            let speed = self.speed_damp_factor + 0.1;
            self.apply_velocity_damping(delta_time, rot, speed);
        } else {
            let rot = self.rot_speed_damp_factor;
            self.apply_velocity_damping(delta_time, rot, self.speed_damp_factor);
        }
    }

    /// `IVP_Standard_Gravity_Controller::do_simulation_controller`
    /// (`ivp_environment.cpp:1588`) for one core: damping, the commit of
    /// the pending changes, then `speed += dt * gravity` (zero in this game,
    /// but the sums are formed, so a negative zero becomes positive).
    pub fn gravity_controller_step(&mut self, delta_time: f32, gravity: [f32; 3]) {
        if self.flags & FLAG_SKIP_GRAVITY_MASK != 0 {
            return;
        }
        self.damp_velocities(delta_time);
        self.commit_speed_changes();
        let dt = e(delta_time);
        let new_y = dt * e(gravity[1]) + e(self.speed[1]);
        let new_z = dt * e(gravity[2]) + e(self.speed[2]);
        self.speed[2] = r(new_z);
        self.speed[0] = r(dt * e(gravity[0]) + e(self.speed[0]));
        self.speed[1] = r(new_y);
    }

    /// The per-core part of `simulate_single_sim_unit_psi`
    /// (`ivp_simulation_unit.cpp:718-746`): the matrix and position at the
    /// start of the PSI, the commit of pending changes, then the float bits
    /// of `1 - |speed|^2` the unit's verdict is formed from.
    pub fn psi_core_step(&mut self, now: IvpTime) -> u32 {
        let alpha = r(now.since(self.time_of_last_psi));
        let q = self.q_world_f_core_next_psi;
        q.set_matrix(&mut self.m_world_f_core_last_psi);
        for i in 0..3 {
            self.m_world_f_core_last_psi.vv[i] = r(e(alpha) * e(self.delta_world_f_core_psis[i])
                + e(self.pos_world_f_core_last_psi[i]));
        }
        self.commit_speed_changes();
        self.movement_state &= 0xFFFF_00FF;
        let s = self.speed;
        let quad = (e(s[0]) * e(s[0]) + e(s[1]) * e(s[1])) + e(s[2]) * e(s[2]);
        r(1.0 - quad).to_bits()
    }

    /// `update_time_of_last_motion` (`ivp_core.cpp:482`).
    pub fn update_time_of_last_motion(&mut self, now: IvpTime) {
        let split = now.split_sum();
        self.time_of_last_motion = split;
        self.time_of_last_long_motion = split;
    }

    /// `classify_motion_state` (`ivp_core.cpp:1670`): 1 moving, 2 recently
    /// moving, 3 idle long enough to sleep. `elapsed_limit` is the
    /// environment field at +0x88 (0.3).
    pub fn classify_motion_state(&mut self, now: IvpTime, elapsed_limit: f32) -> i32 {
        let short_pos = e(f32::from_bits(0x38d1_b717));
        let short_rot = e(f32::from_bits(0x37d1_b717));
        let long_pos = e(f32::from_bits(0x3c23_d70b));
        let long_rot = e(f32::from_bits(0x3d23_d70b));
        let long_elapsed = e(4.0f32);
        let radius = e(self.upper_limit_radius);
        // Both dot products sum w, z, y, then x (the x product's operand
        // order differs between the two in the listing; products commute).
        let rot_term = |a: &IvpQuat, b: &IvpQuat| {
            let dot = ((e(a.w) * e(b.w) + e(a.z) * e(b.z)) + e(a.y) * e(b.y)) + e(a.x) * e(b.x);
            (((1.0 - dot * dot) + (1.0 - dot * dot)) * radius) * radius
        };
        let pos = self.pos_world_f_core_last_psi;
        let quad = |last: [f32; 3]| {
            let dx = e(pos[0]) - e(last[0]);
            let dy = e(pos[1]) - e(last[1]);
            let dz = e(pos[2]) - e(last[2]);
            (dz * dz + dy * dy) + dx * dx
        };
        let mut short_moved = quad(self.pos_last_motion) > short_pos;
        if !short_moved {
            short_moved = rot_term(&self.q_last_motion, &self.q_world_f_core_next_psi) > short_rot;
        }
        if !short_moved {
            let elapsed = now.since(self.time_of_last_motion);
            return if elapsed > e(elapsed_limit) { 3 } else { 2 };
        }
        self.q_last_motion = self.q_world_f_core_next_psi;
        self.pos_last_motion = pos;
        let split = now.split_sum();
        self.time_of_last_motion = split;
        let mut long_moved = quad(self.pos_last_long_motion) > long_pos;
        if !long_moved {
            long_moved =
                rot_term(&self.q_last_long_motion, &self.q_world_f_core_last_psi) > long_rot;
        }
        if !long_moved {
            let elapsed = now.since(self.time_of_last_long_motion);
            return if elapsed > long_elapsed { 3 } else { 1 };
        }
        self.q_last_long_motion = self.q_world_f_core_last_psi;
        self.pos_last_long_motion = pos;
        self.time_of_last_long_motion = split;
        1
    }

    /// `IVP_StopCoreMovement`: frozen, every velocity and pending change
    /// zero.
    pub fn stop_movement(&mut self) {
        self.movement_state = (self.movement_state & 0xFFFF_FF08) | MOVEMENT_FROZEN;
        self.rot_speed_change = [0.0; 3];
        self.speed_change = [0.0; 3];
        self.rot_speed = [0.0; 3];
        self.speed = [0.0; 3];
        self.delta_world_f_core_psis = [0.0; 3];
    }

    /// `synchronize_simulation_time_snapshot` (`ivp_core.cpp:506`).
    pub fn synchronize_simulation_time_snapshot(&mut self, inv_delta_time: f32) {
        self.i_delta_time = inv_delta_time;
        self.q_world_f_core_next_psi = self.q_world_f_core_last_psi;
        self.speed = [0.0; 3];
        self.delta_world_f_core_psis = [0.0; 3];
        self.abs_omega = 0.0;
        self.current_speed = 0.0;
        self.max_surface_rot_speed = 0.0;
        self.rotation_axis_ws = [1.0, 0.0, 0.0];
    }

    /// `calc_interpolated_transform` (`ivp_core.cpp:1395`): the core's pose
    /// at `now` between this PSI and the next.
    #[must_use]
    pub fn calc_interpolated_transform(&self, now: IvpTime) -> IvpMatrix {
        if self.is_frozen() {
            return self.m_world_f_core_last_psi;
        }
        let delta_ext = now.since(self.time_of_last_psi);
        let delta = r(delta_ext);
        if !(delta_ext < 0.0 || delta_ext > 0.0) {
            return self.m_world_f_core_last_psi;
        }
        let alpha = r(e(delta) * e(self.i_delta_time));
        let mut q = IvpQuat::IDENTITY;
        q.set_interpolate_quat(
            &self.q_world_f_core_last_psi,
            &self.q_world_f_core_next_psi,
            alpha,
        );
        let mut out = self.m_world_f_core_last_psi;
        q.set_matrix(&mut out);
        for i in 0..3 {
            out.vv[i] = r(e(delta) * e(self.delta_world_f_core_psis[i])
                + e(self.pos_world_f_core_last_psi[i]));
        }
        out
    }

    /// `get_surface_speed` (`ivp_core.cpp:1923`): the world velocity of a
    /// world point moving with the core.
    #[must_use]
    pub fn surface_speed(&self, position_ws: [f32; 3]) -> [f32; 3] {
        let w = self.m_world_f_core_last_psi.vmult(self.rot_speed).map(e);
        let vv = self.m_world_f_core_last_psi.vv;
        let dx = e(position_ws[0]) - e(vv[0]);
        let dy = e(position_ws[1]) - e(vv[1]);
        let dz = e(position_ws[2]) - e(vv[2]);
        let cx = r(dz * w[1] - w[2] * dy);
        let cy = r(w[2] * dx - dz * w[0]);
        let cz = dy * w[0] - w[1] * dx;
        [
            r(e(cx) + e(self.speed[0])),
            r(e(cy) + e(self.speed[1])),
            r(cz + e(self.speed[2])),
        ]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn moving_core() -> Core {
        let mut core = Core {
            movement_state: 1,
            mass: 100.0,
            rot_inertia: [10.0, 20.0, 30.0],
            upper_limit_radius: 5.0,
            ..Core::default()
        };
        core.recompute_inverse_inertia();
        core
    }

    #[test]
    fn inverse_inertia_and_isotropy() {
        let mut core = moving_core();
        assert_eq!(core.inv_mass, 0.01);
        assert_eq!(core.inv_rot_inertia, [0.1, 0.05, r(1.0 / 30.0)]);
        assert_eq!(core.flags & FLAG_ISOTROPIC, 0);
        assert_eq!(core.d_angular_limit_factor, 0.1);
        core.set_rotation_inertia([4.0, 4.0, 4.0]);
        assert_eq!(core.flags & FLAG_ISOTROPIC, FLAG_ISOTROPIC);
        core.set_mass(200.0);
        assert_eq!(core.rot_inertia, [8.0, 8.0, 8.0]);
        assert_eq!(core.inv_mass, 0.005);
    }

    #[test]
    fn pushes_accumulate_and_commit() {
        let mut core = moving_core();
        core.push_core_linear([1.0, 0.0, -2.0], 50.0);
        assert_eq!(core.speed_change, [0.5, 0.0, -1.0]);
        core.push_core_angular_cs([1.0, 1.0, 0.0], 2.0);
        assert_eq!(core.rot_speed_change, [r(0.2), r(e(0.05f32) * 2.0), 0.0]);
        core.push_core_angular_ws([0.0, 0.0, 3.0], 1.0);
        assert!((core.rot_speed_change[2] - 0.1).abs() < 1e-7);
        core.commit_speed_changes();
        assert_eq!(core.speed, [0.5, 0.0, -1.0]);
        assert_eq!(core.speed_change, [0.0; 3]);
        assert_eq!(core.rot_speed[0], 0.2);
        assert!((core.rot_speed[2] - 0.1).abs() < 1e-7);
    }

    #[test]
    fn angular_push_rotates_into_core_space() {
        let mut core = moving_core();
        // A quarter turn about z: world x is core -y.
        let q = IvpQuat::new(
            0.0,
            0.0,
            std::f32::consts::FRAC_1_SQRT_2,
            std::f32::consts::FRAC_1_SQRT_2,
        );
        q.set_matrix(&mut core.m_world_f_core_last_psi);
        core.push_core_angular_ws([1.0, 0.0, 0.0], 1.0);
        assert!(core.rot_speed_change[0].abs() < 1e-6);
        assert!((core.rot_speed_change[1] + 0.05).abs() < 1e-6);
    }

    #[test]
    fn damping_linear_and_exponential() {
        let mut core = moving_core();
        core.speed = [10.0, 0.0, 0.0];
        core.rot_speed = [1.0, 1.0, 1.0];
        core.apply_velocity_damping(0.1, [1.0, 1.0, 1.0], 1.0);
        assert_eq!(core.speed[0], 9.0);
        assert_eq!(core.rot_speed[1], 0.9);
        core.apply_velocity_damping(1.0, [1.0, 1.0, 1.0], 1.0);
        assert_eq!(core.speed[0], r(e(9.0f32) * (-1.0f64).exp()));
        // Movement byte 2 adds 0.1 to every factor.
        let mut slow = moving_core();
        slow.movement_state = 2;
        slow.speed = [1.0, 0.0, 0.0];
        slow.speed_damp_factor = 0.4;
        slow.damp_velocities(0.5);
        assert_eq!(slow.speed[0], r(1.0 - 0.5 * e(0.4f32 + 0.1)));
    }

    #[test]
    fn gravity_controller_commits_and_clears_negative_zero() {
        let mut core = moving_core();
        core.speed = [-0.0, 1.0, 0.0];
        core.speed_change = [1.0, 0.0, 0.0];
        core.gravity_controller_step(0.1, [0.0; 3]);
        assert_eq!(core.speed, [1.0, 1.0, 0.0]);
        let mut neg = moving_core();
        neg.speed = [-0.0, 0.0, 0.0];
        neg.gravity_controller_step(0.1, [0.0; 3]);
        assert_eq!(neg.speed[0].to_bits(), 0);
        let mut skipped = moving_core();
        skipped.flags |= 0x100;
        skipped.speed_change = [1.0, 0.0, 0.0];
        skipped.gravity_controller_step(0.1, [0.0; 3]);
        assert_eq!(skipped.speed, [0.0; 3]);
    }

    #[test]
    fn core_step_extrapolates_and_reports_speed() {
        let mut core = moving_core();
        core.pos_world_f_core_last_psi = [1.0, 2.0, 3.0];
        core.delta_world_f_core_psis = [10.0, 0.0, 0.0];
        core.time_of_last_psi = IvpTime::new(0.0, 0.5);
        core.speed_change = [2.0, 0.0, 0.0];
        let bits = core.psi_core_step(IvpTime::new(0.0, 0.75));
        assert_eq!(core.m_world_f_core_last_psi.vv, [3.5, 2.0, 3.0]);
        assert_eq!(core.speed, [2.0, 0.0, 0.0]);
        assert!((f32::from_bits(bits)) < 0.0, "speed above one is negative");
        core.speed = [0.5, 0.0, 0.0];
        core.speed_change = [0.0; 3];
        let bits = core.psi_core_step(IvpTime::new(0.0, 0.75));
        assert_eq!(f32::from_bits(bits), 0.75);
    }

    #[test]
    fn motion_classification() {
        let mut core = moving_core();
        let now = IvpTime::new(1.0, 0.5);
        core.time_of_last_motion = IvpTime::new(1.0, 0.4);
        assert_eq!(core.classify_motion_state(now, 0.3), 2);
        core.time_of_last_motion = IvpTime::new(0.0, 0.5);
        assert_eq!(core.classify_motion_state(now, 0.3), 3);
        core.pos_world_f_core_last_psi = [1.0, 0.0, 0.0];
        assert_eq!(core.classify_motion_state(now, 0.3), 1);
        assert_eq!(core.pos_last_motion, [1.0, 0.0, 0.0]);
        assert_eq!(core.pos_last_long_motion, [1.0, 0.0, 0.0]);
        assert_eq!(core.time_of_last_motion, now);
        // A short move within the long threshold: idle for long once 4 s pass.
        core.pos_world_f_core_last_psi = [1.02, 0.0, 0.0];
        core.time_of_last_long_motion = IvpTime::new(0.0, 0.0);
        assert_eq!(core.classify_motion_state(IvpTime::new(5.0, 0.0), 0.3), 3);
    }

    #[test]
    fn interpolated_transform_and_surface_speed() {
        let mut core = moving_core();
        core.time_of_last_psi = IvpTime::new(0.0, 0.0);
        core.i_delta_time = 10.0;
        core.pos_world_f_core_last_psi = [0.0; 3];
        core.delta_world_f_core_psis = [1.0, 0.0, 0.0];
        let m = core.calc_interpolated_transform(IvpTime::new(0.0, 0.05));
        assert_eq!(m.vv[0], 0.05);
        core.movement_state = MOVEMENT_FROZEN;
        assert_eq!(
            core.calc_interpolated_transform(IvpTime::new(0.0, 0.05)).vv,
            [0.0; 3]
        );
        core.rot_speed = [0.0, 0.0, 1.0];
        core.speed = [1.0, 0.0, 0.0];
        assert_eq!(core.surface_speed([1.0, 0.0, 0.0]), [1.0, 1.0, 0.0]);
    }

    #[test]
    fn impulses_and_stop() {
        let mut core = moving_core();
        core.apply_impulse_at_point([0.0, 1.0, 0.0], [1.0, 0.0, 0.0], [1.0, 0.0, 0.0]);
        assert_eq!(core.speed_change, [0.01, 0.0, 0.0]);
        assert_eq!(core.rot_speed_change[2], r(-1.0 / 30.0));
        core.add_speed_change([1.0, 2.0, 3.0]);
        core.add_rot_speed_change([1.0, 0.0, 0.0]);
        assert_eq!(core.speed_change[1], 2.0);
        core.stop_movement();
        assert!(core.is_frozen());
        assert_eq!(core.speed_change, [0.0; 3]);
        core.synchronize_simulation_time_snapshot(4.0);
        assert_eq!(core.i_delta_time, 4.0);
        let mut t = core.clone();
        t.update_time_of_last_motion(IvpTime::new(3.0, 0.25));
        assert_eq!(t.time_of_last_long_motion, IvpTime::new(3.0, 0.25));
    }
}
