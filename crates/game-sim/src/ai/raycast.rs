//! The AI's ray, box, capsule and sphere tests (`Common/common_airaycast.cpp`,
//! declared in `include/common_aisteer.h`): pure geometry on the 53-bit
//! model, each value rounded where the reference stores a `float`.
//!
//! The functions answer what the reference answers for every input,
//! including the unordered (NaN) cases, which the x87 comparisons send to
//! one fixed side; the comments name that side where it is not obvious.
//! Two baseline paths read uninitialised stack memory; the C++ reference
//! zero-fills those points and so does the port.

use neuron_core::math::{Matrix, Transform, Vector, transform_xform};

use super::fp::{
    add, column0, column1, column2, dot_zyx, e, fast_inv_sqrt, inline_fast_length, r, sub,
};

/// An oriented box (`OrientedBox` in `common_ini.h`): centre, three axes
/// (not necessarily unit length) and the half extents along them.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct OrientedBox {
    /// +0x00 the centre.
    pub center: Vector,
    /// +0x0c the first axis.
    pub axis_x: Vector,
    /// +0x18 the second axis.
    pub axis_y: Vector,
    /// +0x24 the third axis.
    pub axis_z: Vector,
    /// +0x30 the half extents.
    pub half_extent: Vector,
}

/// A local box as `{min_x, max_x, min_y, max_y, min_z, max_z}`, the shape
/// `TransformedBoxesIntersect` and `RayTransformedBoxNearestHit` take.
pub type AabbMinMax = [f32; 6];

/// The 15-axis separating-axis test of two oriented boxes (`OBBIntersect`).
#[must_use]
pub fn obb_intersect(a: &OrientedBox, b: &OrientedBox) -> bool {
    let (ax, ay, az) = (a.axis_x, a.axis_y, a.axis_z);
    let (bx, by, bz) = (b.axis_x, b.axis_y, b.axis_z);
    let ah = a.half_extent;
    let bh = b.half_extent;

    let tx = r(e(b.center.x) - e(a.center.x));
    let ty = r(e(b.center.y) - e(a.center.y));
    let tz = r(e(b.center.z) - e(a.center.z));

    let r00 = r((e(ax.z) * e(bx.z) + e(bx.x) * e(ax.x)) + e(ax.y) * e(bx.y));
    let r01 = r((e(by.z) * e(ax.z) + e(by.y) * e(ax.y)) + e(by.x) * e(ax.x));
    let r02 = r((e(bz.z) * e(ax.z) + e(bz.y) * e(ax.y)) + e(bz.x) * e(ax.x));
    let ta0 = r((e(ty) * e(ax.y) + e(tx) * e(ax.x)) + e(tz) * e(ax.z));
    let (a00, a01, a02) = (r00.abs(), r01.abs(), r02.abs());
    if ((e(a00) * e(bh.x) + e(a02) * e(bh.z)) + e(a01) * e(bh.y)) + e(ah.x) < e(ta0.abs()) {
        return false;
    }

    let r10 = r((e(ay.y) * e(bx.y) + e(ay.z) * e(bx.z)) + e(ay.x) * e(bx.x));
    let r11 = r((e(ay.z) * e(by.z) + e(ay.y) * e(by.y)) + e(by.x) * e(ay.x));
    let r12 = r((e(ay.z) * e(bz.z) + e(ay.y) * e(bz.y)) + e(ay.x) * e(bz.x));
    let ta1 = r((e(tz) * e(ay.z) + e(ty) * e(ay.y)) + e(tx) * e(ay.x));
    let (a10, a11, a12) = (r10.abs(), r11.abs(), r12.abs());
    if ((e(a10) * e(bh.x) + e(a12) * e(bh.z)) + e(a11) * e(bh.y)) + e(ah.y) < e(ta1.abs()) {
        return false;
    }

    let r20 = r((e(az.y) * e(bx.y) + e(az.z) * e(bx.z)) + e(bx.x) * e(az.x));
    let r21 = r((e(az.z) * e(by.z) + e(az.y) * e(by.y)) + e(by.x) * e(az.x));
    let r22 = r((e(az.z) * e(bz.z) + e(az.y) * e(bz.y)) + e(az.x) * e(bz.x));
    let ta2 = r((e(tz) * e(az.z) + e(ty) * e(az.y)) + e(tx) * e(az.x));
    let (a20, a21, a22) = (r20.abs(), r21.abs(), r22.abs());
    if ((e(a22) * e(bh.z) + e(a21) * e(bh.y)) + e(a20) * e(bh.x)) + e(ah.z) < e(ta2.abs()) {
        return false;
    }

    if ((e(a20) * e(ah.z) + e(a00) * e(ah.x)) + e(a10) * e(ah.y)) + e(bh.x)
        < ((e(tz) * e(bx.z) + e(tx) * e(bx.x)) + e(ty) * e(bx.y)).abs()
    {
        return false;
    }
    if ((e(a21) * e(ah.z) + e(a01) * e(ah.x)) + e(a11) * e(ah.y)) + e(bh.y)
        < ((e(tz) * e(by.z) + e(ty) * e(by.y)) + e(tx) * e(by.x)).abs()
    {
        return false;
    }
    if ((e(a22) * e(ah.z) + e(a02) * e(ah.x)) + e(a12) * e(ah.y)) + e(bh.z)
        < ((e(tz) * e(bz.z) + e(ty) * e(bz.y)) + e(tx) * e(bz.x)).abs()
    {
        return false;
    }

    // The nine cross axes, each radius summed as (p1 + p2) + (p3 + p4).
    let cross_tests = [
        (
            (e(a02) * e(bh.y) + e(a01) * e(bh.z)) + (e(a10) * e(ah.z) + e(a20) * e(ah.y)),
            e(ta2) * e(r10) - e(r20) * e(ta1),
        ),
        (
            (e(a11) * e(ah.z) + e(a21) * e(ah.y)) + (e(a02) * e(bh.x) + e(a00) * e(bh.z)),
            e(ta2) * e(r11) - e(r21) * e(ta1),
        ),
        (
            (e(a12) * e(ah.z) + e(a22) * e(ah.y)) + (e(a01) * e(bh.x) + e(a00) * e(bh.y)),
            e(ta2) * e(r12) - e(r22) * e(ta1),
        ),
        (
            (e(a20) * e(ah.x) + e(a00) * e(ah.z)) + (e(a12) * e(bh.y) + e(a11) * e(bh.z)),
            e(r20) * e(ta0) - e(ta2) * e(r00),
        ),
        (
            (e(a21) * e(ah.x) + e(a01) * e(ah.z)) + (e(a12) * e(bh.x) + e(a10) * e(bh.z)),
            e(r21) * e(ta0) - e(ta2) * e(r01),
        ),
        (
            (e(a22) * e(ah.x) + e(a02) * e(ah.z)) + (e(a11) * e(bh.x) + e(a10) * e(bh.y)),
            e(r22) * e(ta0) - e(ta2) * e(r02),
        ),
        (
            (e(a10) * e(ah.x) + e(a00) * e(ah.y)) + (e(a22) * e(bh.y) + e(a21) * e(bh.z)),
            e(ta1) * e(r00) - e(r10) * e(ta0),
        ),
        (
            (e(a11) * e(ah.x) + e(a01) * e(ah.y)) + (e(a20) * e(bh.z) + e(a22) * e(bh.x)),
            e(ta1) * e(r01) - e(r11) * e(ta0),
        ),
        (
            (e(a12) * e(ah.x) + e(a02) * e(ah.y)) + (e(a20) * e(bh.y) + e(a21) * e(bh.x)),
            e(ta1) * e(r02) - e(r12) * e(ta0),
        ),
    ];
    for (radius, projection) in cross_tests {
        if radius < projection.abs() {
            return false;
        }
    }
    true
}

