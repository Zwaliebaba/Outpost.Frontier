//! The emitters (`particlefx_emitters.cpp`): the shape update of the cone,
//! cube and sphere emitters, the shared emission (`UpdateEmitter`), the
//! shape samplers with their random draws in the reference's order, and
//! the hand-over to the appearance (`SubmitParticle`).

use std::rc::Rc;

use neuron_core::math::{Transform, Vector, inverse_transform, rotate, transform_xform};
use neuron_core::num::trunc_to_i32;

use super::ParticleFx;
use super::node::{FLAG_DEAD, NodeId, ShapeState};
use super::props::{PropsData, ShapeProps};
use super::ramp::round;
use super::services::ParticleServices;

/// `kTwoPi`, `6.2831855f`.
const TWO_PI: f32 = 6.283_185_5;
/// `kDegToRad`, `0.017453292f`.
const DEG_TO_RAD: f32 = 0.017_453_292;

fn xyz(v: [f32; 3]) -> Vector {
    Vector::new(v[0], v[1], v[2])
}

/// `FastNormalize3`: one Newton step of the `0x5f3759df` inverse square
/// root on the length squared rounded to a float.
pub(crate) fn fast_normalize(v: &mut [f32; 3]) {
    let length_sq = round(
        f64::from(v[0]) * f64::from(v[0])
            + f64::from(v[1]) * f64::from(v[1])
            + f64::from(v[2]) * f64::from(v[2]),
    );
    let guess = inv_sqrt_guess(length_sq);
    let scale =
        (1.5 - f64::from(guess) * f64::from(guess) * f64::from(length_sq) * 0.5) * f64::from(guess);
    for c in v.iter_mut() {
        *c = round(scale * f64::from(*c));
    }
}

/// The `0x5f3759df` guess: the bits minus half the float's bits, an
/// arithmetic shift.
pub(crate) fn inv_sqrt_guess(value: f32) -> f32 {
    f32::from_bits(
        0x5F37_59DF_i32
            .wrapping_sub(value.to_bits().cast_signed() >> 1)
            .cast_unsigned(),
    )
}

/// `SpreadDirection(azimuth, spread)`: the sine of the spread is shared by
/// x and z.
fn spread_direction(azimuth: f32, spread: f64) -> [f32; 3] {
    let sin_spread = spread.sin();
    let az = f64::from(azimuth);
    [
        round(az.cos() * sin_spread),
        round(spread.cos()),
        round(-(az.sin() * sin_spread)),
    ]
}

impl ParticleFx {
    /// The emitter's update (`Pad5`): the shape parameters at `dt + age`,
    /// then `UpdateEmitter`.
    pub(crate) fn emitter_update(&mut self, id: NodeId, dt: f32, svc: &mut dyn ParticleServices) {
        let node = self.n(id);
        let props_rc = Rc::clone(&node.props);
        let PropsData::Emitter(props) = &props_rc.data else {
            return;
        };
        let outer = node.outer_time;
        let time = round(f64::from(dt) + f64::from(node.age));
        let k = f64::from(DEG_TO_RAD);
        let mut shape = ShapeState::default();
        match &props.shape {
            ShapeProps::Cone {
                min_radius,
                max_radius,
                min_spread,
                max_spread,
            } => {
                shape.min_radius = round(min_radius.eval(outer, time));
                shape.radius_range =
                    round(max_radius.eval(outer, time) - f64::from(shape.min_radius));
                shape.min_spread = round(min_spread.eval(outer, time) * k);
                shape.spread_range =
                    round(max_spread.eval(outer, time) * k - f64::from(shape.min_spread));
            }
            ShapeProps::Cube {
                width,
                height,
                depth,
                min_spread,
                max_spread,
            } => {
                shape.width = round(width.eval(outer, time));
                shape.height = round(height.eval(outer, time));
                shape.depth = round(depth.eval(outer, time));
                shape.min_spread = round(min_spread.eval(outer, time) * k);
                shape.spread_range =
                    round(max_spread.eval(outer, time) * k - f64::from(shape.min_spread));
            }
            ShapeProps::Sphere {
                min_radius,
                max_radius,
            } => {
                shape.min_radius = round(min_radius.eval(outer, time));
                shape.radius_range =
                    round(max_radius.eval(outer, time) - f64::from(shape.min_radius));
            }
        }
        if let Some(state) = self.emitter_mut(id) {
            // Each shape keeps only its own members up to date; the others
            // stay at their construction value of 0.
            state.shape = shape;
        }
        self.emit(id, dt, svc);
    }

