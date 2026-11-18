//! The fields (`particlefx_fields.cpp`): each class's update, the
//! `BeginField` that brings the field into the space of the appearance it
//! acts on, and `ApplyFieldToVel` on one particle.

use std::rc::Rc;

use neuron_core::math::{Transform, Vector, inverse_transform, rotate, transform_xform};

use super::ParticleFx;
use super::emitter::inv_sqrt_guess;
use super::node::{Behaviour, FieldState, NodeId};
use super::props::{FieldProps, PropsData};
use super::ramp::round;

/// `kDustKillAgeBits`: the age a particle outside the dust field gets.
const DUST_KILL_AGE: f32 = f32::MAX;

/// `LerpVel3(vel, target, alpha)` with the x target unrounded: one rounding
/// per axis, at the store.
fn lerp_vel(vel: &mut [f32], target_x: f64, target_y: f32, target_z: f32, alpha: f32) {
    let a = f64::from(alpha);
    vel[0] = round((target_x - f64::from(vel[0])) * a + f64::from(vel[0]));
    vel[1] = round((f64::from(target_y) - f64::from(vel[1])) * a + f64::from(vel[1]));
    vel[2] = round((f64::from(target_z) - f64::from(vel[2])) * a + f64::from(vel[2]));
}

/// The float sum `a + b` per axis.
fn add3(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
}