/// The box `TransformedBoxesIntersect` and `RayTransformedBoxNearestHit`
/// build from a transform and a local box (`BuildOBBFromTransformAndAABB`):
/// the rotation's columns as axes, `|max - min| * 0.5` (the double 0.5),
/// and the transformed local centre.
#[must_use]
pub fn obb_from_transform(xform: &Transform, aabb: &AabbMinMax) -> OrientedBox {
    let rotation = &xform.rotation;
    let half_extent = Vector::new(
        r((e(aabb[1]) - e(aabb[0])).abs() * 0.5),
        r((e(aabb[3]) - e(aabb[2])).abs() * 0.5),
        r((e(aabb[5]) - e(aabb[4])).abs() * 0.5),
    );
    let local_center = add(Vector::new(aabb[0], aabb[2], aabb[4]), half_extent);
    OrientedBox {
        center: transform_xform(xform, local_center),
        axis_x: column0(rotation),
        axis_y: column1(rotation),
        axis_z: column2(rotation),
        half_extent,
    }
}

/// `TransformedBoxesIntersect`: whether two transformed local boxes
/// overlap.
#[must_use]
pub fn transformed_boxes_intersect(
    xform_a: &Transform,
    aabb_a: &AabbMinMax,
    xform_b: &Transform,
    aabb_b: &AabbMinMax,
) -> bool {
    obb_intersect(
        &obb_from_transform(xform_a, aabb_a),
        &obb_from_transform(xform_b, aabb_b),
    )
}

/// `RaySphereIntersect`: the nearest non-negative ray parameter at which
/// the ray meets the sphere, `None` when it misses. `ray_dir` is taken to
/// be unit length, as in the reference.
#[must_use]
pub fn ray_sphere_intersect(
    ray_origin: Vector,
    ray_dir: Vector,
    sphere_center: Vector,
    radius: f32,
) -> Option<f32> {
    let lx = e(sphere_center.x) - e(ray_origin.x);
    let ly = e(sphere_center.y) - e(ray_origin.y);
    let lz = e(sphere_center.z) - e(ray_origin.z);
    let b = r((lz * e(ray_dir.z) + ly * e(ray_dir.y)) + lx * e(ray_dir.x));
    let disc = (e(b) * e(b) - ((lx * lx + lz * lz) + ly * ly)) + e(radius) * e(radius);
    // Less and unordered leave.
    if !(disc >= 0.0) {
        return None;
    }
    let sq = disc.sqrt();
    let t0 = r(e(b) - sq);
    let t1 = r(sq + e(b));
    if t0 < 0.0 {
        if !(t1 >= 0.0) {
            return None;
        }
        return Some(t1);
    }
    if t1 < 0.0 {
        return Some(t0);
    }
    Some(if t0 < t1 { t0 } else { t1 })
}