    fn emitter_mut(&mut self, id: NodeId) -> Option<&mut super::node::EmitterState> {
        match &mut self.n_mut(id).behaviour {
            super::node::Behaviour::Emitter(e) => Some(e),
            _ => None,
        }
    }

    /// `UpdateEmitter(dt)`.
    #[allow(clippy::too_many_lines)]
    fn emit(&mut self, id: NodeId, dt: f32, svc: &mut dyn ParticleServices) {
        self.tick_lifetime(id, dt);
        let Some(appearance) = self.emitter_mut(id).and_then(|e| e.appearance) else {
            return;
        };
        if self.n(appearance).is_dead() || self.n(id).flags & FLAG_DEAD != 0 {
            if let Some(e) = self.emitter_mut(id) {
                e.appearance = None;
            }
            self.release(appearance, svc);
            return;
        }
        let xform = self.relative_transform(id, appearance);
        let node = self.n(id);
        let props_rc = Rc::clone(&node.props);
        let PropsData::Emitter(props) = &props_rc.data else {
            return;
        };
        let (outer, age, lod_input, world, parent) = (
            node.outer_time,
            node.age,
            node.lod_input,
            node.world,
            node.parent,
        );
        let pressure = round(props.pressure.eval(outer, age));
        let init_life_span = round(props.init_life_span.eval(outer, age));
        let frequency = round(props.frequency.eval(outer, age));

        let state = self.emitter_mut(id).unwrap_or_else(|| unreachable!());
        state.xform = xform;
        state.pressure = pressure;
        state.init_life_span = init_life_span;
        let elapsed_ext = f64::from(age) - f64::from(state.last_emit);
        let elapsed = round(elapsed_ext);
        let mut emit_count = trunc_to_i32(elapsed_ext * f64::from(frequency));
        if state.reset_history {
            state.reset_history = false;
            emit_count = emit_count
                .cast_unsigned()
                .wrapping_add(props.initial_particles)
                .cast_signed();
            state.prev_pos = [world[9], world[10], world[11]];
        }
        // The keyed bursts of the first child of Emitter_EmitCount.
        if let Some(keys) = props.emit_count.child(0) {
            for index in 0..keys.key_count() {
                let Some((time, value)) = keys.key(index) else {
                    continue;
                };
                if time < state.last_emit {
                    continue;
                }
                if time > age {
                    break;
                }
                emit_count = trunc_to_i32(f64::from(emit_count) + f64::from(value));
            }
        }
        let fraction_ext = props.lod_curve.eval(outer, lod_input) * f64::from(emit_count)
            + f64::from(state.fraction);
        state.fraction = round(fraction_ext);
        let mut emit_now = trunc_to_i32(fraction_ext);
        #[allow(clippy::cast_precision_loss)]
        let emit_now_f = emit_now as f32;
        state.fraction = round(f64::from(state.fraction) - f64::from(emit_now_f));
        if emit_now == 0 {
            if frequency == 0.0 {
                state.last_emit = age;
            }
            return;
        }
        let delta =
            inverse_transform(&Transform::from_words(world), xyz(state.prev_pos)).to_words();
        let inv_count_ext = 1.0 / f64::from(emit_now_f);
        let mut full_delta = delta;
        let mut velocity = [0.0_f32; 3];
        state.prev_pos = [world[9], world[10], world[11]];
        let inv_count = round(inv_count_ext);
        let mut step = delta.map(|d| round(f64::from(d) * inv_count_ext));
        if elapsed > 0.0 {
            let neg_inv = 1.0 / -f64::from(elapsed);
            let scaled = delta.map(|d| round(f64::from(d) * neg_inv));
            let approach = props.velocity_approach.eval(outer, age);
            velocity = scaled.map(|s| round(f64::from(s) * approach));
        }
        let mut age_offset = elapsed;
        let time_step = round(f64::from(inv_count) * f64::from(elapsed));
        if self.n(appearance).parent == parent {
            step = [0.0; 3];
            full_delta = [0.0; 3];
        }
        let max_particles = round(props.max_particles.eval(outer, age));
        let live = self.n(appearance).appearance().map_or(0, |a| a.live);
        let room = f64::from(max_particles) - f64::from(live.cast_signed());
        if room < f64::from(emit_now_f) {
            emit_now = trunc_to_i32(room);
        }
        self.appearance_begin_fields(appearance);
        if emit_now > 0 {
            loop {
                age_offset = round(f64::from(age_offset) - f64::from(time_step));
                for axis in 0..3 {
                    full_delta[axis] = round(f64::from(full_delta[axis]) - f64::from(step[axis]));
                }
                self.emit_particle(id, &props.shape, age_offset, full_delta, velocity);
                emit_now -= 1;
                if emit_now == 0 {
                    break;
                }
            }
        }
        // Pad48: every field's end is a no-op.
        if let Some(state) = self.emitter_mut(id) {
            state.last_emit = round(f64::from(elapsed) + f64::from(state.last_emit));
        }
    }

