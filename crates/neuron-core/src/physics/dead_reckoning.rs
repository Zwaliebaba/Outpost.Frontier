//! `CDeadReckonedVector` (`CppSource/Common/common_deadreckonedvector.cpp`,
//! inline members in `include/common_cobject.h:1144-1233`): a quadratic
//! through the three newest network samples, and a second quadratic that
//! blends the rendered position into it over a short window.
//!
//! On the 53-bit model: every `Vector` component the reference stores is
//! an `f32`, coefficients, lengths and times stay `f64`.

use crate::math::Vector;

fn ext(v: f32) -> f64 {
    f64::from(v)
}

fn vec_sub(a: Vector, b: Vector) -> Vector {
    Vector::new(a.x - b.x, a.y - b.y, a.z - b.z)
}

fn vec_add(a: Vector, b: Vector) -> Vector {
    Vector::new(a.x + b.x, a.y + b.y, a.z + b.z)
}

fn scale(v: Vector, s: f64) -> Vector {
    Vector::new(
        (ext(v.x) * s) as f32,
        (ext(v.y) * s) as f32,
        (ext(v.z) * s) as f32,
    )
}

fn half(v: Vector) -> Vector {
    Vector::new(v.x * 0.5, v.y * 0.5, v.z * 0.5)
}

fn length_xzy(v: Vector) -> f64 {
    let (x, y, z) = (ext(v.x), ext(v.y), ext(v.z));
    ((x * x + z * z) + y * y).sqrt()
}

/// One dead-reckoned vector (`CDeadReckonedVector`, 0xd0 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct DeadReckonedVector {
    /// +0x08: the samples, newest first.
    pub samples: [Vector; 3],
    /// +0x30: their time stamps.
    pub timestamps: [f64; 3],
    /// +0x48: the newest sample as of the last `AddSample`.
    pub base_position: Vector,
    /// +0x54: the projected position as of the last `AddSample`.
    pub blend_base_position: Vector,
    /// +0x60: the rendered position.
    pub projected_position: Vector,
    /// +0x6c: the fitted quadratic's position, kept current by `Doit`.
    pub tracked_position: Vector,
    /// +0x78: the fit's velocity at the newest sample.
    pub velocity: Vector,
    /// +0x84: the blend's velocity term.
    pub velocity2: Vector,
    /// +0x90: the last velocity `Doit` evaluated.
    pub converged_velocity: Vector,
    /// +0x9c: the fit's acceleration.
    pub acceleration: Vector,
    /// +0xa8: the blend's acceleration.
    pub acceleration2: Vector,
    /// +0xb4: samples added since the last reset.
    pub sample_count: i32,
    /// +0xb8: the blend window (`ConvergeTime`).
    pub converge_window: f64,
    /// +0xc0: time since the last sample.
    pub elapsed_time: f64,
    /// +0xc8: the cap on the window for a straight-line fit (0.25).
    pub max_corrective_time: f64,
}

impl Default for DeadReckonedVector {
    /// The constructor: samples and stamps zero (the reference leaves them
    /// indeterminate), then [`DeadReckonedVector::reset`].
    fn default() -> Self {
        let mut v = Self {
            samples: [Vector::ZERO; 3],
            timestamps: [0.0; 3],
            base_position: Vector::ZERO,
            blend_base_position: Vector::ZERO,
            projected_position: Vector::ZERO,
            tracked_position: Vector::ZERO,
            velocity: Vector::ZERO,
            velocity2: Vector::ZERO,
            converged_velocity: Vector::ZERO,
            acceleration: Vector::ZERO,
            acceleration2: Vector::ZERO,
            sample_count: 0,
            converge_window: 0.0,
            elapsed_time: 0.0,
            max_corrective_time: 0.25,
        };
        v.reset();
        v
    }
}

impl DeadReckonedVector {
    /// `Reset`: everything but the samples and their stamps.
    pub fn reset(&mut self) {
        self.base_position = Vector::ZERO;
        self.blend_base_position = Vector::ZERO;
        self.projected_position = Vector::ZERO;
        self.tracked_position = Vector::ZERO;
        self.velocity = Vector::ZERO;
        self.velocity2 = Vector::ZERO;
        self.converged_velocity = Vector::ZERO;
        self.acceleration = Vector::ZERO;
        self.acceleration2 = Vector::ZERO;
        self.sample_count = 0;
        self.converge_window = 0.0;
        self.elapsed_time = 0.0;
        self.max_corrective_time = 0.25;
    }

