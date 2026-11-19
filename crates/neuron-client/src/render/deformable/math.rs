//! The vector, matrix and quaternion helpers of `Deformable2.cpp` in the
//! reference's operation order (decision T11: `f64` arithmetic rounded to
//! `f32` at each store the reference makes), and word-array views of the
//! `I3DMathEngine` slots it calls (`neuron_core::math`).
//!
//! The trigonometry (`acos`, `sin`, `cos`, `atan2`) is the C run time's in
//! the reference and the host's here (Tier B where it matters).

use neuron_core::math::{
    Matrix, Quaternion, Transform, Vector, matrix_to_quaternion, mul_matrix, mul_quat,
    mul_transform, quat_slerp, quaternion_to_matrix, transform_matrix, transform_xform,
    transpose_transform,
};

#[allow(clippy::cast_possible_truncation)]
fn f(x: f64) -> f32 {
    x as f32
}

// --- I3DMathEngine as word arrays ---------------------------------------------------------------

/// `MulTransform(dst, a, b)` (slot +0x1C).
#[must_use]
pub fn mul_transform12(a: &[f32; 12], b: &[f32; 12]) -> [f32; 12] {
    mul_transform(&Transform::from_words(*a), &Transform::from_words(*b)).to_words()
}

/// `MulMatrix(dst, a, b)` (slot +0x20).
#[must_use]
pub fn mul_matrix9(a: &[f32; 9], b: &[f32; 9]) -> [f32; 9] {
    mul_matrix(&Matrix::from_words(*a), &Matrix::from_words(*b)).to_words()
}

/// `TransformMatrix(dst, m, v)` (slot +0x2C).
#[must_use]
pub fn transform_matrix9(m: &[f32; 9], v: &[f32; 3]) -> [f32; 3] {
    transform_matrix(&Matrix::from_words(*m), Vector::from_words(*v)).to_words()
}

/// `TransformXform(dst, t, v)` (slot +0x28).
#[must_use]
pub fn transform_xform12(t: &[f32; 12], v: &[f32; 3]) -> [f32; 3] {
    transform_xform(&Transform::from_words(*t), Vector::from_words(*v)).to_words()
}

/// `TransposeTransform(dst, m, v)` (slot +0x38): `MathTransformVec3`.
#[must_use]
pub fn transpose_transform9(m: &[f32; 9], v: &[f32; 3]) -> [f32; 3] {
    transpose_transform(&Matrix::from_words(*m), Vector::from_words(*v)).to_words()
}

/// `MatrixToQuaternion(dst, m)` (slot +0x50): `{w, x, y, z}`.
#[must_use]
pub fn matrix_to_quat(m: &[f32; 9]) -> [f32; 4] {
    matrix_to_quaternion(&Matrix::from_words(*m)).to_words()
}

/// `QuaternionToMatrix(dst, q)` (slot +0x54).
#[must_use]
pub fn quat_to_matrix(q: &[f32; 4]) -> [f32; 9] {
    quaternion_to_matrix(&Quaternion::from_words(*q)).to_words()
}

/// `MulQuat(dst, a, b)` (slot +0x18).
#[must_use]
pub fn mul_quat4(a: &[f32; 4], b: &[f32; 4]) -> [f32; 4] {
    mul_quat(&Quaternion::from_words(*a), &Quaternion::from_words(*b)).to_words()
}

/// `QuatSlerp(dst, a, b, t)` (slot +0x58).
#[must_use]
pub fn slerp4(a: &[f32; 4], b: &[f32; 4], t: f32) -> [f32; 4] {
    quat_slerp(&Quaternion::from_words(*a), &Quaternion::from_words(*b), t).to_words()
}

/// The first nine words of a transform.
#[must_use]
pub fn rot9(m: &[f32; 12]) -> [f32; 9] {
    core::array::from_fn(|i| m[i])
}

/// Three words from `at`.
#[must_use]
pub fn vec3(words: &[f32], at: usize) -> [f32; 3] {
    [words[at], words[at + 1], words[at + 2]]
}

