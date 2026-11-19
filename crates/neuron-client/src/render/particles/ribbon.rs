//! The dust and beam draws (`FLDustAppearance::DrawOrEnqueue`,
//! `FLBeamAppearance::FillRibbon`, `EmitRing`, `JoinRing`,
//! `EvalRingParams`, `PackRingColor` and `ApplyDrawState` in
//! `particlefx_appearances.cpp`).
//!
//! The dust draws camera-space streaks from each particle's last
//! camera-space position to its current one, faded by the camera's speed
//! and by a jerk in it; the camera transform it composes is the one its
//! next spawns record. The beam draws a ribbon of two rails through its
//! particles, from a tip ahead of the first one (a beam field's position,
//! or the first segment extrapolated), with an optional cap ring at the
//! tip.

use neuron_core::math::{Matrix, Transform, Vector, transform_matrix, transform_xform};
use neuron_core::num::trunc_to_i32_via_i64;

use super::ParticleFx;
use super::draw::{COUNTER_DRAWS, RS_FILLMODE};
use super::fill::{bits, d, ev, flip_up, frame_block, inv_sqrt_step, quiet, with_alpha};
use super::node::NodeId;
use super::pool::Particle;
use super::ramp::{IDENTITY, mul, round};
use super::services::{Camera, ParticleDraw};
use neuron_core::fx_data::NodeClass;

fn matrix_of(t: &[f32; 12]) -> Matrix {
    Matrix::from_words(std::array::from_fn(|i| t[i]))
}

fn sub(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    std::array::from_fn(|i| round(d(a[i]) - d(b[i])))
}

fn add(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    std::array::from_fn(|i| round(d(a[i]) + d(b[i])))
}

fn scaled(v: [f32; 3], s: f32) -> [f32; 3] {
    std::array::from_fn(|i| round(d(s) * d(v[i])))
}

/// `BeamLongSegmentXyz`: `(x*x + y*y) + z*z` past the float `0.01`.
fn long_xyz(v: [f32; 3]) -> bool {
    ((d(v[0]) * d(v[0]) + d(v[1]) * d(v[1])) + d(v[2]) * d(v[2])) > d(0.01_f32)
}

/// `BeamLongSegmentXzy`: `(x*x + z*z) + y*y`.
fn long_xzy(v: [f32; 3]) -> bool {
    ((d(v[0]) * d(v[0]) + d(v[2]) * d(v[2])) + d(v[1]) * d(v[1])) > d(0.01_f32)
}

/// `PackRingColor({r, g, b, a})`: `ftol(a * 255)`, then each channel's
/// byte shifted in; alpha is not masked.
fn pack_ring(rgba: [f32; 4]) -> u32 {
    let byte = |v: f32| trunc_to_i32_via_i64(d(v) * 255.0).cast_unsigned();
    let mut packed = byte(rgba[3]);
    for channel in [rgba[0], rgba[1], rgba[2]] {
        packed = (packed << 8) | (byte(channel) & 0xFF);
    }
    packed
}

/// The ribbon state (`FBeamRingState`): the last ring's four indices
/// (`0xffff` before the first), the rings made, the camera-space basis and
/// the last ring's position.
struct Ring {
    indices: [u16; 4],
    counter: i32,
    xform: [f32; 12],
    previous: [f32; 3],
}

/// What a ring needs of the node.
struct RingNode {
    props: std::rc::Rc<super::props::NodeProps>,
    outer: f32,
    lod: f32,
    parity: bool,
    frame: [u32; 5],
    flags: u32,
    context: usize,
}

impl RingNode {
    fn billboard(&self) -> &super::props::BillboardProps {
        match &self.props.data {
            super::props::PropsData::Billboard(b) => b,
            _ => unreachable!("a billboard appearance"),
        }
    }

    /// The common head of `EvalRingParams` and `EmitRing`: half width,
    /// colour with alpha, and the rail's V by the ring's parity.
    fn params(&self, ring: &Ring, p: &Particle) -> (f32, [f32; 4], u32) {
        let b = self.billboard();
        let t = round(d(p.inv_duration) * d(p.age));
        let scale = round(b.lod_curve.eval(self.outer, self.lod));
        let size_scale = round(ev(b.primary_size.as_ref(), self.outer, t) * d(scale));
        let half_width = round((ev(b.tex_frame.as_ref(), self.outer, t) * d(size_scale)) * 0.5);
        let alpha = round(ev(b.size.as_ref(), self.outer, t));
        let rgb = b.color.eval_rgb(self.outer, t);
        let v = if (ring.counter & 1) == i32::from(self.parity) {
            self.frame[2]
        } else {
            self.frame[4]
        };
        (half_width, [rgb[0], rgb[1], rgb[2], alpha], v)
    }
}

