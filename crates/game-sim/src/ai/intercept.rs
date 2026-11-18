//! Intercept, clearance and heading-cone queries
//! (`Common/common_aiintercept.cpp`, declared in `include/common_aisteer.h`):
//! the distance an AI ship has to close on another object, whether a point
//! lies outside an object's heading cone, the shape-biased dodge direction
//! and the turn-circle cost `MoveIdea` caps its throttle with.
//!
//! The reference reads both objects through an `FAiTarget` record, a lazy
//! cache of `get_type`, `get_physical_radius` and the box corners over one
//! `IObjInspect`. An [`ObjectView`] is already one consistent snapshot, so
//! the functions take views: the cached position is the view's `center`
//! (what `get_physical_radius` answers), the cached radius its `radius`,
//! the cached flags its `type_flags`. A null interface is `None`.
//! `get_physical_radius` on a view always succeeds.

use neuron_core::math::{Transform, Vector, inverse_transform, rotate, transform_xform};

use super::fp::{
    add, column2, dot_zyx, e, inline_fast_length, neg, normalize_by_length, r, scale, sub,
};
use super::raycast::{AabbMinMax, ray_transformed_box_nearest_hit};
use super::world::{AiWorld, MotionModel, ObjectView, ThrustEquipType};

/// The ship-like type bits (`0x30000`) both clearance gates test.
const SHIP_LIKE: u32 = 0x30000;
/// The extra target bits the `OfIface` gates accept on their own (`0x207`).
const IFACE_DIRECT: u32 = 0x207;

/// `(z*z + y*y) + x*x` of three unrounded differences.
fn len_sq_zyx(dx: f64, dy: f64, dz: f64) -> f64 {
    (dz * dz + dy * dy) + dx * dx
}

/// `ClampCos11`: above 1 is 1, below -1 is -1, NaN passes.
fn clamp_cos11(v: f64) -> f64 {
    // `f64::clamp` passes NaN, as the two compares do.
    v.clamp(-1.0, 1.0)
}

/// The archetype box of a view as `{min x, max x, min y, max y, min z,
/// max z}`; a failed `get_archetype_extents` zeroes both corners.
fn aabb_of(view: &ObjectView) -> AabbMinMax {
    let (min, max) = view.extents.unwrap_or((Vector::ZERO, Vector::ZERO));
    [min.x, max.x, min.y, max.y, min.z, max.z]
}

/// The negated column 2 of a view's orientation (its heading), zero for
/// `None`.
fn heading_of(view: Option<&ObjectView>) -> Vector {
    view.map_or(Vector::ZERO, |v| neg(column2(&v.orientation)))
}

/// `ComputeCachedRadialClearance` (`common_aiintercept.cpp:34`): the fast
/// distance between the two centres less both radii, at least 0 (NaN
/// passes).
fn cached_radial_clearance(elem: &ObjectView, target_center: Vector, target_radius: f32) -> f64 {
    let dx = e(elem.center.x) - e(target_center.x);
    let dy = e(elem.center.y) - e(target_center.y);
    let dz = e(elem.center.z) - e(target_center.z);
    let dist = r(inline_fast_length(len_sq_zyx(dx, dy, dz)));
    let clearance = e(dist) - (e(elem.radius) + e(target_radius));
    if clearance < 0.0 { 0.0 } else { clearance }
}

