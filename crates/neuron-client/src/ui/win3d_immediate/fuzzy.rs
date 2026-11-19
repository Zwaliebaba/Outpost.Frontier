//! The fuzzy lines of the layer (`Win3DDrawFuzzyLine` and
//! `Win3DDrawFuzzyArc` of `win3d_immediate.cpp`), which the tractor beam
//! draws (`cli_tractor_beam.cpp`, the game's, P5): a jittered line or arc
//! in segments of about the record's width, as one line strip without a
//! texture, or one camera-facing textured quad per segment with additive
//! blending.
//!
//! The jitter is Common's `RandomVector` over the C runtime's `rand`; the
//! generator is the caller's ([`CrtRand`], divergence D513). Arithmetic
//! follows decision T11: `f64` evaluation, rounding at each store the
//! reference makes to a `float`; the trigonometry is the platform's.

use neuron_core::math::{Quaternion, Vector, quaternion_to_matrix, transform_matrix};
use neuron_core::num::trunc_to_i32;
use neuron_core::rng::CrtRand;

use super::{Win3dLayer, Win3dPipeline, capability, primitive};

/// `kFuzzyLineQuadScale`, `kFuzzyArcQuadScale`.
const LINE_QUAD_SCALE: f32 = 1.25;
const ARC_QUAD_SCALE: f32 = 1.2;
/// `kWin3DBlendOne`.
const BLEND_ONE: u32 = 2;

/// `FWin3DFuzzyLine`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FuzzyLine {
    /// `bCurved`: the arc about `control` instead of the straight line.
    pub curved: bool,
    /// The texture record (a shape's), `None` for a line strip.
    pub texture: Option<u32>,
    pub start: [f32; 3],
    /// The straight line's end; the arc ends at `control` plus
    /// `end - control` rescaled to the start's radius.
    pub end: [f32; 3],
    /// The arc's centre.
    pub control: [f32; 3],
    /// The camera position the textured quads turn towards.
    pub eye: [f32; 3],
    /// `Win3DColor3f`.
    pub color: [f32; 3],
    /// The segment length, and the arc's chord.
    pub width: f32,
    /// The jitter radius over the width.
    pub jitter_scale: f32,
    /// The straight line's side scale of each quad.
    pub side_scale: f32,
}

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

fn add(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
}