// --- skinning ------------------------------------------------------------------------------------

/// `ScaleBoneMatrix`: `out[i] = weight * m[i]`.
#[must_use]
pub fn scale_bone_matrix(m: &[f32; 12], weight: f32) -> [f32; 12] {
    core::array::from_fn(|i| f(f64::from(weight) * f64::from(m[i])))
}

/// `AccumulateBoneMatrix`: `out[i] += weight * m[i]`, rounded once.
pub fn accumulate_bone_matrix(out: &mut [f32; 12], m: &[f32; 12], weight: f32) {
    for i in 0..12 {
        out[i] = f(f64::from(weight) * f64::from(m[i]) + f64::from(out[i]));
    }
}

/// `TransformPoint`: a 3x4 transform (translation in words 9 to 11)
/// applied to a point.
#[must_use]
pub fn transform_point(m: &[f32; 12], p: &[f32; 3]) -> [f32; 3] {
    let d = |i: usize| f64::from(m[i]);
    let (x, y, z) = (f64::from(p[0]), f64::from(p[1]), f64::from(p[2]));
    [
        f(((d(2) * z + d(1) * y) + d(0) * x) + d(9)),
        f(((d(5) * z + d(3) * x) + d(4) * y) + d(10)),
        f(((d(8) * z + d(6) * x) + d(7) * y) + d(11)),
    ]
}

/// `TransformDirection`: the rotation alone.
#[must_use]
pub fn transform_direction(m: &[f32; 12], v: &[f32; 3]) -> [f32; 3] {
    let d = |i: usize| f64::from(m[i]);
    let (x, y, z) = (f64::from(v[0]), f64::from(v[1]), f64::from(v[2]));
    [
        f((d(2) * z + d(1) * y) + d(0) * x),
        f((d(5) * z + d(3) * x) + d(4) * y),
        f((d(8) * z + d(6) * x) + d(7) * y),
    ]
}

/// The blended normal's renormalisation: the bit-trick estimate of
/// `1/sqrt(x)` (`0x5F3759DF`) and one Newton step in `f64`.
#[must_use]
pub fn renormalise(n: [f32; 3]) -> [f32; 3] {
    let len_sq = f(
        (f64::from(n[1]) * f64::from(n[1]) + f64::from(n[2]) * f64::from(n[2]))
            + f64::from(n[0]) * f64::from(n[0]),
    );
    let bits = len_sq.to_bits().cast_signed();
    let estimate = f32::from_bits(0x5F37_59DF_u32.wrapping_sub((bits >> 1).cast_unsigned()));
    let y0 = f64::from(estimate);
    let inv_len = (1.5 - y0 * y0 * f64::from(len_sq) * 0.5) * y0;
    [
        f(f64::from(n[0]) * inv_len),
        f(f64::from(n[1]) * inv_len),
        f(f64::from(n[2]) * inv_len),
    ]
}

/// The root's inverse `Skin` composes: the transposed rotation and the
/// negated `TransposeTransform` of the position.
#[must_use]
pub fn inverse_root(root: &[f32; 12]) -> [f32; 12] {
    let r = rot9(root);
    let p = transpose_transform9(&r, &vec3(root, 9));
    [
        r[0], r[3], r[6], r[1], r[4], r[7], r[2], r[5], r[8], -p[0], -p[1], -p[2],
    ]
}

/// A 3x4 identity.
pub const IDENTITY12: [f32; 12] = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0];

/// A 3x3 identity.
pub const IDENTITY9: [f32; 9] = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0];

/// The transpose of a 3x3.
#[must_use]
pub fn transpose9(m: &[f32; 9]) -> [f32; 9] {
    [m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]]
}

// --- the IK helpers -----------------------------------------------------------------------------

/// `IkLen3`: `sqrt(x*x + y*y + z*z)` in `f64`, not rounded.
#[must_use]
pub fn len3(v: &[f32; 3]) -> f64 {
    let (x, y, z) = (f64::from(v[0]), f64::from(v[1]), f64::from(v[2]));
    (x * x + y * y + z * z).sqrt()
}

