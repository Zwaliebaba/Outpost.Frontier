//! The remaining zone queries of `common_zone.cpp`: `ClassifyPoint`
//! (and its box tail `ClassifyBox`), `GetImmersion`, the segment clips of
//! `Intersect`, and the culls `FrustumCull` and `SphereCull`, in the
//! reference's operation order.
//!
//! The clips and `ClassifyPoint` are exact (Tier A). The frustum cull turns
//! planes with x87 `FSIN`, `FCOS` and `FPTAN`; here those come from the Rust
//! standard library, so its answers are Tier B (decision T11): a plane
//! that grazes a zone can fall on the other side.

// The reference's x87 comparisons send unordered operands (NaN) to one
// side; the port spells each outcome as the comparison that is true exactly
// when the reference takes that side, which needs negated comparisons.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use neuron_core::math::{Matrix, Transform, Vector, transform_matrix, transpose_transform};

use super::shape::clamp_to_box_surface;
use super::{Zone, ZoneShape};
use crate::data::Reports;

/// `Geometry::Frustum`: an apex frame looking down its `-Z` column.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Frustum {
    /// The apex and its axes (columns).
    pub xform: Transform,
    /// The full horizontal angle, radians.
    pub angle_x: f32,
    /// The full vertical angle, radians.
    pub angle_y: f32,
    /// The far distance.
    pub distance: f32,
}

/// `Geometry::Sphere`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Sphere {
    /// The centre.
    pub center: Vector,
    /// The radius.
    pub radius: f32,
}

/// `kCullHalfPi`.
const HALF_PI: f32 = 1.570_796_4;
/// `kCullPi`: pi as a `float`.
const PI: f32 = core::f32::consts::PI;
/// `GFrustumCapSteps`.
const CAP_STEPS: i32 = 5;

fn v(x: f32, y: f32, z: f32) -> Vector {
    Vector { x, y, z }
}

fn e(x: f32) -> f64 {
    f64::from(x)
}

fn sub(a: Vector, b: Vector) -> Vector {
    v(a.x - b.x, a.y - b.y, a.z - b.z)
}

fn add(a: Vector, b: Vector) -> Vector {
    v(a.x + b.x, a.y + b.y, a.z + b.z)
}

/// `Vec3Scale`: `s * v`, float stores.
fn scale(a: Vector, s: f32) -> Vector {
    v(s * a.x, s * a.y, s * a.z)
}

fn negate(a: Vector) -> Vector {
    v(-a.x, -a.y, -a.z)
}

/// `(x*x + y*y) + z*z`, unrounded.
fn length_sq(a: Vector) -> f64 {
    (e(a.x) * e(a.x) + e(a.y) * e(a.y)) + e(a.z) * e(a.z)
}

/// A segment, start to end.
#[derive(Clone, Copy)]
struct Segment {
    start: Vector,
    end: Vector,
}

/// `WriteClipPoint`: `start + (end - start) * t`.
fn clip_point(s: Segment, t: f32) -> Vector {
    let dx = e(s.end.x) - e(s.start.x);
    let dy = e(s.end.y) - e(s.start.y);
    let dz = (e(s.end.z) - e(s.start.z)) as f32;
    let offset_x = (dx * e(t)) as f32;
    v(
        offset_x + s.start.x,
        (dy * e(t) + e(s.start.y)) as f32,
        (e(dz) * e(t) + e(s.start.z)) as f32,
    )
}

/// `ClipSegmentSphere`: where the segment enters the sphere.
fn clip_sphere(center: Vector, radius: f32, s: Segment) -> Option<Vector> {
    let d = sub(s.end, s.start);
    let f = sub(s.start, center);
    let a = ((e(d.z) * e(d.z) + e(d.y) * e(d.y)) + e(d.x) * e(d.x)) as f32;
    let b = (e(d.z) * e(f.z) + e(d.y) * e(f.y)) + e(f.x) * e(d.x);
    let c0 = (e(f.z) * e(f.z) + e(f.y) * e(f.y)) + e(f.x) * e(f.x);
    let r = e(radius);
    let disc = (b * b - (c0 - r * r) * e(a)) as f32;
    if !(disc >= 0.0) {
        return None;
    }
    if !(disc > 0.0) {
        let t = -(b / e(a));
        let tf = t as f32;
        if !(t >= 0.0) || !(tf <= 1.0) {
            return None;
        }
        let offset_z = d.z * tf;
        return Some(v(
            (e(d.x) * e(tf) + e(s.start.x)) as f32,
            (e(d.y) * e(tf) + e(s.start.y)) as f32,
            offset_z + s.start.z,
        ));
    }
    let root = e(disc).sqrt();
    let inv_a = 1.0 / e(a);
    let t1 = ((-b - root) * inv_a) as f32;
    let t2 = ((root - b) * inv_a) as f32;
    if t1 > 1.0 || t2 < 0.0 {
        return None;
    }
    let t = if t1 >= 0.0 { t1 } else { t2 };
    let offset_y = d.y * t;
    let offset_z = d.z * t;
    Some(v(
        (e(d.x) * e(t) + e(s.start.x)) as f32,
        offset_y + s.start.y,
        offset_z + s.start.z,
    ))
}

