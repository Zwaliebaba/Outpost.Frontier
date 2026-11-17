//! The list transforms of `I3DMathEngine` (+0x40..+0x4C) and the "fast dot"
//! kernels behind them (`TransformListFast`, `FFastDot` in `x86math.cpp`).
//!
//! Each matrix row is classified lane by lane as zero, one or general. A
//! row's dot product then skips the zero lanes and the multiplications by
//! one, which changes both the result (a skipped `0 * inf` is not NaN, a
//! skipped `+0 * -x` cannot make a `-0`) and the rounding. When the three
//! rows together save fewer than four operations, a plain loop with its
//! own addition order runs instead.
//!
//! The kernels return their sum unrounded on the stack; the caller adds
//! the translation to it before the single `float` store.

use super::kernels::mul_vec_by_transpose;
use super::types::{Matrix, Transform, Vector};
use super::x87::{ext, round};

/// `ClassifyLane`: `0.0` (either sign), `1.0`, or anything else.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Lane {
    Zero,
    One,
    General,
}

impl Lane {
    fn of(value: f32) -> Self {
        if value == 0.0 {
            Self::Zero
        } else if value == 1.0 {
            Self::One
        } else {
            Self::General
        }
    }

    fn index(self) -> usize {
        match self {
            Self::Zero => 0,
            Self::One => 1,
            Self::General => 2,
        }
    }
}

/// `G_FastDotSavedOps`, indexed by `a * 9 + b * 3 + c` over the lane
/// indices.
const SAVED_OPS: [u32; 27] = [
    5, 5, 4, 5, 4, 3, 4, 3, 2, 5, 4, 3, 4, 3, 2, 3, 2, 1, 4, 3, 2, 3, 2, 1, 2, 1, 0,
];

/// One bound row (`FFastDot`).
struct FastDot {
    a: f32,
    b: f32,
    c: f32,
    lanes: (Lane, Lane, Lane),
    saved_ops: u32,
}

impl FastDot {
    /// `BindFastDot`.
    fn bind(row: [f32; 3]) -> Self {
        let [a, b, c] = row;
        let lanes = (Lane::of(a), Lane::of(b), Lane::of(c));
        let id = lanes.0.index() * 9 + lanes.1.index() * 3 + lanes.2.index();
        Self {
            a,
            b,
            c,
            lanes,
            saved_ops: SAVED_OPS[id],
        }
    }

    /// The selected `FastDot???` kernel; the result stays on the stack.
    fn eval(&self, v: Vector) -> f64 {
        use Lane::{General as G, One as O, Zero as Z};
        let (x, y, z) = (ext(v.x), ext(v.y), ext(v.z));
        let (a, b, c) = (ext(self.a), ext(self.b), ext(self.c));
        match self.lanes {
            (Z, Z, Z) => 0.0,
            (Z, Z, O) => z,
            (Z, Z, G) => z * c,
            (Z, O, Z) => y,
            (Z, O, O) => z + y,
            (Z, O, G) => z * c + y,
            (Z, G, Z) => y * b,
            (Z, G, O) => y * b + z,
            (Z, G, G) => z * c + y * b,
            (O, Z, Z) => x,
            (O, Z, O) => z + x,
            (O, Z, G) => z * c + x,
            (O, O, Z) => y + x,
            (O, O, O) => z + y + x,
            (O, O, G) => z * c + y + x,
            (O, G, Z) => y * b + x,
            (O, G, O) => y * b + z + x,
            (O, G, G) => z * c + y * b + x,
            (G, Z, Z) => x * a,
            (G, Z, O) => x * a + z,
            (G, Z, G) => z * c + x * a,
            (G, O, Z) => x * a + y,
            (G, O, O) => x * a + z + y,
            (G, O, G) => z * c + x * a + y,
            (G, G, Z) => y * b + x * a,
            (G, G, O) => y * b + x * a + z,
            (G, G, G) => z * c + y * b + x * a,
        }
    }
}