/// `EstimateLeadInterceptDistance` (`common_aiintercept.cpp:73`): the
/// distance from `elem`'s surface (its position pushed out by its radius
/// toward the target) to the target's transformed archetype box; 0 when
/// that point is already inside the box, the fast distance to the
/// target's centre when the ray misses. (The reference also fills the
/// record's corner cache, which nothing here reads.)
#[must_use]
pub fn estimate_lead_intercept_distance(elem: &ObjectView, target: &ObjectView) -> f64 {
    let elem_pos = elem.position;
    let mut dir = sub(target.position, elem_pos);
    let inv = 1.0 / len_sq_zyx(e(dir.x), e(dir.y), e(dir.z)).sqrt();
    dir = Vector::new(r(e(dir.x) * inv), r(e(dir.y) * inv), r(e(dir.z) * inv));

    let offset = scale(dir, elem.radius);
    let ray_origin = add(elem_pos, offset);
    let aabb = aabb_of(target);
    match ray_transformed_box_nearest_hit(ray_origin, dir, &target.transform(), &aabb) {
        None => {
            let c = target.center;
            inline_fast_length(len_sq_zyx(
                e(c.x) - e(elem_pos.x),
                e(c.y) - e(elem_pos.y),
                e(c.z) - e(elem_pos.z),
            ))
        }
        Some(hit) => {
            if hit.starts_inside {
                return 0.0;
            }
            inline_fast_length(len_sq_zyx(
                e(hit.point.x) - e(ray_origin.x),
                e(hit.point.y) - e(ray_origin.y),
                e(hit.point.z) - e(ray_origin.z),
            ))
        }
    }
}

/// `GetInterceptClearance` (`common_aiintercept.cpp:187`): the radial
/// clearance when both objects carry the ship-like bits, otherwise
/// [`estimate_lead_intercept_distance`].
#[must_use]
pub fn get_intercept_clearance(elem: &ObjectView, target: &ObjectView) -> f64 {
    let have_both = elem.type_flags & SHIP_LIKE != 0 && target.type_flags & SHIP_LIKE != 0;
    if have_both {
        cached_radial_clearance(elem, target.center, target.radius)
    } else {
        estimate_lead_intercept_distance(elem, target)
    }
}

/// `IsWithinInterceptRange` (`common_aiintercept.cpp:167`): the clearance
/// at most `threshold` (NaN is outside).
#[must_use]
pub fn is_within_intercept_range(elem: &ObjectView, target: &ObjectView, threshold: f32) -> bool {
    get_intercept_clearance(elem, target) <= e(threshold)
}

/// The gate of the `OfIface` pair: both ship-like, or the target has any
/// of `0x207`.
fn iface_gate(elem: &ObjectView, target: &ObjectView) -> bool {
    (elem.type_flags & SHIP_LIKE != 0 && target.type_flags & SHIP_LIKE != 0)
        || target.type_flags & IFACE_DIRECT != 0
}

/// `GetInterceptClearanceOfIface` (`common_aiintercept.cpp:324`): as
/// [`get_intercept_clearance`] with the wider gate; the estimate branch is
/// not clamped.
#[must_use]
pub fn get_intercept_clearance_of_iface(elem: &ObjectView, target: &ObjectView) -> f64 {
    if iface_gate(elem, target) {
        cached_radial_clearance(elem, target.center, target.radius)
    } else {
        estimate_lead_intercept_distance(elem, target)
    }
}

/// `IsWithinInterceptRangeOfIface` (`common_aiintercept.cpp:236`).
#[must_use]
pub fn is_within_intercept_range_of_iface(
    elem: &ObjectView,
    target: &ObjectView,
    threshold: f32,
) -> bool {
    get_intercept_clearance_of_iface(elem, target) <= e(threshold)
}

/// `IsPointOutsideHeadingCone` (`common_aiintercept.cpp:383`): whether the
/// direction from the object's centre to `point` makes a cosine below
/// `cos_threshold` with its heading. `None` measures from the origin with
/// a zero heading.
#[must_use]
pub fn is_point_outside_heading_cone(
    point: Vector,
    iface: Option<&ObjectView>,
    cos_threshold: f32,
) -> bool {
    let centre = iface.map_or(Vector::ZERO, |v| v.center);
    let d = sub(point, centre);
    let length = len_sq_zyx(e(d.x), e(d.y), e(d.z)).sqrt();
    let dir = if length > e(0.01) {
        let inv = 1.0 / length;
        Vector::new(r(e(d.x) * inv), r(e(d.y) * inv), r(e(d.z) * inv))
    } else {
        Vector::ZERO
    };
    let dot = clamp_cos11(dot_zyx(dir, heading_of(iface)));
    dot < e(cos_threshold)
}

