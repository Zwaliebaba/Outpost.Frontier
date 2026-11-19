//! The billboard fills (`particlefx_appearances.cpp`): basic quads and
//! triangles (`FillQuadGeometry`, `FillTriangleGeometry`), rect and perp
//! (`FillGeometry`), oriented and dust (their `DrawOrEnqueue`, which fill
//! and draw in one), each writing into its class's draw context.
//!
//! Arithmetic follows decision T11 as the reference's x87 code does: what
//! stays on the stack is `f64`, every store to a `float` rounds. The frame
//! words (texture, `u0`, `v0`, `u1`, `v1`) are copied as words where the
//! reference copies them, and through [`quiet`] where it loads and stores
//! them through the x87.

use std::rc::Rc;

use neuron_core::math::{
    Matrix, Transform, Vector, general_inverse, transform_matrix, transform_xform,
};
use neuron_core::num::trunc_to_i32_via_i64;

use super::ParticleFx;
use super::draw::{COUNTER_DRAWS, context_of, pool_kind};
use super::emitter::inv_sqrt_guess;
use super::node::NodeId;
use super::pool::Particle;
use super::props::{BillboardProps, PropsData};
use super::ramp::{IDENTITY, NestedSingle, mul, round};
use super::services::{Camera, ParticleDraw, ParticleServices};

pub(crate) fn d(value: f32) -> f64 {
    f64::from(value)
}

/// A word through an x87 load and store: a signalling NaN comes back quiet.
pub(crate) const fn quiet(word: u32) -> u32 {
    if word & 0x7F80_0000 == 0x7F80_0000 && word & 0x007F_FFFF != 0 {
        word | 0x0040_0000
    } else {
        word
    }
}

pub(crate) const fn bits(value: f32) -> u32 {
    value.to_bits()
}

/// A ramp slot's `Eval(outer, inner)`; a slot the class does not build is a
/// null pointer the reference would call through (D379), 0 here.
pub(crate) fn ev(slot: Option<&NestedSingle>, outer: f32, inner: f32) -> f64 {
    slot.map_or(0.0, |s| s.eval(outer, inner))
}

/// `ftol(alpha * 255) << 24` over the packed colour.
pub(crate) fn with_alpha(color: u32, alpha: f32) -> u32 {
    color | trunc_to_i32_via_i64(d(alpha) * 255.0).cast_unsigned() << 24
}

/// The `0x5f3759df` step: `(1.5 - ((g * g) * x) * 0.5) * g` with the guess
/// taken from `seed`'s bits.
pub(crate) fn inv_sqrt_step(seed: f32, x: f64) -> f64 {
    let g = d(inv_sqrt_guess(seed));
    (1.5 - ((g * g) * x) * 0.5) * g
}

/// `ComputeSinCosSse`, the module's sine and cosine on a CPU with SSE (the
/// table `SelectParticleFxMathImplementation` picks, D381): the angle
/// reduced by quadrants of `2 / pi` and a polynomial in single precision.
/// Returns `[cos, sin]`.
#[must_use]
pub fn sin_cos(angle: f32) -> [f32; 2] {
    let k = |w: u32| f32::from_bits(w);
    let cvtt = |v: f32| neuron_core::num::trunc_to_i32(f64::from(v));
    let angle_bits = angle.to_bits();
    let abs = f32::from_bits(angle_bits & 0x7FFF_FFFF);
    let mut c = abs * k(0x3F22_F983);
    let mut s = c;
    c += 1.0;
    let cq = cvtt(c);
    let sq = cvtt(s);
    let cos_sign = (cq.cast_unsigned() << 30) & 0x8000_0000;
    let sin_sign = (angle_bits & 0x8000_0000) ^ ((sq.cast_unsigned() << 30) & 0x8000_0000);
    c -= cq as f32;
    if cq & 1 != 0 {
        c = 1.0 - c;
    }
    s -= sq as f32;
    if sq & 1 != 0 {
        s = 1.0 - s;
    }
    let cos_factor = f32::from_bits(c.to_bits() | cos_sign);
    let sin_factor = f32::from_bits(s.to_bits() | sin_sign);
    let c2 = c * c;
    let s2 = s * s;
    let mut cv = c2 * k(0xBB99_6966);
    let mut sv = s2 * k(0xBB99_6966);
    cv += k(0x3DA3_35E3);
    sv += k(0x3DA3_35E3);
    cv *= c2;
    sv *= s2;
    cv += k(0xBF25_5DE7);
    sv += k(0xBF25_5DE7);
    cv *= c2;
    sv *= s2;
    cv += k(0x3FC9_0FDB);
    sv += k(0x3FC9_0FDB);
    [cv * cos_factor, sv * sin_factor]
}

/// The frame words of a texture wrapper: texture, `u0`, `v0`, `u1`, `v1`.
pub(crate) fn frame_block(frame: &super::services::FrameOut) -> [u32; 5] {
    [
        frame.handle,
        frame.uv[0],
        frame.uv[1],
        frame.uv[2],
        frame.uv[3],
    ]
}