/// `TransformListFast`. `source` of `None` means the output is also the
/// input (`Dst == Source`); then each row's kernel reads the components
/// the previous rows of the same vector already replaced.
fn transform_list_fast(
    matrix: &Matrix,
    translation: Option<Vector>,
    dst: &mut [Vector],
    source: Option<&[Vector]>,
) {
    let count = source.map_or(dst.len(), |s| s.len().min(dst.len()));
    let read = |dst: &[Vector], i: usize| source.map_or(dst[i], |s| s[i]);
    let rows = matrix.m.map(FastDot::bind);
    let m = matrix.m.map(|row| row.map(ext));

    if rows.iter().map(|r| r.saved_ops).sum::<u32>() < 4 {
        for i in 0..count {
            let v = read(dst, i);
            let (x, y, z) = (ext(v.x), ext(v.y), ext(v.z));
            dst[i] = match translation {
                // 7209b..720e8: the first row adds X, Z, Y; the others Z, Y, X.
                Some(t) => Vector::new(
                    round(((x * m[0][0] + z * m[0][2]) + y * m[0][1]) + ext(t.x)),
                    round(((z * m[1][2] + y * m[1][1]) + x * m[1][0]) + ext(t.y)),
                    round(((z * m[2][2] + y * m[2][1]) + x * m[2][0]) + ext(t.z)),
                ),
                // 71a49..71a8d: Z, Y, X.
                None => Vector::new(
                    round((z * m[0][2] + y * m[0][1]) + x * m[0][0]),
                    round((z * m[1][2] + y * m[1][1]) + x * m[1][0]),
                    round((z * m[2][2] + y * m[2][1]) + x * m[2][0]),
                ),
            };
        }
        return;
    }

    let offset = translation.map(|t| t.to_words().map(ext));
    for i in 0..count {
        for (lane, row) in rows.iter().enumerate() {
            let sum = row.eval(read(dst, i));
            let value = match offset {
                Some(t) => round(sum + t[lane]),
                None => round(sum),
            };
            match lane {
                0 => dst[i].x = value,
                1 => dst[i].y = value,
                _ => dst[i].z = value,
            }
        }
    }
}

fn is_zero(v: Vector) -> bool {
    v.x == 0.0 && v.y == 0.0 && v.z == 0.0
}

/// `TransformListXform` (+0x40): [`super::transform_xform`] over a list,
/// through the fast-dot kernels. A zero translation (either sign) runs
/// [`transform_list_matrix`] instead. Transforms `min(src.len(),
/// dst.len())` vectors.
pub fn transform_list_xform(xform: &Transform, src: &[Vector], dst: &mut [Vector]) {
    if is_zero(xform.translation) {
        transform_list_matrix(&xform.rotation, src, dst);
    } else {
        transform_list_fast(&xform.rotation, Some(xform.translation), dst, Some(src));
    }
}

/// `TransformListXform(V, X, V, n)`.
pub fn transform_list_xform_in_place(xform: &Transform, v: &mut [Vector]) {
    if is_zero(xform.translation) {
        transform_list_matrix_in_place(&xform.rotation, v);
    } else {
        transform_list_fast(&xform.rotation, Some(xform.translation), v, None);
    }
}

/// `TransformListMatrix` (+0x44): `matrix * v` for each vector.
pub fn transform_list_matrix(matrix: &Matrix, src: &[Vector], dst: &mut [Vector]) {
    transform_list_fast(matrix, None, dst, Some(src));
}

/// `TransformListMatrix(V, M, V, n)`.
pub fn transform_list_matrix_in_place(matrix: &Matrix, v: &mut [Vector]) {
    transform_list_fast(matrix, None, v, None);
}

/// `TransposeTransformList` (+0x48): the list transform with the
/// transposed matrix.
pub fn transpose_transform_list(matrix: &Matrix, src: &[Vector], dst: &mut [Vector]) {
    transform_list_matrix(&matrix.transpose(), src, dst);
}

/// `TransposeTransformList(V, M, V, n)`.
pub fn transpose_transform_list_in_place(matrix: &Matrix, v: &mut [Vector]) {
    transform_list_matrix_in_place(&matrix.transpose(), v);
}

/// The rigid inverse `InverseTransformList` builds: the transposed
/// rotation and `transpose(R) * -t`.
fn rigid_inverse(xform: &Transform) -> Transform {
    let t = xform.translation;
    let neg = Vector::new(-t.x, -t.y, -t.z);
    Transform::new(
        xform.rotation.transpose(),
        mul_vec_by_transpose(&xform.rotation, neg),
    )
}

