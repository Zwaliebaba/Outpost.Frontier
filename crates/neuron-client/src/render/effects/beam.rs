//! `FBeamEffect`'s geometry (`beam_effect.cpp`): the spear's single
//! segment and the bolt's tube of several, built into the frame's
//! [`EffectVertexBatch`], and `BuildGeometry`'s choice between them.
//!
//! The shape comes from the beam's archetype as plain numbers
//! ([`BeamShape`], named by the archetype file's keys). The arithmetic is
//! the reference's, in its operand order, with a store to `f32` wherever it
//! stores a `float` (decision T11); the transforms are the math engine's
//! `TransformXform`.

// The negated comparisons are the reference's unordered tests: NaN takes
// the branch that `!(a < b)` names.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use neuron_core::math::{Transform, Vector, transform_xform};

use super::batch::{EffectVertexBatch, ftol_low_byte};
use super::set::{EffectBase, EffectCamera, SpaceEffect, WalkFrame};

/// `kUvCore`, the tip cross's centre `u`.
const UV_CORE: f32 = 0.75;
/// `kUvMid`.
const UV_MID: f32 = 0.25;
/// `kUvUAlt0`.
const UV_TRAIL_0: f32 = 0.55;
/// `kUvUAlt1`.
const UV_TRAIL_1: f32 = 0.45;
/// `kLifetimeIntensityCap`, the fade's length in seconds.
pub const LIFETIME_INTENSITY_CAP: f32 = 0.3;

/// A beam archetype's numbers (`FBeamEffectArch` from +0x04), by the keys
/// of the `[BeamSpear]` and `[BeamBolt]` sections.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct BeamShape {
    /// +0x04 0 spear (one segment), 1 bolt (the tube); any other value
    /// builds nothing.
    pub segment_mode: i32,
    /// +0x08 `head_texture`: 1 takes the texture's upper half.
    pub head_texture: i32,
    /// +0x0c `trail_texture`: 1 takes the first 0.45 of the trail band.
    pub trail_texture: i32,
    /// +0x10.
    pub tip_color: [f32; 3],
    /// +0x1c.
    pub tail_color: [f32; 3],
    /// +0x28.
    pub core_color: [f32; 3],
    /// +0x34.
    pub sec_core_color: [f32; 3],
    /// +0x40 `outter_color`.
    pub outer_color: [f32; 3],
    /// +0x4c `sec_outter_color`.
    pub sec_outer_color: [f32; 3],
    /// +0x58.
    pub head_brightness: f32,
    /// +0x5c.
    pub trail_brightness: f32,
    /// +0x60.
    pub tip_length: f32,
    /// +0x64.
    pub tail_length: f32,
    /// +0x68.
    pub core_length: f32,
    /// +0x6c.
    pub core_width: f32,
    /// +0x74.
    pub head_width: f32,
    /// +0x78.
    pub flash_size: f32,
}

/// `FBeamEffect` +0x70..+0x80, the state its geometry reads.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct BeamState {
    /// +0x70 bit 0 visible this frame, bit 1 a single segment only, bit 3
    /// the muzzle flash outside the cockpit.
    pub flags: u8,
    /// +0x78 the time since the first draw, −1 before it.
    pub progress: f32,
    /// +0x7c the time left once killed (at most 0.3 s), −1 while alive.
    pub lifetime: f32,
    /// +0x80 the time the beam takes to reach its full length.
    pub progress_denom: f32,
}

impl Default for BeamState {
    /// The constructor's values.
    fn default() -> Self {
        Self {
            flags: 0,
            progress: -1.0,
            lifetime: -1.0,
            progress_denom: 0.1,
        }
    }
}

/// What `BuildGeometry` reads besides the beam: the `particles` game
/// option and whether the cockpit camera is current.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BeamFrame {
    /// `GetGameOption(0x26) != 0`.
    pub particles: bool,
    /// `IsFirstPersonCameraCurrent()`.
    pub first_person: bool,
}

fn f32_of(value: f64) -> f32 {
    value as f32
}

fn sum(a: f32, b: f32) -> f32 {
    f32_of(f64::from(a) + f64::from(b))
}

fn product(a: f32, b: f32) -> f32 {
    f32_of(f64::from(a) * f64::from(b))
}