fn sub(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

/// `ScaleVec3`.
fn scale(v: [f32; 3], s: f32) -> [f32; 3] {
    [s * v[0], s * v[1], s * v[2]]
}

/// `FuzzyVecLength`: `(x*x + y*y) + z*z`, unrounded.
fn vec_length(v: [f32; 3]) -> f64 {
    (e(v[0]) * e(v[0]) + e(v[1]) * e(v[1]) + e(v[2]) * e(v[2])).sqrt()
}

/// `FuzzyInlineLength`: `(y*y + z*z) + x*x`, unrounded.
fn inline_length(v: [f32; 3]) -> f64 {
    (e(v[1]) * e(v[1]) + e(v[2]) * e(v[2]) + e(v[0]) * e(v[0])).sqrt()
}

/// `FuzzyCross`: `d x e`, each component rounded.
fn cross(d: [f32; 3], f: [f32; 3]) -> [f32; 3] {
    [
        r(e(f[2]) * e(d[1]) - e(f[1]) * e(d[2])),
        r(e(d[2]) * e(f[0]) - e(f[2]) * e(d[0])),
        r(e(f[1]) * e(d[0]) - e(d[1]) * e(f[0])),
    ]
}

/// `FuzzyRescale`: `v` scaled to the length `wanted`.
fn rescale(v: [f32; 3], wanted: f32) -> [f32; 3] {
    let s = e(wanted) / vec_length(v);
    [r(e(v[0]) * s), r(e(v[1]) * s), r(e(v[2]) * s)]
}

/// Common's `RandomVector(radius)`: three `rand() - 0x3fff` draws scaled to
/// the radius (no rejection; all three equal divides by zero).
#[must_use]
pub fn random_vector(radius: f32, rand: &mut CrtRand) -> [f32; 3] {
    let x = (rand.rand() - 0x3FFF) as f32;
    let y = (rand.rand() - 0x3FFF) as f32;
    let z = f64::from(rand.rand() - 0x3FFF);
    let s = e(radius) / (z * z + e(y) * e(y) + e(x) * e(x)).sqrt();
    [r(e(x) * s), r(e(y) * s), r(s * z)]
}

/// `FuzzyJitter`.
fn jitter(p: [f32; 3], radius: f32, rand: &mut CrtRand) -> [f32; 3] {
    add(p, random_vector(radius, rand))
}

/// `FuzzyEmitQuad`: one textured quad, `mid` plus and minus `along` and
/// `side`.
fn emit_quad(
    layer: &mut Win3dLayer,
    pipe: &mut dyn Win3dPipeline,
    handle: u32,
    mid: [f32; 3],
    along: [f32; 3],
    side: [f32; 3],
) {
    layer.bind_texture(handle);
    layer.begin(primitive::QUADS);
    for (uv, corner) in [
        ((0.0, 0.0), add(mid, along)),
        ((1.0, 0.0), add(mid, side)),
        ((1.0, 1.0), sub(mid, along)),
        ((0.0, 1.0), sub(mid, side)),
    ] {
        layer.tex_coord(uv.0, uv.1);
        layer.vertex(corner[0], corner[1], corner[2]);
    }
    layer.end(pipe);
}

/// The side vector of a quad from `cur` to `next` and its midpoint, the
/// side rescaled to `half` times `|mid - cur|`.
fn side_and_mid(
    cur: [f32; 3],
    next: [f32; 3],
    eye: [f32; 3],
    half: Option<f64>,
    mid_first: bool,
) -> ([f32; 3], [f32; 3]) {
    let side = cross(sub(next, cur), sub(eye, cur));
    let half_scaled = scale(sub(next, cur), 0.5);
    let mid = if mid_first {
        add(half_scaled, cur)
    } else {
        add(cur, half_scaled)
    };
    let len = inline_length(sub(mid, cur));
    let wanted = r(half.map_or(len, |h| len * h));
    (rescale(side, wanted), mid)
}

impl Win3dLayer {
    /// `Win3DDrawFuzzyLine` (the arc's record goes to
    /// [`Win3dLayer::draw_fuzzy_arc`]). `frame_handle` is
    /// `GetTextureFrame(texture, 0)`'s handle.
    pub fn draw_fuzzy_line(
        &mut self,
        pipe: &mut dyn Win3dPipeline,
        line: &FuzzyLine,
        rand: &mut CrtRand,
        frame_handle: &mut dyn FnMut(u32) -> u32,
    ) {
        if line.curved {
            self.draw_fuzzy_arc(pipe, line, rand, frame_handle);
            return;
        }
        let dir = sub(line.end, line.start);
        let length_ext = inline_length(dir);
        let length = r(length_ext);
        let count = trunc_to_i32((length_ext / e(line.width)).floor() + 1.0);
        let step = r(e(length) / f64::from(count));
        let unit = rescale(dir, step);
        self.color3f(line.color[0], line.color[1], line.color[2]);
        let radius = line.jitter_scale * line.width;
        let Some(texture) = line.texture else {
            self.disable(pipe, capability::TEXTURE_2D);
            self.disable(pipe, capability::BLEND);
            self.begin(primitive::LINE_STRIP);
            self.vertex(line.start[0], line.start[1], line.start[2]);
            for index in 1..count {
                let s = f64::from(index);
                let step_y = r(e(unit[1]) * s);
                let step_z = r(e(unit[2]) * s);
                let on_line = [
                    r(e(unit[0]) * s + e(line.start[0])),
                    step_y + line.start[1],
                    step_z + line.start[2],
                ];
                let p = jitter(on_line, radius, rand);
                self.vertex(p[0], p[1], p[2]);
            }
            self.vertex(line.end[0], line.end[1], line.end[2]);
            self.end(pipe);
            return;
        };
        let handle = frame_handle(texture);
        self.enable(pipe, capability::BLEND);
        self.set_blend_func(pipe, BLEND_ONE, BLEND_ONE);
        let mut cur = line.start;
        for index in 1..=count {
            let next = if index == count {
                line.end
            } else {
                #[allow(clippy::cast_precision_loss)]
                let offset = scale(unit, index as f32);
                jitter(add(line.start, offset), radius, rand)
            };
            let (side, mid) = side_and_mid(cur, next, line.eye, Some(0.5), false);
            let along = scale(sub(cur, mid), LINE_QUAD_SCALE);
            let side_quad = scale(scale(side, LINE_QUAD_SCALE), line.side_scale);
            emit_quad(self, pipe, handle, mid, along, side_quad);
            cur = next;
        }
    }

    /// `Win3DDrawFuzzyArc`: the arc about `control` from `start` through
    /// the angle to `end`, stepped by quaternion turns of `start -
    /// control`; nothing when either radius is under 0.001.
    #[allow(clippy::too_many_lines)]
    pub fn draw_fuzzy_arc(
        &mut self,
        pipe: &mut dyn Win3dPipeline,
        line: &FuzzyLine,
        rand: &mut CrtRand,
        frame_handle: &mut dyn FnMut(u32) -> u32,
    ) {
        let from_centre = sub(line.start, line.control);
        let to_end = sub(line.end, line.control);
        let radius_ext = inline_length(from_centre);
        let radius = r(radius_ext);
        if radius_ext < 0.001 || inline_length(to_end) < 0.001 {
            return;
        }
        let jitter_radius = line.jitter_scale * line.width;
        let to_end = rescale(to_end, radius);
        let radius_sq = radius * radius;
        let cosine_ext = (e(to_end[2]) * e(from_centre[2])
            + e(to_end[1]) * e(from_centre[1])
            + e(to_end[0]) * e(from_centre[0]))
            / e(radius_sq);
        let mut cosine = r(cosine_ext);
        if cosine_ext < -1.0 {
            cosine = -1.0;
        } else if cosine > 1.0 {
            cosine = 1.0;
        }
        let angle = r(e(cosine).acos());
        let width = e(line.width);
        // NaN stays NaN, as the reference's ordered compares leave it.
        let chord = r(1.0 - width * width / (e(radius_sq) + e(radius_sq))).clamp(-1.0, 1.0);
        let count = trunc_to_i32((e(angle) / e(chord).acos()).floor() + 1.0);
        let step = r(e(angle) / f64::from(count));
        let axis = cross(from_centre, to_end);
        let source = Vector::new(from_centre[0], from_centre[1], from_centre[2]);
        let turn = |w: f32, factor: f32| -> [f32; 3] {
            let v = scale(axis, factor);
            let m = quaternion_to_matrix(&Quaternion::new(w, v[0], v[1], v[2]));
            let t = transform_matrix(&m, source);
            [t.x, t.y, t.z]
        };
        let Some(texture) = line.texture else {
            self.disable(pipe, capability::TEXTURE_2D);
            self.disable(pipe, capability::BLEND);
            self.color3f(line.color[0], line.color[1], line.color[2]);
            self.begin(primitive::LINE_STRIP);
            self.vertex(line.start[0], line.start[1], line.start[2]);
            for index in 1..count {
                let half = f64::from(index) * e(step) * 0.5;
                let axis_len =
                    (e(axis[0]) * e(axis[0]) + e(axis[2]) * e(axis[2]) + e(axis[1]) * e(axis[1]))
                        .sqrt();
                let rotated = turn(r(half.cos()), r(half.sin() / axis_len));
                let on_arc = add(rotated, line.control);
                let p = jitter(on_arc, jitter_radius, rand);
                self.vertex(p[0], p[1], p[2]);
            }
            let last = add(to_end, line.control);
            self.vertex(last[0], last[1], last[2]);
            self.end(pipe);
            return;
        };
        let handle = frame_handle(texture);
        self.enable(pipe, capability::BLEND);
        self.set_blend_func(pipe, BLEND_ONE, BLEND_ONE);
        let mut cur = line.start;
        for index in 1..=count {
            let next = if index == count {
                add(line.control, to_end)
            } else {
                let half_ext = f64::from(index) * e(step) * 0.5;
                let half = r(half_ext);
                let sine = r(half_ext.sin());
                let rotated = turn(r(e(half).cos()), r(e(sine) / vec_length(axis)));
                jitter(add(line.control, rotated), jitter_radius, rand)
            };
            let (side, mid) = side_and_mid(cur, next, line.eye, None, true);
            let along = scale(sub(cur, mid), ARC_QUAD_SCALE);
            let side_quad = scale(side, ARC_QUAD_SCALE);
            emit_quad(self, pipe, handle, mid, along, side_quad);
            cur = next;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ui::win3d_immediate::{PipeCall, RecordingPipeline};

    fn line(texture: Option<u32>) -> FuzzyLine {
        FuzzyLine {
            curved: false,
            texture,
            start: [0.0, 0.0, -10.0],
            end: [10.0, 0.0, -10.0],
            control: [0.0; 3],
            eye: [0.0, 0.0, 0.0],
            color: [1.0, 0.5, 0.25],
            width: 2.5,
            jitter_scale: 0.1,
            side_scale: 1.0,
        }
    }

    #[test]
    fn an_untextured_line_is_one_strip_of_count_plus_one_points() {
        let mut layer = Win3dLayer::new(640, 480);
        let mut pipe = RecordingPipeline::default();
        let mut rand = CrtRand::default();
        layer.draw_fuzzy_line(&mut pipe, &line(None), &mut rand, &mut |t| t);
        let draws: Vec<_> = pipe
            .calls
            .iter()
            .filter_map(|c| match c {
                PipeCall::Draw(prim, v) => Some((*prim, v.len())),
                _ => None,
            })
            .collect();
        // 10 / 2.5 = 4, plus 1: five segments, six points.
        assert_eq!(draws, [(primitive::LINE_STRIP, 6)]);
    }

    #[test]
    fn a_textured_line_draws_a_quad_per_segment() {
        let mut layer = Win3dLayer::new(640, 480);
        let mut pipe = RecordingPipeline::default();
        let mut rand = CrtRand::default();
        layer.draw_fuzzy_line(&mut pipe, &line(Some(7)), &mut rand, &mut |t| t + 0x100);
        let quads = pipe
            .calls
            .iter()
            .filter(|c| matches!(c, PipeCall::DrawIndexed(..)))
            .count();
        assert_eq!(quads, 5);
    }

    #[test]
    fn a_degenerate_arc_draws_nothing() {
        let mut layer = Win3dLayer::new(640, 480);
        let mut pipe = RecordingPipeline::default();
        let mut rand = CrtRand::default();
        let mut l = line(None);
        l.curved = true;
        l.control = l.start;
        layer.draw_fuzzy_line(&mut pipe, &l, &mut rand, &mut |t| t);
        assert!(pipe.calls.is_empty());
    }
}