/// `IkCross3`: each component a difference of two `f64` products.
#[must_use]
pub fn cross3(l: &[f32; 3], r: &[f32; 3]) -> [f32; 3] {
    let d = |v: &[f32; 3], i: usize| f64::from(v[i]);
    let z = d(l, 0) * d(r, 1) - d(l, 1) * d(r, 0);
    let y = d(l, 2) * d(r, 0) - d(r, 2) * d(l, 0);
    let x = d(l, 1) * d(r, 2) - d(l, 2) * d(r, 1);
    [f(x), f(y), f(z)]
}

/// `IkColsToMat`.
#[must_use]
pub fn cols_to_mat(c0: &[f32; 3], c1: &[f32; 3], c2: &[f32; 3]) -> [f32; 9] {
    [
        c0[0], c1[0], c2[0], c0[1], c1[1], c2[1], c0[2], c1[2], c2[2],
    ]
}

/// `IkLookAt`: the frame whose column 0 (flag 4), 1 (flag 8) or 2 (flag
/// 0x10) is `dir`, completed from the parent's columns; zero without one
/// of the flags.
#[must_use]
pub fn look_at(dir: &[f32; 3], parent: &[f32; 9], flags: u8) -> [f32; 9] {
    let mut c0 = [0.0f32; 3];
    let mut c1 = [0.0f32; 3];
    let mut c2 = [0.0f32; 3];
    if flags & 4 != 0 {
        c0 = *dir;
        let (p2, p5, p8) = (
            f64::from(parent[2]),
            f64::from(parent[5]),
            f64::from(parent[8]),
        );
        c2[0] = f(f64::from(c0[2]) * p5 - f64::from(c0[1]) * p8);
        c2[1] = f(p8 * f64::from(c0[0]) - f64::from(c0[2]) * p2);
        c2[2] = f(f64::from(c0[1]) * p2 - p5 * f64::from(c0[0]));
        let (y0, y1, y2) = (f64::from(c2[0]), f64::from(c2[1]), f64::from(c2[2]));
        let inv = 1.0 / (y2 * y2 + y1 * y1 + y0 * y0).sqrt();
        c1 = [f(y0 * inv), f(y1 * inv), f(y2 * inv)];
        c2 = cross3(&c0, &c1);
    } else if flags & 8 != 0 {
        c1 = *dir;
        let temp = [parent[0], parent[3], parent[6]];
        c2 = cross3(&temp, &c1);
        let inv = 1.0 / len3(&c2);
        c2 = c2.map(|c| f(f64::from(c) * inv));
        c0 = cross3(&c1, &c2);
    } else if flags & 0x10 != 0 {
        c2 = *dir;
        c1 = [parent[1], parent[4], parent[7]];
        c0 = cross3(&c1, &c2);
        let inv = 1.0 / len3(&c0);
        c0 = c0.map(|c| f(f64::from(c) * inv));
        c1 = cross3(&c2, &c0);
    }
    cols_to_mat(&c0, &c1, &c2)
}

/// The double widening of `1e-5f` (`G_IkLogEps`).
const LOG_EPS: f64 = 1.0e-5_f32 as f64;
/// The double `1e-5` (`G_IkExpEps`).
const EXP_EPS: f64 = 1.0e-5;

/// `IkQuatLogVec`: a quaternion `{w, x, y, z}` to `acos(w) / sqrt(1 - w*w)`
/// times its vector part.
#[must_use]
pub fn quat_log_vec(q: &[f32; 4]) -> [f32; 3] {
    let w = f64::from(q[0]);
    if (1.0 - w).abs() < LOG_EPS {
        return [0.0; 3];
    }
    let scale = w.acos() / (1.0 - w * w).sqrt();
    [
        f(scale * f64::from(q[1])),
        f(scale * f64::from(q[2])),
        f(scale * f64::from(q[3])),
    ]
}

