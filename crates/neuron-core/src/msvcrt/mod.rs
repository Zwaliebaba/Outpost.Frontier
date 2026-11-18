//! C runtime contracts the C++ relies on (work package P1.1).
//!
//! The Rust standard library parses and formats numbers differently from
//! the Microsoft C runtime. These functions reproduce the results the C++
//! reference gets: the wrapping `atoi` and `_wtoi` of
//! `CppSource/include/msvcrt_compat.h`, the legacy `atof` of
//! `CppSource/Common/msvcrt_atof.cpp`, the `%g` conversion of the
//! Universal C Runtime the reference links, and the VC6 number texts of
//! `CppSource/include/msvcrt_numeric_format.h` (P4.13, P4.14) with the
//! legacy `printf` they fall back to.

mod atof;
mod format;
mod legacy;
mod vc6;

pub use atof::atof;
pub use format::format_g;
pub use legacy::{Printed, PrintfArg, PrintfError, legacy_format, legacy_vsnprintf};
pub use vc6::{format_lua_number, numeric_vsnprintf};

use crate::hash::c_str;

/// The "C" locale `isspace`: 0x09..=0x0D and the space.
pub(crate) fn is_c_space(byte: u8) -> bool {
    byte == b' ' || (0x09..=0x0D).contains(&byte)
}

/// MSVCRT `atoi` (`MsvcrtAtoi`): leading white space, one optional sign,
/// then decimal digits accumulated modulo 2^32 with no overflow check.
#[must_use]
pub fn atoi(text: &[u8]) -> i32 {
    let text = c_str(text);
    let mut rest = text
        .iter()
        .copied()
        .skip_while(|&b| is_c_space(b))
        .peekable();
    let negative = match rest.peek() {
        Some(b'-') => {
            rest.next();
            true
        }
        Some(b'+') => {
            rest.next();
            false
        }
        _ => false,
    };
    let total = rest
        .map_while(|b| b.is_ascii_digit().then(|| u32::from(b - b'0')))
        .fold(0u32, |total, digit| {
            total.wrapping_mul(10).wrapping_add(digit)
        });
    let total = if negative {
        total.wrapping_neg()
    } else {
        total
    };
    i32::from_ne_bytes(total.to_ne_bytes())
}

/// The first code unit of each run of ten decimal digits `_wchartodigit`
/// accepts.
const WIDE_DIGIT_ZEROS: [u16; 18] = [
    0x0030, 0x0660, 0x06F0, 0x0966, 0x09E6, 0x0A66, 0x0AE6, 0x0B66, 0x0C66, 0x0CE6, 0x0D66, 0x0E50,
    0x0ED0, 0x0F20, 0x1040, 0x17E0, 0x1810, 0xFF10,
];

/// `_wchartodigit`: the value of a decimal digit in one of the 18 runs of
/// digits the runtime knows (ASCII, Arabic-Indic, Devanagari, ...,
/// fullwidth), `None` for anything else.
#[must_use]
pub fn wide_digit(unit: u16) -> Option<u32> {
    WIDE_DIGIT_ZEROS
        .iter()
        .find(|&&zero| unit < zero + 10)
        .and_then(|&zero| (unit >= zero).then(|| u32::from(unit - zero)))
}

fn is_wide_space(unit: u16) -> bool {
    matches!(unit, 0x09..=0x0D | 0x20 | 0xA0 | 0x1680 | 0x180E | 0x2000..=0x200A | 0x2028 | 0x2029 | 0x202F | 0x205F | 0x3000)
}

/// MSVCRT `_wtoi` (`MsvcrtWtoi`): as [`atoi`] over UTF-16 code units, with
/// the CRT's wide white space and the 18 runs of decimal digits it knows.
/// The text ends at the first NUL code unit.
#[must_use]
pub fn wtoi(text: &[u16]) -> i32 {
    let end = text.iter().position(|&u| u == 0).unwrap_or(text.len());
    let mut rest = text[..end]
        .iter()
        .copied()
        .skip_while(|&u| is_wide_space(u))
        .peekable();
    let negative = match rest.peek() {
        Some(0x2D) => {
            rest.next();
            true
        }
        Some(0x2B) => {
            rest.next();
            false
        }
        _ => false,
    };
    let total = rest.map_while(wide_digit).fold(0u32, |total, digit| {
        total.wrapping_mul(10).wrapping_add(digit)
    });
    let total = if negative {
        total.wrapping_neg()
    } else {
        total
    };
    i32::from_ne_bytes(total.to_ne_bytes())
}

/// `wcstol(text, nullptr, radix)` of the Universal C Runtime: wide white
/// space, one optional sign, an optional `0x` for radix 16 (radix 0 picks
/// 16, 8 or 10 from the prefix), then digits; [`wide_digit`] digits and
/// ASCII letters count. Out of range saturates to `i32::MIN` or
/// `i32::MAX`; no digits gives 0. The text ends at its first NUL.
///
/// # Panics
///
/// When `radix` is neither 0 nor in 2..=36.
#[must_use]
pub fn wcstol(text: &[u16], radix: u32) -> i32 {
    let (negative, magnitude, overflow) = strtox(text, radix);
    let limit = if negative {
        1u64 << 31
    } else {
        (1u64 << 31) - 1
    };
    if overflow || magnitude > limit {
        return if negative { i32::MIN } else { i32::MAX };
    }
    let magnitude = i64::try_from(magnitude).unwrap_or(i64::MAX);
    i32::try_from(if negative { -magnitude } else { magnitude }).unwrap_or(i32::MAX)
}