/// The half-space clip `RayAABBIntersect` runs once per box face
/// (`ClipHalfSpace`).
fn clip_half_space(denom: f32, numer: f32, t_min: &mut f32, t_max: &mut f32) -> bool {
    if denom > 0.0 {
        if e(denom) * e(*t_max) < e(numer) {
            return false;
        }
        if e(denom) * e(*t_min) < e(numer) {
            *t_min = r(e(numer) / e(denom));
        }
    } else if denom < 0.0 {
        if e(denom) * e(*t_min) < e(numer) {
            return false;
        }
        if e(denom) * e(*t_max) < e(numer) {
            *t_max = r(e(numer) / e(denom));
        }
    } else if !(numer <= 0.0) {
        // Parallel (or NaN): greater and unordered reject.
        return false;
    }
    true
}

/// `RayAABBIntersect`: the slab test of a local ray against the box
/// `[-half_extent, half_extent]`, narrowing `[t_min, t_max]` in place.
/// True only when a face narrowed the interval.
pub fn ray_aabb_intersect(
    local_origin: Vector,
    local_dir: Vector,
    half_extent: Vector,
    t_min: &mut f32,
    t_max: &mut f32,
) -> bool {
    let in_min = *t_min;
    let in_max = *t_max;
    let faces = [
        (local_dir.x, -r(e(local_origin.x) + e(half_extent.x))),
        (-local_dir.x, r(e(local_origin.x) - e(half_extent.x))),
        (local_dir.y, -r(e(local_origin.y) + e(half_extent.y))),
        (-local_dir.y, r(e(local_origin.y) - e(half_extent.y))),
        (local_dir.z, -r(e(local_origin.z) + e(half_extent.z))),
        (-local_dir.z, r(e(local_origin.z) - e(half_extent.z))),
    ];
    for (denom, numer) in faces {
        if !clip_half_space(denom, numer, t_min, t_max) {
            return false;
        }
    }
    // An untouched interval (NaN compares as different, as `FCOMP` does).
    *t_min != in_min || *t_max != in_max
}

/// What `RayOrientedBoxIntersect` reports.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum BoxHit {
    /// Hit type 0: the ray misses.
    Miss,
    /// Hit type 1: the origin is inside; the exit point. The reference
    /// leaves its second point slot untouched.
    Inside(Vector),
    /// Hit type 2: entered from outside; the entry and exit points.
    Through(Vector, Vector),
}

impl BoxHit {
    /// The reference's hit type (0, 1 or 2).
    #[must_use]
    pub fn hit_type(self) -> i32 {
        match self {
            Self::Miss => 0,
            Self::Inside(_) => 1,
            Self::Through(..) => 2,
        }
    }
}

/// `RayOrientedBoxIntersect`: the ray against an oriented box.
#[must_use]
pub fn ray_oriented_box_intersect(ray_origin: Vector, ray_dir: Vector, b: &OrientedBox) -> BoxHit {
    let o = sub(ray_origin, b.center);
    let local_origin = Vector::new(
        r((e(o.z) * e(b.axis_x.z) + e(o.y) * e(b.axis_x.y)) + e(o.x) * e(b.axis_x.x)),
        r((e(o.z) * e(b.axis_y.z) + e(o.y) * e(b.axis_y.y)) + e(o.x) * e(b.axis_y.x)),
        r((e(o.z) * e(b.axis_z.z) + e(o.y) * e(b.axis_z.y)) + e(o.x) * e(b.axis_z.x)),
    );
    let local_dir = Vector::new(
        r(
            (e(b.axis_x.y) * e(ray_dir.y) + e(b.axis_x.z) * e(ray_dir.z))
                + e(b.axis_x.x) * e(ray_dir.x),
        ),
        r(
            (e(b.axis_y.y) * e(ray_dir.y) + e(b.axis_y.z) * e(ray_dir.z))
                + e(b.axis_y.x) * e(ray_dir.x),
        ),
        r(
            (e(b.axis_z.y) * e(ray_dir.y) + e(b.axis_z.z) * e(ray_dir.z))
                + e(ray_dir.x) * e(b.axis_z.x),
        ),
    );
    let mut t_min = 0.0_f32;
    let mut t_max = f32::MAX;
    if !ray_aabb_intersect(
        local_origin,
        local_dir,
        b.half_extent,
        &mut t_min,
        &mut t_max,
    ) {
        return BoxHit::Miss;
    }
    // The z product is stored before its add; x and y add to the unrounded
    // product.
    let exit_z_product = r(e(t_max) * e(ray_dir.z));
    let exit = Vector::new(
        r(e(t_max) * e(ray_dir.x) + e(ray_origin.x)),
        r(e(t_max) * e(ray_dir.y) + e(ray_origin.y)),
        r(e(exit_z_product) + e(ray_origin.z)),
    );
    if t_min > 0.0 {
        let scaled = Vector::new(
            r(e(t_min) * e(ray_dir.x)),
            r(e(t_min) * e(ray_dir.y)),
            r(e(t_min) * e(ray_dir.z)),
        );
        return BoxHit::Through(add(scaled, ray_origin), exit);
    }
    BoxHit::Inside(exit)
}

