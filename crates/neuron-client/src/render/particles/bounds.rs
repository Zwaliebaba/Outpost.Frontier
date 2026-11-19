//! Bounding spheres: each appearance's `GetBoundingSphere` (node `+0x8c`,
//! `particlefx_appearances.cpp`) and the effect's merge of them
//! (`FFxEffectObject::MergeNodeBoundsSphere`, `Slot15`).
//!
//! A billboard list's sphere starts at its first particle, merges the
//! tail, then every particle between in list order (Ritter's two-sphere
//! merge with the `0x5f3759df` length); the radius is the class's size
//! ramps at the particle's time (perp, rect, oriented and beam read the
//! first particle's time for the tail, as the reference does); the centre
//! goes to world space through the node's transform.

use neuron_core::fx_data::NodeClass;
use neuron_core::math::{Transform, Vector, transform_xform};

use super::ParticleFx;
use super::draw::GR_GENERIC;
use super::emitter::inv_sqrt_guess;
use super::fill::{d, ev};
use super::node::NodeId;
use super::pool::Particle;
use super::props::{BillboardProps, PropsData};
use super::ramp::round;
use super::system::pool_of;

/// `GetBoundingSphere`'s result for a node with no sphere of its own.
pub const NO_SPHERE: i32 = 0xFFFF_FFF9_u32.cast_signed();

/// The two length orders of the list merges.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Length {
    /// `(x*x + y*y) + z*z` kept unrounded.
    Xyz,
    /// `(y*y + z*z) + x*x` rounded to a float.
    Yzx,
}

/// `MergePerpRectBoundsSphere` (and the beam's twin): `sphere` grows to
/// hold `centre` with `radius`.
fn merge(sphere: &mut [f32; 4], centre: [f32; 3], radius: f32, length: Length) {
    let mut difference: [f32; 3] = std::array::from_fn(|i| round(d(centre[i]) - d(sphere[i])));
    let (length_sq, rounded_sq) = match length {
        Length::Xyz => {
            let sq = (d(difference[0]) * d(difference[0]) + d(difference[1]) * d(difference[1]))
                + d(difference[2]) * d(difference[2]);
            (sq, round(sq))
        }
        Length::Yzx => {
            let rounded = round(
                (d(difference[1]) * d(difference[1]) + d(difference[2]) * d(difference[2]))
                    + d(difference[0]) * d(difference[0]),
            );
            (d(rounded), rounded)
        }
    };
    let g = d(inv_sqrt_guess(rounded_sq));
    let inverse = (1.5 - ((g * g) * length_sq) * 0.5) * g;
    let distance = inverse * length_sq;
    let rounded = round(distance);
    if distance + d(sphere[3]) < d(radius) {
        *sphere = [centre[0], centre[1], centre[2], radius];
        return;
    }
    if d(radius) + d(rounded) < d(sphere[3]) {
        return;
    }
    let new_radius = round(((d(radius) + d(sphere[3])) + d(rounded)) * 0.5);
    if !(d(rounded) <= 1e-6) {
        let inverse_distance = 1.0 / d(rounded);
        difference = difference.map(|v| round(inverse_distance * d(v)));
        let step = round(d(new_radius) - d(sphere[3]));
        for (i, v) in difference.iter().enumerate() {
            let offset = round(d(step) * d(*v));
            sphere[i] = round(d(offset) + d(sphere[i]));
        }
    }
    sphere[3] = new_radius;
}

/// How a class sizes a particle's sphere.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Radius {
    /// Basic, perp: the size ramp times the scale.
    Size,
    /// Rect: the larger of the two side ramps (`b > c ? b : c`) times the
    /// scaled size.
    RectSides,
    /// Oriented: `c > b ? c : b`.
    OrientedSides,
}

impl ParticleFx {
    fn list_radius(b: &BillboardProps, outer: f32, inner: f32, scale: f32, kind: Radius) -> f32 {
        let primary = ev(b.primary_size.as_ref(), outer, inner) * d(scale);
        if kind == Radius::Size {
            return round(primary);
        }
        let primary = round(primary);
        let side_b = round(ev(b.tex_frame.as_ref(), outer, inner));
        let side_c = ev(b.axis_override.as_ref(), outer, inner);
        let side = if kind == Radius::RectSides {
            if d(side_b) > side_c {
                d(side_b)
            } else {
                side_c
            }
        } else if side_c > d(side_b) {
            side_c
        } else {
            d(side_b)
        };
        round(side * d(primary))
    }

