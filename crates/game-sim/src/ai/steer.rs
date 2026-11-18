//! Steering frames and turn throttles (`Common/common_aisteer.cpp`, the
//! slerp of `common_obstacleavoidance.cpp`, `RandomVector` of
//! `common_misc.cpp`): orientation bases built from a direction, the
//! random spread frame of the gun and missile aim, and
//! `TurnHelper::get_angular_throttle`.
//!
//! Sines, cosines, arc sines and arc cosines come from the Rust standard
//! library where the reference uses `FSIN`, `FCOS` and the CRT's
//! `_CIacos` / `_CIasin` (decision T11), so their results are Tier B.

use neuron_core::math::{
    Matrix, Quaternion, Vector, matrix_to_quaternion, mul_matrix, mul_quat, quaternion_to_matrix,
    transform_matrix, transpose_transform,
};

use super::fp::{
    column0, column1, column2, cross, dot_zyx, e, from_columns, neg, normalize_by_length,
    normalize_or_zero, normalize3, r, scale, sub,
};
use super::world::AiRand;

/// The clamped `float` dot of `BuildOrientationBasis`'s fallback
/// (`ClampedFloatDot`): the unrounded value is tested above 1, the
/// rounded one below -1.
fn clamped_float_dot(a: Vector, b: Vector) -> f32 {
    let d = r(dot_zyx(a, b));
    // NaN passes, as in the reference.
    d.clamp(-1.0, 1.0)
}

/// `BuildOrientationBasis`: the basis `[right | up | z]` with
/// `z = normalize(-forward)`, `up` from `up_hint` when it is not within
/// about 2.6° of `z`, otherwise from `reference`'s columns. A forward
/// shorter than 0.01 gives the zero matrix.
#[must_use]
pub fn build_orientation_basis(
    reference: &Matrix,
    forward: Vector,
    up_hint: Option<Vector>,
) -> Matrix {
    let z_raw = neg(forward);
    let z_raw_sq = (e(z_raw.y) * e(z_raw.y) + e(z_raw.z) * e(z_raw.z)) + e(z_raw.x) * e(z_raw.x);
    if z_raw_sq < 0.0001 {
        return Matrix::default();
    }
    let z = normalize_or_zero(z_raw);

    if let Some(hint) = up_hint {
        let mut d = dot_zyx(hint, z);
        d = d.clamp(-1.0, 1.0);
        if d.abs() < 0.999 {
            let right = normalize_or_zero(cross(hint, z));
            let up = cross(z, right);
            return from_columns(right, up, z);
        }
    }

    let mut right = column0(reference);
    let mut up = column1(reference);
    let d0 = clamped_float_dot(right, z);
    let d1 = clamped_float_dot(up, z);
    if e(d1).abs() < e(0.9999) {
        up = normalize_or_zero(sub(up, scale(z, d1)));
        right = normalize3(cross(up, z));
        if dot_zyx(right, column0(reference)) < 0.0 {
            // The reference also negates `up`, which it then recomputes.
            right = scale(right, -1.0);
            up = normalize3(cross(z, right));
        }
    } else {
        right = normalize3(sub(right, scale(z, d0)));
        up = normalize3(cross(z, right));
        if dot_zyx(up, column1(reference)) < 0.0 {
            right = scale(right, -1.0);
            up = scale(up, -1.0);
        }
    }
    from_columns(right, up, z)
}

