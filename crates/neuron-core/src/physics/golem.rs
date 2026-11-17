//! The golem controllers: `IVP_Controller_Golem` with its two leaves,
//! `FLControllerGolem` (animated and welded parts) and
//! `CRemotePhysicsController` (remote correction, `PhySys::RCBegin`)
//! (`CppSource/Common/common_controllergolem.cpp`,
//! `common_remotephysics.cpp:1-300`). Both run at controller priority 500,
//! after the game's force controllers and the standard controller.

use super::ivp::core::Core;
use super::ivp::math::{IvpQuat, IvpTime, double_asin_vector, e, r};
use super::ivp::psi::EventSim;
use super::remote::RemotePhysicsSimulation;
use crate::math::{Quaternion, Vector};

/// The golems' controller priority (`IVP_Controller_Motion`, 500).
pub const GOLEM_PRIORITY: i32 = 500;

/// `FLControllerGolemParms` (`FLControllerGolemParms_Init`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GolemParms {
    /// +0x00.
    pub force_factor: f32,
    /// +0x04.
    pub damp_factor: f32,
    /// +0x08.
    pub torque_factor: f32,
    /// +0x0c; written, never read by the servo.
    pub angular_damp_factor: f32,
    /// +0x10.
    pub max_translation_force: [f32; 3],
    /// +0x20, broadcast to all three axes.
    pub max_torque: f32,
    /// +0x24.
    pub max_delta_position: f32,
    /// +0x28.
    pub delta_orientation: f32,
}

impl Default for GolemParms {
    fn default() -> Self {
        Self {
            force_factor: 0.8,
            damp_factor: 1.0,
            torque_factor: 0.8,
            angular_damp_factor: 1.0,
            max_translation_force: [1.0e6; 3],
            max_torque: 1.0e6,
            max_delta_position: 10.0,
            delta_orientation: 1.570_796_4,
        }
    }
}

impl GolemParms {
    /// The parameters `AttachGolemController` builds from the `GOLEM_*`
    /// constants for a free part.
    #[must_use]
    pub fn for_part(c: &super::PhySysConstants) -> Self {
        Self {
            force_factor: c.golem_force_factor,
            damp_factor: c.golem_damp_factor,
            torque_factor: c.golem_torque_factor,
            angular_damp_factor: c.golem_angular_damp_factor,
            max_translation_force: c.golem_max_translation_force.to_words(),
            max_torque: c.golem_max_torque,
            max_delta_position: c.golem_max_delta_position,
            delta_orientation: c.golem_delta_orientation,
        }
    }

    /// The parameters `PhySys::RCBegin` builds.
    #[must_use]
    pub fn for_remote_correction(c: &super::PhySysConstants) -> Self {
        Self {
            force_factor: 1.0,
            damp_factor: 1.0,
            torque_factor: 1.0,
            angular_damp_factor: 1.0,
            max_translation_force: [1.0e12; 3],
            max_torque: 1.0e12,
            max_delta_position: c.rc_max_delta_position,
            delta_orientation: c.rc_max_delta_orientation,
        }
    }
}

/// What a golem asks of its body after its step.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum GolemOutcome {
    /// The servo edited the core's velocities.
    Servoed,
    /// `beam_object_to_new_position(quat, position, 1)`: put the object at
    /// this world pose.
    Beam {
        /// The object's world rotation.
        rotation: IvpQuat,
        /// The object's world position.
        position: [f32; 3],
    },
}

