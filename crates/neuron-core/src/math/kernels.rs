//! The single-value kernels of `I3DMathEngine` (`x86math.cpp`).
//!
//! Each function names the interface method it ports. Where the reference
//! stores an output word before it reads the next input word, a call whose
//! output is also an input sees the stored word; the `*_assign`,
//! `*_assign_rhs` and `*_in_place` variants are those aliased calls
//! (`MulTransform(A, A, B)`, `MulTransform(B, A, B)`, `Rotate(V, X, V)`).
//! Kernels whose reference reads every input before the first store have no
//! such variant, because aliasing does not change their result.

use super::types::{Matrix, Quaternion, Transform, Vector};
use super::x87::{Words, ext, round};

/// `G_MinDet`: [`inverse`] and [`general_inverse`] treat a determinant
/// whose magnitude is not above this as singular.
pub const MIN_DET: f32 = 1e-8;

/// The outcome of [`inverse`].
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MatrixInverse {
    /// The adjugate divided by the determinant, or the plain adjugate when
    /// the matrix is singular. The reference writes it either way.
    pub matrix: Matrix,
    /// The method returned `GR_OK`: the determinant's magnitude is above
    /// [`MIN_DET`]. `GR_INVALID_PARMS` otherwise.
    pub invertible: bool,
}

/// The outcome of [`general_inverse`].
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GeneralInverse {
    /// The inverse rotation and the translation the reference derives.
    pub transform: Transform,
    /// The `W` output: the determinant, recomputed, times the reciprocal
    /// determinant.
    pub w: f32,
    /// The method returned `GR_OK`, as for [`MatrixInverse::invertible`].
    pub invertible: bool,
}

const VECTOR: usize = 3;
const MATRIX: usize = 9;
const TRANSFORM: usize = 12;
const QUAT: usize = 4;

fn put_words(dst: &mut [f32], at: usize, words: &[f32]) {
    dst[at..at + words.len()].copy_from_slice(words);
}

fn get_words<const N: usize>(src: &[f32], at: usize) -> [f32; N] {
    core::array::from_fn(|i| src[at + i])
}

// --- Matrix ------------------------------------------------------------------------------

/// `Det` (+0x14): the six triple products summed in the C++ order and
/// returned as `float`.
#[must_use]
pub fn det(source: &Matrix) -> f32 {
    let s = source.m.map(|row| row.map(ext));
    round(
        s[0][0] * s[1][1] * s[2][2] + s[0][1] * s[1][2] * s[2][0] + s[0][2] * s[1][0] * s[2][1]
            - s[0][0] * s[1][2] * s[2][1]
            - s[0][1] * s[1][0] * s[2][2]
            - s[0][2] * s[1][1] * s[2][0],
    )
}

/// `WriteAdjugate`: the transposed cofactors times `scale`, stored in the
/// C++ order (column by column).
fn write_adjugate(w: &mut Words<'_>, d: usize, s: usize, scale: f32) {
    macro_rules! s {
        ($r:literal, $c:literal) => {
            w.ld(s + $r * 3 + $c)
        };
    }
    let k = ext(scale);
    w.st(d, (s!(1, 1) * s!(2, 2) - s!(1, 2) * s!(2, 1)) * k);
    w.st(d + 3, -(s!(1, 0) * s!(2, 2) - s!(1, 2) * s!(2, 0)) * k);
    w.st(d + 6, (s!(1, 0) * s!(2, 1) - s!(1, 1) * s!(2, 0)) * k);
    w.st(d + 1, -(s!(0, 1) * s!(2, 2) - s!(0, 2) * s!(2, 1)) * k);
    w.st(d + 4, (s!(0, 0) * s!(2, 2) - s!(0, 2) * s!(2, 0)) * k);
    w.st(d + 7, -(s!(0, 0) * s!(2, 1) - s!(0, 1) * s!(2, 0)) * k);
    w.st(d + 2, (s!(0, 1) * s!(1, 2) - s!(0, 2) * s!(1, 1)) * k);
    w.st(d + 5, -(s!(0, 0) * s!(1, 2) - s!(0, 2) * s!(1, 0)) * k);
    w.st(d + 8, (s!(0, 0) * s!(1, 1) - s!(0, 1) * s!(1, 0)) * k);
}