/// `FlipBasicAppearanceFrame`: flag `0x1000` swaps `u0` and `u1`, `0x2000`
/// `v0` and `v1`; the word moving up goes through the x87.
pub(crate) fn flip_up(flags: u32, block: &mut [u32; 5]) {
    if flags & 0x1000 != 0 {
        let previous = block[3];
        block[3] = quiet(block[1]);
        block[1] = previous;
    }
    if flags & 0x2000 != 0 {
        let previous = block[4];
        block[4] = quiet(block[2]);
        block[2] = previous;
    }
}

/// The flips after a frame lookup in the perp and oriented fills: the word
/// moving down goes through the x87.
pub(crate) fn flip_down(flags: u32, block: &mut [u32; 5]) {
    if flags & 0x1000 != 0 {
        let previous = block[1];
        block[1] = quiet(block[3]);
        block[3] = previous;
    }
    if flags & 0x2000 != 0 {
        let previous = block[2];
        block[2] = quiet(block[4]);
        block[4] = previous;
    }
}

/// `f32` of a frame word.
pub(crate) fn fw(word: u32) -> f32 {
    f32::from_bits(word)
}

/// What a fill reads of its node.
pub(crate) struct FillNode {
    pub(crate) props: Rc<super::props::NodeProps>,
    pub(crate) world: [f32; 12],
    pub(crate) outer: f32,
    pub(crate) lod: f32,
    pub(crate) head: Option<u32>,
    pub(crate) texture: super::node::TextureWrapper,
    pub(crate) context: usize,
    pub(crate) kind: super::pool::PoolKind,
}

impl FillNode {
    pub(crate) fn billboard(&self) -> &BillboardProps {
        match &self.props.data {
            PropsData::Billboard(b) => b,
            _ => unreachable!("a billboard appearance"),
        }
    }

    /// `Appearance_LODCurve` at the LOD input: the fills' scale.
    pub(crate) fn scale(&self) -> f64 {
        self.billboard().lod_curve.eval(self.outer, self.lod)
    }

    /// The frame of a frame ramp's value (`ftol(position * (count - 1))`).
    pub(crate) fn frame_of(&self, position: f64) -> i32 {
        let span = self.texture.frame_count.wrapping_sub(1);
        trunc_to_i32_via_i64(position * f64::from(span))
    }

    /// `LookupFrame(frame, block)`: the library's frame into the block.
    pub(crate) fn lookup(&self, frame: i32, block: &mut [u32; 5], svc: &mut dyn ParticleServices) {
        if self.texture.handle == -1 {
            return;
        }
        if let Some(out) = svc.texture_frame(self.texture.handle, frame) {
            *block = frame_block(&out);
        }
    }
}

impl ParticleFx {
    pub(crate) fn fill_node(&self, id: NodeId) -> FillNode {
        let node = self.n(id);
        let app = self.app(id);
        FillNode {
            props: Rc::clone(&node.props),
            world: node.world,
            outer: node.outer_time,
            lod: node.lod_input,
            head: app.head,
            texture: app.texture,
            context: context_of(node.class()).unwrap_or(0),
            kind: pool_kind(node.class()),
        }
    }

    pub(crate) fn particle(&self, n: &FillNode, index: u32) -> Particle {
        *self.pools.get(n.kind).item(index)
    }

