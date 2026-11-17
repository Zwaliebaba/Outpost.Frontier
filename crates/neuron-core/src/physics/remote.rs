//! `CRemotePhysicsSimulation` (`CppSource/Common/common_remotephysics.cpp:380-860`,
//! inline members in `include/common_server.h:963-1110`): dead reckoning
//! of a remote object's position and orientation from network samples.
//! The orientation is carried as two dead-reckoned basis axes.

use super::dead_reckoning::DeadReckonedVector;
use crate::math::{Matrix, Quaternion, Vector, matrix_to_quaternion, quaternion_to_matrix};

fn ext(v: f32) -> f64 {
    f64::from(v)
}

/// The two tolerances `CheckForSync` scales (`.data` 2.0 and 0.5).
const SYNC_POSITION_TOLERANCE: f64 = 2.0;
const SYNC_ORIENT_TOLERANCE: f64 = 0.5;

/// One remote object's dead reckoning (`CRemotePhysicsSimulation`).
#[derive(Clone, Debug, PartialEq)]
pub struct RemotePhysicsSimulation {
    /// +0x08: the position.
    pub position_drv: DeadReckonedVector,
    /// +0xd8: the orientation's first column (world X axis of the object).
    pub orient_axis_x_drv: DeadReckonedVector,
    /// +0x1a8: the orientation's second column.
    pub orient_axis_y_drv: DeadReckonedVector,
    /// +0x278: the velocity `Doit` publishes.
    pub velocity: Vector,
    /// +0x284: the velocity at the last update, blended from.
    pub blend_from_velocity: Vector,
    /// +0x290: the fit's velocity at the end of the window, blended to.
    pub blend_to_velocity: Vector,
    /// +0x29c: the orientation `Doit` rebuilt. The constructor does not set
    /// it; the port starts at the identity.
    pub orientation: Quaternion,
    /// +0x2ac: the position dead reckoner's projected position.
    pub projected_position: Vector,
    /// +0x2b8: samples since the last reset.
    pub sample_count: u32,
    /// +0x2c0: time since the last update.
    pub simulation_time: f64,
    /// +0x2c8: the blend window.
    pub converge_window: f64,
    /// +0x2d8: the forced-sync back-off (5 s, doubled on each time-out).
    pub sync_backoff: f64,
    /// +0x2e0: `SetLogging`; only `tracef` reads it, and nothing calls that.
    pub logging: bool,
}

impl Default for RemotePhysicsSimulation {
    fn default() -> Self {
        let mut sim = Self {
            position_drv: DeadReckonedVector::default(),
            orient_axis_x_drv: DeadReckonedVector::default(),
            orient_axis_y_drv: DeadReckonedVector::default(),
            velocity: Vector::ZERO,
            blend_from_velocity: Vector::ZERO,
            blend_to_velocity: Vector::ZERO,
            orientation: Quaternion::IDENTITY,
            projected_position: Vector::ZERO,
            sample_count: 0,
            simulation_time: 0.0,
            converge_window: 0.0,
            sync_backoff: 5.0,
            logging: false,
        };
        sim.reset();
        sim
    }
}

impl RemotePhysicsSimulation {
    /// `Reset`: the trailing fields only; the dead reckoners and the
    /// orientation stay.
    pub fn reset(&mut self) {
        self.velocity = Vector::ZERO;
        self.blend_from_velocity = Vector::ZERO;
        self.blend_to_velocity = Vector::ZERO;
        self.projected_position = Vector::ZERO;
        self.sample_count = 0;
        self.converge_window = 0.0;
        self.simulation_time = 0.0;
        self.sync_backoff = 5.0;
    }

    /// `RemoteUpdate`: a network sample of position and orientation.
    pub fn remote_update(&mut self, position: Vector, orientation: Quaternion, timestamp: f64) {
        let basis = quaternion_to_matrix(&orientation);
        self.position_drv.add_sample(position, timestamp);
        let axis_x = Vector::new(basis.m[0][0], basis.m[1][0], basis.m[2][0]);
        self.orient_axis_x_drv.add_sample(axis_x, timestamp);
        let axis_y = Vector::new(basis.m[0][1], basis.m[1][1], basis.m[2][1]);
        self.orient_axis_y_drv.add_sample(axis_y, timestamp);
        self.sample_count = self.sample_count.wrapping_add(1);
        self.blend_from_velocity = self.velocity;
        let window_d = self.position_drv.converge_window;
        let window_f = window_d as f32;
        let accel = self.position_drv.acceleration;
        let velocity = self.position_drv.velocity;
        let half = Vector::new(accel.x * 0.5, accel.y * 0.5, accel.z * 0.5);
        let scaled_x = (ext(half.x) * window_d) as f32;
        let scaled_y = (ext(half.y) * window_d) as f32;
        let scaled_z = (ext(half.z) * window_d) as f32;
        let product_y = scaled_y * window_f;
        let product_z = scaled_z * window_f;
        self.blend_to_velocity = Vector::new(
            (ext(scaled_x) * ext(window_f) + ext(velocity.x)) as f32,
            product_y + velocity.y,
            product_z + velocity.z,
        );
        self.converge_window = self.position_drv.converge_window;
        self.simulation_time = 0.0;
    }