fn inverse_words(w: &mut Words<'_>, d: usize, s: usize) -> bool {
    let determinant = det(&Matrix::from_words(get_words(w.0, s)));
    if determinant.abs() > MIN_DET {
        // `1.0f / Determinant`, rounded as the `float` argument.
        write_adjugate(w, d, s, round(1.0 / ext(determinant)));
        true
    } else {
        write_adjugate(w, d, s, 1.0);
        false
    }
}

/// `Inverse` (+0x0C): the adjugate over the determinant.
#[must_use]
pub fn inverse(source: &Matrix) -> MatrixInverse {
    let mut words = [0.0; 2 * MATRIX];
    put_words(&mut words, MATRIX, &source.to_words());
    let invertible = inverse_words(&mut Words(&mut words), 0, MATRIX);
    MatrixInverse {
        matrix: Matrix::from_words(get_words(&words, 0)),
        invertible,
    }
}

/// `Inverse(M, M)`: later cofactors read the entries already replaced.
/// Returns whether the matrix was invertible.
pub fn inverse_in_place(matrix: &mut Matrix) -> bool {
    let mut words = matrix.to_words();
    let invertible = inverse_words(&mut Words(&mut words), 0, 0);
    *matrix = Matrix::from_words(words);
    invertible
}

/// `Scale` (+0x10): every entry times `factor`.
#[must_use]
pub fn scale(source: &Matrix, factor: f32) -> Matrix {
    Matrix::from_rows(source.m.map(|row| row.map(|v| round(ext(v) * ext(factor)))))
}

/// `MulMatrix` row loop (71670..716e1).
fn mul_matrix_words(w: &mut Words<'_>, d: usize, a: usize, b: usize) {
    for row in 0..3 {
        let ar = a + row * 3;
        let dr = d + row * 3;
        macro_rules! a {
            ($i:literal) => {
                w.ld(ar + $i)
            };
        }
        macro_rules! b {
            ($i:literal) => {
                w.ld(b + $i)
            };
        }
        w.st(dr, (a!(2) * b!(6) + b!(3) * a!(1)) + a!(0) * b!(0));
        w.st(dr + 1, (b!(1) * a!(0) + a!(2) * b!(7)) + b!(4) * a!(1));
        w.st(dr + 2, (b!(2) * a!(0) + a!(2) * b!(8)) + b!(5) * a!(1));
    }
}

fn mul_matrix_with(a: &Matrix, b: &Matrix, d: usize) -> [f32; 3 * MATRIX] {
    let mut words = [0.0; 3 * MATRIX];
    put_words(&mut words, MATRIX, &a.to_words());
    put_words(&mut words, 2 * MATRIX, &b.to_words());
    let (a, b) = (MATRIX, 2 * MATRIX);
    mul_matrix_words(&mut Words(&mut words), d, a, b);
    words
}

/// `MulMatrix` (+0x20): `a * b`.
#[must_use]
pub fn mul_matrix(a: &Matrix, b: &Matrix) -> Matrix {
    Matrix::from_words(get_words(&mul_matrix_with(a, b, 0), 0))
}

/// `MulMatrix(A, A, B)`: `a = a * b`, later rows reading the rows already
/// replaced.
pub fn mul_matrix_assign(a: &mut Matrix, b: &Matrix) {
    *a = Matrix::from_words(get_words(&mul_matrix_with(a, b, MATRIX), MATRIX));
}

/// `MulMatrix(B, A, B)`: `b = a * b`, later entries reading the entries
/// of `b` already replaced.
pub fn mul_matrix_assign_rhs(a: &Matrix, b: &mut Matrix) {
    *b = Matrix::from_words(get_words(&mul_matrix_with(a, b, 2 * MATRIX), 2 * MATRIX));
}

// --- Transform ---------------------------------------------------------------------------