    /// `FxBasicAppearance::FillQuadGeometry`: camera-space quads, turned by
    /// the angle ramp or stretched along the screen velocity (flag 4).
    pub(crate) fn fill_basic_quads(
        &mut self,
        id: NodeId,
        camera: &mut dyn Camera,
        svc: &mut dyn ParticleDraw,
    ) {
        let n = self.fill_node(id);
        let b = n.billboard();
        let scale_value = n.scale();
        let scale = round(scale_value);
        if scale_value == 0.0 {
            return;
        }
        let frame_lookup = n.texture.frame_count != 1 && b.flags & 8 == 0;
        let streak = b.flags & 4 != 0;
        let view = camera.view_transform();
        let view_world = Transform::from_words(mul(&view, &n.world));
        let mut block = frame_block(&n.texture.frame);
        flip_up(b.flags, &mut block);
        let mut last_frame = -1;
        let mut cursor = n.head;
        while let Some(index) = cursor {
            let p = self.particle(&n, index);
            cursor = p.next;
            let c = transform_xform(&view_world, Vector::from_words(p.pos)).to_words();
            if c[2] > 0.0 {
                continue;
            }
            let inner = round(d(p.inv_duration) * d(p.age));
            let alpha = round(ev(b.size.as_ref(), n.outer, inner));
            let size = round(ev(b.primary_size.as_ref(), n.outer, inner) * d(scale));
            if alpha < f32::from_bits(0x3C23_D70A) {
                continue;
            }
            let diffuse = with_alpha(b.color.eval_packed(n.outer, inner), alpha);
            let angle = round(ev(b.tex_frame.as_ref(), n.outer, inner));
            let aspect = round(ev(b.common_tex_frame.as_ref(), n.outer, inner));
            let [cos, sin] = sin_cos(angle);
            if frame_lookup {
                let frame = n.frame_of(ev(b.axis_override.as_ref(), n.outer, inner));
                if frame != last_frame {
                    last_frame = frame;
                    n.lookup(frame, &mut block, svc);
                    flip_up(b.flags, &mut block);
                }
            }
            let (mut bx, mut by) = (0.0_f32, 0.0_f32);
            let (ux, uy, px, py);
            if streak {
                let v = transform_matrix(
                    &Matrix::from_words(view_world.to_words()[..9].try_into().unwrap_or([0.0; 9])),
                    Vector::from_words(p.vel),
                )
                .to_words();
                let n0 = d(c[1]) * d(v[2]) - d(v[1]) * d(c[2]);
                let n1 = d(v[0]) * d(c[2]) - d(c[0]) * d(v[2]);
                let n2 = d(c[0]) * d(v[1]) - d(v[0]) * d(c[1]);
                let ua = round(d(c[2]) * n1 - d(c[1]) * n2);
                let ub = round(d(c[0]) * n2 - d(c[2]) * n0);
                let uc = n0 * d(c[1]) - d(c[0]) * n1;
                let length_sq = round((d(ub) * d(ub) + d(ua) * d(ua)) + uc * uc);
                let inverse = inv_sqrt_step(length_sq, d(length_sq));
                let nx = round(d(ua) * inverse);
                let ny = round(d(ub) * inverse);
                let nz = uc * inverse;
                let shift = ((d(v[1]) * d(ny) + d(v[0]) * d(nx)) + d(v[2]) * nz)
                    * d(f32::from_bits(0x3D08_8880));
                let shift_x = d(nx) * shift;
                let shift_y = round(shift * d(ny));
                bx = round(d(c[0]) - shift_x);
                by = round(d(c[1]) - d(shift_y));
                ux = round(d(nx) * d(size));
                let uy_unrounded = d(ny) * d(size);
                uy = round(uy_unrounded);
                px = round(-uy_unrounded * d(aspect));
                py = round(d(ux) * d(aspect));
            } else {
                let aspect_size = d(aspect) * d(size);
                px = round(d(cos) * aspect_size);
                py = round(d(sin) * aspect_size);
                ux = round(d(cos) * d(size));
                uy = round(d(sin) * d(size));
            }
            let mut indices = [0_u16; 4];
            for (corner, slot) in indices.iter_mut().enumerate() {
                let u_index = if streak {
                    if corner < 2 { 3 } else { 1 }
                } else if corner == 0 || corner == 2 {
                    3
                } else {
                    1
                };
                let v_index = if streak {
                    if corner == 0 || corner == 2 { 2 } else { 4 }
                } else if corner < 2 {
                    2
                } else {
                    4
                };
                let (x, y) = if streak {
                    match corner {
                        0 => ((d(bx) - d(ux)) - d(px), (d(by) - d(uy)) - d(py)),
                        1 => ((d(c[0]) + d(ux)) - d(px), (d(c[1]) + d(uy)) - d(py)),
                        2 => ((d(bx) - d(ux)) + d(px), (d(by) - d(uy)) + d(py)),
                        _ => ((d(c[0]) + d(px)) + d(ux), (d(c[1]) + d(py)) + d(uy)),
                    }
                } else {
                    match corner {
                        0 => ((d(c[0]) + d(uy)) + d(px), (d(c[1]) - d(py)) + d(ux)),
                        1 => ((d(c[0]) - d(px)) + d(uy), (d(c[1]) + d(ux)) + d(py)),
                        2 => ((d(c[0]) + d(px)) - d(uy), (d(c[1]) - d(py)) - d(ux)),
                        _ => ((d(c[0]) - d(px)) - d(uy), (d(c[1]) + d(py)) - d(ux)),
                    }
                };
                let v = if corner == 0 {
                    quiet(block[v_index])
                } else {
                    block[v_index]
                };
                *slot = self.draw.contexts[n.context].vertex([
                    bits(round(x)),
                    bits(round(y)),
                    bits(c[2]),
                    diffuse,
                    quiet(block[u_index]),
                    v,
                ]);
            }
            self.draw.contexts[n.context].list_quad(indices[0], indices[1], indices[2], indices[3]);
        }
    }