/// `SetBatchColorRgb`: each component's byte is the low byte of
/// `_ftol(scale * component * 255.0)`, alpha 0xff.
fn set_scaled_color(batch: &mut EffectVertexBatch, scale: f32, [r, g, b]: [f32; 3]) {
    let byte = |c: f32| ftol_low_byte(f64::from(scale) * f64::from(c) * 255.0);
    batch.color = [byte(r), byte(g), byte(b), 0xff];
}

/// An index as the reference keeps it (`static_cast<unsigned short>`).
fn index(i: usize) -> u16 {
    i as u16
}

fn append(batch: &mut EffectVertexBatch, p: Vector) -> u16 {
    index(batch.append(p.x, p.y, p.z))
}

fn negated(v: Vector) -> Vector {
    Vector::new(-v.x, -v.y, -v.z)
}

fn added(a: Vector, b: Vector) -> Vector {
    Vector::new(sum(a.x, b.x), sum(a.y, b.y), sum(a.z, b.z))
}

/// The two texture bands: the head's `v` pair and the trail's.
fn uv_bands(shape: &BeamShape) -> ([f32; 2], [f32; 2]) {
    let head = if shape.head_texture == 1 {
        [0.0, 0.5]
    } else {
        [0.5, 1.0]
    };
    let trail = if shape.trail_texture == 1 {
        [0.0, UV_TRAIL_1]
    } else {
        [UV_TRAIL_0, 1.0]
    };
    (head, trail)
}

/// `FBeamEffect::BuildGeometry` (slot 0x1c) after its transform: the
/// growth `t` and the fade, then the builder. `world` is the transform the
/// reference picks, the engine instance's times the local one for a beam
/// on an object, otherwise the local one. The answer is false when nothing
/// was built (the option off, or a faded beam).
pub fn build_geometry(
    state: &mut BeamState,
    shape: &BeamShape,
    world: &Transform,
    frame: BeamFrame,
    batch: &mut EffectVertexBatch,
) -> bool {
    state.flags &= 0xfe;
    if !frame.particles {
        return false;
    }
    // TEST AH,5 / JP: only strictly below the denominator.
    let t = if state.progress < state.progress_denom {
        f32_of(f64::from(state.progress) / f64::from(state.progress_denom))
    } else {
        1.0
    };
    let mut intensity = 1.0;
    if state.lifetime > 0.0 {
        let quotient = f64::from(state.lifetime) / f64::from(LIFETIME_INTENSITY_CAP);
        intensity = f32_of(quotient);
        if quotient <= 0.0 {
            return false;
        }
    }
    if state.flags & 2 != 0 || t == 0.0 {
        build_single_segment(state.flags, shape, world, 0.0, intensity, frame, batch);
        return true;
    }
    match shape.segment_mode {
        0 => build_single_segment(state.flags, shape, world, t, intensity, frame, batch),
        1 => build_multi_segment(shape, world, t, intensity, batch),
        _ => {}
    }
    true
}