/// `IVP_Controller_Golem`: a servo that drags its body to an extrapolated
/// target pose, teleporting when it falls too far behind.
#[derive(Clone, Debug, PartialEq)]
pub struct Golem {
    /// +0x04: the target position extrapolated to this PSI.
    pub current_target_position: [f32; 3],
    /// +0x14: the target rotation for this PSI.
    pub current_target_quat: IvpQuat,
    /// +0x24.
    pub max_translation_force: [f32; 3],
    /// +0x34.
    pub max_torque: [f32; 3],
    /// +0x44.
    pub force_factor: f32,
    /// +0x48.
    pub damp_factor: f32,
    /// +0x4c.
    pub torque_factor: f32,
    /// +0x50.
    pub angular_damp_factor: f32,
    /// +0x60: decaying sum of squared position errors (never read back).
    pub smoothed_position_error_sq: f32,
    /// +0x64: `set_target_position`'s stamp.
    pub target_position_time: IvpTime,
    /// +0x6c: `set_target_quaternion`'s stamp.
    pub target_quat_time: IvpTime,
    /// +0x74.
    pub inv_rot_delta_time: f32,
    /// +0x78.
    pub has_target_rot_quat: bool,
    /// +0x7c.
    pub target_quat: IvpQuat,
    /// +0x8c.
    pub target_rot_quat: IvpQuat,
    /// +0x9c.
    pub target_position: [f32; 3],
    /// +0xac.
    pub target_speed: [f32; 3],
    /// +0xbc.
    pub max_delta_position: f32,
    /// +0xc0.
    pub delta_orientation: f32,
}

impl Golem {
    /// The constructor; the stamps start at zero (the reference leaves
    /// them unwritten until the first `set_target_*`).
    #[must_use]
    pub fn new(parms: &GolemParms) -> Self {
        Self {
            current_target_position: [0.0; 3],
            current_target_quat: IvpQuat::IDENTITY,
            max_translation_force: parms.max_translation_force,
            max_torque: [parms.max_torque; 3],
            force_factor: parms.force_factor,
            damp_factor: parms.damp_factor,
            torque_factor: parms.torque_factor,
            angular_damp_factor: parms.angular_damp_factor,
            smoothed_position_error_sq: 0.0,
            target_position_time: IvpTime::default(),
            target_quat_time: IvpTime::default(),
            inv_rot_delta_time: 1.0,
            has_target_rot_quat: false,
            target_quat: IvpQuat::IDENTITY,
            target_rot_quat: IvpQuat::IDENTITY,
            target_position: [0.0; 3],
            target_speed: [0.0; 3],
            max_delta_position: parms.max_delta_position,
            delta_orientation: parms.delta_orientation,
        }
    }

    /// `set_target_position`.
    pub fn set_target_position(&mut self, position: Vector, speed: Vector, time: IvpTime) {
        self.target_position_time = time;
        self.target_position = position.to_words();
        self.target_speed = speed.to_words();
    }

    /// `set_target_quaternion`: the rotation (IVP order), optionally a
    /// second rotation to slerp toward over `delta_time`.
    pub fn set_target_quaternion(
        &mut self,
        quat: IvpQuat,
        time: IvpTime,
        rot_quat: Option<IvpQuat>,
        delta_time: f32,
    ) {
        self.target_quat = quat;
        if let Some(rot) = rot_quat {
            self.target_rot_quat = rot;
            self.has_target_rot_quat = true;
            self.inv_rot_delta_time = 1.0 / delta_time;
        } else {
            self.has_target_rot_quat = false;
        }
        self.target_quat_time = time;
    }

    /// `reset_time`: moves the position stamp back by `delta`.
    pub fn reset_time(&mut self, delta: IvpTime) {
        self.target_position_time.base -= delta.base;
        let mut offset = e(self.target_position_time.offset) - e(delta.offset);
        self.target_position_time.offset = r(offset);
        while offset > 1.0 {
            self.target_position_time.base += 1.0;
            offset = e(self.target_position_time.offset) - 1.0;
            self.target_position_time.offset = r(offset);
        }
        if !(self.target_position_time.offset >= 0.0) {
            loop {
                self.target_position_time.base -= 1.0;
                offset = e(self.target_position_time.offset) + 1.0;
                self.target_position_time.offset = r(offset);
                if offset >= 0.0 {
                    break;
                }
            }
        }
    }

    fn beam(&self) -> GolemOutcome {
        GolemOutcome::Beam {
            rotation: self.current_target_quat,
            position: self.current_target_position,
        }
    }