    /// `FxBasicAppearance::FillTriangleGeometry`: one camera-space triangle
    /// per particle with the UV table of flag 1.
    pub(crate) fn fill_basic_triangles(
        &mut self,
        id: NodeId,
        camera: &mut dyn Camera,
        svc: &mut dyn ParticleDraw,
    ) {
        let n = self.fill_node(id);
        let b = n.billboard();
        let frame_lookup = n.texture.frame_count != 1 && b.flags & 8 == 0;
        let view = camera.view_transform();
        let view_world = Transform::from_words(mul(&view, &n.world));
        let mut block = frame_block(&n.texture.frame);
        flip_up(b.flags, &mut block);
        let (mut tri_u, mut tri_v) = triangle_uv(b.flags);
        map_triangle_uv(&block, &mut tri_u, &mut tri_v, true, !frame_lookup);
        let scale = round(n.scale());
        let mut last_frame = -1;
        let mut cursor = n.head;
        while let Some(index) = cursor {
            let p = self.particle(&n, index);
            cursor = p.next;
            let inner = round(d(p.inv_duration) * d(p.age));
            let alpha = round(ev(b.size.as_ref(), n.outer, inner));
            let size = round(ev(b.primary_size.as_ref(), n.outer, inner) * d(scale));
            if alpha < f32::from_bits(0x3C23_D70A) {
                continue;
            }
            let c = transform_xform(&view_world, Vector::from_words(p.pos)).to_words();
            let diffuse = with_alpha(b.color.eval_packed(n.outer, inner), alpha);
            let size2 = round(d(size) + d(size));
            let angle = round(ev(b.tex_frame.as_ref(), n.outer, inner));
            let [cos, sin] = sin_cos(angle);
            let xa = round(d(cos) * d(size2));
            let ya = round(d(sin) * d(size2));
            let xb = round(d(cos) * d(size));
            let yb = round(d(sin) * d(size));
            if frame_lookup {
                let frame = n.frame_of(ev(b.axis_override.as_ref(), n.outer, inner));
                if frame != last_frame {
                    last_frame = frame;
                    n.lookup(frame, &mut block, svc);
                    flip_up(b.flags, &mut block);
                    (tri_u, tri_v) = triangle_uv(b.flags);
                    map_triangle_uv(&block, &mut tri_u, &mut tri_v, false, true);
                }
            }
            let mut indices = [0_u16; 3];
            for (corner, slot) in indices.iter_mut().enumerate() {
                let (x, y) = match corner {
                    0 => ((d(c[0]) + d(xa)) - d(yb), (d(c[1]) - d(ya)) - d(xb)),
                    1 => ((d(c[0]) - d(xa)) - d(yb), (d(c[1]) + d(ya)) - d(xb)),
                    _ => (d(sin) * d(alpha) + d(c[0]), d(cos) * d(alpha) + d(c[1])),
                };
                let uv = 2 - corner;
                *slot = self.draw.contexts[n.context].vertex([
                    bits(round(x)),
                    bits(round(y)),
                    bits(c[2]),
                    diffuse,
                    bits(tri_u[uv]),
                    bits(tri_v[uv]),
                ]);
            }
            self.draw.contexts[n.context].triangle(indices[2], indices[1], indices[0]);
        }
    }