/// What `RayTransformedBoxNearestHit` reports when the ray hits.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct NearestBoxHit {
    /// The point nearer the ray origin (the exit point when inside).
    pub point: Vector,
    /// The origin was inside the box.
    pub starts_inside: bool,
}

/// `RayTransformedBoxNearestHit`: Eberly's ray-against-box separating-axis
/// test, then the nearer of the entry and exit points. `None` on a miss
/// (the reference also clears its `starts_inside` flag then).
#[must_use]
pub fn ray_transformed_box_nearest_hit(
    ray_origin: Vector,
    ray_dir: Vector,
    xform: &Transform,
    aabb: &AabbMinMax,
) -> Option<NearestBoxHit> {
    let b = obb_from_transform(xform, aabb);
    let diff = sub(ray_origin, b.center);
    let he = b.half_extent;
    let (u0, u1, u2) = (b.axis_x, b.axis_y, b.axis_z);
    let d = ray_dir;

    let wdu0 = (e(u0.z) * e(d.z) + e(u0.y) * e(d.y)) + e(u0.x) * e(d.x);
    let awdu0 = r(wdu0.abs());
    let ddu0 = (e(diff.z) * e(u0.z) + e(diff.y) * e(u0.y)) + e(diff.x) * e(u0.x);
    if ddu0.abs() > e(he.x) && ddu0 * wdu0 >= 0.0 {
        return None;
    }
    let wdu1 = (e(u1.x) * e(d.x) + e(u1.y) * e(d.y)) + e(u1.z) * e(d.z);
    let awdu1 = r(wdu1.abs());
    let ddu1 = (e(u1.z) * e(diff.z) + e(u1.y) * e(diff.y)) + e(u1.x) * e(diff.x);
    if ddu1.abs() > e(he.y) && ddu1 * wdu1 >= 0.0 {
        return None;
    }
    let wdu2 = (e(u2.x) * e(d.x) + e(u2.y) * e(d.y)) + e(u2.z) * e(d.z);
    let awdu2 = r(wdu2.abs());
    let ddu2 = (e(u2.z) * e(diff.z) + e(u2.y) * e(diff.y)) + e(u2.x) * e(diff.x);
    if ddu2.abs() > e(he.z) && ddu2 * wdu2 >= 0.0 {
        return None;
    }

    // d × diff; the first cross test uses the unrounded z.
    let wxdx = r(e(diff.z) * e(d.y) - e(diff.y) * e(d.z));
    let wxdy = r(e(diff.x) * e(d.z) - e(diff.z) * e(d.x));
    let wxdz_exact = e(diff.y) * e(d.x) - e(diff.x) * e(d.y);
    let wxdz = r(wxdz_exact);

    let t0 = ((wxdz_exact * e(u0.z) + e(wxdy) * e(u0.y)) + e(wxdx) * e(u0.x)).abs();
    if t0 > e(awdu2) * e(he.y) + e(he.z) * e(awdu1) {
        return None;
    }
    let t1 = ((e(u1.z) * e(wxdz) + e(u1.y) * e(wxdy)) + e(u1.x) * e(wxdx)).abs();
    if t1 > e(awdu2) * e(he.x) + e(he.z) * e(awdu0) {
        return None;
    }
    let t2 = ((e(u2.z) * e(wxdz) + e(u2.y) * e(wxdy)) + e(u2.x) * e(wxdx)).abs();
    if e(he.y) * e(awdu0) + e(awdu1) * e(he.x) < t2 {
        return None;
    }

    // The reference ignores the clipper's answer and switches on the hit
    // type; for type 0 the C++ reads its two zero-filled points.
    let (p0, p1) = match ray_oriented_box_intersect(ray_origin, ray_dir, &b) {
        BoxHit::Inside(exit) => {
            return Some(NearestBoxHit {
                point: exit,
                starts_inside: true,
            });
        }
        BoxHit::Through(entry, exit) => (entry, exit),
        BoxHit::Miss => (Vector::ZERO, Vector::ZERO),
    };
    let d0x = e(p0.x) - e(ray_origin.x);
    let d0y = e(p0.y) - e(ray_origin.y);
    let d0z = e(p0.z) - e(ray_origin.z);
    let d1x = e(p1.x) - e(ray_origin.x);
    let d1y = e(p1.y) - e(ray_origin.y);
    let d1z = r(e(p1.z) - e(ray_origin.z));
    let dist0 = (d0z * d0z + d0y * d0y) + d0x * d0x;
    let dist1 = (e(d1z) * e(d1z) + d1y * d1y) + d1x * d1x;
    Some(NearestBoxHit {
        point: if !(dist1 > dist0) { p1 } else { p0 },
        starts_inside: false,
    })
}