/// `IkQuatExp`: an axis times its angle to a quaternion.
#[must_use]
pub fn quat_exp(axis: &[f32; 3]) -> [f32; 4] {
    let len = len3(axis);
    if !(len > EXP_EPS) {
        return [1.0, 0.0, 0.0, 0.0];
    }
    let w = f(len.cos());
    let scale = len.sin() / len;
    [
        w,
        f(scale * f64::from(axis[0])),
        f(scale * f64::from(axis[1])),
        f(scale * f64::from(axis[2])),
    ]
}

/// `IkQuatLog`: twice the log of `b * conj(a)`, `b` negated when the dot
/// product is negative.
#[must_use]
pub fn quat_log(a: &[f32; 4], b: &[f32; 4]) -> [f32; 3] {
    let d = |q: &[f32; 4], i: usize| f64::from(q[i]);
    let dot = d(a, 3) * d(b, 3) + d(a, 2) * d(b, 2) + d(a, 1) * d(b, 1) + d(a, 0) * d(b, 0);
    let use_b = if dot >= 0.0 { *b } else { b.map(|c| -c) };
    let conj = [a[0], -a[1], -a[2], -a[3]];
    let prod = mul_quat4(&use_b, &conj);
    let l = quat_log_vec(&prod);
    [l[0] + l[0], l[1] + l[1], l[2] + l[2]]
}

/// `AxisAngleToQuat`.
#[must_use]
pub fn axis_angle_to_quat(axis: &[f32; 3], angle: f32) -> [f32; 4] {
    let half = f64::from(angle) * 0.5;
    let length = (f64::from(axis[0]) * f64::from(axis[0])
        + f64::from(axis[1]) * f64::from(axis[1])
        + f64::from(axis[2]) * f64::from(axis[2]))
    .sqrt();
    let scale = f(half.sin() / length);
    [
        f(half.cos()),
        f(f64::from(scale) * f64::from(axis[0])),
        f(f64::from(scale) * f64::from(axis[1])),
        f(f64::from(scale) * f64::from(axis[2])),
    ]
}

/// `G_IkGimbalEps`: 2^-19.
const GIMBAL_EPS: f32 = 1.907_348_6e-6;

/// `IkEulerRebuild`: the rotation of a yaw, pitch and roll.
#[must_use]
pub fn euler_rebuild(yaw: f32, pitch: f32, roll: f32) -> [f32; 9] {
    let (sin_y, cos_y) = (f64::from(yaw).sin(), f64::from(yaw).cos());
    let (sin_p, cos_p) = (f64::from(pitch).sin(), f64::from(pitch).cos());
    let (sin_r, cos_r) = (f64::from(roll).sin(), f64::from(roll).cos());
    let sin_p_sin_y = sin_p * sin_y;
    let sin_r_cos_y = f(sin_r * cos_y);
    let cos_r_cos_y = f(cos_r * cos_y);
    [
        f(cos_r * cos_p),
        f(sin_p_sin_y * cos_r - f64::from(sin_r_cos_y)),
        f(f64::from(cos_r_cos_y) * sin_p + sin_r * sin_y),
        f(sin_r * cos_p),
        f(sin_p_sin_y * sin_r + f64::from(cos_r_cos_y)),
        f(f64::from(sin_r_cos_y) * sin_p - cos_r * sin_y),
        f(-sin_p),
        f(cos_p * sin_y),
        f(cos_p * cos_y),
    ]
}

/// The joint limits of a joint payload (`Relative` words 16 to 21: roll
/// min and max at +0x40, yaw at +0x4C, pitch at +0x54).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct JointLimits {
    /// +0x40, +0x44.
    pub roll: [f32; 2],
    /// +0x4C, +0x50.
    pub yaw: [f32; 2],
    /// +0x54, +0x58.
    pub pitch: [f32; 2],
}

impl JointLimits {
    /// From the 23 words of a joint payload.
    #[must_use]
    pub fn from_relative(words: &[u32; 23]) -> Self {
        let w = |i: usize| f32::from_bits(words[i]);
        Self {
            roll: [w(0x40 / 4), w(0x44 / 4)],
            yaw: [w(0x4C / 4), w(0x50 / 4)],
            pitch: [w(0x54 / 4), w(0x58 / 4)],
        }
    }
}