/// The local ends of a segment in a zone's frame.
fn local_ends(zone: &Zone, s: Segment) -> (Vector, Vector) {
    let ls = transpose_transform(&zone.orientation, sub(s.start, zone.pos));
    let le = transpose_transform(&zone.orientation, sub(s.end, zone.pos));
    (ls, le)
}

/// `ClipSegmentEllipsoid`, radii `zone.size`.
fn clip_ellipsoid(zone: &Zone, s: Segment) -> Option<Vector> {
    let (ls, le) = local_ends(zone, s);
    let dim = zone.size;
    let inv_x = 1.0 / e(dim.x);
    let inv_y = 1.0 / e(dim.y);
    let inv_z = 1.0 / e(dim.z);
    let inv_z_f = inv_z as f32;
    let unit = Segment {
        start: v(
            (e(ls.x) * inv_x) as f32,
            (e(ls.y) * inv_y) as f32,
            (e(ls.z) * inv_z) as f32,
        ),
        end: v(
            (e(le.x) * inv_x) as f32,
            (inv_y * e(le.y)) as f32,
            (e(inv_z_f) * e(le.z)) as f32,
        ),
    };
    let hit = clip_sphere(Vector::ZERO, 1.0, unit)?;
    let scaled = v(hit.x * dim.x, dim.y * hit.y, hit.z * dim.z);
    let world = transform_matrix(&zone.orientation, scaled);
    Some(v(
        zone.pos.x + world.x,
        zone.pos.y + world.y,
        zone.pos.z + world.z,
    ))
}

/// `ClipSideWall`: the roots of one side wall inside the height band.
fn clip_side_wall(
    a: f32,
    b: f64,
    disc: f64,
    ls: Vector,
    le: Vector,
    half_h: f32,
    t_best: &mut f32,
) {
    let h = e(half_h);
    if disc > 0.0 {
        let root = disc.sqrt();
        let two_a = e(a) + e(a);
        let t1 = ((-b - root) / two_a) as f32;
        let t2 = ((root - b) / two_a) as f32;
        if t1 >= 0.0 && t1 <= *t_best {
            let dy = e(le.y) - e(ls.y);
            let dy_f = dy as f32;
            if h >= (dy * e(t1) + e(ls.y)).abs() {
                *t_best = if t2 >= 0.0 && t2 <= t1 && h >= (e(dy_f) * e(t2) + e(ls.y)).abs() {
                    t2
                } else {
                    t1
                };
                return;
            }
        }
        if t2 >= 0.0 && t2 <= *t_best && h >= ((e(le.y) - e(ls.y)) * e(t2) + e(ls.y)).abs() {
            *t_best = t2;
        }
        return;
    }
    if disc == 0.0 {
        let t = -b / (e(a) + e(a));
        let tf = t as f32;
        if t >= 0.0 && tf <= *t_best && h >= ((e(le.y) - e(ls.y)) * e(tf) + e(ls.y)).abs() {
            *t_best = tf;
        }
    }
}

/// `CapCrossing`: the parameter and the squared radial distance where the
/// segment crosses a cap plane, when it does before `t_best`.
fn cap_crossing(
    num: f32,
    dir_y: f32,
    ls: Vector,
    dx_f: f32,
    dz_f: f32,
    t_best: f32,
) -> Option<(f32, f64)> {
    if !((dir_y > 0.0 && num > 0.0) || (dir_y < 0.0 && num < 0.0)) {
        return None;
    }
    let t = e(num) / e(dir_y);
    let tf = t as f32;
    if !(t > 0.0) || !(tf < t_best) {
        return None;
    }
    let offset_x = dx_f * tf;
    let x = e(ls.x) + e(offset_x);
    let z = e(ls.z) + e(dz_f) * e(tf);
    Some((tf, z * z + x * x))
}

/// The side-wall set-up shared by the cylinder and the ring.
struct Walls {
    ls: Vector,
    le: Vector,
    ld: Vector,
    half_h: f32,
    dx_f: f32,
    dz_f: f32,
    a: f32,
    b: f64,
    c0: f64,
}