    /// `FxRectAppearance::FillGeometry`: node-space quads along the
    /// velocity, widened across the view direction, optionally centred
    /// (flag 1), stretched by speed (flag 4) and faded edge-on (flag
    /// `0x10`).
    #[allow(clippy::too_many_lines)]
    pub(crate) fn fill_rect(
        &mut self,
        id: NodeId,
        camera: &mut dyn Camera,
        svc: &mut dyn ParticleDraw,
    ) {
        const TINY_LENGTH: f32 = f32::from_bits(0x3586_37BD);
        const TINY_ALPHA: f32 = f32::from_bits(0x38D1_B717);
        const SPEED_STRETCH: f32 = f32::from_bits(0x3D08_850A);
        const FACING_CULL: f32 = f32::from_bits(0x3F7C_1C5C);
        const FACING_FADE: f32 = f32::from_bits(0x3F5D_B3D7);
        const FACING_FADE_SCALE: f32 = f32::from_bits(0x4106_B33D);
        self.draw.counters[4] = self.draw.counters[4].wrapping_add(1);
        let n = self.fill_node(id);
        let b = n.billboard();
        let frame_lookup = n.texture.frame_count != 1 && b.flags & 8 == 0;
        let stretch = b.flags & 4 != 0;
        let centred = b.flags & 1 != 0;
        let view_fade = b.flags & 0x10 != 0;
        let inv_node = general_inverse(&Transform::from_words(n.world)).transform;
        let mut block = frame_block(&n.texture.frame);
        flip_up(b.flags, &mut block);
        let mut last_frame = -1;
        let camera_position = camera.position();
        let local_camera =
            transform_xform(&inv_node, Vector::from_words(camera_position)).to_words();
        let scale = round(n.scale());
        let mut cursor = n.head;
        while let Some(index) = cursor {
            let p = self.particle(&n, index);
            cursor = p.next;
            let inner = round(d(p.inv_duration) * d(p.age));
            let mut alpha = round(ev(b.size.as_ref(), n.outer, inner));
            let size_scale = round(ev(b.primary_size.as_ref(), n.outer, inner) * d(scale));
            let length = round(ev(b.tex_frame.as_ref(), n.outer, inner) * d(size_scale));
            let half_width =
                round((ev(b.axis_override.as_ref(), n.outer, inner) * d(size_scale)) * 0.5);
            if length <= TINY_LENGTH || half_width <= TINY_LENGTH || alpha <= TINY_ALPHA {
                continue;
            }
            let position = p.pos;
            let mut velocity = p.vel;
            let velocity_sq = ((d(velocity[2]) * d(velocity[2])) + d(velocity[1]) * d(velocity[1]))
                + d(velocity[0]) * d(velocity[0]);
            let speed = round(inv_sqrt_step(round(velocity_sq), velocity_sq) * velocity_sq);
            if !(speed <= TINY_LENGTH) {
                let inverse = 1.0 / d(speed);
                velocity[0] = round(d(velocity[0]) * inverse);
                velocity[1] = round(d(velocity[1]) * inverse);
                velocity[2] = round(inverse * d(velocity[2]));
            } else {
                velocity = [0.0, 1.0, 0.0];
            }
            let mut direction = [
                round(d(position[0]) - d(local_camera[0])),
                round(d(position[1]) - d(local_camera[1])),
                round(d(position[2]) - d(local_camera[2])),
            ];
            if view_fade {
                let dir_sq = ((d(direction[2]) * d(direction[2]))
                    + d(direction[0]) * d(direction[0]))
                    + d(direction[1]) * d(direction[1]);
                let dir_length = inv_sqrt_step(round(dir_sq), dir_sq) * dir_sq;
                if !(dir_length <= d(TINY_LENGTH)) {
                    let inverse = 1.0 / dir_length;
                    direction[0] = round(d(direction[0]) * inverse);
                    direction[1] = round(d(direction[1]) * inverse);
                    direction[2] = round(inverse * d(direction[2]));
                } else {
                    direction = [0.0, 1.0, 0.0];
                }
                let facing_value = (((d(direction[2]) * d(velocity[2]))
                    + d(velocity[1]) * d(direction[1]))
                    + d(direction[0]) * d(velocity[0]))
                .abs();
                let facing = round(facing_value);
                if facing_value > d(FACING_CULL) {
                    continue;
                }
                if facing > FACING_FADE {
                    let fade = (d(FACING_CULL) - d(facing)) * d(FACING_FADE_SCALE);
                    alpha = round((d(alpha) * fade) * fade);
                }
                if alpha <= TINY_ALPHA {
                    continue;
                }
            }
            let diffuse = with_alpha(b.color.eval_packed(n.outer, inner), alpha);
            if frame_lookup {
                let frame = n.frame_of(ev(b.frame.as_ref(), n.outer, inner));
                if frame != last_frame {
                    last_frame = frame;
                    n.lookup(frame, &mut block, svc);
                    flip_up(b.flags, &mut block);
                }
            }
            let (p0, p1);
            if centred {
                let half = if stretch {
                    ((d(speed) * d(SPEED_STRETCH)) + d(length)) * 0.5
                } else {
                    d(length) * 0.5
                };
                let ex = round(d(velocity[0]) * half);
                let ey = round(d(velocity[1]) * half);
                let ez = round(half * d(velocity[2]));
                p0 = [
                    round(d(position[0]) - d(ex)),
                    round(d(position[1]) - d(ey)),
                    round(d(position[2]) - d(ez)),
                ];
                p1 = [
                    round(d(ex) + d(position[0])),
                    round(d(ey) + d(position[1])),
                    round(d(ez) + d(position[2])),
                ];
            } else {
                p0 = position;
                if stretch {
                    let extent = (d(speed) * d(SPEED_STRETCH)) + d(length);
                    let ex = d(velocity[0]) * extent;
                    let ey = round(extent * d(velocity[1]));
                    let ez = round(extent * d(velocity[2]));
                    p1 = [
                        round(ex + d(position[0])),
                        round(d(ey) + d(position[1])),
                        round(d(ez) + d(position[2])),
                    ];
                } else {
                    let ex = d(velocity[0]) * d(length);
                    let ey = d(velocity[1]) * d(length);
                    let ez = round(d(velocity[2]) * d(length));
                    p1 = [
                        round(ex + d(position[0])),
                        round(ey + d(position[1])),
                        round(d(ez) + d(position[2])),
                    ];
                }
            }
            let nx = (d(direction[2]) * d(velocity[1])) - (d(velocity[2]) * d(direction[1]));
            let ny = round((d(velocity[2]) * d(direction[0])) - (d(direction[2]) * d(velocity[0])));
            let nz = (d(direction[1]) * d(velocity[0])) - (d(direction[0]) * d(velocity[1]));
            let normal_sq = ((nx * nx) + d(ny) * d(ny)) + (nz * nz);
            let normal_length = inv_sqrt_step(round(normal_sq), normal_sq) * normal_sq;
            if normal_length <= d(TINY_LENGTH) {
                continue;
            }
            let width_scale = d(half_width) / normal_length;
            let q = [
                round(width_scale * nx),
                round(d(ny) * width_scale),
                round(nz * width_scale),
            ];
            let context = &mut self.draw.contexts[n.context];
            let corner = |base: [f32; 3], sign: f64| {
                [
                    bits(round(d(base[0]) + sign * d(q[0]))),
                    bits(round(d(base[1]) + sign * d(q[1]))),
                    bits(round(d(base[2]) + sign * d(q[2]))),
                ]
            };
            let a = corner(p1, -1.0);
            let ia = context.vertex([a[0], a[1], a[2], diffuse, quiet(block[3]), quiet(block[2])]);
            let bb = corner(p1, 1.0);
            let ib = context.vertex([
                bb[0],
                bb[1],
                bb[2],
                diffuse,
                quiet(block[1]),
                quiet(block[2]),
            ]);
            let cc = corner(p0, -1.0);
            let ic = context.vertex([
                cc[0],
                cc[1],
                cc[2],
                diffuse,
                quiet(block[3]),
                quiet(block[4]),
            ]);
            let dd = corner(p0, 1.0);
            let id_ = context.vertex([
                dd[0],
                dd[1],
                dd[2],
                diffuse,
                quiet(block[1]),
                quiet(block[4]),
            ]);
            context.list_quad(ia, ib, ic, id_);
        }
    }