    /// The position error with the core-to-object shift taken out.
    fn position_delta(&self, core: &Core, shift_core_f_object: [f32; 3]) -> [f32; 3] {
        let vv = core.m_world_f_core_last_psi.vv;
        let mut delta = [
            self.current_target_position[0] - vv[0],
            self.current_target_position[1] - vv[1],
            self.current_target_position[2] - vv[2],
        ];
        let shift = core.m_world_f_core_last_psi.vmult(shift_core_f_object);
        delta = super::ivp::math::sub(delta, shift);
        delta
    }

    /// `IVP_Controller_Golem::do_simulation_controller`: extrapolate the
    /// target, then servo the core's velocities or ask for a beam.
    pub fn simulate(
        &mut self,
        core: &mut Core,
        event: EventSim,
        now: IvpTime,
        shift_core_f_object: [f32; 3],
    ) -> GolemOutcome {
        let dt = event.delta_time;
        let idt = event.i_delta_time;
        let pos_elapsed = now.since(self.target_position_time);
        for i in [2, 0, 1] {
            self.current_target_position[i] =
                r(pos_elapsed * e(self.target_speed[i]) + e(self.target_position[i]));
        }
        if self.has_target_rot_quat {
            let quat_elapsed = now.since(self.target_quat_time);
            let alpha = r(quat_elapsed * e(self.inv_rot_delta_time));
            let (from, to) = (self.target_quat, self.target_rot_quat);
            self.current_target_quat
                .set_interpolate_quat(&from, &to, alpha);
        } else {
            self.current_target_quat = self.target_quat;
        }

        let mut delta = self.position_delta(core, shift_core_f_object);
        let quad =
            (e(delta[1]) * e(delta[1]) + e(delta[2]) * e(delta[2])) + e(delta[0]) * e(delta[0]);
        self.smoothed_position_error_sq = r(e(self.smoothed_position_error_sq) * e(0.9f32) + quad);
        if quad > e(self.max_delta_position) * e(self.max_delta_position) {
            return self.beam();
        }

        delta = delta.map(|c| c * self.force_factor);
        let damp_dt = self.damp_factor * dt;
        for i in 0..3 {
            delta[i] = r(e(damp_dt) * e(self.target_speed[i]) + e(delta[i]));
        }
        let neg_damp_dt = -(self.damp_factor * dt);
        for i in 0..3 {
            delta[i] = r(e(neg_damp_dt) * e(core.speed[i]) + e(delta[i]));
        }
        let idt_sq = e(idt) * e(idt);
        for axis in (0..3).rev() {
            let force = (e(core.mass) * e(delta[axis]) * idt_sq).abs();
            if force >= e(self.max_translation_force[axis]) {
                let mut clamped =
                    e(core.inv_mass) * e(dt) * e(dt) * e(self.max_translation_force[axis]);
                if !(delta[axis] >= 0.0) {
                    clamped = -clamped;
                }
                delta[axis] = r(clamped);
            }
        }
        for i in [2, 0, 1] {
            core.speed[i] = r(e(delta[i]) * e(idt) + e(core.speed[i]));
        }

        let inv_target = self.current_target_quat.invert_unit();
        let relative = IvpQuat::mult(&inv_target, &core.q_world_f_core_next_psi);
        let mut rot_error = double_asin_vector([relative.x, relative.y, relative.z]);
        let rot_quad = (e(rot_error[2]) * e(rot_error[2]) + e(rot_error[1]) * e(rot_error[1]))
            + e(rot_error[0]) * e(rot_error[0]);
        if !(e(self.delta_orientation) * e(self.delta_orientation) >= rot_quad) {
            return self.beam();
        }
        let q = core.q_world_f_core_next_psi;
        let t = self.current_target_quat;
        let quat_dot = ((e(q.w) * e(t.w) + e(q.z) * e(t.z)) + e(q.y) * e(t.y)) + e(t.x) * e(q.x);
        let mut rot_scale = e(idt) * e(self.torque_factor);
        if quat_dot >= 0.0 {
            rot_scale = -rot_scale;
        }
        for i in [2, 0, 1] {
            rot_error[i] = r(e(rot_error[i]) * rot_scale);
        }
        let neg_damp = -e(self.damp_factor);
        let mut rot_delta = [0.0f32; 3];
        for i in 0..3 {
            rot_delta[i] = r(neg_damp * e(core.rot_speed[i]) + e(rot_error[i]));
        }
        for axis in (0..3).rev() {
            let torque = e(idt) * e(core.rot_inertia[axis]) * e(rot_delta[axis]);
            if torque.abs() >= e(self.max_torque[axis]) {
                let mut clamped = e(core.inv_rot_inertia[axis]) * e(dt) * e(self.max_torque[axis]);
                if !(torque >= 0.0) {
                    clamped = -clamped;
                }
                rot_delta[axis] = r(clamped);
            }
        }
        for i in 0..3 {
            core.rot_speed[i] += rot_delta[i];
        }
        GolemOutcome::Servoed
    }
}