fn walls(zone: &Zone, s: Segment, height: f32) -> Walls {
    let (ls, le) = local_ends(zone, s);
    let half_h = (e(height) * 0.5) as f32;
    let dx = e(le.x) - e(ls.x);
    let dz = e(le.z) - e(ls.z);
    let a = (dz * dz + dx * dx) as f32;
    let mut b = e(ls.z) * dz + e(ls.x) * dx;
    b += b;
    let c0 = e(ls.z) * e(ls.z) + e(ls.x) * e(ls.x);
    let ld = transpose_transform(&zone.orientation, sub(s.end, s.start));
    Walls {
        ls,
        le,
        ld,
        half_h,
        dx_f: dx as f32,
        dz_f: dz as f32,
        a,
        b,
        c0,
    }
}

impl Walls {
    fn disc(&self, radius: f64) -> f64 {
        self.b * self.b - ((self.c0 - radius * radius) * e(self.a)) * 4.0
    }

    /// The two caps, kept when `inside` accepts the radial distance.
    fn caps(&self, t_best: &mut f32, inside: impl Fn(f64) -> bool) {
        for num in [
            (-e(self.half_h) - e(self.ls.y)) as f32,
            (e(self.half_h) - e(self.ls.y)) as f32,
        ] {
            if let Some((t, radial)) =
                cap_crossing(num, self.ld.y, self.ls, self.dx_f, self.dz_f, *t_best)
                && inside(radial)
            {
                *t_best = t;
            }
        }
    }
}

fn finish_wall_clip(s: Segment, t_best: f32) -> Option<Vector> {
    if !(t_best >= 0.0) || !(t_best <= 1.0) {
        return None;
    }
    Some(clip_point(s, t_best))
}

/// `ClipSegmentCylinder`, radius `size.x`, height `size.y`.
fn clip_cylinder(zone: &Zone, s: Segment) -> Option<Vector> {
    let w = walls(zone, s, zone.size.y);
    let radius = e(zone.size.x);
    let mut t_best = 2.0_f32;
    clip_side_wall(w.a, w.b, w.disc(radius), w.ls, w.le, w.half_h, &mut t_best);
    let radius_sq = radius * radius;
    w.caps(&mut t_best, |radial| radius_sq > radial);
    finish_wall_clip(s, t_best)
}

/// `ClipSegmentRing`, outer radius `size.x`, inner `size.y`, height
/// `size.z`.
fn clip_ring(zone: &Zone, s: Segment) -> Option<Vector> {
    let w = walls(zone, s, zone.size.z);
    let outer = e(zone.size.x);
    let inner = e(zone.size.y);
    let mut t_best = 2.0_f32;
    clip_side_wall(w.a, w.b, w.disc(outer), w.ls, w.le, w.half_h, &mut t_best);
    clip_side_wall(w.a, w.b, w.disc(inner), w.ls, w.le, w.half_h, &mut t_best);
    let (outer_sq, inner_sq) = (outer * outer, inner * inner);
    w.caps(&mut t_best, |radial| radial < outer_sq && radial > inner_sq);
    finish_wall_clip(s, t_best)
}

/// `BoxFacePoint`: `ls + (le - ls) * t`, float stores.
fn box_face_point(ls: Vector, le: Vector, t: f32) -> Vector {
    let q = sub(le, ls);
    let scaled = v(q.x * t, q.y * t, q.z * t);
    add(ls, scaled)
}

/// `ClipSegmentBox`, full size `zone.size`. A start inside the box returns
/// the start in the box's local frame, as the C++ does.
fn clip_box(zone: &Zone, s: Segment) -> Option<Vector> {
    let (ls, le) = local_ends(zone, s);
    let half_x = e(zone.size.x) * 0.5;
    let half_x_f = half_x as f32;
    let half_y_f = (e(zone.size.y) * 0.5) as f32;
    let half_z_f = (e(zone.size.z) * 0.5) as f32;
    if half_x > e(ls.x).abs() && half_y_f > ls.y.abs() && half_z_f > ls.z.abs() {
        return Some(ls);
    }
    let ld = transpose_transform(&zone.orientation, sub(s.end, s.start));
    let mut t_best = 2.0_f32;

    // The X faces: not compared with the best so far.
    if let Some(num) = face_num(ld.x, half_x_f, ls.x) {
        let tf = (num / e(ld.x)) as f32;
        let p = box_face_point(ls, le, tf);
        if half_y_f > p.y.abs() && half_z_f > p.z.abs() {
            t_best = tf;
        }
    }
    // The Y faces.
    if let Some(num) = face_num(ld.y, half_y_f, ls.y) {
        let t = num / e(ld.y);
        let tf = t as f32;
        if t < e(t_best) {
            let p = box_face_point(ls, le, tf);
            if half_x_f > p.x.abs() && half_z_f > p.z.abs() {
                t_best = tf;
            }
        }
    }
    // The Z faces, each with its own stores.
    if let Some(num) = face_num(ld.z, half_z_f, ls.z) {
        let t = num / e(ld.z);
        let tf = t as f32;
        if t < e(t_best) {
            let q = sub(le, ls);
            let px = (e(ls.x) + e(q.x) * e(tf)) as f32;
            let inside = if ld.z > 0.0 {
                let py = (e(ls.y) + e(q.y) * e(tf)) as f32;
                half_x_f > px.abs() && half_y_f > py.abs()
            } else {
                let py = e(ls.y) + e(q.y) * e(tf);
                half_x_f > px.abs() && e(half_y_f) > py.abs()
            };
            if inside {
                t_best = tf;
            }
        }
    }
    if !(t_best <= 1.0) {
        return None;
    }
    Some(clip_point(s, t_best))
}