    /// `FxPerpAppearance::FillGeometry`: node-space squares across the
    /// velocity, their second axis carried from frame to frame in the
    /// particle (`+0x30`), faded when seen edge-on (flag `0x10`).
    #[allow(clippy::too_many_lines)]
    pub(crate) fn fill_perp(
        &mut self,
        id: NodeId,
        camera: &mut dyn Camera,
        svc: &mut dyn ParticleDraw,
    ) {
        const TINY_LENGTH: f32 = f32::from_bits(0x3586_37BD);
        const TINY_AXIS_SQ: f32 = f32::from_bits(0x322B_CC76);
        const FACING_CULL: f32 = f32::from_bits(0x3F7C_1C5C);
        const FACING_FADE: f32 = f32::from_bits(0x3F5D_B3D7);
        const FACING_FADE_SCALE: f32 = f32::from_bits(0x4106_B33D);
        self.draw.counters[5] = self.draw.counters[5].wrapping_add(1);
        let n = self.fill_node(id);
        let b = n.billboard();
        let frame_lookup = n.texture.frame_count != 1 && b.flags & 8 == 0;
        let camera_fade = b.flags & 0x10 != 0;
        let inv_node = general_inverse(&Transform::from_words(n.world)).transform;
        let mut block = frame_block(&n.texture.frame);
        flip_up(b.flags, &mut block);
        let mut last_frame = -1;
        let camera_position = camera.position();
        let scale = round(n.scale());
        let mut cursor = n.head;
        while let Some(index) = cursor {
            let p = self.particle(&n, index);
            cursor = p.next;
            let inner = round(d(p.age) * d(p.inv_duration));
            let mut alpha = round(ev(b.size.as_ref(), n.outer, inner));
            let half_unrounded = (ev(b.primary_size.as_ref(), n.outer, inner) * d(scale)) * 0.5;
            let half = round(half_unrounded);
            if half_unrounded <= d(TINY_LENGTH) || alpha <= TINY_LENGTH {
                continue;
            }
            let axis = p.extra;
            let mut velocity = p.vel;
            let velocity_sq = ((d(velocity[0]) * d(velocity[0])) + d(velocity[2]) * d(velocity[2]))
                + d(velocity[1]) * d(velocity[1]);
            let velocity_length = velocity_sq * inv_sqrt_step(round(velocity_sq), velocity_sq);
            if !(velocity_length <= d(TINY_LENGTH)) {
                let inverse = 1.0 / velocity_length;
                velocity = velocity.map(|v| round(d(v) * inverse));
            } else {
                velocity = [0.0, 1.0, 0.0];
                let dx = -d(axis[0]);
                let dy = 1.0 - d(axis[1]);
                let dz = -d(axis[2]);
                let tiny = d(TINY_AXIS_SQ);
                if dx * dx <= tiny && dy * dy <= tiny && dz * dz <= tiny {
                    velocity[0] = 1.0;
                    velocity[1] = 0.0;
                }
            }
            let position = p.pos;
            if camera_fade {
                let local =
                    transform_xform(&inv_node, Vector::from_words(camera_position)).to_words();
                let mut to = [
                    round(d(local[0]) - d(position[0])),
                    round(d(local[1]) - d(position[1])),
                    round(d(local[2]) - d(position[2])),
                ];
                let sq = ((d(to[0]) * d(to[0])) + d(to[2]) * d(to[2])) + d(to[1]) * d(to[1]);
                let length = sq * inv_sqrt_step(round(sq), sq);
                if !(length <= d(TINY_LENGTH)) {
                    let inverse = 1.0 / length;
                    to = to.map(|v| round(d(v) * inverse));
                } else {
                    to = [0.0, 1.0, 0.0];
                }
                let dot = ((d(to[0]) * d(velocity[0])) + d(to[2]) * d(velocity[2]))
                    + d(to[1]) * d(velocity[1]);
                let facing_value = 1.0 - dot.abs();
                let facing = round(facing_value);
                if facing_value > d(FACING_CULL) {
                    continue;
                }
                if facing > FACING_FADE {
                    let fade = (d(FACING_CULL) - d(facing)) * d(FACING_FADE_SCALE);
                    alpha = round((d(alpha) * fade) * fade);
                }
                if alpha <= TINY_LENGTH {
                    continue;
                }
            }
            let diffuse = with_alpha(b.color.eval_packed(n.outer, inner), alpha);
            if frame_lookup {
                let frame = n.frame_of(ev(b.tex_frame.as_ref(), n.outer, inner));
                if frame != last_frame {
                    last_frame = frame;
                    n.lookup(frame, &mut block, svc);
                    flip_down(b.flags, &mut block);
                }
            }
            let nx = round((d(velocity[2]) * d(axis[1])) - (d(velocity[1]) * d(axis[2])));
            let ny = round((d(axis[2]) * d(velocity[0])) - (d(velocity[2]) * d(axis[0])));
            let nz = round((d(velocity[1]) * d(axis[0])) - (d(axis[1]) * d(velocity[0])));
            let normal_sq = round(((d(nx) * d(nx)) + d(nz) * d(nz)) + d(ny) * d(ny));
            let inverse = inv_sqrt_step(normal_sq, d(normal_sq));
            let unx = d(nx) * inverse;
            let uny = round(d(ny) * inverse);
            let unz_unrounded = d(nz) * inverse;
            let unz = round(unz_unrounded);
            let new_axis = [
                round((unz_unrounded * d(velocity[1])) - (d(uny) * d(velocity[2]))),
                round((d(velocity[2]) * unx) - (d(unz) * d(velocity[0]))),
                round((d(uny) * d(velocity[0])) - (d(velocity[1]) * unx)),
            ];
            self.pools.get_mut(n.kind).item_mut(index).extra = new_axis;
            let h = d(half);
            let snx = round(unx * h);
            let sny = round(d(uny) * h);
            let snz = round(d(unz) * h);
            let sax = round(d(new_axis[0]) * h);
            let say = round(d(new_axis[1]) * h);
            let saz_unrounded = d(new_axis[2]) * h;
            let saz = round(saz_unrounded);
            let sum = [
                round(d(sax) + d(snx)),
                round(d(say) + d(sny)),
                round(saz_unrounded + d(snz)),
            ];
            let difference = [d(sax) - d(snx), d(say) - d(sny), d(saz) - d(snz)];
            let context = &mut self.draw.contexts[n.context];
            let ia = context.vertex([
                bits(round(difference[0] + d(position[0]))),
                bits(round(difference[1] + d(position[1]))),
                bits(round(difference[2] + d(position[2]))),
                diffuse,
                quiet(block[3]),
                quiet(block[2]),
            ]);
            let ib = context.vertex([
                bits(round(d(position[0]) - d(sum[0]))),
                bits(round(d(position[1]) - d(sum[1]))),
                bits(round(d(position[2]) - d(sum[2]))),
                diffuse,
                block[1],
                block[2],
            ]);
            let ic = context.vertex([
                bits(round(d(sum[0]) + d(position[0]))),
                bits(round(d(sum[1]) + d(position[1]))),
                bits(round(d(sum[2]) + d(position[2]))),
                diffuse,
                block[3],
                block[4],
            ]);
            let id_ = context.vertex([
                bits(round(d(position[0]) - difference[0])),
                bits(round(d(position[1]) - difference[1])),
                bits(round(d(position[2]) - difference[2])),
                diffuse,
                block[1],
                block[4],
            ]);
            context.list_quad(ia, ib, ic, id_);
        }
    }