/// `InverseTransformList` (+0x4C): [`transform_list_xform`] with the rigid
/// inverse of `xform` (correct only for an orthonormal rotation).
pub fn inverse_transform_list(xform: &Transform, src: &[Vector], dst: &mut [Vector]) {
    transform_list_xform(&rigid_inverse(xform), src, dst);
}

/// `InverseTransformList(V, X, V, n)`.
pub fn inverse_transform_list_in_place(xform: &Transform, v: &mut [Vector]) {
    transform_list_xform_in_place(&rigid_inverse(xform), v);
}

#[cfg(test)]
mod tests {
    use super::super::kernels::{transform_matrix, transform_xform};
    use super::*;

    #[test]
    fn saved_ops_count_the_skipped_operations() {
        for (id, &saved_ops) in SAVED_OPS.iter().enumerate() {
            let lanes = [id / 9, id / 3 % 3, id % 3];
            let zeros = lanes.iter().filter(|&&l| l == 0).count();
            let ones = lanes.iter().filter(|&&l| l == 1).count();
            let saved = zeros + ones + zeros.min(2);
            assert_eq!(saved_ops as usize, saved, "kernel {id}");
        }
    }

    #[test]
    fn identity_uses_the_kernels_and_keeps_vectors() {
        let src = [Vector::new(1.0, f32::INFINITY, -0.0)];
        let mut dst = [Vector::ZERO];
        transform_list_matrix(&Matrix::IDENTITY, &src, &mut dst);
        // The skipped `0 * inf` terms leave the components untouched.
        assert_eq!(dst[0].x, 1.0);
        assert_eq!(dst[0].y, f32::INFINITY);
        assert_eq!(dst[0].z.to_bits(), (-0.0f32).to_bits());
        // The single-vector kernel multiplies through and makes NaNs.
        assert!(transform_matrix(&Matrix::IDENTITY, src[0]).x.is_nan());
    }

    #[test]
    fn general_matrices_use_the_plain_loop() {
        let m = Matrix::from_rows([[0.5, 0.25, 2.0], [3.0, -1.5, 0.75], [1.25, 4.0, -2.5]]);
        let x = Transform::new(m, Vector::new(1.0, -2.0, 0.5));
        let src = [Vector::new(0.1, 0.2, 0.3), Vector::new(-7.0, 8.5, 1e-3)];
        let mut dst = [Vector::ZERO; 2];
        transform_list_xform(&x, &src, &mut dst);
        for (s, d) in src.iter().zip(dst) {
            let single = transform_xform(&x, *s);
            assert!((single.x - d.x).abs() <= 1e-5 * single.x.abs().max(1.0));
            assert!((single.y - d.y).abs() <= 1e-5 * single.y.abs().max(1.0));
            assert!((single.z - d.z).abs() <= 1e-5 * single.z.abs().max(1.0));
        }
    }

    #[test]
    fn in_place_kernel_rows_read_replaced_components() {
        // Row 1 is (1, 0, 0): its kernel returns the X the first row
        // already stored.
        let m = Matrix::from_rows([[0.0, 1.0, 0.0], [1.0, 0.0, 0.0], [0.0, 0.0, 1.0]]);
        let mut v = [Vector::new(1.0, 2.0, 3.0)];
        transform_list_matrix_in_place(&m, &mut v);
        assert_eq!(v[0], Vector::new(2.0, 2.0, 3.0));
        let mut copy = [Vector::ZERO];
        transform_list_matrix(&m, &[Vector::new(1.0, 2.0, 3.0)], &mut copy);
        assert_eq!(copy[0], Vector::new(2.0, 1.0, 3.0));
    }

    #[test]
    fn inverse_list_undoes_a_rigid_transform() {
        let (s, c) = 0.3f32.sin_cos();
        let x = Transform::new(
            Matrix::from_rows([[c, -s, 0.0], [s, c, 0.0], [0.0, 0.0, 1.0]]),
            Vector::new(5.0, -1.0, 2.0),
        );
        let src = [Vector::new(1.0, 2.0, 3.0)];
        let mut moved = [Vector::ZERO];
        transform_list_xform(&x, &src, &mut moved);
        let mut back = [Vector::ZERO];
        inverse_transform_list(&x, &moved, &mut back);
        assert!((back[0].x - 1.0).abs() < 1e-5);
        assert!((back[0].y - 2.0).abs() < 1e-5);
        assert!((back[0].z - 3.0).abs() < 1e-5);
    }
}