impl ParticleFx {
    /// `FLDustAppearance::DrawOrEnqueue`.
    #[allow(clippy::too_many_lines)]
    pub(crate) fn draw_dust(
        &mut self,
        id: NodeId,
        camera: &mut dyn Camera,
        svc: &mut dyn ParticleDraw,
    ) {
        const FADE_NEAR: f32 = 100.0;
        const FADE_FAR: f32 = 1200.0;
        const JERK_LOW: f32 = 20.0;
        const JERK_HIGH: f32 = 35.0;
        const STREAK_SCALE: f32 = f32::from_bits(0x3D0F_5C29);
        const STREAK_MAX: f32 = 100.0;
        const TAIL_SCALE: f32 = f32::from_bits(0x3F26_6666);
        const HEAD_SCALE: f32 = f32::from_bits(0x3FC4_EC4F);
        const MIN_ALPHA: f32 = f32::from_bits(0x3C23_D70A);
        self.draw.counters[8] = self.draw.counters[8].wrapping_add(1);
        let n = self.fill_node(id);
        if n.head.is_none() {
            return;
        }
        let b = n.billboard();
        let view = camera.view_transform();
        let view_xform = mul(&view, &n.world);
        self.app_mut(id).view_xform = view_xform;
        let (verts, indices) = self.queued_counts(id);
        let camera_position = camera.position();
        let app = self.app(id);
        let (last_pos, last_step, last_dt) =
            (app.last_camera_pos, app.last_camera_step, app.last_dt);
        let delta = sub(camera_position, last_pos);
        let step = transform_matrix(&matrix_of(&view), Vector::from_words(delta)).to_words();
        let length_sq =
            (d(step[0]) * d(step[0]) + d(step[1]) * d(step[1])) + d(step[2]) * d(step[2]);
        let length = length_sq.sqrt();
        let step_length = round(length);
        let speed = length / d(last_dt);
        let mut fade = if speed < d(FADE_NEAR) {
            let rise = 1.0 - speed / d(FADE_NEAR);
            round(1.0 - rise * rise)
        } else if speed < d(FADE_FAR) {
            let fall = (speed - d(FADE_NEAR)) / (d(FADE_FAR) - d(FADE_NEAR));
            round(1.0 - fall * fall)
        } else {
            0.0
        };
        let jerk_value = (d(last_step) - d(step_length)).abs();
        let jerk = round(jerk_value);
        if jerk_value > d(JERK_LOW) {
            if !(jerk < JERK_HIGH) {
                fade = 0.0;
            } else {
                fade =
                    round((1.0 - (d(jerk) - d(JERK_LOW)) / (d(JERK_HIGH) - d(JERK_LOW))) * d(fade));
            }
        }
        if self.context_begin(n.context, verts, indices, svc) {
            let mut block = frame_block(&n.texture.frame);
            flip_up(b.flags, &mut block);
            let (u0, v0, u1, v1) = (block[1], block[2], block[3], block[4]);
            let view_world = Transform::from_words(view_xform);
            let mut cursor = n.head;
            while let Some(index) = cursor {
                let p = self.particle(&n, index);
                cursor = p.next;
                let last_view = p.extra;
                let c = transform_xform(&view_world, Vector::from_words(p.pos)).to_words();
                if !(c[2] > -1.0) && !(last_view[2] > -1.0) {
                    let inner = round(d(p.inv_duration) * d(p.age));
                    let alpha = round(ev(b.size.as_ref(), n.outer, inner) * d(fade));
                    let half = round(ev(b.primary_size.as_ref(), n.outer, inner) * 0.5);
                    let mz = round(d(c[2]) - d(last_view[2]));
                    let my = round(d(c[1]) - d(last_view[1]));
                    let mx = round(d(c[0]) - d(last_view[0]));
                    let nx = d(c[1]) * d(mz) - d(c[2]) * d(my);
                    let ny = d(c[2]) * d(mx) - d(c[0]) * d(mz);
                    let nz = d(c[0]) * d(my) - d(c[1]) * d(mx);
                    let mut ux = round(d(c[2]) * ny - d(c[1]) * nz);
                    let mut uy = round(d(c[0]) * nz - d(c[2]) * nx);
                    let uz_unrounded = nx * d(c[1]) - d(c[0]) * ny;
                    let length_sq =
                        round((d(uy) * d(uy) + d(ux) * d(ux)) + uz_unrounded * uz_unrounded);
                    let inverse = inv_sqrt_step(length_sq, d(length_sq));
                    ux = round(d(ux) * inverse);
                    uy = round(d(uy) * inverse);
                    let mut uz = round(uz_unrounded * inverse);
                    let smz = round(d(mz) * d(STREAK_SCALE));
                    let smy = round(d(my) * d(STREAK_SCALE));
                    let smx = round(d(mx) * d(STREAK_SCALE));
                    let inv_dt = 1.0 / d(last_dt);
                    let wz = round(d(smz) * inv_dt);
                    let wy = round(d(smy) * inv_dt);
                    let wx = round(d(smx) * inv_dt);
                    let mut projection = ((d(wy) * d(uy) + d(wx) * d(ux)) + d(wz) * d(uz)) * 0.5;
                    if projection < 0.025 {
                        ux = 0.0;
                        uy = 1.0;
                        uz = 0.0;
                    }
                    if projection > d(STREAK_MAX) {
                        projection = d(STREAK_MAX);
                    }
                    let reach = projection + d(half);
                    let az = round(d(uz) * reach);
                    let ay = round(d(uy) * reach);
                    let ax = round(reach * d(ux));
                    let bz = round(d(az) * d(TAIL_SCALE));
                    let by = round(d(ay) * d(TAIL_SCALE));
                    let bx = round(d(ax) * d(TAIL_SCALE));
                    let dz = round(d(az) * d(HEAD_SCALE));
                    let dy = round(d(ay) * d(HEAD_SCALE));
                    let dx = round(d(ax) * d(HEAD_SCALE));
                    let side_x = round(-d(uy) * d(half));
                    let side_y = round(d(ux) * d(half));
                    if alpha > MIN_ALPHA {
                        let diffuse = with_alpha(b.color.eval_packed(n.outer, inner), alpha);
                        let context = &mut self.draw.contexts[n.context];
                        let i0 = context.vertex([
                            bits(round(d(c[0]) - d(dx))),
                            bits(round(d(c[1]) - d(dy))),
                            bits(round(d(c[2]) - d(dz))),
                            diffuse,
                            quiet(u1),
                            quiet(v0),
                        ]);
                        let i1 = context.vertex([
                            bits(round(d(c[0]) + d(side_x))),
                            bits(round(d(c[1]) + d(side_y))),
                            bits(c[2]),
                            diffuse,
                            quiet(u1),
                            quiet(v1),
                        ]);
                        let i2 = context.vertex([
                            bits(round(d(c[0]) - d(side_x))),
                            bits(round(d(c[1]) - d(side_y))),
                            bits(c[2]),
                            diffuse,
                            quiet(u0),
                            quiet(v0),
                        ]);
                        let i3 = context.vertex([
                            bits(round(d(c[0]) + d(bx))),
                            bits(round(d(c[1]) + d(by))),
                            bits(round(d(c[2]) + d(bz))),
                            diffuse,
                            quiet(u0),
                            quiet(v1),
                        ]);
                        context.list_quad(i0, i1, i2, i3);
                    }
                }
                self.pools.get_mut(n.kind).item_mut(index).extra = c;
            }
            if self.context_end(n.context, svc) != 0 {
                self.draw.counters[COUNTER_DRAWS] =
                    self.draw.counters[COUNTER_DRAWS].wrapping_add(1);
                self.blend_and_texture(id, svc);
                svc.set_modelview(&IDENTITY);
                self.context_draw(n.context, svc);
            }
        }
        let app = self.app_mut(id);
        app.last_camera_pos = camera_position;
        app.last_camera_step = step_length;
    }