    /// `FxOrientedAppearance::DrawOrEnqueue`: node-space quads on the axes
    /// of the set's local transform (`+0x8c`) at each particle's time,
    /// faded edge-on (flag `0x40`), filled and drawn at once (no batching).
    #[allow(clippy::too_many_lines)]
    pub(crate) fn draw_oriented(
        &mut self,
        id: NodeId,
        camera: &mut dyn Camera,
        svc: &mut dyn ParticleDraw,
    ) {
        const TINY_SCALE: f32 = f32::from_bits(0x2D2F_EBFF);
        const TINY_LENGTH: f32 = f32::from_bits(0x3586_37BD);
        const FACING_CULL: f32 = f32::from_bits(0x3F7C_1C5C);
        const FACING_FADE: f32 = f32::from_bits(0x3F5D_B3D7);
        const FACING_FADE_SCALE: f32 = f32::from_bits(0x4106_B33D);
        self.draw.counters[6] = self.draw.counters[6].wrapping_add(1);
        let n = self.fill_node(id);
        if n.head.is_none() {
            return;
        }
        let b = n.billboard();
        let frame_lookup = n.texture.frame_count != 1 && b.flags & 8 == 0;
        let (verts, indices) = self.queued_counts(id);
        if !self.context_begin(n.context, verts, indices, svc) {
            return;
        }
        let camera_fade = b.flags & 0x40 != 0;
        let mut inv_node = Transform::from_words(IDENTITY);
        let mut camera_position = [0.0; 3];
        if camera_fade {
            inv_node = general_inverse(&Transform::from_words(n.world)).transform;
            camera_position = camera.position();
        }
        let mut block = frame_block(&n.texture.frame);
        flip_up(b.flags, &mut block);
        let scale = round(n.scale());
        let mut last_frame = -1;
        let mut cursor = n.head;
        while let Some(index) = cursor {
            let p = self.particle(&n, index);
            cursor = p.next;
            let inner = round(d(p.inv_duration) * d(p.age));
            let mut alpha = round(ev(b.size.as_ref(), n.outer, inner));
            let size_scale = round(ev(b.primary_size.as_ref(), n.outer, inner) * d(scale));
            let height = round(ev(b.tex_frame.as_ref(), n.outer, inner) * d(size_scale));
            let width = round(ev(b.axis_override.as_ref(), n.outer, inner) * d(size_scale));
            // `IsNonzeroParticleField`: zero (either sign) only.
            if alpha == 0.0 {
                continue;
            }
            if size_scale < TINY_SCALE {
                continue;
            }
            let mut position = p.pos;
            let m = match b.local_transform.as_ref() {
                Some(t) if !t.is_identity() => {
                    let m = t.evaluate(n.outer, inner);
                    position = [
                        round(d(m[9]) + d(position[0])),
                        round(d(position[1]) + d(m[10])),
                        round(d(position[2]) + d(m[11])),
                    ];
                    m
                }
                _ => IDENTITY,
            };
            if camera_fade {
                let (b1, b4, b7) = (d(m[1]), d(m[4]), d(m[7]));
                let (a0, a3, a6) = (m[0], m[3], d(m[6]));
                let normal = [
                    round(d(a3) * b7 - a6 * b4),
                    round(a6 * b1 - b7 * d(a0)),
                    round(b4 * d(a0) - d(a3) * b1),
                ];
                let local =
                    transform_xform(&inv_node, Vector::from_words(camera_position)).to_words();
                let mut direction = [
                    round(d(local[0]) - d(position[0])),
                    round(d(local[1]) - d(position[1])),
                    round(d(local[2]) - d(position[2])),
                ];
                let sq = ((d(direction[0]) * d(direction[0])) + d(direction[2]) * d(direction[2]))
                    + d(direction[1]) * d(direction[1]);
                let length = inv_sqrt_step(round(sq), sq) * sq;
                if !(length <= d(TINY_LENGTH)) {
                    let inverse = 1.0 / length;
                    direction = direction.map(|v| round(d(v) * inverse));
                } else {
                    direction = [0.0, 1.0, 0.0];
                }
                let sum = (d(normal[0]) * d(direction[0]) + d(direction[2]) * d(normal[2]))
                    + d(direction[1]) * d(normal[1]);
                let facing_value = 1.0 - sum.abs();
                let facing = round(facing_value);
                if facing_value > d(FACING_CULL) {
                    continue;
                }
                if facing > FACING_FADE {
                    let fade = (d(FACING_CULL) - d(facing)) * d(FACING_FADE_SCALE);
                    alpha = round((d(alpha) * fade) * fade);
                }
                if alpha == 0.0 {
                    continue;
                }
            }
            let diffuse = with_alpha(b.color.eval_packed(n.outer, inner), alpha);
            if frame_lookup {
                let frame = n.frame_of(ev(b.frame.as_ref(), n.outer, inner));
                if frame != last_frame {
                    last_frame = frame;
                    n.lookup(frame, &mut block, svc);
                    flip_down(b.flags, &mut block);
                }
            }
            let a = [
                round(d(m[0]) * d(width)),
                round(d(m[3]) * d(width)),
                round(d(m[6]) * d(width)),
            ];
            let bv = [
                round(d(m[1]) * d(height)),
                round(d(m[4]) * d(height)),
                round(d(m[7]) * d(height)),
            ];
            let minus_a: [f32; 3] = std::array::from_fn(|i| round(d(position[i]) - d(a[i])));
            let plus_a: [f32; 3] = std::array::from_fn(|i| round(d(a[i]) + d(position[i])));
            let c3: [f32; 3] = std::array::from_fn(|i| round(d(minus_a[i]) - d(bv[i])));
            let c2: [f32; 3] = std::array::from_fn(|i| round(d(plus_a[i]) - d(bv[i])));
            let c1: [f32; 3] = std::array::from_fn(|i| round(d(minus_a[i]) + d(bv[i])));
            let c0: [f32; 3] = std::array::from_fn(|i| round(d(plus_a[i]) + d(bv[i])));
            let context = &mut self.draw.contexts[n.context];
            let i0 = context.vertex([
                bits(c0[0]),
                bits(c0[1]),
                bits(c0[2]),
                diffuse,
                quiet(block[3]),
                quiet(block[2]),
            ]);
            let i1 = context.vertex([
                bits(c1[0]),
                bits(c1[1]),
                bits(c1[2]),
                diffuse,
                block[1],
                block[2],
            ]);
            let i2 = context.vertex([
                bits(c2[0]),
                bits(c2[1]),
                bits(c2[2]),
                diffuse,
                block[3],
                block[4],
            ]);
            let i3 = context.vertex([
                bits(c3[0]),
                bits(c3[1]),
                bits(c3[2]),
                diffuse,
                quiet(block[1]),
                quiet(block[4]),
            ]);
            context.list_quad(i0, i1, i2, i3);
        }
        if self.context_end(n.context, svc) == 0 {
            return;
        }
        self.draw.counters[COUNTER_DRAWS] = self.draw.counters[COUNTER_DRAWS].wrapping_add(1);
        self.blend_and_texture(id, svc);
        let view = camera.view_transform();
        svc.set_modelview(&mul(&view, &n.world));
        self.context_draw(n.context, svc);
    }
}