    /// The node's `GetBoundingSphere(out)`: 0 with `out` written, else the
    /// node's error (`out` left alone, but for a mesh appearance, whose
    /// empty list still moves `out`'s centre through its transform).
    pub fn bounding_sphere(&self, id: NodeId, out: &mut [f32; 4]) -> i32 {
        let node = self.n(id);
        let class = node.class();
        let world = Transform::from_words(node.world);
        let to_world = |out: &mut [f32; 4]| {
            let c = transform_xform(&world, Vector::new(out[0], out[1], out[2])).to_words();
            out[..3].copy_from_slice(&c);
        };
        let Some(app) = node.appearance() else {
            return NO_SPHERE;
        };
        match class {
            NodeClass::MeshAppearance => {
                // No meshes in this increment: the list is empty.
                to_world(out);
                return GR_GENERIC;
            }
            NodeClass::ParticleAppearance => return GR_GENERIC,
            // Dust has no sphere of its own.
            NodeClass::DustAppearance => return NO_SPHERE,
            _ => {}
        }
        let PropsData::Billboard(b) = &node.props.data else {
            return NO_SPHERE;
        };
        let (Some(kind), Some(head), Some(tail)) = (pool_of(class), app.head, app.tail) else {
            return GR_GENERIC;
        };
        let pool = self.pools.get(kind);
        let item = |i: u32| -> Particle { *pool.item(i) };
        let outer = node.outer_time;
        let time = |p: &Particle| round(d(p.inv_duration) * d(p.age));
        let first = item(head);
        if class == NodeClass::BeamAppearance {
            let sphere_of = |p: &Particle, t: f32| {
                let primary = round(ev(b.primary_size.as_ref(), outer, t));
                let frame = ev(b.tex_frame.as_ref(), outer, t);
                let r = round((frame * d(primary)) * 0.5);
                [p.pos[0], p.pos[1], p.pos[2], r]
            };
            let t0 = round(d(first.age) * d(first.inv_duration));
            *out = sphere_of(&first, t0);
            if head != tail {
                let tail_sphere = sphere_of(&item(tail), t0);
                merge(
                    out,
                    [tail_sphere[0], tail_sphere[1], tail_sphere[2]],
                    tail_sphere[3],
                    Length::Xyz,
                );
                let mut cursor = first.next;
                while let Some(i) = cursor.filter(|&i| i != tail) {
                    let p = item(i);
                    let s = sphere_of(&p, time(&p));
                    merge(out, [s[0], s[1], s[2]], s[3], Length::Yzx);
                    cursor = p.next;
                }
            }
            to_world(out);
            return 0;
        }
        let (radius, tail_own_time, length) = match class {
            NodeClass::BasicAppearance => (Radius::Size, true, Length::Xyz),
            NodeClass::PerpAppearance => (Radius::Size, false, Length::Xyz),
            NodeClass::RectAppearance => (Radius::RectSides, false, Length::Yzx),
            NodeClass::OrientedAppearance => (Radius::OrientedSides, false, Length::Yzx),
            _ => return NO_SPHERE,
        };
        let scale = round(b.lod_curve.eval(outer, node.lod_input));
        let t0 = time(&first);
        *out = [
            first.pos[0],
            first.pos[1],
            first.pos[2],
            Self::list_radius(b, outer, t0, scale, radius),
        ];
        if head != tail {
            let tail_p = item(tail);
            let tail_time = if tail_own_time { time(&tail_p) } else { t0 };
            let r = Self::list_radius(b, outer, tail_time, scale, radius);
            merge(out, tail_p.pos, r, length);
            let mut cursor = first.next;
            while let Some(i) = cursor.filter(|&i| i != tail) {
                let p = item(i);
                let r = Self::list_radius(b, outer, time(&p), scale, radius);
                merge(out, p.pos, r, length);
                cursor = p.next;
            }
        }
        to_world(out);
        0
    }
}

/// `MergeNodeBoundsSphere`: the first sphere a record's node answers, then
/// every further one merged in; `false` when none answers (`sphere` keeps
/// what the failed calls left).
pub(crate) fn merge_node_spheres(
    fx: &ParticleFx,
    nodes: impl Iterator<Item = NodeId>,
    sphere: &mut [f32; 4],
) -> bool {
    let mut count = 0_u32;
    for id in nodes {
        if count == 0 {
            if fx.bounding_sphere(id, sphere) == 0 {
                count = 1;
            }
            continue;
        }
        let mut node_sphere = [0.0_f32; 4];
        if fx.bounding_sphere(id, &mut node_sphere) != 0 {
            continue;
        }
        count += 1;
        let dx = round(d(node_sphere[0]) - d(sphere[0]));
        let dy = round(d(node_sphere[1]) - d(sphere[1]));
        let dz_wide = d(node_sphere[2]) - d(sphere[2]);
        let dz = round(dz_wide);
        let dot = (dz_wide * d(dz) + d(dy) * d(dy)) + d(dx) * d(dx);
        let seed = d(inv_sqrt_guess(round(dot)));
        let correction = 1.5 - ((seed * seed) * dot) * 0.5;
        let distance = round((correction * seed) * dot);
        if d(distance) + d(sphere[3]) < d(node_sphere[3]) {
            *sphere = node_sphere;
        } else if !(d(node_sphere[3]) + d(distance) < d(sphere[3])) {
            let new_radius = ((d(node_sphere[3]) + d(sphere[3])) + d(distance)) * 0.5;
            if !(d(distance) <= 1e-6) {
                let reciprocal = 1.0 / d(distance);
                let normal_x = d(dx) * reciprocal;
                let normal_y = round(reciprocal * d(dy));
                let normal_z = round(d(dz) * reciprocal);
                let offset = new_radius - d(sphere[3]);
                let offset_stored = round(offset);
                let shift_x = offset * normal_x;
                let shift_y = d(normal_y) * d(offset_stored);
                let shift_z = round(d(normal_z) * d(offset_stored));
                sphere[0] = round(shift_x + d(sphere[0]));
                sphere[1] = round(shift_y + d(sphere[1]));
                sphere[2] = round(d(shift_z) + d(sphere[2]));
            }
            sphere[3] = round(new_radius);
        }
    }
    count != 0
}