/// The numerator of a box face crossing, when the segment moves towards
/// that face from outside it.
fn face_num(dir: f32, half: f32, start: f32) -> Option<f64> {
    if dir > 0.0 {
        let num = -e(half) - e(start);
        (num > 0.0).then_some(num)
    } else if dir < 0.0 {
        let num = e(half) - e(start);
        (num < 0.0).then_some(num)
    } else {
        None
    }
}

impl Zone {
    /// `ClassifyPoint`: the point's distance from the centre over the
    /// shape's radius along it (below 1 inside); for a ring, along the tube
    /// from its middle circle; for a box, [`Zone::classify_box`].
    #[must_use]
    pub fn classify_point(&self, point: Vector) -> f64 {
        if self.shape == ZoneShape::Box {
            return self.classify_box(point);
        }
        let small = f64::from(1.0e-4_f32);
        let (length_f, radius) = if self.shape == ZoneShape::Ring {
            let local = transpose_transform(&self.orientation, sub(point, self.pos));
            let mid = (e(self.size.y) + e(self.size.x)) * 0.5;
            let s = mid / (e(local.z) * e(local.z) + e(local.x) * e(local.x)).sqrt();
            let rim_x = (s * e(local.x)) as f32;
            let rim_y = (0.0 * s) as f32;
            let rim_z = e(local.z) * s;
            let mut tube = v(
                local.x - rim_x,
                local.y - rim_y,
                (e(local.z) - rim_z) as f32,
            );
            let tube_length =
                ((e(tube.y) * e(tube.y) + e(tube.z) * e(tube.z)) + e(tube.x) * e(tube.x)).sqrt();
            if !(tube_length < small) {
                let inv = (1.0 / tube_length) as f32;
                tube = v(inv * tube.x, inv * tube.y, inv * tube.z);
            }
            (tube_length as f32, ring_surface_scale(self.size, tube))
        } else {
            let delta = sub(point, self.pos);
            let length = ((e(delta.y) * e(delta.y) + e(delta.z) * e(delta.z))
                + e(delta.x) * e(delta.x))
            .sqrt();
            let length_f = length as f32;
            if length < small {
                return 0.0;
            }
            let inv = 1.0 / e(length_f);
            let dir = v(
                (e(delta.x) * inv) as f32,
                (e(delta.y) * inv) as f32,
                (e(delta.z) * inv) as f32,
            );
            (length_f, self.radius_along(dir))
        };
        if !(radius > small) {
            return 0.0;
        }
        e(length_f) / radius
    }

    /// `ClassifyBox`: `(d + r^2) / r^2` with `d` the signed squared distance
    /// to the surface (negative inside) and `r` the bounding radius.
    #[must_use]
    pub fn classify_box(&self, point: Vector) -> f64 {
        let half = [self.size.x * 0.5, self.size.y * 0.5, self.size.z * 0.5];
        let local = transpose_transform(&self.orientation, sub(point, self.pos));
        let (clamped, outside) = clamp_to_box_surface(half, local);
        let diff = sub(local, clamped);
        let mut value = (e(diff.y) * e(diff.y) + e(diff.z) * e(diff.z)) + e(diff.x) * e(diff.x);
        if !outside {
            value = -value;
        }
        let value_f = value as f32;
        let radius = e(self.radius());
        let radius_sq = radius * radius;
        (e(value_f) + radius_sq) / radius_sq
    }