/// `CRemotePhysicsController`: a golem whose target is the dead reckoning
/// of network samples, and which beams its body onto it every PSI.
#[derive(Clone, Debug, PartialEq)]
pub struct RemotePhysicsController {
    /// The golem base.
    pub golem: Golem,
    /// The dead reckoning (`CRemotePhysicsSimulation` at +0xd0).
    pub simulation: RemotePhysicsSimulation,
}

impl RemotePhysicsController {
    /// `PhySys::RCBegin`'s construction: the golem from `parms`, the
    /// simulation seeded with one sample at time 0 that does not count,
    /// and `velocity` as its published velocity. The angular velocity
    /// argument of `RCBegin` is not used by the reference.
    #[must_use]
    pub fn new(
        parms: &GolemParms,
        position: Vector,
        orientation: Quaternion,
        velocity: Vector,
    ) -> Self {
        let mut controller = Self {
            golem: Golem::new(parms),
            simulation: RemotePhysicsSimulation::default(),
        };
        controller
            .simulation
            .remote_update(position, orientation, 0.0);
        controller.simulation.velocity = velocity;
        controller.simulation.sample_count = 0;
        controller
    }

    /// `RemoteUpdate` (`PhySys::RCSet`). The caller wakes the core.
    pub fn remote_update(&mut self, position: Vector, orientation: Quaternion, timestamp: f64) {
        self.simulation
            .remote_update(position, orientation, timestamp);
    }