/// `BuildPerpendicularFrame`: the frame `[right | up | -direction]` with
/// `right = normalize(reference × -direction)` (reference defaults to
/// `+Y`); `None` when `direction` is shorter than about 0.003 or when
/// `|dot(-direction, reference)|` is not below `dot_threshold`.
#[must_use]
pub fn build_perpendicular_frame(
    direction: Vector,
    dot_threshold: f32,
    reference: Option<Vector>,
) -> Option<Matrix> {
    let len_sq = (e(direction.x) * e(direction.x) + e(direction.y) * e(direction.y))
        + e(direction.z) * e(direction.z);
    if len_sq < 1e-05 {
        return None;
    }
    let mut n = neg(direction);
    let n_len = ((e(n.x) * e(n.x) + e(n.y) * e(n.y)) + e(n.z) * e(n.z)).sqrt();
    n = if n_len > e(0.01) {
        normalize_by_length(n, r(n_len))
    } else {
        Vector::ZERO
    };
    let rf = reference.unwrap_or(Vector::new(0.0, 1.0, 0.0));

    let mut dot = (e(n.z) * e(rf.z) + e(n.y) * e(rf.y)) + e(rf.x) * e(n.x);
    dot = dot.clamp(-1.0, 1.0);
    if !(e(dot_threshold) > dot.abs()) {
        return None;
    }

    let mut right = Vector::new(
        r(e(n.z) * e(rf.y) - e(n.y) * e(rf.z)),
        r(e(rf.z) * e(n.x) - e(n.z) * e(rf.x)),
        r(e(n.y) * e(rf.x) - e(rf.y) * e(n.x)),
    );
    let right_len =
        ((e(right.z) * e(right.z) + e(right.y) * e(right.y)) + e(right.x) * e(right.x)).sqrt();
    right = if right_len > e(0.01) {
        normalize_by_length(right, r(right_len))
    } else {
        Vector::ZERO
    };
    let up = Vector::new(
        r(e(right.z) * e(n.y) - e(right.y) * e(n.z)),
        r(e(n.z) * e(right.x) - e(right.z) * e(n.x)),
        r(e(right.y) * e(n.x) - e(n.y) * e(right.x)),
    );
    Some(from_columns(right, up, n))
}

/// `SteeringFrameFromRelativeDir`: [`build_orientation_basis`] toward
/// `rel_dir`, with column 1 of the perpendicular frame (threshold 0.999)
/// as the up hint when that frame exists.
#[must_use]
pub fn steering_frame_from_relative_dir(
    reference: &Matrix,
    rel_dir: Vector,
    up_hint: Vector,
) -> Matrix {
    match build_perpendicular_frame(rel_dir, 0.999, Some(up_hint)) {
        Some(frame) => build_orientation_basis(reference, rel_dir, Some(column1(&frame))),
        None => build_orientation_basis(reference, rel_dir, None),
    }
}

/// `RandomVector(radius)`: three draws less 0x3fff scaled to `radius`.
/// Three draws of 0x3fff divide by zero, as in the reference.
pub fn random_vector(rand: &mut impl AiRand, radius: f32) -> Vector {
    let x = (rand.rand() - 0x3fff) as f32;
    let y = (rand.rand() - 0x3fff) as f32;
    let z = f64::from(rand.rand() - 0x3fff);
    let s = e(radius) / ((z * z + e(y) * e(y)) + e(x) * e(x)).sqrt();
    Vector::new(r(e(x) * s), r(e(y) * s), r(s * z))
}

/// `BuildRandomSpreadFrame`: `reference` turned about a random axis by
/// `spread_angle`, re-based on world up (or on the reference's up and
/// then rolled by `bank_angle` when that is positive). Draws three
/// values for the axis and, with a bank angle, one for the roll sign.
pub fn build_random_spread_frame(
    rand: &mut impl AiRand,
    reference: &Matrix,
    spread_angle: f32,
    bank_angle: f32,
) -> Matrix {
    let quat0 = matrix_to_quaternion(reference);
    let axis = random_vector(rand, 1.0);
    let half = e(spread_angle) * e(0.5);
    let axis_len = ((e(axis.y) * e(axis.y) + e(axis.z) * e(axis.z)) + e(axis.x) * e(axis.x)).sqrt();
    let k = r(half.sin() / axis_len);
    let quat1 = Quaternion::new(
        r(half.cos()),
        r(e(axis.x) * e(k)),
        r(e(axis.y) * e(k)),
        r(e(axis.z) * e(k)),
    );
    let frame = quaternion_to_matrix(&mul_quat(&quat0, &quat1));
    let use_reference_up = bank_angle > 0.0;
    let forward = neg(column2(&frame));
    let up_hint = if use_reference_up {
        column1(reference)
    } else {
        Vector::new(0.0, 1.0, 0.0)
    };
    let mut working = build_orientation_basis(&frame, forward, Some(up_hint));
    if use_reference_up {
        // `rand() * (1/32768) < 0` never holds; the draw is still made.
        let mut roll = e(bank_angle);
        if f64::from(rand.rand()) * e(3.051_757_8e-5) < 0.0 {
            roll = -roll;
        }
        let cos_roll = r(roll.cos());
        let sin_roll = roll.sin();
        for row in &mut working.m {
            let c0 = e(row[0]);
            let c1 = e(row[1]);
            row[0] = r(sin_roll * c1 + c0 * e(cos_roll));
            row[1] = r(e(cos_roll) * c1 - c0 * sin_roll);
        }
    }
    working
}

