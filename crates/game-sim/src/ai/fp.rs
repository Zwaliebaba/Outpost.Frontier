//! The 53-bit x87 model the AI helpers are written in (decision T11).
//!
//! `FLD dword` widens an `f32` exactly ([`e`]); every x87 `+ - * /` and
//! `FSQRT` is then an `f64` operation; an `FSTP dword`, a `float` local, a
//! `float` argument or a `float` return rounds once ([`r`]). A C++
//! expression of two `float` operands is evaluated on the x87 stack too,
//! so even `a - b` with both `float` is `r(e(a) - e(b))` here, which is
//! what the vector helpers below do.

use neuron_core::math::{Matrix, Vector};

/// `FLD dword`: an `f32` on the 53-bit stack.
#[inline]
pub(crate) fn e(value: f32) -> f64 {
    f64::from(value)
}

/// `FSTP dword` or a `/fp:precise` store to `float`.
#[inline]
pub(crate) fn r(value: f64) -> f32 {
    value as f32
}

/// The `float` difference of two vectors, one rounding per component
/// (Common's `Sub` and `VecSubInPlace3`).
#[inline]
pub(crate) fn sub(a: Vector, b: Vector) -> Vector {
    Vector::new(r(e(a.x) - e(b.x)), r(e(a.y) - e(b.y)), r(e(a.z) - e(b.z)))
}

/// The `float` sum of two vectors (Common's `Add`).
#[inline]
pub(crate) fn add(a: Vector, b: Vector) -> Vector {
    Vector::new(r(e(a.x) + e(b.x)), r(e(a.y) + e(b.y)), r(e(a.z) + e(b.z)))
}

/// A vector times a `float` (Common's `Scale` and `VecScaleInPlace3`).
#[inline]
pub(crate) fn scale(a: Vector, s: f32) -> Vector {
    Vector::new(r(e(a.x) * e(s)), r(e(a.y) * e(s)), r(e(a.z) * e(s)))
}

/// `VecNegate3`: exact.
#[inline]
pub(crate) fn neg(a: Vector) -> Vector {
    Vector::new(-a.x, -a.y, -a.z)
}

/// `(a.z*b.z + a.y*b.y) + a.x*b.x`, unrounded (Common's `DotZyx`,
/// `BankDot`).
#[inline]
pub(crate) fn dot_zyx(a: Vector, b: Vector) -> f64 {
    (e(a.z) * e(b.z) + e(a.y) * e(b.y)) + e(a.x) * e(b.x)
}

/// `(x*x + y*y) + z*z`, unrounded.
#[inline]
pub(crate) fn len_sq_xyz(a: Vector) -> f64 {
    (e(a.x) * e(a.x) + e(a.y) * e(a.y)) + e(a.z) * e(a.z)
}

/// `VecCross3`: `a × b`, each component rounded once.
#[inline]
pub(crate) fn cross(a: Vector, b: Vector) -> Vector {
    Vector::new(
        r(e(a.y) * e(b.z) - e(a.z) * e(b.y)),
        r(e(a.z) * e(b.x) - e(a.x) * e(b.z)),
        r(e(a.x) * e(b.y) - e(a.y) * e(b.x)),
    )
}

/// `VecNormalizeInPlaceByLength3`: the reciprocal of `length` stays on
/// the stack for the three products.
#[inline]
pub(crate) fn normalize_by_length(v: Vector, length: f32) -> Vector {
    let inv = 1.0 / e(length);
    Vector::new(r(e(v.x) * inv), r(e(v.y) * inv), r(e(v.z) * inv))
}

/// `VecNormalize3`: `sqrt((z*z + y*y) + x*x)` and its reciprocal on the
/// stack; at or below 0.01f (or NaN) the result is zero.
#[inline]
pub(crate) fn normalize3(v: Vector) -> Vector {
    let len = ((e(v.z) * e(v.z) + e(v.y) * e(v.y)) + e(v.x) * e(v.x)).sqrt();
    if len > e(0.01) {
        let inv = 1.0 / len;
        Vector::new(r(e(v.x) * inv), r(e(v.y) * inv), r(e(v.z) * inv))
    } else {
        Vector::ZERO
    }
}