/// `BeamEffectBuildSingleSegment`: the tip cross of `head_width`, fanned
/// from the origin, then the core along local z from `-(tip_length)` to
/// `tail_length`, both grown by `t` from `core_width`, with its four arms.
/// 12 vertices and 36 indices.
pub fn build_single_segment(
    flags: u8,
    shape: &BeamShape,
    world: &Transform,
    t: f32,
    intensity: f32,
    frame: BeamFrame,
    batch: &mut EffectVertexBatch,
) {
    let mut near_r = shape.core_width;
    if t == 0.0 && flags & 8 != 0 && !frame.first_person {
        near_r = shape.flash_size;
    }
    let mut tip_r = shape.head_width;
    if tip_r < near_r {
        tip_r = near_r;
    }
    let delta0 = f32_of(f64::from(shape.tip_length) - f64::from(near_r));
    let delta1 = f32_of(f64::from(shape.tail_length) - f64::from(near_r));
    let end_r = f32_of(f64::from(delta1) * f64::from(t) + f64::from(near_r));
    let start_r_neg = f32_of(-(f64::from(delta0) * f64::from(t) + f64::from(near_r)));
    let origin = world.translation;

    let vx = Vector::new(near_r, 0.0, 0.0);
    let vy = Vector::new(0.0, near_r, 0.0);
    let tip_x = Vector::new(tip_r, 0.0, 0.0);
    let tip_y = Vector::new(0.0, tip_r, 0.0);
    let xf = |v: Vector| transform_xform(world, v);
    let pos_x = xf(vx);
    let neg_x = xf(negated(vx));
    let pos_y = xf(vy);
    let neg_y = xf(negated(vy));
    let tip_pos_x = xf(tip_x);
    let tip_neg_x = xf(negated(tip_x));
    let tip_pos_y = xf(tip_y);
    let tip_neg_y = xf(negated(tip_y));
    let start = xf(Vector::new(0.0, 0.0, start_r_neg));
    let end = xf(Vector::new(0.0, 0.0, end_r));

    let ([v0, v1], [u0, u1]) = uv_bands(shape);
    let v_mid = f32_of((f64::from(v0) + f64::from(v1)) * 0.5);
    let scale0 = product(intensity, shape.head_brightness);
    let scale1 = product(intensity, shape.trail_brightness);

    set_scaled_color(batch, scale0, shape.core_color);
    (batch.u, batch.v) = (UV_CORE, v_mid);
    let i0 = append(batch, origin);
    set_scaled_color(batch, scale0, shape.outer_color);
    (batch.u, batch.v) = (0.5, v0);
    let i1 = append(batch, tip_pos_x);
    batch.v = v1;
    let i2 = append(batch, tip_pos_y);
    (batch.u, batch.v) = (1.0, v1);
    let i3 = append(batch, tip_neg_x);
    batch.v = v0;
    let i4 = append(batch, tip_neg_y);

    set_scaled_color(batch, scale1, shape.core_color);
    (batch.u, batch.v) = (0.0, u0);
    let a0 = append(batch, origin);
    set_scaled_color(batch, scale1, shape.tip_color);
    (batch.u, batch.v) = (UV_MID, u1);
    let a1 = append(batch, start);
    set_scaled_color(batch, scale1, shape.tail_color);
    let a2 = append(batch, end);
    set_scaled_color(batch, scale1, shape.outer_color);
    (batch.u, batch.v) = (UV_MID, u0);
    let a3 = append(batch, pos_x);
    let a4 = append(batch, neg_x);
    let a5 = append(batch, pos_y);
    let a6 = append(batch, neg_y);

    batch.push_triangle(i0, i1, i2);
    batch.push_triangle(i0, i3, i2);
    batch.push_triangle(i0, i3, i4);
    batch.push_triangle(i0, i1, i4);
    for a in [a3, a4, a5, a6] {
        batch.push_triangle(a0, a1, a);
    }
    for a in [a3, a4, a5, a6] {
        batch.push_triangle(a0, a2, a);
    }
}