/// `IkEulerOutOfRange`.
#[must_use]
pub fn euler_out_of_range(limits: &JointLimits, yaw: f32, pitch: f32, roll: f32) -> i32 {
    let mut count = 0;
    if yaw < limits.yaw[0] || limits.yaw[1] < yaw {
        count += 1;
    }
    if pitch < limits.pitch[0] || limits.pitch[1] < pitch {
        count += 1;
    }
    if roll < limits.roll[0] || limits.roll[1] < roll {
        count += 1;
    }
    count
}

/// `IkEulerExtract`: yaw, pitch and roll of a rotation, the alternative
/// solution when `alt`.
#[must_use]
pub fn euler_extract(m: &[f32; 9], alt: bool) -> (f32, f32, f32) {
    let (m0, m3, m6) = (f64::from(m[0]), f64::from(m[3]), f64::from(m[6]));
    let hyp = (m0 * m0 + m3 * m3).sqrt();
    if !(hyp > f64::from(GIMBAL_EPS)) {
        return if alt {
            (
                f(f64::from(m[5]).atan2(-f64::from(m[4]))),
                f(m6.atan2(-hyp)),
                0.0,
            )
        } else {
            (
                f((-f64::from(m[5])).atan2(f64::from(m[4]))),
                f((-m6).atan2(hyp)),
                0.0,
            )
        };
    }
    if alt {
        return (
            f(f64::from(m[7]).atan2(-f64::from(m[8]))),
            f((-m6).atan2(-hyp)),
            f(m3.atan2(-m0)),
        );
    }
    (
        f(f64::from(m[7]).atan2(f64::from(m[8]))),
        f((-m6).atan2(hyp)),
        f(m3.atan2(m0)),
    )
}

/// `IkEulerClamp`: the rotation with its angles held to the joint's
/// limits (the alternative solution when it is no further out).
#[must_use]
pub fn euler_clamp(limits: &JointLimits, m: &[f32; 9]) -> [f32; 9] {
    let (mut yaw, mut pitch, mut roll) = euler_extract(m, false);
    let score = euler_out_of_range(limits, yaw, pitch, roll);
    if score != 0 {
        let (ay, ap, ar) = euler_extract(m, true);
        if euler_out_of_range(limits, ay, ap, ar) <= score {
            (yaw, pitch, roll) = (ay, ap, ar);
        }
    }
    let mut changed = false;
    let mut hold = |v: &mut f32, bound: f32, below: bool| {
        if (below && *v < bound) || (!below && *v > bound) {
            *v = bound;
            changed = true;
        }
    };
    hold(&mut yaw, limits.yaw[0], true);
    hold(&mut yaw, limits.yaw[1], false);
    hold(&mut pitch, limits.pitch[0], true);
    hold(&mut pitch, limits.pitch[1], false);
    hold(&mut roll, limits.roll[0], true);
    hold(&mut roll, limits.roll[1], false);
    if changed {
        euler_rebuild(yaw, pitch, roll)
    } else {
        *m
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn renormalise_is_close_to_unit() {
        let n = renormalise([3.0, 4.0, 0.0]);
        let len = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]).sqrt();
        // One Newton step on the bit-trick estimate: within 0.2 per cent.
        assert!((len - 1.0).abs() < 2e-3, "{len}");
    }

    #[test]
    fn euler_round_trip() {
        let m = euler_rebuild(0.3, -0.2, 0.1);
        let (y, p, r) = euler_extract(&m, false);
        assert!((y - 0.3).abs() < 1e-5 && (p + 0.2).abs() < 1e-5 && (r - 0.1).abs() < 1e-5);
    }

    #[test]
    fn quat_exp_log_inverse() {
        let q = quat_exp(&[0.1, 0.2, -0.05]);
        let l = quat_log(&[1.0, 0.0, 0.0, 0.0], &q);
        assert!((l[0] - 0.2).abs() < 1e-5 && (l[1] - 0.4).abs() < 1e-5);
    }
}