    /// `FLBeamAppearance::ApplyDrawState`: the blend states, the frame's
    /// texture (none and wireframe with flag `0x400`, else solid), the fill
    /// mode set around the modelview, restored before the draw.
    pub(crate) fn beam_draw_state(
        &mut self,
        id: NodeId,
        camera: &mut dyn Camera,
        svc: &mut dyn ParticleDraw,
    ) {
        let node = self.n(id);
        let (blend, flags) = match &node.props.data {
            super::props::PropsData::Billboard(b) => (b.blend_info, b.flags),
            _ => ([0, 0], 0),
        };
        svc.set_render_state(super::draw::RS_SRCBLEND, blend[0]);
        svc.set_render_state(super::draw::RS_DESTBLEND, blend[1]);
        let (mode, texture) = if flags & 0x400 == 0x400 {
            (2, 0)
        } else {
            (3, self.app(id).texture.frame.handle)
        };
        svc.set_stage_texture(0, texture);
        let saved = svc.render_state(RS_FILLMODE);
        svc.set_render_state(RS_FILLMODE, mode);
        let view = camera.view_transform();
        svc.set_modelview(&mul(&view, &self.n(id).world));
        svc.set_render_state(RS_FILLMODE, saved);
    }

    fn ring_vertex(
        &mut self,
        context: usize,
        pos: [f32; 3],
        diffuse: u32,
        u: u32,
        v: u32,
        convert: bool,
    ) -> u16 {
        let words = if convert {
            [
                quiet(bits(pos[0])),
                quiet(bits(pos[1])),
                quiet(bits(pos[2])),
                diffuse,
                quiet(u),
                quiet(v),
            ]
        } else {
            [bits(pos[0]), bits(pos[1]), bits(pos[2]), diffuse, u, v]
        };
        self.draw.contexts[context].vertex(words)
    }