/// `wcstoul(text, nullptr, radix)`: as [`wcstol`], with out of range
/// giving `u32::MAX` and a `-` negating modulo 2^32.
///
/// # Panics
///
/// When `radix` is neither 0 nor in 2..=36.
#[must_use]
pub fn wcstoul(text: &[u16], radix: u32) -> u32 {
    let (negative, magnitude, overflow) = strtox(text, radix);
    let Ok(magnitude) = u32::try_from(magnitude) else {
        return u32::MAX;
    };
    if overflow {
        u32::MAX
    } else if negative {
        magnitude.wrapping_neg()
    } else {
        magnitude
    }
}

/// The sign, the magnitude (at most 2^32, larger values set the flag) and
/// whether it overflowed 32 bits.
fn strtox(text: &[u16], radix: u32) -> (bool, u64, bool) {
    assert!(radix == 0 || (2..=36).contains(&radix), "radix {radix}");
    let end = text.iter().position(|&u| u == 0).unwrap_or(text.len());
    let text = &text[..end];
    let digit = |unit: u16| {
        wide_digit(unit).or_else(|| {
            u8::try_from(unit)
                .ok()
                .filter(u8::is_ascii_alphabetic)
                .map(|b| u32::from(b.to_ascii_uppercase() - b'A') + 10)
        })
    };
    let mut at = text.iter().take_while(|&&u| is_wide_space(u)).count();
    let negative = match text.get(at) {
        Some(0x2D) => {
            at += 1;
            true
        }
        Some(0x2B) => {
            at += 1;
            false
        }
        _ => false,
    };
    let mut radix = radix;
    if (radix == 0 || radix == 16)
        && text.get(at).copied().and_then(digit) == Some(0)
        && matches!(text.get(at + 1), Some(0x78 | 0x58))
    {
        radix = 16;
        at += 2;
    } else if radix == 0 {
        radix = if text.get(at).copied().and_then(digit) == Some(0) {
            8
        } else {
            10
        };
    }
    let mut magnitude: u64 = 0;
    let mut overflow = false;
    for &unit in &text[at.min(text.len())..] {
        let Some(value) = digit(unit).filter(|&v| v < radix) else {
            break;
        };
        magnitude = magnitude * u64::from(radix) + u64::from(value);
        if magnitude > u64::from(u32::MAX) {
            overflow = true;
            magnitude = u64::from(u32::MAX) + 1;
        }
    }
    (negative, magnitude, overflow)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn atoi_wraps_like_msvcrt() {
        // Values measured against msvcrt.dll, quoted in msvcrt_compat.h.
        assert_eq!(atoi(b"2806013121"), -1_488_954_175);
        assert_eq!(atoi(b"4294967296"), 0);
        assert_eq!(atoi(b"-2806013121"), 1_488_954_175);
        assert_eq!(atoi(b"99999999999"), 1_215_752_191);
        assert_eq!(atoi(b" \x0b\t+12x"), 12);
        assert_eq!(atoi(b"+-1"), 0);
        assert_eq!(atoi(b"1\x002"), 1);
    }

    #[test]
    fn wcstol_saturates_and_wcstoul_wraps() {
        let w = |text: &str| -> Vec<u16> { text.encode_utf16().collect() };
        assert_eq!(wcstol(&w(" -12x"), 10), -12);
        assert_eq!(wcstol(&w("2147483648"), 10), i32::MAX);
        assert_eq!(wcstol(&w("-2147483648"), 10), i32::MIN);
        assert_eq!(wcstol(&w("-99999999999"), 10), i32::MIN);
        assert_eq!(wcstol(&w("abc"), 10), 0);
        assert_eq!(wcstoul(&w("0xffffff00"), 16), 0xFFFF_FF00);
        assert_eq!(wcstoul(&w("0x1ffffffff"), 16), u32::MAX);
        assert_eq!(wcstoul(&w("-1"), 10), u32::MAX);
        assert_eq!(wcstoul(&w("0xg"), 16), 0);
        assert_eq!(wcstol(&w("\u{ff11}\u{ff12}"), 10), 12);
        assert_eq!(wcstol(&w("010"), 0), 8);
    }

    #[test]
    fn wtoi_accepts_the_crt_digit_runs() {
        let fullwidth: Vec<u16> = vec![0x3000, 0x2D, 0xFF11, 0xFF12];
        assert_eq!(wtoi(&fullwidth), -12);
        let arabic: Vec<u16> = vec![0x0663, 0x0030];
        assert_eq!(wtoi(&arabic), 30);
        assert_eq!(wtoi(&[0x0669 + 1]), 0);
    }
}