    /// The shape's `EmitParticle(age_offset, pos_offset, vel_in)`.
    fn emit_particle(
        &mut self,
        id: NodeId,
        shape_props: &ShapeProps,
        age_offset: f32,
        pos_offset: [f32; 3],
        vel_in: [f32; 3],
    ) {
        let Some(state) = self.n(id).emitter() else {
            return;
        };
        let shape = state.shape;
        let pressure = state.pressure;
        let (dir, pos) = match shape_props {
            ShapeProps::Cone { .. } => {
                let azimuth = round(self.rand.next_unit() * f64::from(TWO_PI));
                let spread = self.rand.next_unit() * f64::from(shape.spread_range)
                    + f64::from(shape.min_spread);
                let dir = spread_direction(azimuth, spread);
                let radius = self.rand.next_unit() * f64::from(shape.radius_range)
                    + f64::from(shape.min_radius);
                (dir, dir.map(|d| round(f64::from(d) * radius)))
            }
            ShapeProps::Cube { .. } => {
                let azimuth = round(self.rand.next_unit() * f64::from(TWO_PI));
                let spread = self.rand.next_unit() * f64::from(shape.spread_range)
                    + f64::from(shape.min_spread);
                let dir = spread_direction(azimuth, spread);
                let y = round((self.rand.next_unit() - 0.5) * f64::from(shape.height) * 2.0);
                let x = round((self.rand.next_unit() - 0.5) * f64::from(shape.width) * 2.0);
                let z = round((self.rand.next_unit() - 0.5) * f64::from(shape.depth) * 2.0);
                (dir, [x, y, z])
            }
            ShapeProps::Sphere { .. } => {
                let y = round((self.rand.next_unit() - 0.5) * 2.0);
                let x = round((self.rand.next_unit() - 0.5) * 2.0);
                let z = round((self.rand.next_unit() - 0.5) * 2.0);
                let mut dir = [x, y, z];
                fast_normalize(&mut dir);
                let radius = self.rand.next_unit() * f64::from(shape.radius_range)
                    + f64::from(shape.min_radius);
                (dir, dir.map(|d| round(f64::from(d) * radius)))
            }
        };
        let vel = dir.map(|d| round(f64::from(d) * f64::from(pressure)));
        self.submit_particle(id, pos, vel, age_offset, pos_offset, vel_in);
    }

    /// `SubmitParticle`: the particle moved back along its velocity by the
    /// age offset and by the emitter's motion, into the appearance's space;
    /// the appearance's check (`Pad46`) decides whether it spawns (`Pad47`).
    fn submit_particle(
        &mut self,
        id: NodeId,
        pos: [f32; 3],
        vel: [f32; 3],
        age_offset: f32,
        pos_offset: [f32; 3],
        vel_in: [f32; 3],
    ) {
        let age = f64::from(age_offset);
        let q = vel.map(|v| round(f64::from(v) * age));
        let sum_x = f64::from(q[0]) + f64::from(pos[0]);
        let sum_y = f64::from(pos[1]) + f64::from(q[1]);
        let sum_z = round(f64::from(pos[2]) + f64::from(q[2]));
        let placed_pos = [
            round(sum_x + f64::from(pos_offset[0])),
            round(sum_y + f64::from(pos_offset[1])),
            round(f64::from(sum_z) + f64::from(pos_offset[2])),
        ];
        let moved = [0, 1, 2].map(|i| round(f64::from(vel[i]) + f64::from(vel_in[i])));
        let Some(state) = self.n(id).emitter() else {
            return;
        };
        let xform = Transform::from_words(state.xform);
        let init_life_span = state.init_life_span;
        let Some(appearance) = state.appearance else {
            return;
        };
        let rotated = rotate(&xform, xyz(moved)).to_words();
        let placed = transform_xform(&xform, xyz(placed_pos)).to_words();
        let six = [
            placed[0], placed[1], placed[2], rotated[0], rotated[1], rotated[2],
        ];
        if self.appearance_check(appearance, &six, age_offset, init_life_span) {
            self.appearance_spawn(appearance, &six, age_offset, init_life_span);
        }
    }
}