/// `BuildPerpendicularBasis`: two unit vectors perpendicular to `dir`,
/// which is first normalised in place unless `already_normalized`.
/// Returns `(out1, out2)` with `out2 = out1 × dir`.
pub fn build_perpendicular_basis(dir: &mut Vector, already_normalized: bool) -> (Vector, Vector) {
    if !already_normalized {
        let inv = 1.0 / ((e(dir.x) * e(dir.x) + e(dir.y) * e(dir.y)) + e(dir.z) * e(dir.z)).sqrt();
        *dir = Vector::new(r(inv * e(dir.x)), r(inv * e(dir.y)), r(inv * e(dir.z)));
    }
    let d = *dir;
    // Greater and unordered take the y-dominant arm.
    let out1 = if !(d.y.abs() <= d.x.abs()) {
        let s = fast_inv_sqrt(r(e(d.y) * e(d.y) + e(d.z) * e(d.z)));
        Vector::new(0.0, r(s * e(d.z)), r(-(s * e(d.y))))
    } else {
        let s = fast_inv_sqrt(r(e(d.x) * e(d.x) + e(d.z) * e(d.z)));
        Vector::new(r(-(s * e(d.z))), 0.0, r(s * e(d.x)))
    };
    let out2 = Vector::new(
        r(e(out1.z) * e(d.y) - e(out1.y) * e(d.z)),
        r(e(out1.x) * e(d.z) - e(out1.z) * e(d.x)),
        r(e(out1.y) * e(d.x) - e(out1.x) * e(d.y)),
    );
    (out1, out2)
}

/// A capsule: a segment from `start` along `dir` (not unit length) with
/// a radius (the 7-dword record `{Radius, SegStart, SegDir}`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Capsule {
    /// The radius.
    pub radius: f32,
    /// The segment's start.
    pub start: Vector,
    /// The segment, start to end.
    pub dir: Vector,
}

/// `RaySegmentCapsuleIntersect`: up to two ray parameters at which the
/// ray crosses the capsule, lateral roots first, then the start cap's,
/// then the end cap's, as the reference orders them.
#[must_use]
#[allow(clippy::too_many_lines)]
pub fn ray_segment_capsule_intersect(
    ray_origin: Vector,
    ray_dir: Vector,
    capsule: &Capsule,
) -> Vec<f32> {
    let radius = capsule.radius;
    let seg_start = capsule.start;
    let seg_dir = capsule.dir;

    let axis_len_sq =
        (e(seg_dir.x) * e(seg_dir.x) + e(seg_dir.y) * e(seg_dir.y)) + e(seg_dir.z) * e(seg_dir.z);
    let height = r(inline_fast_length(axis_len_sq));
    let inv_axis_len = 1.0 / axis_len_sq.sqrt();
    let mut unit_axis = Vector::new(
        r(e(seg_dir.x) * inv_axis_len),
        r(e(seg_dir.y) * inv_axis_len),
        r(e(seg_dir.z) * inv_axis_len),
    );
    let (b1, b2) = build_perpendicular_basis(&mut unit_axis, true);

    let d1 = r((e(b1.x) * e(ray_dir.x) + e(b1.z) * e(ray_dir.z)) + e(b1.y) * e(ray_dir.y));
    let d2 = r((e(b2.x) * e(ray_dir.x) + e(b2.z) * e(ray_dir.z)) + e(b2.y) * e(ray_dir.y));
    let d3 = r(
        (e(unit_axis.x) * e(ray_dir.x) + e(unit_axis.z) * e(ray_dir.z))
            + e(unit_axis.y) * e(ray_dir.y),
    );

    let d2_sq = r(e(d2) * e(d2));
    let d1_sq = r(e(d1) * e(d1));
    let dir_len_sq = (e(d3) * e(d3) + e(d2_sq)) + e(d1_sq);
    let inv_dir_len = r(1.0 / inline_fast_length(dir_len_sq));

    let rx = e(ray_origin.x) - e(seg_start.x);
    let ry = e(ray_origin.y) - e(seg_start.y);
    let rz = e(ray_origin.z) - e(seg_start.z);
    let o1 = r((e(b1.z) * rz + ry * e(b1.y)) + rx * e(b1.x));
    let o2 = r((e(b2.z) * rz + ry * e(b2.y)) + rx * e(b2.x));
    let o3 = r((rz * e(unit_axis.z) + ry * e(unit_axis.y)) + rx * e(unit_axis.x));

    let radius_sq = e(radius) * e(radius);

    // Less and unordered take the general path.
    if e(d3).abs() >= 0.999_999_999_999 {
        let disc = (radius_sq - e(o1) * e(o1)) - e(o2) * e(o2);
        if !(disc >= 0.0) {
            return Vec::new();
        }
        let sq = disc.sqrt();
        return vec![
            r(-((e(o3) + sq) * e(inv_dir_len))),
            r(((e(height) - e(o3)) + sq) * e(inv_dir_len)),
        ];
    }

    let a = r(e(d2_sq) + e(d1_sq));
    let mut b = r(e(d2) * e(o2) + e(d1) * e(o1));
    let mut c = r((e(o2) * e(o2) + e(o1) * e(o1)) - radius_sq);
    let cyl_disc_exact = e(b) * e(b) - e(c) * e(a);
    if cyl_disc_exact < 0.0 {
        return Vec::new();
    }
    let cyl_disc = r(cyl_disc_exact);

    let mut out = Vec::with_capacity(2);
    let in_axis = |coord: f64| coord >= 0.0 && coord <= e(height);
    if cyl_disc > 0.0 {
        let sq = e(cyl_disc).sqrt();
        let inv_a = 1.0 / e(a);
        let t0_exact = (-e(b) - sq) * inv_a;
        let t0 = r(t0_exact);
        if in_axis(t0_exact * e(d3) + e(o3)) {
            out.push(r(e(t0) * e(inv_dir_len)));
        }
        let t1 = r((sq - e(b)) * inv_a);
        if in_axis(e(t1) * e(d3) + e(o3)) {
            out.push(r(e(t1) * e(inv_dir_len)));
        }
        if out.len() == 2 {
            return out;
        }
    } else {
        let t_exact = -(e(b) / e(a));
        let t = r(t_exact);
        if in_axis(t_exact * e(d3) + e(o3)) {
            // The tangent root returns at once with one hit.
            return vec![r(e(t) * e(inv_dir_len))];
        }
    }

    // The cap at the segment's start.
    b = r(e(o3) * e(d3) + e(b));
    c = r(e(o3) * e(o3) + e(c));
    let disc_apex = e(b) * e(b) - e(c);
    let apex_far = if disc_apex > 0.0 {
        let sq = disc_apex.sqrt();
        let near = -e(b) - sq;
        if e(d3) * near + e(o3) <= 0.0 {
            out.push(r(near * e(inv_dir_len)));
            if out.len() == 2 {
                return out;
            }
        }
        Some(sq - e(b))
    } else if disc_apex == 0.0 {
        Some(-e(b))
    } else {
        None
    };
    if let Some(far) = apex_far
        && e(d3) * far + e(o3) <= 0.0
    {
        out.push(r(far * e(inv_dir_len)));
        if out.len() == 2 {
            return out;
        }
    }

    // The cap at the segment's end.
    let b_top_exact = e(b) - e(d3) * e(height);
    b = r(b_top_exact);
    let c_top = (e(height) - (e(o3) + e(o3))) * e(height) + e(c);
    let disc_top = b_top_exact * e(b) - c_top;
    let top_far = if disc_top > 0.0 {
        let sq = disc_top.sqrt();
        let near = -e(b) - sq;
        if e(d3) * near + e(o3) >= e(height) {
            out.push(r(near * e(inv_dir_len)));
            if out.len() == 2 {
                return out;
            }
        }
        Some(sq - e(b))
    } else if disc_top == 0.0 {
        Some(-e(b))
    } else {
        None
    };
    if let Some(far) = top_far
        && e(d3) * far + e(o3) >= e(height)
    {
        out.push(r(far * e(inv_dir_len)));
    }
    out
}