/// `MulTransform` (+0x1C) body: the rows interleaved with the translation
/// as the reference stores them.
fn mul_transform_words(w: &mut Words<'_>, d: usize, a: usize, b: usize) {
    macro_rules! a {
        ($i:literal) => {
            w.ld(a + $i)
        };
    }
    macro_rules! b {
        ($i:literal) => {
            w.ld(b + $i)
        };
    }
    w.st(d, (a!(0) * b!(0) + a!(1) * b!(3)) + b!(6) * a!(2));
    w.st(d + 1, (a!(1) * b!(4) + b!(7) * a!(2)) + b!(1) * a!(0));
    w.st(d + 2, (a!(1) * b!(5) + b!(8) * a!(2)) + b!(2) * a!(0));
    w.st(
        d + 9,
        ((a!(1) * b!(10) + b!(11) * a!(2)) + b!(9) * a!(0)) + a!(9),
    );
    w.st(d + 3, (a!(4) * b!(3) + a!(3) * b!(0)) + a!(5) * b!(6));
    w.st(d + 4, (a!(5) * b!(7) + a!(4) * b!(4)) + a!(3) * b!(1));
    w.st(d + 5, (a!(5) * b!(8) + a!(4) * b!(5)) + a!(3) * b!(2));
    w.st(
        d + 10,
        ((b!(11) * a!(5) + b!(10) * a!(4)) + b!(9) * a!(3)) + a!(10),
    );
    w.st(d + 6, (a!(7) * b!(3) + a!(8) * b!(6)) + b!(0) * a!(6));
    w.st(d + 7, (a!(8) * b!(7) + a!(7) * b!(4)) + b!(1) * a!(6));
    w.st(d + 8, (a!(8) * b!(8) + a!(7) * b!(5)) + b!(2) * a!(6));
    w.st(
        d + 11,
        ((a!(8) * b!(11) + a!(7) * b!(10)) + b!(9) * a!(6)) + a!(11),
    );
}

fn mul_transform_with(a: &Transform, b: &Transform, d: usize) -> [f32; 3 * TRANSFORM] {
    let mut words = [0.0; 3 * TRANSFORM];
    put_words(&mut words, TRANSFORM, &a.to_words());
    put_words(&mut words, 2 * TRANSFORM, &b.to_words());
    let (a, b) = (TRANSFORM, 2 * TRANSFORM);
    mul_transform_words(&mut Words(&mut words), d, a, b);
    words
}

/// `MulTransform` (+0x1C): `a * b`, the transform that applies `b` first.
#[must_use]
pub fn mul_transform(a: &Transform, b: &Transform) -> Transform {
    Transform::from_words(get_words(&mul_transform_with(a, b, 0), 0))
}

/// `MulTransform(A, A, B)`: `a = a * b` with the reference's read-after-
/// store order.
pub fn mul_transform_assign(a: &mut Transform, b: &Transform) {
    *a = Transform::from_words(get_words(&mul_transform_with(a, b, TRANSFORM), TRANSFORM));
}

/// `MulTransform(B, A, B)`: `b = a * b` with the reference's read-after-
/// store order.
pub fn mul_transform_assign_rhs(a: &Transform, b: &mut Transform) {
    let words = mul_transform_with(a, b, 2 * TRANSFORM);
    *b = Transform::from_words(get_words(&words, 2 * TRANSFORM));
}

fn general_inverse_words(w: &mut Words<'_>, d: usize, s: usize) -> (f32, bool) {
    macro_rules! m {
        ($r:literal, $c:literal) => {
            w.ld(s + $r * 3 + $c)
        };
    }
    macro_rules! t {
        ($i:literal) => {
            w.ld(s + 9 + $i)
        };
    }
    let determinant = round(
        m!(0, 0) * (m!(1, 1) * m!(2, 2) - m!(2, 1) * m!(1, 2))
            - m!(0, 1) * (m!(1, 0) * m!(2, 2) - m!(2, 0) * m!(1, 2))
            + m!(0, 2) * (m!(1, 0) * m!(2, 1) - m!(2, 0) * m!(1, 1)),
    );
    let mut inv_det = 1.0f32;
    let invertible = determinant.abs() > MIN_DET;
    if invertible {
        inv_det = round(1.0 / ext(determinant));
    }
    write_adjugate(w, d, s, inv_det);
    let k = ext(inv_det);
    // The source is read again after the rotation was written.
    w.st(
        d + 9,
        -(m!(0, 1) * m!(1, 2) * t!(2) + m!(1, 1) * m!(2, 2) * t!(0) + m!(2, 1) * m!(0, 2) * t!(1)
            - m!(0, 1) * m!(2, 2) * t!(1)
            - m!(1, 1) * m!(0, 2) * t!(2)
            - m!(2, 1) * m!(1, 2) * t!(0))
            * k,
    );
    w.st(
        d + 10,
        (m!(0, 0) * m!(1, 2) * t!(2) + m!(1, 0) * m!(2, 2) * t!(0) + m!(2, 0) * m!(0, 2) * t!(1)
            - m!(0, 0) * m!(2, 2) * t!(1)
            - m!(1, 0) * m!(0, 2) * t!(2)
            - m!(2, 0) * m!(1, 2) * t!(0))
            * k,
    );
    w.st(
        d + 11,
        -(m!(0, 0) * m!(1, 1) * t!(2) + m!(1, 0) * m!(2, 1) * t!(0) + m!(2, 0) * m!(0, 1) * t!(1)
            - m!(0, 0) * m!(2, 1) * t!(1)
            - m!(1, 0) * m!(0, 1) * t!(2)
            - m!(2, 0) * m!(1, 1) * t!(0))
            * k,
    );
    let out_w = round(
        (m!(0, 0) * m!(1, 1) * m!(2, 2)
            + m!(1, 0) * m!(2, 1) * m!(0, 2)
            + m!(2, 0) * m!(0, 1) * m!(1, 2)
            - m!(2, 0) * m!(1, 1) * m!(0, 2)
            - m!(0, 0) * m!(2, 1) * m!(1, 2)
            - m!(1, 0) * m!(0, 1) * m!(2, 2))
            * k,
    );
    (out_w, invertible)
}