/// `TransformVectorByMatrix`: the engine's `TransformMatrix`.
#[must_use]
pub fn transform_vector_by_matrix(m: &Matrix, v: Vector) -> Vector {
    transform_matrix(m, v)
}

/// `RotateTowardBySlerp`: `from` turned toward `to` by `angle` about
/// their cross product; `to` itself once they are within about 2.6°.
#[must_use]
pub fn rotate_toward_by_slerp(from: Vector, to: Vector, angle: f32) -> Vector {
    let dot = (e(from.x) * e(to.x) + e(to.z) * e(from.z)) + e(from.y) * e(to.y);
    if dot > 0.999 {
        return to;
    }
    let ax = e(from.y) * e(to.z) - e(to.y) * e(from.z);
    let ay = e(to.x) * e(from.z) - e(to.z) * e(from.x);
    let az = r(e(to.y) * e(from.x) - e(from.y) * e(to.x));
    let half = e(angle) * e(0.5);
    let axis_len = ((e(az) * e(az) + ax * ax) + ay * ay).sqrt();
    let s = r(half.sin() / axis_len);
    let quat = Quaternion::new(r(half.cos()), r(e(s) * ax), r(e(s) * ay), r(e(az) * e(s)));
    transform_matrix(&quaternion_to_matrix(&quat), from)
}

/// What `TurnHelper::get_angular_throttle` returns: the world-space
/// axis throttle that turns frame `a` toward frame `b`, and the angle
/// left to turn.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct AngularThrottle {
    /// The axis throttle, world space.
    pub throttle: Vector,
    /// The angle between the frames, radians, in `[-π, π]`.
    pub angle: f32,
}