/// `BeamEffectBuildMultiSegment`: the tip cross of `head_width`, then a
/// tube from `-(tip_length)` behind the origin through the `core_width`
/// ring, the ring pushed forward to `core_length` and on to `tail_length`
/// beyond it, all grown by `t`. 17 vertices and 60 indices.
pub fn build_multi_segment(
    shape: &BeamShape,
    world: &Transform,
    t: f32,
    intensity: f32,
    batch: &mut EffectVertexBatch,
) {
    let near_r = if t == 0.0 {
        shape.flash_size
    } else {
        shape.core_width
    };
    let mut tip_r = shape.head_width;
    if tip_r < near_r {
        tip_r = near_r;
    }
    let mut delta0 = f32_of(f64::from(shape.tip_length) - f64::from(near_r));
    let mut delta1 = f32_of(f64::from(shape.tail_length) - f64::from(near_r));
    let mut mid_r = shape.core_length;
    if delta0 < 0.0 {
        delta0 = 0.0;
    }
    if delta1 < 0.0 {
        delta1 = 0.0;
    }
    if mid_r < 0.0 {
        mid_r = 0.0;
    }

    let near_x = Vector::new(near_r, 0.0, 0.0);
    let near_y = Vector::new(0.0, near_r, 0.0);
    let tip_x = Vector::new(tip_r, 0.0, 0.0);
    let tip_y = Vector::new(0.0, tip_r, 0.0);
    let origin = world.translation;
    let mid = Vector::new(0.0, 0.0, product(mid_r, t));
    let start = Vector::new(
        0.0,
        0.0,
        f32_of(-(f64::from(delta0) * f64::from(t) + f64::from(near_r))),
    );
    let end = Vector::new(
        0.0,
        0.0,
        f32_of(f64::from(delta1) * f64::from(t) + f64::from(near_r)),
    );

    let xf = |v: Vector| transform_xform(world, v);
    let near_pos_x = xf(near_x);
    let near_neg_x = xf(negated(near_x));
    let near_pos_y = xf(near_y);
    let near_neg_y = xf(negated(near_y));
    let tip_pos_x = xf(tip_x);
    let tip_neg_x = xf(negated(tip_x));
    let tip_pos_y = xf(tip_y);
    let tip_neg_y = xf(negated(tip_y));
    let xf_start = xf(start);
    let end_mid = xf(added(end, mid));
    let xf_mid = xf(mid);
    let near_pos_x_mid = xf(added(near_x, mid));
    let near_neg_x_mid = xf(added(negated(near_x), mid));
    let near_pos_y_mid = xf(added(near_y, mid));
    let near_neg_y_mid = xf(added(negated(near_y), mid));

    let ([v0, v1], [u0, u1]) = uv_bands(shape);
    let v_mid = f32_of((f64::from(v0) + f64::from(v1)) * 0.5);
    let scale0 = product(intensity, shape.head_brightness);
    let scale1 = product(intensity, shape.trail_brightness);

    set_scaled_color(batch, scale0, shape.core_color);
    (batch.u, batch.v) = (UV_CORE, v_mid);
    let i0 = append(batch, origin);
    set_scaled_color(batch, scale0, shape.outer_color);
    (batch.u, batch.v) = (0.5, v0);
    let i1 = append(batch, tip_pos_x);
    (batch.u, batch.v) = (1.0, v0);
    let i2 = append(batch, tip_pos_y);
    (batch.u, batch.v) = (1.0, v1);
    let i3 = append(batch, tip_neg_x);
    (batch.u, batch.v) = (0.5, v1);
    let i4 = append(batch, tip_neg_y);

    set_scaled_color(batch, scale1, shape.core_color);
    (batch.u, batch.v) = (0.0, u1);
    let a0 = append(batch, origin);
    set_scaled_color(batch, scale1, shape.tip_color);
    (batch.u, batch.v) = (UV_MID, u0);
    let a1 = append(batch, xf_start);
    set_scaled_color(batch, scale1, shape.outer_color);
    (batch.u, batch.v) = (UV_MID, u1);
    let a2 = append(batch, near_pos_x);
    let b0 = append(batch, near_neg_x);
    let b1 = append(batch, near_pos_y);
    let b2 = append(batch, near_neg_y);
    set_scaled_color(batch, scale1, shape.sec_core_color);
    (batch.u, batch.v) = (0.0, u0);
    let b3 = append(batch, xf_mid);
    set_scaled_color(batch, scale1, shape.tail_color);
    (batch.u, batch.v) = (UV_MID, u1);
    let b4 = append(batch, end_mid);
    let [r, g, b] = shape.sec_outer_color;
    batch.set_color(product(scale1, r), product(scale1, g), product(scale1, b));
    (batch.u, batch.v) = (UV_MID, u0);
    let c0 = append(batch, near_pos_x_mid);
    let c1 = append(batch, near_neg_x_mid);
    let c2 = append(batch, near_pos_y_mid);
    let c3 = append(batch, near_neg_y_mid);

    batch.push_triangle(i0, i1, i2);
    batch.push_triangle(i0, i3, i2);
    batch.push_triangle(i0, i3, i4);
    batch.push_triangle(i0, i1, i4);
    for a in [a2, b0, b1, b2] {
        batch.push_triangle(a0, a1, a);
    }
    for (c, a) in [(c0, a2), (c1, b0), (c2, b1), (c3, b2)] {
        batch.push_quad(a0, b3, c, a);
    }
    for c in [c0, c1, c2, c3] {
        batch.push_triangle(b3, b4, c);
    }
}

/// `EFT_CODE_PROJ_EFFECT`, the type `Start` gives every beam.
pub const BEAM_EFFECT_TYPE: usize = 0x2c;

/// `FBeamEffect` (0x84 bytes): the head, the archetype's shape (`Arch`,
/// +0x6c), the state and the culling radius (+0x74).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct BeamEffect {
    /// +0x00..+0x6b.
    pub base: EffectBase,
    /// +0x6c, `None` before `Start` found one.
    pub shape: Option<BeamShape>,
    /// +0x70..+0x80.
    pub state: BeamState,
    /// +0x74.
    pub radius: f32,
}

