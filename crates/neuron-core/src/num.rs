//! Float-to-integer conversions with x86 results (work package P1.1,
//! decision T11).
//!
//! Rust's `as` saturates and maps NaN to zero. The x86 conversion
//! instructions return the "integer indefinite" value (the most negative
//! integer) for NaN and for results out of range, and C++ code compiled for
//! x86 depends on that.

const TWO_POW_31: f64 = 2_147_483_648.0;
const TWO_POW_63: f64 = 9_223_372_036_854_775_808.0;

/// `(int)value` in the reference: truncation toward zero (`cvttsd2si`,
/// `__ftol2_sse`); NaN and out of range give `i32::MIN`.
#[must_use]
pub fn trunc_to_i32(value: f64) -> i32 {
    if value.is_nan() || value >= TWO_POW_31 || value <= -TWO_POW_31 - 1.0 {
        i32::MIN
    } else {
        value as i32
    }
}

/// `(__int64)value` in the reference: truncation toward zero; NaN and out
/// of range give `i64::MIN`.
#[must_use]
pub fn trunc_to_i64(value: f64) -> i64 {
    if value.is_nan() || !(-TWO_POW_63..TWO_POW_63).contains(&value) {
        i64::MIN
    } else {
        value as i64
    }
}

/// `(int)(__int64)value`: truncation to 64 bits, then the low 32 bits.
#[must_use]
pub fn trunc_to_i32_via_i64(value: f64) -> i32 {
    let [a, b, c, d, ..] = trunc_to_i64(value).to_le_bytes();
    i32::from_le_bytes([a, b, c, d])
}

/// `cvtsd2si` or `FISTP` under the default rounding mode: to nearest, an
/// exact half to even; NaN and out of range give `i32::MIN`.
#[must_use]
pub fn round_even_to_i32(value: f64) -> i32 {
    let rounded = value.round_ties_even();
    if rounded.is_nan() || !(-TWO_POW_31..TWO_POW_31).contains(&rounded) {
        i32::MIN
    } else {
        rounded as i32
    }
}

/// As [`round_even_to_i32`], for 64-bit results.
#[must_use]
pub fn round_even_to_i64(value: f64) -> i64 {
    let rounded = value.round_ties_even();
    if rounded.is_nan() || !(-TWO_POW_63..TWO_POW_63).contains(&rounded) {
        i64::MIN
    } else {
        rounded as i64
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn truncation_and_indefinite() {
        assert_eq!(trunc_to_i32(-1.9), -1);
        assert_eq!(trunc_to_i32(2_147_483_647.9), i32::MAX);
        assert_eq!(trunc_to_i32(-2_147_483_648.9), i32::MIN);
        assert_eq!(trunc_to_i32(2_147_483_648.0), i32::MIN);
        assert_eq!(trunc_to_i32(f64::NAN), i32::MIN);
        assert_eq!(trunc_to_i64(TWO_POW_63), i64::MIN);
        assert_eq!(trunc_to_i64(-TWO_POW_63), i64::MIN);
        assert_eq!(trunc_to_i32_via_i64(4_294_967_297.0), 1);
        assert_eq!(trunc_to_i32_via_i64(1e30), 0);
    }

    #[test]
    fn rounding_to_even() {
        assert_eq!(round_even_to_i32(2.5), 2);
        assert_eq!(round_even_to_i32(-3.5), -4);
        assert_eq!(round_even_to_i32(2_147_483_647.4), i32::MAX);
        assert_eq!(round_even_to_i32(2_147_483_647.5), i32::MIN);
        assert_eq!(round_even_to_i64(f64::INFINITY), i64::MIN);
    }
}
