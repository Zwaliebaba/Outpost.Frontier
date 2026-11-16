//! C runtime contracts the C++ relies on (work package P1.1).
//!
//! The Rust standard library parses and formats numbers differently from
//! the Microsoft C runtime. These functions reproduce the results the C++
//! reference gets: the wrapping `atoi` and `_wtoi` of
//! `CppSource/include/msvcrt_compat.h`, the legacy `atof` of
//! `CppSource/Common/msvcrt_atof.cpp`, and the `%g` conversion of the
//! Universal C Runtime the reference links.

mod atof;
mod format;

pub use atof::atof;
pub use format::format_g;

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

fn wide_digit(unit: u16) -> Option<u32> {
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
    fn wtoi_accepts_the_crt_digit_runs() {
        let fullwidth: Vec<u16> = vec![0x3000, 0x2D, 0xFF11, 0xFF12];
        assert_eq!(wtoi(&fullwidth), -12);
        let arabic: Vec<u16> = vec![0x0663, 0x0030];
        assert_eq!(wtoi(&arabic), 30);
        assert_eq!(wtoi(&[0x0669 + 1]), 0);
    }
}