    /// `GetImmersion`: 0 outside, 1 deep inside, a ramp over the edge
    /// fraction in between.
    #[must_use]
    pub fn immersion(&self, point: Vector) -> f64 {
        let dx = point.x - self.pos.x;
        let dy = point.y - self.pos.y;
        let dz = point.z - self.pos.z;
        let radius = e(self.radius());
        let dist_sq = (e(dx) * e(dx) + e(dz) * e(dz)) + e(dy) * e(dy);
        if radius * radius < dist_sq {
            return 0.0;
        }
        let value = self.classify_point(point);
        let value_f = value as f32;
        if value >= 1.0 {
            return 0.0;
        }
        let edge = e(self.edge_fraction);
        if (1.0 - edge) > e(value_f) || edge < 1.0e-4 {
            return 1.0;
        }
        (1.0 - e(value_f)) / edge
    }

    /// `Intersect(start, end)`: where the segment enters the zone, if it
    /// does. A shape without a clip of its own (mesh, invalid) is reported
    /// and clipped as its bounding sphere.
    pub fn intersect(&self, start: Vector, end: Vector, reports: &mut Reports) -> Option<Vector> {
        let s = Segment { start, end };
        match self.shape {
            ZoneShape::Ellipsoid => return clip_ellipsoid(self, s),
            ZoneShape::Box => return clip_box(self, s),
            ZoneShape::Cylinder => return clip_cylinder(self, s),
            ZoneShape::Ring => return clip_ring(self, s),
            ZoneShape::Sphere => {}
            ZoneShape::Mesh | ZoneShape::Invalid => reports.push(
                super::SOURCE,
                0x55a,
                "*** WARNING: PCI - zone type not supported!",
            ),
        }
        clip_sphere(self.pos, self.radius(), s)
    }
}

/// `RingSurfaceScale` (as in [`super::shape`]).
fn ring_surface_scale(size: Vector, w: Vector) -> f64 {
    let half = e(size.z) * 0.5;
    let abs_y = e(w.y).abs();
    let cutoff = half / (e(size.x) * e(size.x) + half * half).sqrt();
    if abs_y > cutoff {
        return half / abs_y;
    }
    ((e(size.x) - e(size.y)) * 0.5) / (e(w.z) * e(w.z) + e(w.x) * e(w.x)).sqrt()
}

// --- Culls -----------------------------------------------------------------

/// A plane: unit normal and offset (`FCullPlane`).
#[derive(Clone, Copy)]
struct Plane {
    normal: Vector,
    d: f32,
}

/// A quaternion, `w` first (`FCullQuat`).
#[derive(Clone, Copy)]
struct Quat {
    w: f32,
    x: f32,
    y: f32,
    z: f32,
}

/// The ellipsoid the culls build from a zone (`FZoneEllipsoid`).
struct Ellipsoid {
    orientation: Matrix,
    pos: Vector,
    radius: [f32; 3],
    scaled: Matrix,
}

impl Ellipsoid {
    /// `CullEllipsoid` with `EllipsoidSetRadii`.
    fn of(zone: &Zone) -> Self {
        let radius = [zone.size.x, zone.size.y, zone.size.z];
        let mut scaled = zone.orientation;
        for (axis, &r) in radius.iter().enumerate() {
            let square = e(r) * e(r);
            for row in &mut scaled.m {
                row[axis] = (square * e(row[axis])) as f32;
            }
        }
        Self {
            orientation: zone.orientation,
            pos: zone.pos,
            radius,
            scaled,
        }
    }

    /// `EllipsoidMinRadius`.
    fn min_radius(&self) -> f32 {
        let [r0, r1, r2] = self.radius;
        if r0 < r1 {
            if r0 < r2 { r0 } else { r2 }
        } else if r1 < r2 {
            r1
        } else {
            r2
        }
    }

    /// `EllipsoidRadiusAlong`.
    fn radius_along(&self, dir: Vector) -> f64 {
        let a = e(self.radius[0]) * e(self.radius[0]);
        let b = e(self.radius[1]) * e(self.radius[1]);
        let c = e(self.radius[2]) * e(self.radius[2]);
        let (x, y, z) = (e(dir.x), e(dir.y), e(dir.z));
        let sum = ((((x * x) * c) * b) + (((z * z) * b) * a)) + (((y * y) * c) * a);
        let product = (a * (c * b)) * sum;
        if !(product > 1.0e-4) {
            return 0.0;
        }
        product.sqrt() / sum
    }

    /// `EllipsoidPlaneSide`: 0 straddling, 1 on the normal's side, 2 behind.
    fn plane_side(&self, plane: &Plane) -> i32 {
        let spread = transform_matrix(&self.scaled.transpose(), plane.normal);
        let n = plane.normal;
        let dist = ((e(self.pos.y) * e(n.y) + e(self.pos.z) * e(n.z)) + e(n.x) * e(self.pos.x))
            - e(plane.d);
        let extent = (e(spread.z) * e(n.z) + e(spread.x) * e(n.x)) + e(spread.y) * e(n.y);
        if !(extent <= dist * dist) {
            return 0;
        }
        if dist < 0.0 { 2 } else { 1 }
    }