/// `GeneralInverse` (+0x5C): the adjugate over the determinant for the
/// rotation, the reference's translation formula, and `W`.
#[must_use]
pub fn general_inverse(source: &Transform) -> GeneralInverse {
    let mut words = [0.0; 2 * TRANSFORM];
    put_words(&mut words, TRANSFORM, &source.to_words());
    let (w, invertible) = general_inverse_words(&mut Words(&mut words), 0, TRANSFORM);
    GeneralInverse {
        transform: Transform::from_words(get_words(&words, 0)),
        w,
        invertible,
    }
}

/// `GeneralInverse(X, W, X)`: the cofactors and the translation read the
/// words already replaced. Returns `W` and whether the rotation was
/// invertible.
pub fn general_inverse_in_place(transform: &mut Transform) -> (f32, bool) {
    let mut words = transform.to_words();
    let result = general_inverse_words(&mut Words(&mut words), 0, 0);
    *transform = Transform::from_words(words);
    result
}

// --- Vectors -----------------------------------------------------------------------------

/// `MulVecByMatrix`, the body of `TransformMatrix` and `Rotate`.
fn mul_vec_by_matrix_words(w: &mut Words<'_>, d: usize, m: usize, s: usize) {
    macro_rules! m {
        ($i:literal) => {
            w.ld(m + $i)
        };
    }
    macro_rules! s {
        ($i:literal) => {
            w.ld(s + $i)
        };
    }
    w.st(d, (m!(2) * s!(2) + m!(1) * s!(1)) + m!(0) * s!(0));
    w.st(d + 1, (m!(5) * s!(2) + m!(3) * s!(0)) + m!(4) * s!(1));
    w.st(d + 2, (m!(8) * s!(2) + m!(6) * s!(0)) + m!(7) * s!(1));
}

/// `MulVecByTranspose`, the body of `InverseRotate`, `TransposeTransform`
/// and `InverseTransform`.
fn mul_vec_by_transpose_words(w: &mut Words<'_>, d: usize, m: usize, s: usize) {
    macro_rules! m {
        ($i:literal) => {
            w.ld(m + $i)
        };
    }
    macro_rules! s {
        ($i:literal) => {
            w.ld(s + $i)
        };
    }
    w.st(d, (m!(6) * s!(2) + m!(3) * s!(1)) + m!(0) * s!(0));
    w.st(d + 1, (m!(7) * s!(2) + m!(1) * s!(0)) + m!(4) * s!(1));
    w.st(d + 2, (m!(8) * s!(2) + m!(2) * s!(0)) + m!(5) * s!(1));
}

/// `TransformXform` body (71890..718ed): the translation joins each dot
/// product on the stack.
fn transform_xform_words(w: &mut Words<'_>, d: usize, x: usize, s: usize) {
    macro_rules! x {
        ($i:literal) => {
            w.ld(x + $i)
        };
    }
    macro_rules! s {
        ($i:literal) => {
            w.ld(s + $i)
        };
    }
    w.st(d, ((x!(2) * s!(2) + x!(1) * s!(1)) + x!(0) * s!(0)) + x!(9));
    w.st(
        d + 1,
        ((x!(5) * s!(2) + x!(3) * s!(0)) + x!(4) * s!(1)) + x!(10),
    );
    w.st(
        d + 2,
        ((x!(8) * s!(2) + x!(6) * s!(0)) + x!(7) * s!(1)) + x!(11),
    );
}