/// The normalize-or-zero idiom of `ComputeBankedSteeringFrame`
/// (`BankNormalizeOrZero`): `sqrt((x*x + y*y) + z*z)`, its `float` image
/// as the length when the unrounded value is above 0.01f.
#[inline]
pub(crate) fn normalize_or_zero(v: Vector) -> Vector {
    let length = len_sq_xyz(v).sqrt();
    if length > e(0.01) {
        normalize_by_length(v, r(length))
    } else {
        Vector::ZERO
    }
}

/// Column 0 of a matrix (`MatrixColumn0`).
#[inline]
pub(crate) fn column0(m: &Matrix) -> Vector {
    Vector::new(m.m[0][0], m.m[1][0], m.m[2][0])
}

/// Column 1 of a matrix (`MatrixColumn1`).
#[inline]
pub(crate) fn column1(m: &Matrix) -> Vector {
    Vector::new(m.m[0][1], m.m[1][1], m.m[2][1])
}

/// Column 2 of a matrix (`MatrixColumn2`), not negated.
#[inline]
pub(crate) fn column2(m: &Matrix) -> Vector {
    Vector::new(m.m[0][2], m.m[1][2], m.m[2][2])
}

/// A matrix from its three columns.
#[inline]
pub(crate) fn from_columns(c0: Vector, c1: Vector, c2: Vector) -> Matrix {
    Matrix::from_rows([[c0.x, c1.x, c2.x], [c0.y, c1.y, c2.y], [c0.z, c1.z, c2.z]])
}

/// The fast-length idiom `ObstacleAvoidance::Update` and the intercept
/// helpers inline (`InlineFastLength`): the `float` image of `len_sq`
/// feeds the `0x5f3759df` guess (an arithmetic shift of the signed bits),
/// the Newton step and the final product run on the unrounded value.
#[inline]
pub(crate) fn inline_fast_length(len_sq: f64) -> f64 {
    let image = r(len_sq);
    let bits = i32::from_ne_bytes(image.to_bits().to_ne_bytes());
    let guess_bits = 0x5f37_59df_i32.wrapping_sub(bits >> 1);
    let guess = e(f32::from_bits(u32::from_ne_bytes(guess_bits.to_ne_bytes())));
    let refined = (e(1.5) - guess * guess * len_sq * e(0.5)) * guess;
    refined * len_sq
}

/// The fast inverse square root `BuildPerpendicularBasis` calls
/// (`FastInvSqrtX87`): a logical shift of the bits, the result unrounded.
#[inline]
pub(crate) fn fast_inv_sqrt(x: f32) -> f64 {
    let half = e(x) * e(0.5);
    let bits = 0x5f37_59df_u32.wrapping_sub(x.to_bits() >> 1);
    let y = e(f32::from_bits(bits));
    e(1.5) * y - (half * y) * (y * y)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fast_length_is_close_to_sqrt() {
        for v in [1.0_f64, 2.0, 100.0, 12345.0, 0.25] {
            let fast = inline_fast_length(v);
            assert!((fast - v.sqrt()).abs() / v.sqrt() < 2.0e-3, "{v}: {fast}");
        }
        assert!((fast_inv_sqrt(4.0) - 0.5).abs() < 2.0e-3);
    }

    #[test]
    fn normalize_zeroes_short_vectors() {
        assert_eq!(normalize3(Vector::new(0.001, 0.0, 0.0)), Vector::ZERO);
        assert_eq!(
            normalize3(Vector::new(3.0, 0.0, 4.0)),
            Vector::new(0.6, 0.0, 0.8)
        );
        assert_eq!(normalize_or_zero(Vector::new(0.0, 0.0, 0.0)), Vector::ZERO);
    }
}