impl BeamEffect {
    /// The constructor: the base's, the state's and a radius of 50.
    #[must_use]
    pub fn new(type_id: usize) -> Self {
        Self {
            base: EffectBase::new(type_id),
            shape: None,
            state: BeamState::default(),
            radius: 50.0,
        }
    }

    fn flag_visible(&self) -> bool {
        self.state.flags & 1 != 0
    }
}

impl SpaceEffect for BeamEffect {
    fn base(&self) -> &EffectBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut EffectBase {
        &mut self.base
    }

    /// `FBeamEffect::Kill`. A single-segment beam dies at once. Any other
    /// beam leaves its instance, keeping its world pose, and fades over at
    /// most 0.3 s; `TickDraw` kills it at the end.
    fn kill(&mut self, frame: &WalkFrame<'_>) {
        if self.state.flags & 2 != 0 {
            self.base.kill(frame.host);
            return;
        }
        self.base.freeze_to_world(frame.host);
        // TEST AH,5 / JNP and TEST AH,0x41 / JNZ: exactly 0 and NaN stay.
        let lifetime = self.state.lifetime;
        if lifetime < 0.0 || lifetime > LIFETIME_INTENSITY_CAP {
            self.state.lifetime = LIFETIME_INTENSITY_CAP;
        }
    }

    fn is_alive(&self) -> bool {
        true
    }

    /// `FBeamEffect::UpdateVisibility`: without the `code_beam` texture the
    /// flag as it stands; below the performance level the beam is killed.
    /// Otherwise its sphere (`radius` about the world position) must reach
    /// beyond the near plane, start inside the type's `pbubble` (narrowed
    /// by the performance level) and meet both side planes.
    fn update_visibility(&mut self, _dt: f32, cam: &EffectCamera, frame: &WalkFrame<'_>) -> bool {
        if !frame.beam_texture {
            return self.flag_visible();
        }
        let perf = frame.generic_effect_perf;
        if !(self.base.priority >= perf) {
            self.base.kill(frame.host);
            return self.flag_visible();
        }
        let position = self.base.world_transform(frame.host).translation;
        let radius = self.radius;
        let view = transform_xform(&cam.view, position);
        let neg_z = -view.z;
        let far_edge = sum(neg_z, radius);
        if !(cam.z_near < far_edge) {
            return self.flag_visible();
        }
        let info = &frame.types[self.base.type_id];
        let near_distance = f32_of(f64::from(neg_z) - f64::from(radius));
        let narrowing = f32_of(1.0 - f64::from(perf));
        let threshold = f32_of(
            (f64::from(info.pbubble_far) - f64::from(info.pbubble_near)) * f64::from(narrowing)
                + f64::from(info.pbubble_near),
        );
        if !(threshold > near_distance) {
            return self.flag_visible();
        }
        // |lateral| * plane[axis] + view.z * plane[2], stored.
        let plane = |lateral: f32, along: f32, depth: f32| {
            f32_of(
                f64::from(lateral.abs()) * f64::from(along) + f64::from(view.z) * f64::from(depth),
            )
        };
        let dist_x = plane(view.x, cam.plane_x[0], cam.plane_x[2]);
        let dist_y = plane(view.y, cam.plane_y[1], cam.plane_y[2]);
        if dist_x <= radius && dist_y <= radius {
            self.state.flags |= 1;
        } else {
            self.state.flags &= 0xfe;
        }
        self.flag_visible()
    }

    /// `FBeamEffect::TickDraw`: nothing with the `particles` option off,
    /// nothing moves while the single-player server is paused; the growth
    /// clock runs from 0, and a fading beam is killed when its time is out.
    fn tick_draw(&mut self, dt: f32, _cam: &EffectCamera, frame: &WalkFrame<'_>) -> bool {
        if !frame.particles {
            return false;
        }
        if frame.server_paused {
            return true;
        }
        let state = &mut self.state;
        state.progress = if state.progress >= 0.0 {
            sum(state.progress, dt)
        } else {
            0.0
        };
        if !(state.lifetime > 0.0) {
            return true;
        }
        state.lifetime = f32_of(f64::from(state.lifetime) - f64::from(dt));
        if !(state.lifetime <= 0.0) {
            return true;
        }
        self.base.kill(frame.host);
        false
    }