/// `IsAlignedAndOutsideHeadingCone` (`common_aiintercept.cpp:475`): the
/// two headings point the same way (positive dot) and `elem`'s centre is
/// outside the target's heading cone.
#[must_use]
pub fn is_aligned_and_outside_heading_cone(
    elem: &ObjectView,
    target: Option<&ObjectView>,
    cos_threshold: f32,
) -> bool {
    let target_heading = heading_of(target);
    let owner_heading = heading_of(Some(elem));
    let alignment = r(dot_zyx(target_heading, owner_heading));
    let target_centre = target.map_or(Vector::ZERO, |v| v.center);
    let mut dir = sub(elem.center, target_centre);
    let distance = len_sq_zyx(e(dir.x), e(dir.y), e(dir.z)).sqrt();
    dir = if distance > e(0.01) {
        normalize_by_length(dir, r(distance))
    } else {
        Vector::ZERO
    };
    let dot = clamp_cos11(dot_zyx(dir, heading_of(target)));
    if !(alignment > 0.0) {
        return false;
    }
    dot < e(cos_threshold)
}

/// `GetSignedRadialClearanceToPoint` (`common_aiintercept.cpp:569`): the
/// distance from `elem`'s centre to `point` less its radius.
#[must_use]
pub fn signed_radial_clearance_to_point(elem: &ObjectView, point: Vector) -> f64 {
    let dx = e(elem.center.x) - e(point.x);
    let dy = e(elem.center.y) - e(point.y);
    let dz = e(elem.center.z) - e(point.z);
    let dist = r(((dx * dx + dz * dz) + dy * dy).sqrt());
    e(dist) - e(elem.radius)
}

/// `IsTargetCoAlignedAndAhead` (`common_aiintercept.cpp:593`): the forward
/// axes agree and the target's centre is outside the reference's heading
/// cone.
#[must_use]
pub fn is_target_co_aligned_and_ahead(
    target: &ObjectView,
    reference: &ObjectView,
    cos_threshold: f32,
) -> bool {
    let heading_dot = r(dot_zyx(
        column2(&reference.orientation),
        column2(&target.orientation),
    ));
    let d = sub(target.center, reference.center);
    let length = len_sq_zyx(e(d.x), e(d.y), e(d.z)).sqrt();
    let direction = if length > e(0.01) {
        let inv = 1.0 / length;
        Vector::new(r(e(d.x) * inv), r(e(d.y) * inv), r(e(d.z) * inv))
    } else {
        Vector::ZERO
    };
    let cosine = clamp_cos11(dot_zyx(direction, heading_of(Some(reference))));
    if !(heading_dot > 0.0) {
        return false;
    }
    cosine < e(cos_threshold)
}

/// `GetCachedRange` (`common_aiintercept.cpp:671`): the attitude of
/// `reference` toward `object` and back, both 0 when either is missing or
/// the query fails.
pub fn cached_range<W: AiWorld + ?Sized>(
    world: &W,
    reference: Option<W::Handle>,
    object: Option<W::Handle>,
) -> (f32, f32) {
    match (reference, object) {
        (Some(a), Some(b)) => world.attitude_symmetrical(a, b).unwrap_or((0.0, 0.0)),
        _ => (0.0, 0.0),
    }
}

/// `GetNegatedIfaceDirection` (`common_aiintercept.cpp:712`): the heading,
/// zero for `None`.
#[must_use]
pub fn negated_iface_direction(iface: Option<&ObjectView>) -> Vector {
    heading_of(iface)
}