    /// `AddSample`.
    pub fn add_sample(&mut self, pos: Vector, timestamp: f64) {
        if self.sample_count == 0 {
            self.samples = [pos; 3];
            self.timestamps = [timestamp; 3];
        } else {
            if self.sample_count == 1 {
                self.samples[1] = self.samples[0];
                self.samples[0] = pos;
            } else {
                self.samples[2] = self.samples[1];
                self.samples[1] = self.samples[0];
                self.samples[0] = pos;
                self.timestamps[2] = self.timestamps[1];
            }
            self.timestamps[1] = self.timestamps[0];
            self.timestamps[0] = timestamp;
        }
        let dt01 = timestamp - self.timestamps[1];
        self.sample_count = self.sample_count.wrapping_add(1);
        let mut near_straight = false;
        let dt12 = self.timestamps[1] - self.timestamps[2];
        self.base_position = self.samples[0];
        let span = dt12 + dt01;
        let mut prev_elapsed = self.elapsed_time - dt01;
        if prev_elapsed < 0.0 {
            prev_elapsed = 0.0;
        }
        let span_t = prev_elapsed + dt01;
        self.converge_window = dt01;
        let [p0, p1, p2] = self.samples;
        let mut d1 = vec_sub(p1, p0);
        let mut d2 = vec_sub(p1, p2);
        let len1 = length_xzy(d1);
        let len2 = length_xzy(d2);
        let mut quadratic_fit = false;
        if dt01 == 0.0 || dt12 == 0.0 || len1 == 0.0 || len2 == 0.0 {
            near_straight = true;
        } else {
            d1 = scale(d1, 1.0 / len1);
            d2 = scale(d2, 1.0 / len2);
            let dot = (ext(d2.x) * ext(d1.x) + ext(d2.z) * ext(d1.z)) + ext(d2.y) * ext(d1.y);
            if !(dot > 0.0) {
                if dot < -0.996_194 {
                    near_straight = true;
                }
                let inv_span_dt12 = 1.0 / (span * dt12);
                let p2_term = scale(p2, inv_span_dt12 * dt01);
                let inv_dt01 = 1.0 / dt01;
                let p1_term = scale(p1, 1.0 / dt12 + inv_dt01);
                let p0_term = scale(p0, 1.0 / span + inv_dt01);
                let outer = vec_sub(p2_term, p1_term);
                self.velocity = vec_add(p0_term, outer);
                let p2_acc = scale(p2, inv_span_dt12 + inv_span_dt12);
                let p1_acc = scale(p1, 2.0 / (dt12 * dt01));
                let p0_acc = scale(p0, 2.0 / (span * dt01));
                let outer_acc = vec_sub(p2_acc, p1_acc);
                self.acceleration = vec_add(p0_acc, outer_acc);
                quadratic_fit = true;
            }
        }
        if !quadratic_fit {
            self.velocity = if dt01 == 0.0 {
                Vector::ZERO
            } else {
                scale(vec_sub(p0, p1), 1.0 / dt01)
            };
            self.acceleration = Vector::ZERO;
            if self.converge_window > self.max_corrective_time {
                self.converge_window = self.max_corrective_time;
            }
        }
        if self.converge_window > 0.0 {
            let old = self.projected_position;
            self.blend_base_position = old;
            let window = self.converge_window;
            let acc_term = scale(scale(half(self.acceleration), window), window);
            let vel_term = scale(self.velocity, window);
            let anchored = vec_add(vel_term, self.base_position);
            let target = vec_add(acc_term, anchored);
            let offset = vec_sub(target, old);
            if near_straight {
                self.velocity2 = scale(offset, 1.0 / window);
                self.acceleration2 = Vector::ZERO;
            } else {
                let k1 = 1.0 / span_t - 1.0 / window;
                let span_window = span_t + window;
                let k2 = span_t / (span_window * window);
                let k3 = window / (span_t * span_window);
                let p1k3 = scale(p1, k3);
                let old_k1 = scale(old, k1);
                let target_k2 = scale(target, k2);
                let mixed = vec_sub(old_k1, p1k3);
                self.velocity2 = vec_add(target_k2, mixed);

                let l3 = 2.0 / (span_t * (span_t + window));
                let p1x_l3 = (ext(p1.x) * l3) as f32;
                let p1y_l3 = (ext(p1.y) * l3) as f32;
                let p1z_l3 = ext(p1.z) * l3;
                let l1 = 2.0 / (span_t * window);
                let oldx_l1 = ext(old.x) * l1;
                let oldy_l1 = (ext(old.y) * l1) as f32;
                let oldz_l1 = (ext(old.z) * l1) as f32;
                let l2 = 2.0 / (span_window * window);
                let targetx_l2 = (ext(target.x) * l2) as f32;
                let targety_l2 = (ext(target.y) * l2) as f32;
                let targetz_l2 = (ext(target.z) * l2) as f32;
                let diff_x = (ext(p1x_l3) - oldx_l1) as f32;
                let diff_y = p1y_l3 - oldy_l1;
                let diff_z = p1z_l3 - ext(oldz_l1);
                self.acceleration2 = Vector::new(
                    diff_x + targetx_l2,
                    diff_y + targety_l2,
                    (diff_z + ext(targetz_l2)) as f32,
                );
            }
        }
        self.elapsed_time = 0.0;
    }