    /// `FBeamEffect::BuildGeometry` with the world transform it picks.
    fn build_geometry(
        &mut self,
        _cam: &EffectCamera,
        frame: &WalkFrame<'_>,
        batch: &mut EffectVertexBatch,
    ) {
        let Some(shape) = self.shape else {
            return;
        };
        let world = self.base.world_transform(frame.host);
        let beam_frame = BeamFrame {
            particles: frame.particles,
            first_person: frame.first_person,
        };
        build_geometry(&mut self.state, &shape, &world, beam_frame, batch);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_core::math::Matrix;

    fn shape(segment_mode: i32) -> BeamShape {
        BeamShape {
            segment_mode,
            head_texture: 0,
            trail_texture: 1,
            tip_color: [1.0, 0.0, 0.0],
            tail_color: [0.0, 1.0, 0.0],
            core_color: [0.0, 0.0, 1.0],
            sec_core_color: [1.0, 1.0, 0.0],
            outer_color: [0.0, 1.0, 1.0],
            sec_outer_color: [1.0, 0.0, 1.0],
            head_brightness: 1.0,
            trail_brightness: 0.5,
            tip_length: 3.0,
            tail_length: 10.0,
            core_length: 4.0,
            core_width: 1.0,
            head_width: 2.0,
            flash_size: 5.0,
        }
    }

    const FRAME: BeamFrame = BeamFrame {
        particles: true,
        first_person: false,
    };

    fn at(x: f32) -> Transform {
        Transform::new(Matrix::IDENTITY, Vector::new(x, 0.0, 0.0))
    }

    #[test]
    fn the_spear_is_a_tip_cross_and_a_core() {
        let mut batch = EffectVertexBatch::default();
        build_single_segment(0, &shape(0), &at(100.0), 1.0, 1.0, FRAME, &mut batch);
        assert_eq!((batch.vertices.len(), batch.indices.len()), (12, 36));
        let v = &batch.vertices;
        // The origin, the tip cross at head_width, then the core from
        // -tip_length to tail_length.
        assert_eq!((v[0].x, v[0].u, v[0].v), (100.0, UV_CORE, 0.75));
        assert_eq!((v[1].x, v[3].x), (102.0, 98.0));
        assert_eq!((v[6].z, v[7].z), (-3.0, 10.0));
        // core_color (blue) at head_brightness, then tip_color at half.
        assert_eq!(v[0].color, [255, 0, 0, 255]);
        assert_eq!(v[6].color, [0, 0, 127, 255]);
        assert_eq!(&batch.indices[..6], &[0, 1, 2, 0, 3, 2]);
    }

    #[test]
    fn the_bolt_is_a_tip_cross_and_a_tube() {
        let mut batch = EffectVertexBatch::default();
        build_multi_segment(&shape(1), &at(0.0), 0.5, 1.0, &mut batch);
        assert_eq!((batch.vertices.len(), batch.indices.len()), (17, 60));
        let v = &batch.vertices;
        // Start at -(2 * 0.5 + 1), mid at 4 * 0.5, mid + end 2 + (9 * 0.5 + 1).
        assert_eq!((v[6].z, v[11].z, v[12].z), (-2.0, 2.0, 7.5));
        assert_eq!((v[13].x, v[13].z), (1.0, 2.0));
        // sec_outter_color through SetColor, at trail_brightness.
        assert_eq!(v[13].color, [127, 0, 127, 255]);
    }

    #[test]
    fn build_geometry_grows_and_fades() {
        let mut batch = EffectVertexBatch::default();
        let mut state = BeamState {
            progress: 0.0,
            ..BeamState::default()
        };
        // At t = 0 the bolt draws one segment.
        assert!(build_geometry(
            &mut state,
            &shape(1),
            &at(0.0),
            FRAME,
            &mut batch
        ));
        assert_eq!(batch.vertices.len(), 12);
        batch.reset();
        state.progress = 0.05;
        state.lifetime = 0.15;
        state.flags = 1;
        assert!(build_geometry(
            &mut state,
            &shape(1),
            &at(0.0),
            FRAME,
            &mut batch
        ));
        assert_eq!((state.flags, batch.vertices.len()), (0, 17));
        // Half the fade: core_color at half the head's brightness.
        assert_eq!(batch.vertices[0].color[0], 127);
        let off = BeamFrame {
            particles: false,
            ..FRAME
        };
        assert!(!build_geometry(
            &mut state,
            &shape(0),
            &at(0.0),
            off,
            &mut batch
        ));
    }
}