type VectorKernel = fn(&mut Words<'_>, usize, usize, usize);

/// Runs a vector kernel on an operand of `N` words and a separate source.
fn vector_kernel<const N: usize>(
    kernel: VectorKernel,
    operand: [f32; N],
    source: Vector,
) -> Vector {
    let mut words = [0.0; 2 * TRANSFORM];
    put_words(&mut words, 0, &operand);
    put_words(&mut words, TRANSFORM, &source.to_words());
    let d = TRANSFORM + VECTOR;
    kernel(&mut Words(&mut words), d, 0, TRANSFORM);
    Vector::from_words(get_words(&words, d))
}

/// Runs a vector kernel whose output is its source.
fn vector_kernel_in_place<const N: usize>(kernel: VectorKernel, operand: [f32; N], v: &mut Vector) {
    let mut words = [0.0; TRANSFORM + VECTOR];
    put_words(&mut words, 0, &operand);
    put_words(&mut words, TRANSFORM, &v.to_words());
    kernel(&mut Words(&mut words), TRANSFORM, 0, TRANSFORM);
    *v = Vector::from_words(get_words(&words, TRANSFORM));
}

/// `MulVecByTranspose` for the list kernels.
pub(super) fn mul_vec_by_transpose(matrix: &Matrix, source: Vector) -> Vector {
    vector_kernel(mul_vec_by_transpose_words, matrix.to_words(), source)
}

/// `TransformXform` (+0x28): rotate, then translate.
#[must_use]
pub fn transform_xform(xform: &Transform, source: Vector) -> Vector {
    vector_kernel(transform_xform_words, xform.to_words(), source)
}

/// `TransformXform(V, X, V)`.
pub fn transform_xform_in_place(xform: &Transform, v: &mut Vector) {
    vector_kernel_in_place(transform_xform_words, xform.to_words(), v);
}

/// `TransformMatrix` (+0x2C): `matrix * source`.
#[must_use]
pub fn transform_matrix(matrix: &Matrix, source: Vector) -> Vector {
    vector_kernel(mul_vec_by_matrix_words, matrix.to_words(), source)
}

/// `TransformMatrix(V, M, V)`.
pub fn transform_matrix_in_place(matrix: &Matrix, v: &mut Vector) {
    vector_kernel_in_place(mul_vec_by_matrix_words, matrix.to_words(), v);
}

/// `Rotate` (+0x30): the rotation part only.
#[must_use]
pub fn rotate(xform: &Transform, source: Vector) -> Vector {
    transform_matrix(&xform.rotation, source)
}

/// `Rotate(V, X, V)`.
pub fn rotate_in_place(xform: &Transform, v: &mut Vector) {
    transform_matrix_in_place(&xform.rotation, v);
}

/// `InverseRotate` (+0x34): the transposed rotation.
#[must_use]
pub fn inverse_rotate(xform: &Transform, source: Vector) -> Vector {
    transpose_transform(&xform.rotation, source)
}

/// `InverseRotate(V, X, V)`.
pub fn inverse_rotate_in_place(xform: &Transform, v: &mut Vector) {
    transpose_transform_in_place(&xform.rotation, v);
}

/// `TransposeTransform` (+0x38): `transpose(matrix) * source`.
#[must_use]
pub fn transpose_transform(matrix: &Matrix, source: Vector) -> Vector {
    vector_kernel(mul_vec_by_transpose_words, matrix.to_words(), source)
}

/// `TransposeTransform(V, M, V)`.
pub fn transpose_transform_in_place(matrix: &Matrix, v: &mut Vector) {
    vector_kernel_in_place(mul_vec_by_transpose_words, matrix.to_words(), v);
}

/// `InverseTransform` (+0x3C): subtract the translation (rounded to a
/// `float` vector), then apply the transposed rotation. The difference is
/// a copy, so an aliased call gives the same result.
#[must_use]
pub fn inverse_transform(xform: &Transform, source: Vector) -> Vector {
    let t = xform.translation;
    let delta = Vector::new(
        round(ext(source.x) - ext(t.x)),
        round(ext(source.y) - ext(t.y)),
        round(ext(source.z) - ext(t.z)),
    );
    mul_vec_by_transpose(&xform.rotation, delta)
}

/// `TransformQuat` (+0x24): rotates by a quaternion through
/// `v (w^2 - |q|^2) + 2 q (q . v) + 2 w (q x v)`, each named temporary
/// rounded to `float`. Every output reads only its own source component
/// after the temporaries, so aliasing does not matter.
#[must_use]
pub fn transform_quat(quat: &Quaternion, source: Vector) -> Vector {
    let (qw, qx, qy, qz) = (ext(quat.w), ext(quat.x), ext(quat.y), ext(quat.z));
    let (sx, sy, sz) = (ext(source.x), ext(source.y), ext(source.z));
    let dot_vv = ext(round(qx * qx + qy * qy + qz * qz));
    let scale = ext(round(qw * qw - dot_vv));
    let dot_qv = ext(round(qx * sx + qy * sy + qz * sz));
    let cross_x = ext(round(qy * sz - qz * sy));
    let cross_y = ext(round(qz * sx - qx * sz));
    let cross_z = ext(round(qx * sy - qy * sx));
    Vector::new(
        round(sx * scale + qx * (2.0 * dot_qv) + cross_x * (qw * 2.0)),
        round(sy * scale + qy * (2.0 * dot_qv) + cross_y * (qw * 2.0)),
        round(sz * scale + qz * (2.0 * dot_qv) + cross_z * (qw * 2.0)),
    )
}

// --- Quaternions -------------------------------------------------------------------------

fn mul_quat_words(w: &mut Words<'_>, d: usize, a: usize, b: usize) {
    macro_rules! a {
        ($i:literal) => {
            w.ld(a + $i)
        };
    }
    macro_rules! b {
        ($i:literal) => {
            w.ld(b + $i)
        };
    }
    // W, X, Y, Z at words 0..4.
    w.st(
        d,
        a!(0) * b!(0) - (a!(1) * b!(1) + a!(2) * b!(2) + a!(3) * b!(3)),
    );
    w.st(
        d + 1,
        a!(0) * b!(1) + b!(0) * a!(1) + (a!(2) * b!(3) - a!(3) * b!(2)),
    );
    w.st(
        d + 2,
        a!(0) * b!(2) + b!(0) * a!(2) + (a!(3) * b!(1) - a!(1) * b!(3)),
    );
    w.st(
        d + 3,
        a!(0) * b!(3) + b!(0) * a!(3) + (a!(1) * b!(2) - a!(2) * b!(1)),
    );
}

fn mul_quat_with(a: &Quaternion, b: &Quaternion, d: usize) -> [f32; 3 * QUAT] {
    let mut words = [0.0; 3 * QUAT];
    put_words(&mut words, QUAT, &a.to_words());
    put_words(&mut words, 2 * QUAT, &b.to_words());
    mul_quat_words(&mut Words(&mut words), d, QUAT, 2 * QUAT);
    words
}

/// `MulQuat` (+0x18): the Hamilton product `a * b`.
#[must_use]
pub fn mul_quat(a: &Quaternion, b: &Quaternion) -> Quaternion {
    Quaternion::from_words(get_words(&mul_quat_with(a, b, 0), 0))
}

/// `MulQuat(A, A, B)`: `a = a * b`; X, Y and Z read the new W.
pub fn mul_quat_assign(a: &mut Quaternion, b: &Quaternion) {
    *a = Quaternion::from_words(get_words(&mul_quat_with(a, b, QUAT), QUAT));
}

/// `MulQuat(B, A, B)`: `b = a * b`; X, Y and Z read the new W.
pub fn mul_quat_assign_rhs(a: &Quaternion, b: &mut Quaternion) {
    *b = Quaternion::from_words(get_words(&mul_quat_with(a, b, 2 * QUAT), 2 * QUAT));
}

/// `MatrixToQuaternion` (+0x50), from the trace when it is positive, else
/// from the largest diagonal entry. The trace, root and reciprocal stay on
/// the stack; only the four outputs round. A NaN trace takes the diagonal
/// branch, as the reference's `JNZ` on C0/C3 does.
#[must_use]
pub fn matrix_to_quaternion(source: &Matrix) -> Quaternion {
    const NEXT_AXIS: [usize; 3] = [1, 2, 0];
    let s = source.to_words().map(ext);
    let diag = |i: usize| s[i * 4];
    let at = |r: usize, c: usize| s[r * 3 + c];
    let mut q = [0.0f32; 4];
    let trace = s[4] + s[0] + s[8];
    if trace > 0.0 {
        let root = (trace + 1.0).sqrt();
        q[0] = round(0.5 * root);
        let f = 0.5 / root;
        q[1] = round((s[7] - s[5]) * f);
        q[2] = round((s[2] - s[6]) * f);
        q[3] = round((s[3] - s[1]) * f);
    } else {
        let mut i = usize::from(s[4] > s[0]);
        if s[8] > diag(i) {
            i = 2;
        }
        let j = NEXT_AXIS[i];
        let k = NEXT_AXIS[j];
        let root = (diag(i) - (diag(k) + diag(j)) + 1.0).sqrt();
        q[1 + i] = round(0.5 * root);
        let f = 0.5 / root;
        q[0] = round((at(k, j) - at(j, k)) * f);
        q[1 + j] = round((at(i, j) + at(j, i)) * f);
        q[1 + k] = round((at(i, k) + at(k, i)) * f);
    }
    Quaternion::from_words(q)
}

/// `QuaternionToMatrix` (+0x54), with `s = 2 / |q|^2` and the reference's
/// mix of products kept on the stack and products stored to `float`
/// scratch (723c0..724c4). Every input is read before the first output.
#[must_use]
pub fn quaternion_to_matrix(quat: &Quaternion) -> Matrix {
    let (w, x, y, z) = (ext(quat.w), ext(quat.x), ext(quat.y), ext(quat.z));
    let s = 2.0 / (w * w + x * x + y * y + z * z);
    let xs = s * x;
    let ys = ext(round(s * y));
    let zs = ext(round(s * z));
    let wx = ext(round(xs * w));
    let wy = ext(round(ys * w));
    let wz = ext(round(zs * w));
    let xx = xs * x;
    let xy = ys * x;
    let xz = zs * x;
    let yy = ext(round(ys * y));
    let yz = ext(round(zs * y));
    let zz = zs * z;
    Matrix::from_rows([
        [round(1.0 - (yy + zz)), round(xy - wz), round(xz + wy)],
        [round(xy + wz), round(1.0 - (zz + xx)), round(yz - wx)],
        [round(xz - wy), round(yz + wx), round(1.0 - (yy + xx))],
    ])
}

/// `QuatSlerp` (+0x58) and `QuatSlerpValue` (+0x68): despite the name, a
/// normalised linear interpolation. When the dot product is negative
/// (ordered), it interpolates toward `-b`; a NaN dot product takes the
/// forward branch. Each output component reads only its own input
/// components, so aliasing does not matter.
#[must_use]
pub fn quat_slerp(a: &Quaternion, b: &Quaternion, t: f32) -> Quaternion {
    let av = a.to_words().map(ext);
    let bv = b.to_words().map(ext);
    let t = ext(t);
    let dot = av[2] * bv[2] + av[1] * bv[1] + bv[3] * av[3] + av[0] * bv[0];
    let lerped: [f32; 4] = if dot < 0.0 {
        core::array::from_fn(|i| round(av[i] - (av[i] + bv[i]) * t))
    } else {
        core::array::from_fn(|i| round((bv[i] - av[i]) * t + av[i]))
    };
    let d = lerped.map(ext);
    let length = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + d[3] * d[3]).sqrt();
    let inv = 1.0 / length;
    Quaternion::from_words(d.map(|v| round(inv * v)))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rot_z(angle: f32) -> Matrix {
        let (s, c) = angle.sin_cos();
        Matrix::from_rows([[c, -s, 0.0], [s, c, 0.0], [0.0, 0.0, 1.0]])
    }

    #[test]
    fn identity_is_neutral() {
        let m = Matrix::from_rows([[1.0, 2.0, 3.0], [4.0, 5.0, 6.5], [7.0, 8.0, 10.0]]);
        assert_eq!(mul_matrix(&Matrix::IDENTITY, &m), m);
        assert_eq!(mul_matrix(&m, &Matrix::IDENTITY), m);
        let v = Vector::new(1.5, -2.0, 3.25);
        assert_eq!(transform_matrix(&Matrix::IDENTITY, v), v);
        assert_eq!(transform_xform(&Transform::IDENTITY, v), v);
        assert_eq!(inverse_transform(&Transform::IDENTITY, v), v);
        assert_eq!(
            quaternion_to_matrix(&Quaternion::IDENTITY),
            Matrix::IDENTITY
        );
        assert_eq!(
            matrix_to_quaternion(&Matrix::IDENTITY),
            Quaternion::IDENTITY
        );
        assert_eq!(det(&Matrix::IDENTITY), 1.0);
    }

    #[test]
    fn inverse_of_a_regular_matrix() {
        let m = Matrix::from_rows([[2.0, 0.0, 0.0], [0.0, 4.0, 0.0], [0.0, 0.0, 8.0]]);
        let inv = inverse(&m);
        assert!(inv.invertible);
        assert_eq!(
            inv.matrix,
            Matrix::from_rows([[0.5, 0.0, 0.0], [0.0, 0.25, 0.0], [0.0, 0.0, 0.125]])
        );
        let singular = inverse(&Matrix::default());
        assert!(!singular.invertible);
        assert_eq!(singular.matrix, Matrix::default());
    }

    #[test]
    fn in_place_inverse_reads_replaced_entries() {
        let m = Matrix::from_rows([[1.0, 2.0, 0.0], [3.0, 4.0, 0.0], [0.0, 0.0, 1.0]]);
        let mut aliased = m;
        assert!(inverse_in_place(&mut aliased));
        // D[1][1] reads the replaced D[0][0].
        assert_ne!(aliased, inverse(&m).matrix);
    }

    #[test]
    fn rotations_agree() {
        let m = rot_z(0.5);
        let q = matrix_to_quaternion(&m);
        let back = quaternion_to_matrix(&q);
        for (r, b) in m.to_words().iter().zip(back.to_words()) {
            assert!((r - b).abs() < 1e-6);
        }
        let v = Vector::new(1.0, 2.0, 3.0);
        let a = transform_matrix(&m, v);
        let b = transform_quat(&q, v);
        assert!((a.x - b.x).abs() < 1e-5 && (a.y - b.y).abs() < 1e-5 && (a.z - b.z).abs() < 1e-5);
        let back = transpose_transform(&m, a);
        assert!((back.x - v.x).abs() < 1e-5 && (back.y - v.y).abs() < 1e-5);
    }

    #[test]
    fn aliased_products_differ_from_copies() {
        let a = Transform::from_words(core::array::from_fn(|i| (i as f32 + 1.0) * 0.5));
        let b = Transform::from_words(core::array::from_fn(|i| 3.0 - i as f32 * 0.25));
        let mut left = a;
        mul_transform_assign(&mut left, &b);
        let mut right = b;
        mul_transform_assign_rhs(&a, &mut right);
        let plain = mul_transform(&a, &b);
        assert_ne!(left, plain);
        assert_ne!(right, plain);
        // The first stored word reads only unreplaced inputs.
        assert_eq!(left.rotation.m[0][0], plain.rotation.m[0][0]);
        assert_eq!(right.rotation.m[0][0], plain.rotation.m[0][0]);
    }

    #[test]
    fn slerp_takes_the_short_way() {
        let a = Quaternion::IDENTITY;
        let b = Quaternion::new(-1.0, 0.0, 0.0, 0.0);
        assert_eq!(quat_slerp(&a, &b, 0.5), Quaternion::IDENTITY);
        let c = Quaternion::new(0.0, 1.0, 0.0, 0.0);
        let half = quat_slerp(&a, &c, 0.5);
        assert!((half.w - core::f32::consts::FRAC_1_SQRT_2).abs() < 1e-7);
        assert!((half.x - core::f32::consts::FRAC_1_SQRT_2).abs() < 1e-7);
    }

    #[test]
    fn general_inverse_of_a_translation() {
        let x = Transform::new(Matrix::IDENTITY, Vector::new(1.0, 2.0, 3.0));
        let inv = general_inverse(&x);
        assert!(inv.invertible);
        assert_eq!(inv.w, 1.0);
        assert_eq!(inv.transform.rotation, Matrix::IDENTITY);
        assert_eq!(inv.transform.translation, Vector::new(-1.0, -2.0, -3.0));
    }
}