    /// `do_simulation_controller`: dead reckoning one PSI forward, the
    /// golem's targets from it, and a beam onto them.
    pub fn simulate(
        &mut self,
        core: &Core,
        event: EventSim,
        shift_core_f_object: [f32; 3],
    ) -> GolemOutcome {
        self.simulation.doit(f64::from(event.delta_time));
        let p = self.simulation.projected_position;
        self.golem.current_target_position = p.to_words();
        self.golem.target_speed = self.simulation.velocity.to_words();
        self.golem.current_target_quat = IvpQuat::from_math(self.simulation.orientation);
        let delta = self.golem.position_delta(core, shift_core_f_object);
        let quad =
            (e(delta[1]) * e(delta[1]) + e(delta[2]) * e(delta[2])) + e(delta[0]) * e(delta[0]);
        self.golem.smoothed_position_error_sq =
            r(quad + e(self.golem.smoothed_position_error_sq) * e(0.9f32));
        self.golem.beam()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn core() -> Core {
        let mut core = Core {
            movement_state: 1,
            mass: 10.0,
            rot_inertia: [2.0, 2.0, 2.0],
            upper_limit_radius: 1.0,
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
    fn servo_pulls_toward_the_target() {
        let mut golem = Golem::new(&GolemParms::default());
        golem.set_target_position(Vector::new(1.0, 0.0, 0.0), Vector::ZERO, IvpTime::default());
        golem.set_target_quaternion(IvpQuat::IDENTITY, IvpTime::default(), None, 0.0);
        let mut c = core();
        let outcome = golem.simulate(&mut c, EVENT, IvpTime::new(0.0, 0.1), [0.0; 3]);
        assert_eq!(outcome, GolemOutcome::Servoed);
        // delta 0.8 m over 0.1 s.
        assert!((c.speed[0] - 8.0).abs() < 1e-5);
        assert_eq!(golem.smoothed_position_error_sq, 1.0);
    }

    #[test]
    fn far_targets_are_beamed() {
        let mut golem = Golem::new(&GolemParms::default());
        golem.set_target_position(
            Vector::new(100.0, 0.0, 0.0),
            Vector::new(1.0, 0.0, 0.0),
            IvpTime::default(),
        );
        let mut c = core();
        let outcome = golem.simulate(&mut c, EVENT, IvpTime::new(1.0, 0.0), [0.0; 3]);
        assert_eq!(
            outcome,
            GolemOutcome::Beam {
                rotation: IvpQuat::IDENTITY,
                position: [101.0, 0.0, 0.0]
            }
        );
        let turned = IvpQuat::rot_vector([0.0, 0.0, 3.0], 1.0);
        let mut golem = Golem::new(&GolemParms::default());
        golem.set_target_quaternion(turned, IvpTime::default(), None, 0.0);
        let outcome = golem.simulate(&mut core(), EVENT, IvpTime::default(), [0.0; 3]);
        assert!(matches!(outcome, GolemOutcome::Beam { .. }));
    }

    #[test]
    fn force_clamp_and_rotation_servo() {
        let parms = GolemParms {
            max_translation_force: [100.0; 3],
            ..GolemParms::default()
        };
        let mut golem = Golem::new(&parms);
        golem.set_target_position(
            Vector::new(-5.0, 0.0, 0.0),
            Vector::ZERO,
            IvpTime::default(),
        );
        let small = IvpQuat::rot_vector([0.0, 0.0, 0.2], 1.0);
        golem.set_target_quaternion(small, IvpTime::default(), Some(small), 1.0);
        let mut c = core();
        golem.simulate(&mut c, EVENT, IvpTime::default(), [0.0; 3]);
        // Clamp: inv_mass * dt^2 * 100 = 0.1 m, so 1 m/s.
        assert!((c.speed[0] + 1.0).abs() < 1e-5, "{:?}", c.speed);
        assert!(c.rot_speed[2] > 0.0);
    }

    #[test]
    fn reset_time_rebases_the_position_stamp() {
        let mut golem = Golem::new(&GolemParms::default());
        golem.target_position_time = IvpTime::new(5.0, 0.25);
        golem.reset_time(IvpTime::new(2.0, 0.5));
        assert_eq!(golem.target_position_time, IvpTime::new(2.0, 0.75));
        golem.target_position_time = IvpTime::new(1.0, 0.5);
        golem.reset_time(IvpTime::new(0.0, -1.0));
        assert_eq!(golem.target_position_time, IvpTime::new(2.0, 0.5));
    }

    #[test]
    fn remote_controller_beams_onto_the_dead_reckoning() {
        let c = crate::physics::PhySysConstants::default();
        let mut rc = RemotePhysicsController::new(
            &GolemParms::for_remote_correction(&c),
            Vector::new(1.0, 2.0, 3.0),
            Quaternion::IDENTITY,
            Vector::new(4.0, 0.0, 0.0),
        );
        assert_eq!(rc.simulation.sample_count(), 0);
        assert_eq!(rc.simulation.velocity, Vector::new(4.0, 0.0, 0.0));
        rc.remote_update(Vector::new(2.0, 2.0, 3.0), Quaternion::IDENTITY, 1.0);
        let outcome = rc.simulate(&core(), EVENT, [0.0; 3]);
        match outcome {
            GolemOutcome::Beam { position, .. } => {
                // Blending from the never-advanced projection (0) toward 2.25.
                assert!((position[0] - 0.9).abs() < 1e-5, "{position:?}");
            }
            GolemOutcome::Servoed => panic!("remote correction always beams"),
        }
        assert_eq!(GolemParms::for_part(&c).max_delta_position, 10.0);
    }
}