/// `RaySegmentCapsuleFirstHit`: the point of the first non-negative root,
/// `None` when there is none (the C++ reference then copies its
/// zero-filled point).
#[must_use]
pub fn ray_segment_capsule_first_hit(
    ray_origin: Vector,
    ray_dir: Vector,
    capsule: &Capsule,
) -> Option<Vector> {
    ray_segment_capsule_intersect(ray_origin, ray_dir, capsule)
        .into_iter()
        .find(|t| *t >= 0.0)
        .map(|t| {
            let ty = r(e(t) * e(ray_dir.y));
            let tz = r(e(t) * e(ray_dir.z));
            Vector::new(
                r(e(t) * e(ray_dir.x) + e(ray_origin.x)),
                r(e(ty) + e(ray_origin.y)),
                r(e(tz) + e(ray_origin.z)),
            )
        })
}

/// `PointInCapsule`.
#[must_use]
pub fn point_in_capsule(point: Vector, capsule: &Capsule) -> bool {
    let seg_start = capsule.start;
    let seg_dir = capsule.dir;
    let mut ox = e(point.x) - e(seg_start.x);
    let mut oy = e(point.y) - e(seg_start.y);
    let mut oz = e(point.z) - e(seg_start.z);
    let dot_exact = (oz * e(seg_dir.z) + oy * e(seg_dir.y)) + ox * e(seg_dir.x);
    let dot = r(dot_exact);
    // Greater and unordered project.
    if !(dot_exact <= 0.0) {
        let seg_len_sq = r((e(seg_dir.x) * e(seg_dir.x) + e(seg_dir.y) * e(seg_dir.y))
            + e(seg_dir.z) * e(seg_dir.z));
        if !(dot >= seg_len_sq) {
            let t = e(dot) / e(seg_len_sq);
            ox -= e(r(t * e(seg_dir.x)));
            oy -= e(r(t * e(seg_dir.y)));
            oz -= t * e(seg_dir.z);
        } else {
            ox -= e(seg_dir.x);
            oy -= e(seg_dir.y);
            oz -= e(seg_dir.z);
        }
    }
    let dist_sq = (oz * oz + ox * ox) + oy * oy;
    e(capsule.radius) * e(capsule.radius) >= dist_sq
}

/// `ClampedDot3`: the dot clamped to `[-1, 1]`, unrounded; NaN passes.
#[must_use]
pub fn clamped_dot3(a: Vector, b: Vector) -> f64 {
    let dot = dot_zyx(a, b);
    // NaN passes, as in the reference.
    dot.clamp(-1.0, 1.0)
}

/// What `SweptSphereCollisionTime` answers: whether the spheres meet in
/// the step, and the time it writes (`-1` when the quadratic has no
/// usable root).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SweptSphereHit {
    /// The return value.
    pub hit: bool,
    /// The `OutT` value.
    pub t: f32,
}