    /// `ConvergeTime`.
    #[must_use]
    pub fn converge_time(&self) -> f64 {
        self.converge_window
    }

    /// `Doit`: advances by `dt` and evaluates the blend or the fit.
    pub fn doit(&mut self, dt: f64) {
        let elapsed = dt + self.elapsed_time;
        self.elapsed_time = elapsed;
        if self.sample_count == 0 {
            let t = elapsed as f32;
            let c = self.converged_velocity;
            let drift = Vector::new(c.x * t, c.y * t, c.z * t);
            self.projected_position = vec_add(self.samples[0], drift);
            return;
        }
        let elapsed2 = elapsed * elapsed;
        let blending = elapsed < self.converge_window;
        let elapsed2f = elapsed2 as f32;
        let acc_term = scale(half(self.acceleration), elapsed2);
        let vel_term = scale(self.velocity, elapsed);
        let anchored = vec_add(self.base_position, vel_term);
        self.tracked_position = vec_add(acc_term, anchored);
        if !blending {
            self.projected_position = self.tracked_position;
            let h = half(self.acceleration);
            let acc_term = Vector::new(h.x * elapsed2f, h.y * elapsed2f, h.z * elapsed2f);
            self.converged_velocity = vec_add(acc_term, self.velocity);
            return;
        }
        let h2 = half(self.acceleration2);
        let acc_term2 = Vector::new(h2.x * elapsed2f, h2.y * elapsed2f, h2.z * elapsed2f);
        let vel_term2 = scale(self.velocity2, self.elapsed_time);
        let anchored2 = vec_add(vel_term2, self.blend_base_position);
        self.projected_position = vec_add(acc_term2, anchored2);
        self.converged_velocity = vec_add(acc_term2, self.velocity2);
    }

    /// `GetProjectedPosition`.
    #[must_use]
    pub fn projected_position(&self) -> Vector {
        self.projected_position
    }

    /// `GetSample`. The reference does not check the index; the port
    /// panics outside 0..3.
    #[must_use]
    pub fn sample(&self, index: usize) -> Vector {
        self.samples[index]
    }

    /// `CalcError`: the distance from the tracked position.
    #[must_use]
    pub fn calc_error(&self, pos: Vector) -> f64 {
        let t = self.tracked_position;
        let dx = ext(pos.x) - ext(t.x);
        let dy = ext(pos.y) - ext(t.y);
        let dz = ext(pos.z) - ext(t.z);
        ((dx * dx + dz * dz) + dy * dy).sqrt()
    }

    fn normalize(v: &mut Vector) {
        let (x, y, z) = (ext(v.x), ext(v.y), ext(v.z));
        let len = ((x * x + y * y) + z * z).sqrt();
        if len > ext(1e-5f32) {
            let inv = 1.0 / len;
            *v = Vector::new((inv * x) as f32, (inv * y) as f32, (inv * z) as f32);
        }
    }

    /// `NormalizeProjectedPosition`: unit length when longer than 1e-5.
    pub fn normalize_projected_position(&mut self) {
        Self::normalize(&mut self.projected_position);
    }