/// `TurnHelper::get_angular_throttle(A, B, &angle)`.
#[must_use]
pub fn get_angular_throttle(a: &Matrix, b: &Matrix) -> AngularThrottle {
    let relative = mul_matrix(b, &a.transpose());
    let quat = matrix_to_quaternion(&relative);
    let w = quat.w.clamp(-1.0, 1.0);
    let mut angle = r(e(w).acos() * 2.0);

    let mut axis = Vector::new(r(e(quat.x).asin()), r(e(quat.y).asin()), 0.0);
    let asin_z = e(quat.z).asin();
    axis.z = r(asin_z);
    let axis_len_sq = (asin_z * e(axis.z) + e(axis.x) * e(axis.x)) + e(axis.y) * e(axis.y);
    if axis_len_sq > e(1.0e-5) {
        let inv = 1.0 / axis_len_sq.sqrt();
        axis = Vector::new(r(e(axis.x) * inv), r(e(axis.y) * inv), r(inv * e(axis.z)));
    }

    let local = transpose_transform(a, axis);
    let mut scaled = local;
    let abs_x = e(local.x).abs();
    let abs_y = e(local.y).abs();
    let max_xy = if abs_x > abs_y { abs_x } else { abs_y };
    if max_xy > 0.001 {
        let inv_max = 1.0 / max_xy;
        scaled.x = r(e(local.x) * inv_max);
        scaled.y = r(inv_max * e(local.y));
    }
    if e(local.z).abs() > 0.1 {
        scaled.z = if e(local.z) > 0.0 { 1.0 } else { -1.0 };
    }

    let mut throttle = transform_matrix(a, scaled);
    const PI: f32 = core::f32::consts::PI;
    const TWO_PI: f32 = core::f32::consts::TAU;
    if angle > PI {
        throttle = neg(throttle);
        angle = r(e(TWO_PI) - e(angle));
    } else if angle < -PI {
        throttle = neg(throttle);
        angle = r(e(angle) + e(TWO_PI));
    }
    AngularThrottle { throttle, angle }
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_core::rng::CrtRand;

    fn close(a: Vector, b: Vector) -> bool {
        (a.x - b.x).abs() < 1.0e-5 && (a.y - b.y).abs() < 1.0e-5 && (a.z - b.z).abs() < 1.0e-5
    }

    #[test]
    fn basis_faces_forward() {
        let m = build_orientation_basis(
            &Matrix::IDENTITY,
            Vector::new(0.0, 0.0, -1.0),
            Some(Vector::new(0.0, 1.0, 0.0)),
        );
        assert_eq!(m, Matrix::IDENTITY);
        let m = build_orientation_basis(&Matrix::IDENTITY, Vector::new(1.0, 0.0, 0.0), None);
        assert!(close(column2(&m), Vector::new(-1.0, 0.0, 0.0)));
        assert_eq!(
            build_orientation_basis(&Matrix::IDENTITY, Vector::ZERO, None),
            Matrix::default()
        );
    }

    #[test]
    fn perpendicular_frame() {
        let m = build_perpendicular_frame(Vector::new(0.0, 0.0, -2.0), 0.999, None);
        assert_eq!(m, Some(Matrix::IDENTITY));
        assert_eq!(
            build_perpendicular_frame(Vector::new(0.0, 1.0, 0.0), 0.999, None),
            None
        );
        assert_eq!(build_perpendicular_frame(Vector::ZERO, 0.999, None), None);
    }

    #[test]
    fn throttle_turns_toward_target() {
        let same = get_angular_throttle(&Matrix::IDENTITY, &Matrix::IDENTITY);
        assert_eq!(same.angle, 0.0);
        // A quarter turn about +Y.
        let b = Matrix::from_rows([[0.0, 0.0, 1.0], [0.0, 1.0, 0.0], [-1.0, 0.0, 0.0]]);
        let t = get_angular_throttle(&Matrix::IDENTITY, &b);
        assert!(
            (t.angle - std::f32::consts::FRAC_PI_2).abs() < 1.0e-5,
            "{t:?}"
        );
        assert!(t.throttle.y.abs() > 0.99, "{t:?}");
    }

    #[test]
    fn slerp_turns_by_the_angle() {
        let v = rotate_toward_by_slerp(
            Vector::new(1.0, 0.0, 0.0),
            Vector::new(0.0, 1.0, 0.0),
            std::f32::consts::FRAC_PI_2,
        );
        assert!(close(v, Vector::new(0.0, 1.0, 0.0)), "{v:?}");
        let same =
            rotate_toward_by_slerp(Vector::new(1.0, 0.0, 0.0), Vector::new(1.0, 0.0, 0.0), 1.0);
        assert_eq!(same, Vector::new(1.0, 0.0, 0.0));
    }

    #[test]
    fn spread_frame_draws() {
        let mut rand = CrtRand::with_seed(1);
        let m = build_random_spread_frame(&mut rand, &Matrix::IDENTITY, 0.0, 0.0);
        assert!(close(column2(&m), Vector::new(0.0, 0.0, 1.0)), "{m:?}");
        let mut a = CrtRand::with_seed(5);
        let _ = build_random_spread_frame(&mut a, &Matrix::IDENTITY, 0.1, 0.5);
        let mut b = CrtRand::with_seed(5);
        for _ in 0..4 {
            b.rand();
        }
        assert_eq!(a, b);
        let v = random_vector(&mut CrtRand::with_seed(9), 2.0);
        assert!((dot_zyx(v, v).sqrt() - 2.0).abs() < 1.0e-5);
    }
}