/// `SweptSphereCollisionTime`: the earliest time in `[0, 1]` at which
/// sphere A moving from `a0` to `a1` meets sphere B moving from `b0` to
/// `b1`.
#[must_use]
pub fn swept_sphere_collision_time(
    radius_a: f32,
    a0: Vector,
    a1: Vector,
    radius_b: f32,
    b0: Vector,
    b1: Vector,
) -> SweptSphereHit {
    let dax = e(a1.x) - e(a0.x);
    let day = e(a1.y) - e(a0.y);
    let daz = r(e(a1.z) - e(a0.z));
    let dbx = e(b1.x) - e(b0.x);
    let dby = r(e(b1.y) - e(b0.y));
    let dbz = r(e(b1.z) - e(b0.z));
    let px = r(e(b0.x) - e(a0.x));
    let py = r(e(b0.y) - e(a0.y));
    let pz = r(e(b0.z) - e(a0.z));
    let vx = r(dbx - dax);
    let vy = e(dby) - day;
    let vz = e(dbz) - e(daz);
    let radius_sum = r(e(radius_a) + e(radius_b));

    let a = r((vz * vz + vy * vy) + e(vx) * e(vx));
    let b_half = (vz * e(pz) + vy * e(py)) + e(vx) * e(px);
    let b = r(b_half + b_half);
    let c0 = (e(pz) * e(pz) + e(py) * e(py)) + e(px) * e(px);
    let combined = e(radius_sum) * e(radius_sum);

    if c0 <= combined {
        return SweptSphereHit { hit: true, t: 0.0 };
    }
    let disc = r(e(b) * e(b) - ((c0 - combined) * e(a)) * 4.0);
    if disc >= 0.0 && e(a) * e(a) >= e(0.01) {
        let sq = e(disc).sqrt();
        let inv_two_a = 1.0 / (e(a) + e(a));
        let root2 = r((sq - e(b)) * inv_two_a);
        let root1 = (-e(b) - sq) * inv_two_a;
        let t = if e(root2) > root1 { r(root1) } else { root2 };
        return SweptSphereHit { hit: !(t < 0.0), t };
    }
    SweptSphereHit {
        hit: false,
        t: -1.0,
    }
}

/// The clamp, `acos` and `fabs` both bearing helpers end with
/// (`BearingClampedAcos`); the result is unrounded.
fn bearing_clamped_acos(dot: f32) -> f64 {
    let mut dot = dot;
    dot = dot.clamp(-1.0, 1.0);
    e(dot).acos().abs()
}

/// `BearingAngleToIface`: the angle between `self_center - target_center`
/// and column 2 of `self_rotation` (not negated). The centres are what
/// `GetIfacePositionOrZero` gives for each object.
#[must_use]
pub fn bearing_angle_to_object(
    self_center: Vector,
    self_rotation: &Matrix,
    target_center: Vector,
) -> f64 {
    let m = self_rotation.to_words();
    let dx = r(e(self_center.x) - e(target_center.x));
    let dy = r(e(self_center.y) - e(target_center.y));
    let dz = r(e(self_center.z) - e(target_center.z));
    let len = ((e(dz) * e(dz) + e(dy) * e(dy)) + e(dx) * e(dx)).sqrt();
    let (nx, ny, nz) = if len > e(0.01) {
        let inv = 1.0 / len;
        (r(e(dx) * inv), r(e(dy) * inv), e(dz) * inv)
    } else {
        (0.0, 0.0, 0.0)
    };
    let dot = r((nz * e(m[8]) + e(ny) * e(m[5])) + e(m[2]) * e(nx));
    bearing_clamped_acos(dot)
}

/// `BearingAngleToPoint`: as [`bearing_angle_to_object`] against a point,
/// with the reference's other rounding of the normalisation (the inverse
/// length is stored as a `float`).
#[must_use]
pub fn bearing_angle_to_point(self_center: Vector, self_rotation: &Matrix, point: Vector) -> f64 {
    let m = self_rotation.to_words();
    let dx = r(e(self_center.x) - e(point.x));
    let dy = r(e(self_center.y) - e(point.y));
    let dz = r(e(self_center.z) - e(point.z));
    let len = ((e(dz) * e(dz) + e(dy) * e(dy)) + e(dx) * e(dx)).sqrt();
    let (nx, ny, nz) = if len > e(0.01) {
        let inv = r(1.0 / len);
        (e(inv) * e(dx), e(dy) * e(inv), e(dz) * e(inv))
    } else {
        (0.0, 0.0, 0.0)
    };
    let dot = r((nz * e(m[8]) + ny * e(m[5])) + nx * e(m[2]));
    bearing_clamped_acos(dot)
}

#[cfg(test)]
mod tests {
    use super::*;

    const UNIT_BOX: AabbMinMax = [-1.0, 1.0, -1.0, 1.0, -1.0, 1.0];

    #[test]
    fn sphere_hits_and_misses() {
        let o = Vector::new(0.0, 0.0, -10.0);
        let d = Vector::new(0.0, 0.0, 1.0);
        assert_eq!(ray_sphere_intersect(o, d, Vector::ZERO, 2.0), Some(8.0));
        assert_eq!(
            ray_sphere_intersect(o, d, Vector::new(5.0, 0.0, 0.0), 2.0),
            None
        );
        // From inside: the far root.
        assert_eq!(
            ray_sphere_intersect(Vector::ZERO, d, Vector::ZERO, 2.0),
            Some(2.0)
        );
        // Behind.
        assert_eq!(
            ray_sphere_intersect(o, Vector::new(0.0, 0.0, -1.0), Vector::ZERO, 2.0),
            None
        );
    }