    /// `Doit`: advance the three dead reckoners, rebuild the orientation
    /// from the two axes and publish position and velocity.
    pub fn doit(&mut self, dt: f64) {
        self.simulation_time += dt;
        self.position_drv.doit(dt);
        self.orient_axis_x_drv.doit(dt);
        self.orient_axis_y_drv.doit(dt);
        self.orient_axis_x_drv.normalize_projected_position();
        self.orient_axis_x_drv.normalize_tracked_position();
        self.orient_axis_y_drv.normalize_projected_position();
        self.orient_axis_y_drv.normalize_tracked_position();
        let u = self.orient_axis_x_drv.projected_position;
        let w = self.orient_axis_y_drv.projected_position;
        let dot = (ext(w.z) * ext(u.z) + ext(w.y) * ext(u.y)) + ext(w.x) * ext(u.x);
        let proj = Vector::new(
            (ext(u.x) * dot) as f32,
            (ext(u.y) * dot) as f32,
            (ext(u.z) * dot) as f32,
        );
        let perp = Vector::new(w.x - proj.x, w.y - proj.y, w.z - proj.z);
        let inv_perp_len = 1.0
            / ((ext(perp.x) * ext(perp.x) + ext(perp.y) * ext(perp.y)) + ext(perp.z) * ext(perp.z))
                .sqrt();
        let v = Vector::new(
            (ext(perp.x) * inv_perp_len) as f32,
            (ext(perp.y) * inv_perp_len) as f32,
            (ext(perp.z) * inv_perp_len) as f32,
        );
        let cross = Vector::new(
            (ext(v.z) * ext(u.y) - ext(u.z) * ext(v.y)) as f32,
            (ext(u.z) * ext(v.x) - ext(v.z) * ext(u.x)) as f32,
            (ext(v.y) * ext(u.x) - ext(u.y) * ext(v.x)) as f32,
        );
        let basis = Matrix::from_rows([
            [u.x, v.x, cross.x],
            [u.y, v.y, cross.y],
            [u.z, v.z, cross.z],
        ]);
        self.orientation = matrix_to_quaternion(&basis);
        self.projected_position = self.position_drv.projected_position;
        self.velocity = if self.simulation_time < self.converge_window {
            let t = (self.simulation_time / self.converge_window) as f32;
            let (from, to) = (self.blend_from_velocity, self.blend_to_velocity);
            let lerp = |a: f32, b: f32| ((ext(b) - ext(a)) * ext(t) + ext(a)) as f32;
            Vector::new(lerp(from.x, to.x), lerp(from.y, to.y), lerp(from.z, to.z))
        } else {
            self.position_drv.project_velocity(self.simulation_time)
        };
    }

    /// `CheckForSync`: whether the caller should force a resync, from two
    /// independent position samples and an orientation.
    pub fn check_for_sync(
        &mut self,
        param1: Vector,
        param2: Vector,
        orientation: Quaternion,
    ) -> bool {
        if self.simulation_time > self.sync_backoff {
            self.sync_backoff += self.sync_backoff;
            return true;
        }
        let mut blend_scale = 1.0;
        let dx = ext(param1.x) - ext(param2.x);
        let dy = ext(param1.y) - ext(param2.y);
        let dz = ext(param1.z) - ext(param2.z);
        let sample_dist = ((dz * dz + dx * dx) + dy * dy).sqrt();
        if sample_dist > 500.0 {
            blend_scale = sample_dist * 0.002;
            if blend_scale > 10.0 {
                blend_scale = 10.0;
            }
        }
        let tracked = self.position_drv.tracked_position;
        let ex = param2.x - tracked.x;
        let ey = param2.y - tracked.y;
        let ez = param2.z - tracked.z;
        let pos_error = ((ext(ey) * ext(ey) + ext(ez) * ext(ez)) + ext(ex) * ext(ex)).sqrt();
        let mut force_sync = pos_error > SYNC_POSITION_TOLERANCE * blend_scale;
        if !force_sync {
            let basis = quaternion_to_matrix(&orientation);
            let threshold = SYNC_ORIENT_TOLERANCE * blend_scale;
            let ax = self.orient_axis_x_drv.tracked_position;
            let xx = basis.m[0][0] - ax.x;
            let xy = basis.m[1][0] - ax.y;
            let xz = basis.m[2][0] - ax.z;
            let axis_x_error = ((ext(xy) * ext(xy) + ext(xz) * ext(xz)) + ext(xx) * ext(xx)).sqrt();
            if axis_x_error > threshold {
                force_sync = true;
            } else {
                let ay = self.orient_axis_y_drv.tracked_position;
                let yx = basis.m[0][1] - ay.x;
                let yy = ext(basis.m[1][1]) - ext(ay.y);
                let yz = ext(basis.m[2][1]) - ext(ay.z);
                let axis_y_error = ((yz * yz + yy * yy) + ext(yx) * ext(yx)).sqrt();
                if axis_y_error > threshold {
                    force_sync = true;
                }
            }
        }
        if force_sync {
            self.sync_backoff = 5.0;
        }
        force_sync
    }