    /// `EllipsoidSignedDistance`.
    fn signed_distance(&self, point: Vector) -> f64 {
        let dx = e(point.x) - e(self.pos.x);
        let dy = (e(point.y) - e(self.pos.y)) as f32;
        let dz = e(point.z) - e(self.pos.z);
        let length = ((dx * dx + dz * dz) + e(dy) * e(dy)).sqrt();
        let length_f = length as f32;
        if length < 1.0e-4 {
            return -e(self.min_radius());
        }
        let inv = 1.0 / e(length_f);
        let dir = v((dx * inv) as f32, (e(dy) * inv) as f32, (dz * inv) as f32);
        let local = transpose_transform(&self.orientation, dir);
        e(length_f) - self.radius_along(local)
    }
}

fn column(f: &Frustum, c: usize) -> Vector {
    let m = &f.xform.rotation.m;
    v(m[0][c], m[1][c], m[2][c])
}

/// `CullPlaneSet`: the normal made unit, the offset through `point`.
fn plane_set(point: Vector, n: Vector) -> Plane {
    let inv = 1.0 / length_sq(n).sqrt();
    let nx = inv * e(n.x);
    let ny = inv * e(n.y);
    let nz = inv * e(n.z);
    Plane {
        normal: v(nx as f32, ny as f32, nz as f32),
        d: ((nz * e(point.z) + ny * e(point.y)) + nx * e(point.x)) as f32,
    }
}

/// `CullQuatFromAxisAngle`.
fn quat_from_axis_angle(axis: Vector, angle: f32) -> Quat {
    let half = e(angle) * 0.5;
    let length = length_sq(axis).sqrt();
    let s = (half.sin() / length) as f32;
    Quat {
        w: half.cos() as f32,
        x: s * axis.x,
        y: s * axis.y,
        z: s * axis.z,
    }
}

/// `CullQuatRotate`.
fn quat_rotate(q: Quat, a: Vector) -> Vector {
    let cross_x = e(q.z) * e(a.y) - e(a.z) * e(q.y);
    let cross_y = e(a.z) * e(q.x) - e(q.z) * e(a.x);
    let cross_z = (e(a.x) * e(q.y) - e(q.x) * e(a.y)) as f32;
    let two_w = (e(q.w) + e(q.w)) as f32;
    let spin_x = (cross_x * e(two_w)) as f32;
    let spin_y = cross_y * e(two_w);
    let spin_z = e(cross_z) * e(two_w);
    let two_dot = ((e(q.z) * e(a.z) + e(q.y) * e(a.y)) + e(q.x) * e(a.x)) * 2.0;
    let along_x = two_dot * e(q.x);
    let along_y = (two_dot * e(q.y)) as f32;
    let along_z = (two_dot * e(q.z)) as f32;
    let cos2 = (e(q.w) * e(q.w)) * 2.0 - 1.0;
    let keep_x = (cos2 * e(a.x)) as f32;
    let keep_y = (cos2 * e(a.y)) as f32;
    let keep_z = (cos2 * e(a.z)) as f32;
    let sum_x = e(keep_x) + along_x;
    let sum_y = e(keep_y) + e(along_y);
    let sum_z = (e(keep_z) + e(along_z)) as f32;
    v(
        (sum_x - e(spin_x)) as f32,
        (sum_y - spin_y) as f32,
        (e(sum_z) - spin_z) as f32,
    )
}

/// `CullRotateAboutAxis`.
fn rotate_about_axis(a: Vector, axis: Vector, angle: f32) -> Vector {
    let q = quat_from_axis_angle(axis, angle);
    let cross_x = e(q.z) * e(a.y) - e(q.y) * e(a.z);
    let cross_y = e(q.x) * e(a.z) - e(q.z) * e(a.x);
    let cross_z = e(q.y) * e(a.x) - e(q.x) * e(a.y);
    let two_w = (e(q.w) + e(q.w)) as f32;
    let spin = v(
        (cross_x * e(two_w)) as f32,
        (cross_y * e(two_w)) as f32,
        (cross_z * e(two_w)) as f32,
    );
    let two_dot = ((e(q.x) * e(a.x) + e(q.y) * e(a.y)) + e(q.z) * e(a.z)) * 2.0;
    let along = v(
        (two_dot * e(q.x)) as f32,
        (e(q.y) * two_dot) as f32,
        (e(q.z) * two_dot) as f32,
    );
    let cos2 = (e(q.w) * e(q.w)) * 2.0 - 1.0;
    let keep = v(
        (cos2 * e(a.x)) as f32,
        (cos2 * e(a.y)) as f32,
        (cos2 * e(a.z)) as f32,
    );
    let sum_x = e(along.x) + e(keep.x);
    let sum_y = e(along.y) + e(keep.y);
    let sum_z = (e(along.z) + e(keep.z)) as f32;
    v(
        (sum_x - e(spin.x)) as f32,
        (sum_y - e(spin.y)) as f32,
        (e(sum_z) - e(spin.z)) as f32,
    )
}