    /// `JoinRing`: with flag 2 a centre vertex and eight triangles fan the
    /// segment, otherwise two quads join the rails.
    fn join_ring(&mut self, n: &RingNode, ring: &Ring, pos: [f32; 3], diffuse: u32, new: [u16; 4]) {
        let old = ring.indices;
        if n.flags & 2 == 2 {
            let middle = add(pos, ring.previous).map(|v| round(d(v) * 0.5));
            let half = bits(0.5);
            let m = self.ring_vertex(n.context, middle, diffuse, half, half, true);
            let context = &mut self.draw.contexts[n.context];
            for (a, b) in [
                (old[0], old[1]),
                (old[1], new[1]),
                (new[1], new[0]),
                (new[0], old[0]),
                (old[2], old[3]),
                (old[3], new[3]),
                (new[3], new[2]),
                (new[2], old[2]),
            ] {
                context.triangle(a, b, m);
            }
        } else {
            let context = &mut self.draw.contexts[n.context];
            context.quad(old[0], old[1], new[0], new[1]);
            context.quad(old[2], old[3], new[2], new[3]);
        }
    }

    /// `EmitRing(state, particle, pos)`: the ring's four vertices on the two
    /// rails of the basis, joined to the last ring.
    fn emit_ring(&mut self, n: &RingNode, ring: &mut Ring, p: &Particle, pos: [f32; 3]) {
        let (half_width, rgba, v) = n.params(ring, p);
        let diffuse = pack_ring(rgba);
        let x = ring.xform;
        let column = [x[0], x[3], x[6]].map(|w| f32::from_bits(quiet(bits(w))));
        let side = scaled(column, half_width);
        let mut new = [0_u16; 4];
        new[0] = self.ring_vertex(n.context, sub(pos, side), diffuse, n.frame[1], v, true);
        new[1] = self.ring_vertex(n.context, add(pos, side), diffuse, n.frame[3], v, true);
        let side = [x[1], x[4], x[7]].map(|w| round(d(w) * d(half_width)));
        new[2] = self.ring_vertex(n.context, sub(pos, side), diffuse, n.frame[1], v, true);
        new[3] = self.ring_vertex(n.context, add(pos, side), diffuse, n.frame[3], v, true);
        if ring.indices[0] != 0xFFFF {
            self.join_ring(n, ring, pos, diffuse, new);
        }
        ring.indices = new;
        ring.previous = pos;
        ring.counter = ring.counter.wrapping_add(1);
    }