/// `LoadTriangleUvTable`: flag 1 picks `U {0, 0, 1}`, `V {1, 0, 1}`,
/// otherwise `U {0.5, 0, 1}`, `V {0, 1, 1}`.
fn triangle_uv(flags: u32) -> ([f32; 3], [f32; 3]) {
    if flags & 1 != 0 {
        ([0.0, 0.0, 1.0], [1.0, 0.0, 1.0])
    } else {
        ([0.5, 0.0, 1.0], [0.0, 1.0, 1.0])
    }
}

/// `MapTriangleUvTable`: the table into the frame's rectangle; the first
/// mapping rounds the V span to a float.
fn map_triangle_uv(block: &[u32; 5], u: &mut [f32; 3], v: &mut [f32; 3], initial: bool, map: bool) {
    let span_u = d(fw(block[3])) - d(fw(block[1]));
    let span_v_unrounded = d(fw(block[4])) - d(fw(block[2]));
    let span_v = if initial {
        d(round(span_v_unrounded))
    } else {
        span_v_unrounded
    };
    if !map {
        return;
    }
    for value in u.iter_mut() {
        *value = round(d(*value) * span_u + d(fw(block[1])));
    }
    for value in v.iter_mut() {
        *value = round(d(*value) * span_v + d(fw(block[2])));
    }
}