/// `GetIfaceVectorMagnitude` (`common_aiintercept.cpp:728`): the speed,
/// `sqrt((x*x + y*y) + z*z)` of the velocity, unrounded; 0 for `None`.
#[must_use]
pub fn iface_vector_magnitude(iface: Option<&ObjectView>) -> f64 {
    iface.map_or(0.0, |v| {
        let w = v.velocity;
        ((e(w.x) * e(w.x) + e(w.y) * e(w.y)) + e(w.z) * e(w.z)).sqrt()
    })
}

/// `TransformBoundingBoxCorners` (`common_aiintercept.cpp:764`): the eight
/// corners of `[min, max]` through `xform`, X the high bit of the index
/// and Z the low one.
#[must_use]
pub fn transform_bounding_box_corners(xform: &Transform, min: Vector, max: Vector) -> [Vector; 8] {
    core::array::from_fn(|index| {
        let corner = Vector::new(
            if index & 4 != 0 { max.x } else { min.x },
            if index & 2 != 0 { max.y } else { min.y },
            if index & 1 != 0 { max.z } else { min.z },
        );
        transform_xform(xform, corner)
    })
}

/// `ComputeShapeBiasedAvoidDirection` (`common_aiintercept.cpp:794`): a
/// world direction away from the target's centre in `elem`'s frame, biased
/// sideways for a flat obstacle and vertically for a tall one; zero for
/// `None`, `(1, 0, 0)` (not rotated) when the centre is on top of `elem`.
#[must_use]
pub fn shape_biased_avoid_direction(elem: &ObjectView, target: Option<&ObjectView>) -> Vector {
    let Some(target) = target else {
        return Vector::ZERO;
    };
    let frame = elem.transform();
    let local = inverse_transform(&frame, target.center);
    let work_x = local.x;
    let mut work_y = local.y;
    let mut work_z = local.z;
    let local_len_sq =
        (e(local.x) * e(local.x) + e(local.y) * e(local.y)) + e(local.z) * e(local.z);
    if inline_fast_length(local_len_sq) < 1e-5 {
        return Vector::new(1.0, 0.0, 0.0);
    }

    let (min, max) = target.extents.unwrap_or((Vector::ZERO, Vector::ZERO));
    let horizontal = ((e(max.z) - e(min.z)) + (e(max.x) - e(min.x))) * 0.5;
    let vertical = e(max.y) - e(min.y);
    let flat = horizontal / vertical > 2.0;
    let tall = vertical / horizontal > 2.0;

    let mut axial;
    if flat {
        let scaled = e(local.x) * e(0.3);
        let inv = 1.0 / ((e(work_z) * e(work_z) + e(work_y) * e(work_y)) + scaled * scaled).sqrt();
        let inv_f = r(inv);
        axial = scaled * inv;
        work_y = r(e(inv_f) * e(work_y));
        work_z = r(e(work_z) * e(inv_f));
    } else {
        axial = e(work_x);
    }
    if tall {
        let scaled = e(work_y) * e(0.3);
        let inv = 1.0 / ((e(work_z) * e(work_z) + scaled * scaled) + axial * axial).sqrt();
        axial *= inv;
        work_y = r(inv * scaled);
    }

    let neg_axial = -axial;
    let neg_y = -e(work_y);
    let inv2 = 1.0 / (neg_y * neg_y + neg_axial * neg_axial).sqrt();
    let inv2_f = r(inv2);
    let dir = Vector::new(
        r(inv2 * neg_axial),
        r(neg_y * e(inv2_f)),
        r(e(inv2_f) * 0.0),
    );
    rotate(&frame, dir)
}