    /// `CalcPositionError`.
    #[must_use]
    pub fn calc_position_error(&self, point: Vector) -> f64 {
        self.position_drv.calc_error(point)
    }

    /// `GetLatestUpdate`.
    #[must_use]
    pub fn latest_update(&self) -> Vector {
        self.position_drv.samples[0]
    }

    /// `GetSample`.
    #[must_use]
    pub fn sample(&self, index: usize) -> Vector {
        self.position_drv.samples[index]
    }

    /// `GetSampleCount`.
    #[must_use]
    pub fn sample_count(&self) -> u32 {
        self.sample_count
    }

    /// `GetSimulationTime`.
    #[must_use]
    pub fn simulation_time(&self) -> f64 {
        self.simulation_time
    }

    /// `SetLogging`.
    pub fn set_logging(&mut self, enable: bool) {
        self.logging = enable;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn straight_flight_is_followed() {
        let mut sim = RemotePhysicsSimulation::default();
        let q = Quaternion::IDENTITY;
        sim.remote_update(Vector::new(0.0, 0.0, 0.0), q, 0.0);
        sim.remote_update(Vector::new(10.0, 0.0, 0.0), q, 1.0);
        assert_eq!(sim.sample_count(), 2);
        assert_eq!(sim.latest_update(), Vector::new(10.0, 0.0, 0.0));
        sim.doit(0.5);
        assert_eq!(sim.projected_position, Vector::new(15.0, 0.0, 0.0));
        assert_eq!(sim.velocity, Vector::new(10.0, 0.0, 0.0));
        assert!((sim.orientation.w - 1.0).abs() < 1e-6);
        assert_eq!(sim.simulation_time(), 0.5);
        assert!(sim.calc_position_error(Vector::new(15.0, 0.0, 0.0)) < 1e-6);
        assert!(!sim.check_for_sync(Vector::new(15.0, 0.0, 0.0), Vector::new(15.0, 0.0, 0.0), q));
        assert!(sim.check_for_sync(Vector::new(15.0, 0.0, 0.0), Vector::new(25.0, 0.0, 0.0), q));
    }

    #[test]
    fn velocity_blends_inside_the_window() {
        let mut sim = RemotePhysicsSimulation::default();
        let q = Quaternion::IDENTITY;
        sim.remote_update(Vector::ZERO, q, 0.0);
        sim.remote_update(Vector::new(1.0, 0.0, 0.0), q, 0.1);
        sim.doit(0.05);
        // From 0 to the fit's 10 m/s over the 0.1 s window.
        assert!((sim.velocity.x - 5.0).abs() < 1e-4, "{:?}", sim.velocity);
    }

    #[test]
    fn sync_backoff_doubles_on_timeout() {
        let mut sim = RemotePhysicsSimulation {
            simulation_time: 6.0,
            ..RemotePhysicsSimulation::default()
        };
        assert!(sim.check_for_sync(Vector::ZERO, Vector::ZERO, Quaternion::IDENTITY));
        assert_eq!(sim.sync_backoff, 10.0);
        sim.simulation_time = 0.0;
        // Far-apart samples loosen the tolerance: 1000 m gives scale 2.
        let far = Vector::new(1000.0, 0.0, 0.0);
        sim.position_drv.tracked_position = Vector::new(3.0, 0.0, 0.0);
        assert!(!sim.check_for_sync(far, Vector::ZERO, Quaternion::IDENTITY));
        // A turned orientation fails the axis test.
        sim.orient_axis_x_drv.tracked_position = Vector::new(1.0, 0.0, 0.0);
        sim.orient_axis_y_drv.tracked_position = Vector::new(0.0, 1.0, 0.0);
        let turned = Quaternion::new(
            std::f32::consts::FRAC_1_SQRT_2,
            0.0,
            0.0,
            std::f32::consts::FRAC_1_SQRT_2,
        );
        assert!(sim.check_for_sync(Vector::ZERO, Vector::new(3.0, 0.0, 0.0), turned));
        assert_eq!(sim.sync_backoff, 5.0);
        sim.set_logging(true);
        sim.reset();
        assert!(sim.logging);
        assert_eq!(sim.sample(0), Vector::ZERO);
    }
}