    #[test]
    fn box_entry_exit_and_inside() {
        let b = obb_from_transform(&Transform::IDENTITY, &UNIT_BOX);
        assert_eq!(b.half_extent, Vector::new(1.0, 1.0, 1.0));
        let hit =
            ray_oriented_box_intersect(Vector::new(0.0, 0.0, -5.0), Vector::new(0.0, 0.0, 1.0), &b);
        assert_eq!(
            hit,
            BoxHit::Through(Vector::new(0.0, 0.0, -1.0), Vector::new(0.0, 0.0, 1.0))
        );
        let inside = ray_oriented_box_intersect(Vector::ZERO, Vector::new(1.0, 0.0, 0.0), &b);
        assert_eq!(inside, BoxHit::Inside(Vector::new(1.0, 0.0, 0.0)));
        assert_eq!(inside.hit_type(), 1);
        let near = ray_transformed_box_nearest_hit(
            Vector::new(0.0, 0.0, -5.0),
            Vector::new(0.0, 0.0, 1.0),
            &Transform::IDENTITY,
            &UNIT_BOX,
        );
        assert_eq!(
            near,
            Some(NearestBoxHit {
                point: Vector::new(0.0, 0.0, -1.0),
                starts_inside: false
            })
        );
        let away = ray_transformed_box_nearest_hit(
            Vector::new(0.0, 0.0, -5.0),
            Vector::new(0.0, 0.0, -1.0),
            &Transform::IDENTITY,
            &UNIT_BOX,
        );
        assert_eq!(away, None);
    }

    #[test]
    fn boxes_overlap_and_separate() {
        let shifted = Transform::new(Matrix::IDENTITY, Vector::new(1.5, 0.0, 0.0));
        assert!(transformed_boxes_intersect(
            &Transform::IDENTITY,
            &UNIT_BOX,
            &shifted,
            &UNIT_BOX
        ));
        let far = Transform::new(Matrix::IDENTITY, Vector::new(2.5, 0.0, 0.0));
        assert!(!transformed_boxes_intersect(
            &Transform::IDENTITY,
            &UNIT_BOX,
            &far,
            &UNIT_BOX
        ));
    }

    #[test]
    fn capsule_tests() {
        let capsule = Capsule {
            radius: 1.0,
            start: Vector::ZERO,
            dir: Vector::new(0.0, 10.0, 0.0),
        };
        assert!(point_in_capsule(Vector::new(0.5, 5.0, 0.0), &capsule));
        assert!(point_in_capsule(Vector::new(0.0, 10.9, 0.0), &capsule));
        assert!(!point_in_capsule(Vector::new(1.5, 5.0, 0.0), &capsule));
        let hits = ray_segment_capsule_intersect(
            Vector::new(-5.0, 5.0, 0.0),
            Vector::new(1.0, 0.0, 0.0),
            &capsule,
        );
        assert_eq!(hits.len(), 2);
        // The fast square roots are good to about 0.5%.
        assert!(
            (hits[0] - 4.0).abs() < 5.0e-2 && (hits[1] - 6.0).abs() < 5.0e-2,
            "{hits:?}"
        );
        let point = ray_segment_capsule_first_hit(
            Vector::new(-5.0, 5.0, 0.0),
            Vector::new(1.0, 0.0, 0.0),
            &capsule,
        );
        assert!(point.is_some_and(|p| (p.x + 1.0).abs() < 5.0e-2));
    }

    #[test]
    fn basis_is_perpendicular() {
        let mut dir = Vector::new(1.0, 2.0, 3.0);
        let (a, b) = build_perpendicular_basis(&mut dir, false);
        assert!(dot_zyx(a, dir).abs() < 1.0e-3);
        assert!(dot_zyx(b, dir).abs() < 1.0e-3);
        assert!(dot_zyx(a, b).abs() < 1.0e-3);
    }

    #[test]
    fn swept_spheres() {
        let hit = swept_sphere_collision_time(
            1.0,
            Vector::new(-10.0, 0.0, 0.0),
            Vector::new(10.0, 0.0, 0.0),
            1.0,
            Vector::ZERO,
            Vector::ZERO,
        );
        assert!(hit.hit && (hit.t - 0.4).abs() < 1.0e-6, "{hit:?}");
        let touching = swept_sphere_collision_time(
            1.0,
            Vector::ZERO,
            Vector::ZERO,
            1.0,
            Vector::new(1.0, 0.0, 0.0),
            Vector::ZERO,
        );
        assert_eq!(touching, SweptSphereHit { hit: true, t: 0.0 });
    }

    #[test]
    fn clamps_and_bearings() {
        assert_eq!(
            clamped_dot3(Vector::new(2.0, 0.0, 0.0), Vector::new(1.0, 0.0, 0.0)),
            1.0
        );
        let angle =
            bearing_angle_to_point(Vector::ZERO, &Matrix::IDENTITY, Vector::new(0.0, 0.0, -5.0));
        assert!(angle.abs() < 1.0e-6);
        let angle =
            bearing_angle_to_object(Vector::ZERO, &Matrix::IDENTITY, Vector::new(5.0, 0.0, 0.0));
        assert!((angle - std::f64::consts::FRAC_PI_2).abs() < 1.0e-6);
    }
}