/// `EstimateTurnCircleApproachCost` (`common_aiintercept.cpp:916`): how
/// much throttle a ship may use toward `point` when the point lies inside
/// one of its two turn circles (radius `linear_speed / angular_speed`
/// either side of its centre) and within `max(5 * radius, 100)`: the
/// squared positive facing cosine times `distance * angular_speed /
/// linear_speed`; 1 otherwise. A zero speed is taken from the motion model
/// (`get_angular_speed_XY(0, 1)`, `get_speed(1, engine)`; 0 without one).
#[must_use]
pub fn estimate_turn_circle_approach_cost(
    iface: &ObjectView,
    motion: Option<&dyn MotionModel>,
    point: Vector,
    angular_speed: f32,
    linear_speed: f32,
) -> f64 {
    const DEFAULT: f64 = 1.0;
    let center = iface.center;
    let frame = Transform::new(iface.orientation, center);
    let mut to_point = sub(point, center);
    let len_sq = len_sq_zyx(e(to_point.x), e(to_point.y), e(to_point.z));
    let len_sq_f = r(len_sq);
    let dist = inline_fast_length(len_sq);

    let mut max_range = e(iface.radius) * e(5.0);
    if !(max_range > 100.0) {
        max_range = 100.0;
    }
    if !(dist > e(0.01)) || !(dist < max_range) {
        return DEFAULT;
    }

    let inv = 1.0 / e(len_sq_f).sqrt();
    to_point = Vector::new(
        r(e(to_point.x) * inv),
        r(e(to_point.y) * inv),
        r(e(to_point.z) * inv),
    );

    let mut angular = angular_speed;
    let mut linear = linear_speed;
    if angular == 0.0 {
        angular = motion.map_or(0.0, |m| m.angular_speed_xy(0.0, 1.0));
    }
    if linear == 0.0 {
        linear = motion.map_or(0.0, |m| m.speed(1.0, ThrustEquipType::Engine));
    }

    let turn_radius_d = e(linear) / e(angular);
    let turn_radius = r(turn_radius_d);
    if !(turn_radius_d > e(0.01)) {
        return DEFAULT;
    }

    let right = transform_xform(&frame, Vector::new(turn_radius, 0.0, 0.0));
    let left = transform_xform(&frame, Vector::new(-turn_radius, 0.0, 0.0));
    let tr = sub(point, right);
    let dist_right = r(inline_fast_length(len_sq_zyx(e(tr.x), e(tr.y), e(tr.z))));
    let tl = sub(point, left);
    let dist_left = inline_fast_length(len_sq_zyx(e(tl.x), e(tl.y), e(tl.z)));
    let min_dist = if e(dist_right) < dist_left {
        e(dist_right)
    } else {
        dist_left
    };
    if !(min_dist < e(turn_radius)) {
        return DEFAULT;
    }

    let scaled = r(min_dist * e(angular));
    let forward = neg(column2(&frame.rotation));
    let facing_d = (e(to_point.z) * e(forward.z) + e(to_point.y) * e(forward.y))
        + e(forward.x) * e(to_point.x);
    let facing = if facing_d < 0.0 { 0.0 } else { r(facing_d) };
    // `_CIpow(facing, 2.0)`: the square of a float is exact in a double.
    e(facing) * e(facing) * e(scaled) / e(linear)
}

/// `GetObjectTypeOrZero` (`common_aiintercept.cpp:1058`).
#[must_use]
pub fn object_type_or_zero(iface: Option<&ObjectView>) -> u32 {
    iface.map_or(0, |v| v.type_flags)
}

/// `GetIfacePositionOrZero` (`common_aiintercept.cpp:1122`): the centre
/// `get_physical_radius` answers, zero for `None`.
#[must_use]
pub fn iface_position_or_zero(iface: Option<&ObjectView>) -> Vector {
    iface.map_or(Vector::ZERO, |v| v.center)
}