/// The turn angles of sides 2 to 5.
fn side_angle(f: &Frustum, index: usize) -> f32 {
    let horizontal = index == 2 || index == 3;
    let half = e(if horizontal { f.angle_x } else { f.angle_y }) * 0.5;
    if index == 2 || index == 4 {
        (half - e(HALF_PI)) as f32
    } else {
        (e(HALF_PI) - half) as f32
    }
}

/// `FrustumPlane`: side `index` of the frustum as a plane.
fn frustum_plane(f: &Frustum, index: usize) -> Plane {
    let apex = f.xform.translation;
    let (point, normal) = match index {
        0 => (apex, negate(column(f, 2))),
        1 => (sub(apex, scale(column(f, 2), f.distance)), column(f, 2)),
        2..=5 => {
            let axis = column(f, if index <= 3 { 1 } else { 0 });
            let q = quat_from_axis_angle(axis, side_angle(f, index));
            (apex, quat_rotate(q, negate(column(f, 2))))
        }
        _ => (Vector::ZERO, Vector::ZERO),
    };
    plane_set(point, normal)
}

/// `FrustumCullsSphere`: the sphere lies wholly outside one side. Sides 2,
/// 3 and 5 turn their normal with `CullRotateAboutAxis`, side 4 with the
/// quaternion pair, as the C++ does.
fn frustum_culls_sphere(f: &Frustum, center: Vector, radius: f32) -> bool {
    let apex = f.xform.translation;
    let back = negate(column(f, 2));
    for index in 0..6 {
        let (point, normal) = match index {
            0 => (apex, back),
            1 => (sub(apex, scale(column(f, 2), f.distance)), column(f, 2)),
            2 | 3 => (
                apex,
                rotate_about_axis(back, column(f, 1), side_angle(f, index)),
            ),
            4 => {
                let q = quat_from_axis_angle(column(f, 0), side_angle(f, index));
                (apex, quat_rotate(q, back))
            }
            _ => (
                apex,
                rotate_about_axis(back, column(f, 0), side_angle(f, index)),
            ),
        };
        let plane = plane_set(point, normal);
        let n = plane.normal;
        let dist =
            ((e(n.x) * e(center.x) + e(n.z) * e(center.z)) + e(n.y) * e(center.y)) - e(plane.d);
        let r = e(radius);
        if !(dist > r) && dist < -r {
            return true;
        }
    }
    false
}

/// `CapStepCulls`: one plane of the far-cap sweep.
fn cap_step_culls(
    ellipsoid: &Ellipsoid,
    center: Vector,
    sweep: f32,
    step: i32,
    horizontal: bool,
) -> bool {
    let angle = (f64::from(step) * e(sweep)) / f64::from(CAP_STEPS);
    let sin = angle.sin() as f32;
    let neg_cos = -(angle.cos() as f32);
    let dir = if horizontal {
        v(sin, 0.0, neg_cos)
    } else {
        v(0.0, sin, neg_cos)
    };
    ellipsoid.plane_side(&plane_set(center, dir)) == 1
}

/// `BoxSignedDistanceSq`: the signed squared distance to the surface of a
/// box of half extents `half` (negative inside), `z` kept unrounded.
fn box_signed_distance_sq(zone: &Zone, half: [f32; 3], point: Vector) -> f64 {
    let local = transpose_transform(&zone.orientation, sub(point, zone.pos));
    let mut outside = false;
    let mut closest_x = local.x;
    let mut closest_y = local.y;
    let mut closest_z = e(local.z);
    let snap = |dim: f32, l: f32| e(dim).copysign(e(l));
    let raw_x = e(local.x).abs() - e(half[0]);
    let excess_x = raw_x as f32;
    if raw_x >= 0.0 {
        closest_x = snap(half[0], local.x) as f32;
        outside = true;
    }
    let raw_y = e(local.y).abs() - e(half[1]);
    let excess_y = raw_y as f32;
    if raw_y >= 0.0 {
        closest_y = snap(half[1], local.y) as f32;
        outside = true;
    }
    let raw_z = e(local.z).abs() - e(half[2]);
    let excess_z = raw_z as f32;
    if raw_z >= 0.0 {
        closest_z = snap(half[2], local.z);
        outside = true;
    } else if !outside {
        if excess_x >= excess_y {
            if excess_x >= excess_z {
                closest_x = snap(half[0], local.x) as f32;
            } else {
                closest_z = snap(half[2], local.z);
            }
        } else if excess_y >= excess_z {
            closest_y = snap(half[1], local.y) as f32;
        } else {
            closest_z = snap(half[2], local.z);
        }
    }
    let dx = (e(local.x) - e(closest_x)) as f32;
    let dy = (e(local.y) - e(closest_y)) as f32;
    let dz = e(local.z) - closest_z;
    let value = (dz * dz + e(dy) * e(dy)) + e(dx) * e(dx);
    if outside { value } else { -value }
}