    /// `FLBeamAppearance::FillRibbon`.
    pub(crate) fn fill_ribbon(
        &mut self,
        id: NodeId,
        camera: &mut dyn Camera,
        svc: &mut dyn ParticleDraw,
    ) {
        let _ = svc;
        let f = self.fill_node(id);
        self.draw.counters[7] = self.draw.counters[7].wrapping_add(1);
        let Some(head_index) = f.head else {
            return;
        };
        let head = self.particle(&f, head_index);
        let flags = f.billboard().flags;
        let mut tip = [0.0_f32; 3];
        let mut have_tip = false;
        let mut field_tip = false;
        let fields = self.app(id).fields.clone();
        // A null field slot is a pointer the reference reads through (D379).
        for field in fields.into_iter().flatten() {
            let Some(node) = self.node(field) else {
                continue;
            };
            if node.class() != NodeClass::BeamField {
                continue;
            }
            let negated = [f.world[9], f.world[10], f.world[11]].map(|v| -v);
            tip = transform_xform(
                &Transform::from_words(node.world),
                Vector::from_words(negated),
            )
            .to_words();
            if long_xyz(sub(head.pos, tip)) {
                have_tip = true;
                field_tip = true;
            }
            break;
        }
        if !field_tip {
            if self.app(id).live.cast_signed() < 2 {
                return;
            }
            // A list shorter than its count is not followed (D379).
            let Some(next) = head.next.map(|i| self.particle(&f, i)) else {
                return;
            };
            if flags & 0x200 != 0x200 {
                let difference = sub(next.pos, head.pos);
                if long_xyz(difference) {
                    let k = round(d(head.age) / (d(next.age) - d(head.age)));
                    tip = sub(head.pos, scaled(difference, k));
                    have_tip = true;
                }
            }
        }
        let app = self.app(id);
        let n = RingNode {
            props: std::rc::Rc::clone(&f.props),
            outer: f.outer,
            lod: f.lod,
            parity: app.ribbon_parity,
            frame: frame_block(&app.texture.frame),
            flags,
            context: f.context,
        };
        let mut ring = Ring {
            indices: [0xFFFF, 0, 0, 0],
            counter: 0,
            xform: mul(&camera.orientation_block(), &f.world),
            previous: [0.0; 3],
        };
        let mut lead_pos = head.pos;
        let mut tip_no_cap = false;
        if have_tip {
            if flags & 0x100 == 0x100 {
                let (half_width, rgba, v) = n.params(&ring, &head);
                let rgba = [
                    f32::from_bits(quiet(bits(rgba[0]))),
                    rgba[1],
                    rgba[2],
                    rgba[3],
                ];
                let diffuse = pack_ring(rgba);
                let x = ring.xform;
                let mut new = [0_u16; 4];
                let column = [x[0], x[3], x[6]].map(|w| f32::from_bits(quiet(bits(w))));
                let side = scaled(column, half_width);
                new[0] = self.ring_vertex(n.context, sub(tip, side), diffuse, n.frame[1], v, false);
                new[1] = self.ring_vertex(n.context, add(tip, side), diffuse, n.frame[3], v, false);
                let column = [x[1], x[4], x[7]].map(|w| f32::from_bits(quiet(bits(w))));
                let side = scaled(column, half_width);
                new[2] = self.ring_vertex(n.context, sub(tip, side), diffuse, n.frame[1], v, false);
                new[3] = self.ring_vertex(n.context, add(tip, side), diffuse, n.frame[3], v, false);
                ring.previous = tip;
                ring.indices = new;
                ring.counter = ring.counter.wrapping_add(1);
            } else {
                tip_no_cap = true;
                lead_pos = tip;
            }
        }
        self.emit_ring(&n, &mut ring, &head, lead_pos);
        let mut lead = head.next;
        let mut lag = if tip_no_cap { Some(head_index) } else { lead };
        let mut last = lead_pos;
        while let Some(lead_index) = lead {
            let lead_p = self.particle(&f, lead_index);
            let Some(lag_index) = lag else {
                break;
            };
            let lag_p = self.particle(&f, lag_index);
            if long_xzy(sub(lag_p.pos, last)) {
                self.emit_ring(&n, &mut ring, &lead_p, lag_p.pos);
                last = lag_p.pos;
            }
            lead = lead_p.next;
            lag = lag_p.next;
        }
    }
}