/// `SystemManagerTimerTable::FindObjectDockingAtBase`
/// (`common_aiintercept.cpp:1078`): the first docking ring (type bit
/// `0x20`) in the base object's system whose dock target is the base's
/// base id, for a planet or moon (type bits `0x3`). A failed `get_base`
/// leaves the id 0, which a ring without a dock target matches, as in the
/// reference.
pub fn find_object_docking_at_base<W: AiWorld + ?Sized>(
    world: &W,
    base: W::Handle,
) -> Option<W::Handle> {
    let view = world.inspect(base)?;
    if view.type_flags & 0x3 == 0 {
        return None;
    }
    let base_id = view.base.unwrap_or(0);
    let system = world.system_of(base)?;
    world.system_objects(system, 0).into_iter().find(|&h| {
        world.inspect(h).is_some_and(|candidate| {
            candidate.type_flags & 0x20 != 0 && candidate.dock_target.unwrap_or(0) == base_id
        })
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_core::math::Matrix;

    fn ship(position: Vector, radius: f32) -> ObjectView {
        ObjectView {
            position,
            center: position,
            radius,
            orientation: Matrix::IDENTITY,
            type_flags: SHIP_LIKE,
            extents: Some((Vector::new(-1.0, -1.0, -1.0), Vector::new(1.0, 1.0, 1.0))),
            ..ObjectView::default()
        }
    }

    #[test]
    fn radial_clearance_subtracts_radii() {
        let a = ship(Vector::ZERO, 10.0);
        let b = ship(Vector::new(0.0, 0.0, 100.0), 20.0);
        let c = get_intercept_clearance(&a, &b);
        assert!((c - 70.0).abs() < 0.2, "{c}");
        assert!(is_within_intercept_range(&a, &b, 71.0));
        assert!(!is_within_intercept_range(&a, &b, 69.0));
        let close = ship(Vector::new(0.0, 0.0, 5.0), 20.0);
        assert_eq!(get_intercept_clearance(&a, &close), 0.0);
    }

    #[test]
    fn lead_distance_hits_the_box() {
        let mut a = ship(Vector::ZERO, 1.0);
        a.type_flags = 0;
        let b = ship(Vector::new(0.0, 0.0, 10.0), 1.0);
        let d = get_intercept_clearance(&a, &b);
        // From z = 1 (pushed out by the radius) to the box face at z = 9.
        assert!((d - 8.0).abs() < 0.05, "{d}");
    }

    #[test]
    fn heading_cone() {
        // Heading is -Z for the identity orientation.
        let s = ship(Vector::ZERO, 1.0);
        assert!(!is_point_outside_heading_cone(
            Vector::new(0.0, 0.0, -10.0),
            Some(&s),
            0.9
        ));
        assert!(is_point_outside_heading_cone(
            Vector::new(10.0, 0.0, 0.0),
            Some(&s),
            0.9
        ));
    }

    #[test]
    fn turn_circle_cost() {
        let s = ship(Vector::ZERO, 10.0);
        // A point beside the ship, inside the right turn circle.
        let cost =
            estimate_turn_circle_approach_cost(&s, None, Vector::new(20.0, 0.0, -1.0), 1.0, 50.0);
        assert!(cost < 1.0, "{cost}");
        // Far away: the default.
        let far =
            estimate_turn_circle_approach_cost(&s, None, Vector::new(0.0, 0.0, -500.0), 1.0, 50.0);
        assert_eq!(far, 1.0);
    }

    #[test]
    fn corners_and_avoid() {
        let corners = transform_bounding_box_corners(
            &Transform::new(Matrix::IDENTITY, Vector::new(1.0, 2.0, 3.0)),
            Vector::new(-1.0, -1.0, -1.0),
            Vector::new(1.0, 1.0, 1.0),
        );
        assert_eq!(corners[0], Vector::new(0.0, 1.0, 2.0));
        assert_eq!(corners[7], Vector::new(2.0, 3.0, 4.0));
        let a = ship(Vector::ZERO, 1.0);
        assert_eq!(shape_biased_avoid_direction(&a, None), Vector::ZERO);
        assert_eq!(
            shape_biased_avoid_direction(&a, Some(&a)),
            Vector::new(1.0, 0.0, 0.0)
        );
    }
}