fn sub3(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

/// The z, y, x order dot product the collide field computes on the x87
/// stack.
fn dot_zyx(a: [f32; 3], n: [f32; 3]) -> f64 {
    (f64::from(a[2]) * f64::from(n[2]) + f64::from(a[1]) * f64::from(n[1]))
        + f64::from(a[0]) * f64::from(n[0])
}

fn blend(dt: f32, approach: f32) -> f32 {
    let b = round(f64::from(dt) * f64::from(approach) * 4.0);
    if b > 1.0 { 1.0 } else { b }
}

impl ParticleFx {
    fn field_mut(&mut self, id: NodeId) -> &mut FieldState {
        match &mut self.n_mut(id).behaviour {
            Behaviour::Field(f) => f,
            _ => unreachable!("not a field"),
        }
    }

    /// The field's update (`Pad5`).
    pub(crate) fn field_update(&mut self, id: NodeId, dt: f32) {
        let props_rc = Rc::clone(&self.n(id).props);
        let PropsData::Field(props) = &props_rc.data else {
            return;
        };
        if let FieldProps::Dust { .. } = props {
            self.field_mut(id).update_dt = dt;
        }
        self.tick_lifetime(id, dt);
        let node = self.n(id);
        let (outer, age) = (node.outer_time, node.age);
        let state = self.field_mut(id);
        match props {
            FieldProps::Gravity { gravity } => {
                state.magnitude = round(gravity.eval(outer, age) * f64::from(dt));
            }
            FieldProps::Air {
                approach,
                magnitude,
            } => {
                state.approach = round(approach.eval(outer, age));
                state.magnitude = round(magnitude.eval(outer, age));
                state.blend = blend(dt, state.approach);
            }
            FieldProps::Radial {
                magnitude,
                approach,
                radius,
                ..
            } => {
                let radius = radius.eval(outer, age);
                state.dist_scale = if radius < 0.001 {
                    round(1.0 / f64::from(0.001_f32))
                } else {
                    round(1.0 / radius)
                };
                state.approach = round(approach.eval(outer, age));
                state.magnitude = round(magnitude.eval(outer, age));
                state.blend = blend(dt, state.approach);
            }
            FieldProps::Turbulence {
                approach,
                magnitude,
            } => {
                let approach = approach.eval(outer, age);
                state.approach = round(approach);
                state.magnitude = round(magnitude.eval(outer, age));
                let b = f64::from(dt) * f64::from(state.approach) * 4.0;
                state.blend = if b > 1.0 { 1.0 } else { round(b) };
            }
            FieldProps::Collide { reflectivity } => {
                state.reflect = round(reflectivity.eval(outer, age));
                state.update_dt = dt;
            }
            FieldProps::Dust { radius } => {
                let r = radius.eval(outer, age);
                state.radius = round(r);
                state.radius_sq = round(r * r);
            }
            FieldProps::Beam => {}
        }
    }

    /// `BeginField(appearance)`.
    pub(crate) fn field_begin(&mut self, id: NodeId, appearance: NodeId) {
        let props_rc = Rc::clone(&self.n(id).props);
        let PropsData::Field(props) = &props_rc.data else {
            return;
        };
        if matches!(props, FieldProps::Turbulence { .. } | FieldProps::Beam) {
            return;
        }
        let x = Transform::from_words(self.relative_transform(id, appearance));
        let state = self.field_mut(id);
        match props {
            FieldProps::Gravity { .. } => {
                state.cache = rotate(&x, Vector::new(0.0, -state.magnitude, 0.0)).to_words();
            }
            FieldProps::Air { .. } => {
                state.cache = rotate(&x, Vector::new(0.0, state.magnitude, 0.0)).to_words();
            }
            FieldProps::Radial { .. } => {
                state.cache = transform_xform(&x, Vector::ZERO).to_words();
            }
            FieldProps::Collide { .. } => {
                state.begin_xform = x.to_words();
                let n = rotate(&x, Vector::new(0.0, 1.0, 0.0)).to_words();
                let length_sq = f64::from(n[0]) * f64::from(n[0])
                    + f64::from(n[1]) * f64::from(n[1])
                    + f64::from(n[2]) * f64::from(n[2]);
                let inv = 1.0 / length_sq.sqrt();
                state.normal = n.map(|c| round(inv * f64::from(c)));
                state.corner = transform_xform(&x, Vector::new(1.0, 0.0, 1.0)).to_words();
                state.scaled_normal = state.normal.map(|c| c * 5.0);
            }
            FieldProps::Dust { .. } => state.begin_xform = x.to_words(),
            FieldProps::Turbulence { .. } | FieldProps::Beam => {}
        }
    }

    /// `ApplyFieldToVel(pos_vel, age)` on one particle's position and
    /// velocity.
    #[allow(clippy::too_many_lines)]
    pub(crate) fn field_apply(&mut self, id: NodeId, pos_vel: &mut [f32; 6], age: &mut f32) {
        let node = self.n(id);
        let props_rc = Rc::clone(&node.props);
        let PropsData::Field(props) = &props_rc.data else {
            return;
        };
        let outer = node.outer_time;
        let world = node.world;
        let Behaviour::Field(state) = &node.behaviour else {
            return;
        };
        let state = (**state).clone();
        let (pos, vel) = pos_vel.split_at_mut(3);
        match props {
            FieldProps::Gravity { .. } => {
                for axis in 0..3 {
                    vel[axis] += state.cache[axis];
                }
            }
            FieldProps::Air { .. } => {
                lerp_vel(
                    vel,
                    f64::from(state.cache[0]),
                    state.cache[1],
                    state.cache[2],
                    state.blend,
                );
            }
            FieldProps::Radial { attenuation, .. } => {
                let dx = state.cache[0] - pos[0];
                let dy = state.cache[1] - pos[1];
                let dz_ext = f64::from(state.cache[2]) - f64::from(pos[2]);
                let dz = round(dz_ext);
                let dist_ext = (dz_ext * f64::from(dz)
                    + f64::from(dx) * f64::from(dx)
                    + f64::from(dy) * f64::from(dy))
                .sqrt()
                    * f64::from(state.dist_scale);
                let dist = if dist_ext > 1.0 { 1.0 } else { round(dist_ext) };
                let scale = attenuation.eval(outer, dist) * -f64::from(state.magnitude);
                let target_x = f64::from(dx) * scale;
                let target_y = round(f64::from(dy) * scale);
                let target_z = round(f64::from(dz) * scale);
                lerp_vel(vel, target_x, target_y, target_z, state.blend);
            }
            FieldProps::Turbulence { .. } => {
                let r1 = round((self.rand.next_unit() - 0.5) * 2.0);
                let r2 = round((self.rand.next_unit() - 0.5) * 2.0);
                let r3 = (self.rand.next_unit() - 0.5) * 2.0;
                let length_sq =
                    round(r3 * r3 + f64::from(r2) * f64::from(r2) + f64::from(r1) * f64::from(r1));
                let guess = f64::from(inv_sqrt_guess(length_sq));
                let scale = (1.5 - guess * guess * f64::from(length_sq) * 0.5) * guess;
                let dir = [
                    round(f64::from(r2) * scale),
                    round(f64::from(r1) * scale),
                    round(r3 * scale),
                ];
                let push = self.rand.next_unit() * f64::from(state.magnitude);
                let target_x = f64::from(dir[0]) * push;
                let target_y = round(f64::from(dir[1]) * push);
                let target_z = round(f64::from(dir[2]) * push);
                lerp_vel(vel, target_x, target_y, target_z, state.blend);
            }
            FieldProps::Collide { .. } => {
                let n = state.normal;
                let sn = state.scaled_normal;
                let c = state.corner;
                let p = [pos[0], pos[1], pos[2]];
                let t0 = add3(p, sn);
                let t1 = sub3(t0, c);
                let d0_ext = dot_zyx(t1, n);
                if d0_ext < 0.0 {
                    return;
                }
                let d0 = round(d0_ext);
                let s = state.update_dt;
                let v1 = [s * vel[0], s * vel[1], s * vel[2]];
                if dot_zyx(v1, n) < 0.0 {
                    return;
                }
                let v2 = sub3(v1, sn);
                let v3 = add3(v2, t0);
                let v4 = sub3(v3, c);
                let d1 = dot_zyx(v4, n);
                if d1 > 0.0 {
                    return;
                }
                let t = round(f64::from(d0) / (f64::from(d0) - d1));
                let v5 = [v2[0] * t, v2[1] * t, v2[2] * t];
                let hit = add3(v5, t0);
                pos.copy_from_slice(&hit);
                let speed = round(
                    (f64::from(vel[0]) * f64::from(vel[0])
                        + f64::from(vel[1]) * f64::from(vel[1])
                        + f64::from(vel[2]) * f64::from(vel[2]))
                    .sqrt(),
                );
                if !(f64::from(speed) > 0.001) {
                    return;
                }
                let inv_speed = 1.0 / f64::from(speed);
                let dir = [0, 1, 2].map(|i| round(inv_speed * f64::from(vel[i])));
                let dot = dot_zyx(dir, n);
                let k = round(dot + dot);
                let reflected = sub3([k * n[0], k * n[1], k * n[2]], dir);
                let vel_scale = -(f64::from(speed) * f64::from(state.reflect));
                for axis in 0..3 {
                    vel[axis] = round(vel_scale * f64::from(reflected[axis]));
                }
                let residual = (1.0 - f64::from(t)) * f64::from(s);
                let w = [0, 1, 2].map(|i| round(residual * f64::from(vel[i])));
                for axis in 0..3 {
                    pos[axis] -= w[axis];
                }
            }
            FieldProps::Dust { .. } => {
                let local = inverse_transform(
                    &Transform::from_words(world),
                    Vector::new(pos[0], pos[1], pos[2]),
                )
                .to_words();
                let dist_sq_ext = (f64::from(local[1]) * f64::from(local[1])
                    + f64::from(local[0]) * f64::from(local[0]))
                    + f64::from(local[2]) * f64::from(local[2]);
                let dist_sq = round(dist_sq_ext);
                if dist_sq_ext > f64::from(state.radius_sq) {
                    *age = DUST_KILL_AGE;
                    return;
                }
                let radius = f64::from(state.radius);
                let height = 1.0 - (radius - f64::from(local[2])) / (radius + radius);
                let mut fade = 1.0;
                if local[2] < 0.0 {
                    let band = round(f64::from(state.radius_sq) * f64::from(0.8_f32));
                    if dist_sq > band {
                        fade = 1.0
                            - (f64::from(dist_sq) - f64::from(band))
                                / (f64::from(state.radius_sq) - f64::from(band));
                    }
                }
                *age = round(height * fade * 10.0);
            }
            FieldProps::Beam => {}
        }
    }
}