impl Zone {
    /// `FrustumCull`: true when the zone lies outside the frustum. A zone
    /// that is not an ellipsoid is tested as its bounding sphere; an
    /// ellipsoid against the six sides, then, when it straddles the far
    /// side and exactly one of a pair of opposite sides, against planes
    /// turned step by step around that far edge.
    #[must_use]
    pub fn frustum_cull(&self, f: &Frustum) -> bool {
        if self.shape != ZoneShape::Ellipsoid {
            return frustum_culls_sphere(f, self.pos, self.radius());
        }
        let ellipsoid = Ellipsoid::of(self);
        let mut sides = [0i32; 6];
        for (index, side) in sides.iter_mut().enumerate() {
            *side = ellipsoid.plane_side(&frustum_plane(f, index));
            if *side == 2 {
                return true;
            }
        }
        if sides[1] != 0 {
            return false;
        }
        let straddle = u32::from(sides[2] == 0)
            | (u32::from(sides[3] == 0) << 1)
            | (u32::from(sides[4] == 0) << 2)
            | (u32::from(sides[5] == 0) << 3);
        let distance = f.distance;
        let back = negate(column(f, 2));
        let horizontal = straddle & 3;
        if horizontal == 1 || horizontal == 2 {
            let spread = ((e(f.angle_x) * 0.5).tan() * e(distance)) as f32;
            let right = column(f, 0);
            let center = add(f.xform.translation, scale(back, distance));
            let (shift, sweep) = if straddle & 1 != 0 {
                (
                    scale(right, -spread),
                    ((e(f.angle_x) + e(PI)) * -0.5) as f32,
                )
            } else {
                (scale(right, spread), ((e(f.angle_x) + e(PI)) * 0.5) as f32)
            };
            let center = add(shift, center);
            for step in 1..CAP_STEPS {
                if cap_step_culls(&ellipsoid, center, sweep, step, true) {
                    return true;
                }
            }
        }
        let vertical = straddle & 0xc;
        if vertical == 4 || vertical == 8 {
            let spread = ((e(f.angle_y) * 0.5).tan() * e(distance)) as f32;
            let up = column(f, 1);
            let center = add(f.xform.translation, scale(back, distance));
            let (shift, sweep) = if straddle & 4 != 0 {
                (scale(up, spread), ((e(f.angle_y) + e(PI)) * 0.5) as f32)
            } else {
                (scale(up, -spread), ((e(f.angle_y) + e(PI)) * -0.5) as f32)
            };
            let center = add(shift, center);
            for step in 1..CAP_STEPS {
                if cap_step_culls(&ellipsoid, center, sweep, step, false) {
                    return true;
                }
            }
        }
        false
    }

    /// `SphereCull`: true when the sphere does not reach the zone.
    #[must_use]
    pub fn sphere_cull(&self, sphere: &Sphere) -> bool {
        let r = e(sphere.radius);
        match self.shape {
            ZoneShape::Ellipsoid => !(Ellipsoid::of(self).signed_distance(sphere.center) < r),
            ZoneShape::Box => {
                let half = [self.size.x * 0.5, self.size.y * 0.5, self.size.z * 0.5];
                !(box_signed_distance_sq(self, half, sphere.center) < r * r)
            }
            ZoneShape::Cylinder => {
                let half = [self.size.x, (e(self.size.y) * 0.5) as f32, self.size.x];
                let local = transpose_transform(&self.orientation, sub(sphere.center, self.pos));
                let (clamped, outside) = clamp_to_box_surface(half, local);
                let mut value = length_sq(sub(local, clamped));
                if !outside {
                    value = -value;
                }
                !(value < r * r)
            }
            _ => {
                let zone_radius = self.radius();
                let dx = e(sphere.center.x) - e(self.pos.x);
                let dy = e(sphere.center.y) - e(self.pos.y);
                let dz = e(sphere.center.z) - e(self.pos.z);
                let reach = e(zone_radius) + r;
                let sum = (dx * dx + dz * dz) + dy * dy;
                !(reach * reach > sum)
            }
        }
    }
}