    /// `NormalizeTrackedPosition`.
    pub fn normalize_tracked_position(&mut self) {
        Self::normalize(&mut self.tracked_position);
    }

    /// `ProjectVelocity(dt)`: `(0.5 a) dt^2 + v`, with the reference's
    /// mixed roundings per axis.
    #[must_use]
    pub fn project_velocity(&self, dt: f64) -> Vector {
        let h = half(self.acceleration);
        let ax = (ext(h.x) * dt) as f32;
        let ay = (ext(h.y) * dt) as f32;
        let px = (ext(ax) * dt) as f32;
        let py = (ext(ay) * dt) as f32;
        let pz = (dt * ext(h.z)) * dt;
        Vector::new(
            px + self.velocity.x,
            py + self.velocity.y,
            (pz + ext(self.velocity.z)) as f32,
        )
    }

    /// `SetMaxCorrectiveConvergeTime`.
    pub fn set_max_corrective_converge_time(&mut self, time: f64) {
        self.max_corrective_time = time;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn first_samples_converge_on_a_straight_line() {
        let mut v = DeadReckonedVector::default();
        v.add_sample(Vector::new(0.0, 0.0, 0.0), 1.0);
        assert_eq!(v.sample_count, 1);
        assert_eq!(v.converge_time(), 0.0);
        v.add_sample(Vector::new(10.0, 0.0, 0.0), 2.0);
        // Two samples: the stamps make dt12 zero, a straight fit capped at 0.25.
        assert_eq!(v.velocity, Vector::new(10.0, 0.0, 0.0));
        assert_eq!(v.converge_time(), 0.25);
        v.doit(0.25);
        assert_eq!(v.projected_position, Vector::new(12.5, 0.0, 0.0));
        assert_eq!(v.tracked_position, Vector::new(12.5, 0.0, 0.0));
        v.doit(0.25);
        assert_eq!(v.projected_position, Vector::new(15.0, 0.0, 0.0));
        assert_eq!(v.converged_velocity, Vector::new(10.0, 0.0, 0.0));
        assert_eq!(v.calc_error(Vector::new(15.0, 3.0, 4.0)), 5.0);
    }

    #[test]
    fn three_samples_fit_a_parabola() {
        let mut v = DeadReckonedVector::default();
        // x = t^2 sampled at t = 0, 1, 2 (stamps 10, 11, 12).
        v.add_sample(Vector::new(0.0, 0.0, 0.0), 10.0);
        v.add_sample(Vector::new(1.0, 0.0, 0.0), 11.0);
        v.add_sample(Vector::new(4.0, 0.0, 0.0), 12.0);
        // The direction reverses (dot <= 0), so the quadratic fit runs.
        assert!((v.velocity.x - 4.0).abs() < 1e-5, "{:?}", v.velocity);
        assert!((v.acceleration.x - 2.0).abs() < 1e-5);
        assert_eq!(v.converge_time(), 1.0);
        let p = v.project_velocity(1.0);
        assert!((p.x - 5.0).abs() < 1e-5);
        v.doit(2.0);
        assert!((v.projected_position.x - 16.0).abs() < 1e-4);
    }

    #[test]
    fn no_samples_drift_and_reset() {
        let mut v = DeadReckonedVector::default();
        v.samples[0] = Vector::new(1.0, 1.0, 1.0);
        v.converged_velocity = Vector::new(2.0, 0.0, 0.0);
        v.doit(0.5);
        assert_eq!(v.projected_position, Vector::new(2.0, 1.0, 1.0));
        v.set_max_corrective_converge_time(1.0);
        v.reset();
        assert_eq!(v.max_corrective_time, 0.25);
        assert_eq!(v.sample(0), Vector::new(1.0, 1.0, 1.0));
        assert_eq!(v.projected_position(), Vector::ZERO);
    }

    #[test]
    fn normalisation_threshold() {
        let mut v = DeadReckonedVector {
            projected_position: Vector::new(3.0, 0.0, 4.0),
            tracked_position: Vector::new(1e-6, 0.0, 0.0),
            ..DeadReckonedVector::default()
        };
        v.normalize_projected_position();
        v.normalize_tracked_position();
        assert_eq!(v.projected_position, Vector::new(0.6, 0.0, 0.8));
        assert_eq!(v.tracked_position, Vector::new(1e-6, 0.0, 0.0));
    }
}
