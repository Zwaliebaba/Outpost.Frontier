//! The legacy MSVCRT `atof` (`CppSource/Common/msvcrt_atof.cpp`).
//!
//! The old CRT converts decimal text through an 80-bit extended value with
//! its own power tables and truncates exact halfway cases. The result can
//! differ from a correctly rounded parse in the last bit, and data files
//! were written against this behaviour.

use super::is_c_space;
use crate::hash::c_str;

/// An extended value: five 16-bit mantissa words, least significant first,
/// and an exponent with bias 0x3FFF.
#[derive(Clone, Copy, Default)]
struct Extended {
    mantissa: [u16; 5],
    exponent: i32,
}

const ZERO: Extended = Extended {
    mantissa: [0; 5],
    exponent: 0,
};

const POSITIVE_POWERS: [[u16; 6]; 29] = [
    [0x0000, 0x0000, 0x0000, 0x0000, 0xA000, 0x4002],
    [0x0000, 0x0000, 0x0000, 0x0000, 0xC800, 0x4005],
    [0x0000, 0x0000, 0x0000, 0x0000, 0xFA00, 0x4008],
    [0x0000, 0x0000, 0x0000, 0x0000, 0x9C40, 0x400C],
    [0x0000, 0x0000, 0x0000, 0x0000, 0xC350, 0x400F],
    [0x0000, 0x0000, 0x0000, 0x0000, 0xF424, 0x4012],
    [0x0000, 0x0000, 0x0000, 0x8000, 0x9896, 0x4016],
    [0x0000, 0x0000, 0x0000, 0x2000, 0xBEBC, 0x4019],
    [0x0000, 0x0000, 0x0400, 0xC9BF, 0x8E1B, 0x4034],
    [0x0000, 0xA100, 0xCCED, 0x1BCE, 0xD3C2, 0x404E],
    [0xF020, 0xB59E, 0x2B70, 0xADA8, 0x9DC5, 0x4069],
    [0x5DD0, 0x25FD, 0x1AE5, 0x4F8E, 0xEB19, 0x4083],
    [0x9671, 0x95D7, 0x0E43, 0x8D05, 0xAF29, 0x409E],
    [0xBFF9, 0x44A0, 0x81ED, 0x8F12, 0x8281, 0x40B9],
    [0x3CBF, 0xA6D5, 0xFFCF, 0x1F49, 0xC278, 0x40D3],
    [0xC66F, 0x8CE0, 0x80E9, 0x47C9, 0x93BA, 0x41A8],
    [0x85BC, 0x556B, 0x3927, 0xF78D, 0xE070, 0x427C],
    [0xDDBC, 0xDE8E, 0x9DF9, 0xEBFB, 0xAA7E, 0x4351],
    [0xE6A1, 0xE376, 0xF2CC, 0x2F29, 0x8184, 0x4426],
    [0x1028, 0xAA17, 0xAEF8, 0xE310, 0xC4C5, 0x44FA],
    [0xA7EB, 0xF3D4, 0xEBF7, 0x4AE1, 0x957A, 0x45CF],
    [0xCC65, 0x91C7, 0xA60E, 0xA0AE, 0xE319, 0x46A3],
    [0x650D, 0x0C17, 0x8175, 0x7586, 0xC976, 0x4D48],
    [0x4258, 0xA7E4, 0x3993, 0x353B, 0xB2B8, 0x53ED],
    [0xA74D, 0x5DE5, 0xC53D, 0x3B5D, 0x9E8B, 0x5A92],
    [0x5DFF, 0xF0A6, 0x20A1, 0x54C0, 0x8CA5, 0x6137],
    [0xFDD1, 0x5A8B, 0xD88B, 0x5D25, 0xF989, 0x67DB],
    [0x95AA, 0xF3F8, 0xBF27, 0xC8A2, 0xDD5D, 0x6E80],
    [0xC94C, 0x979B, 0x8A20, 0x5202, 0xC460, 0x7525],
];
const NEGATIVE_POWERS: [[u16; 6]; 29] = [
    [0xCCCD, 0xCCCD, 0xCCCC, 0xCCCC, 0xCCCC, 0x3FFB],
    [0x3D71, 0xD70A, 0x70A3, 0x0A3D, 0xA3D7, 0x3FF8],
    [0x645A, 0xDF3B, 0x8D4F, 0x6E97, 0x8312, 0x3FF5],
    [0xD3C3, 0x652C, 0xE219, 0x1758, 0xD1B7, 0x3FF1],
    [0x0FD0, 0x8423, 0x1B47, 0xAC47, 0xA7C5, 0x3FEE],
    [0xA640, 0x69B6, 0xAF6C, 0xBD05, 0x8637, 0x3FEB],
    [0x3D33, 0x42BC, 0xE57A, 0x94D5, 0xD6BF, 0x3FE7],
    [0xFDC2, 0xCEFD, 0x8461, 0x7711, 0xABCC, 0x3FE4],
    [0x4C2F, 0xE15B, 0xC44D, 0x94BE, 0xE695, 0x3FC9],
    [0xC492, 0x3B53, 0x4475, 0x14CD, 0x9ABE, 0x3FAF],
    [0x67DE, 0x94BA, 0x4539, 0x1EAD, 0xCFB1, 0x3F94],
    [0x2324, 0xE2C6, 0xBABC, 0x313B, 0x8B61, 0x3F7A],
    [0x5561, 0xC159, 0xB17E, 0x7C53, 0xBB12, 0x3F5F],
    [0xEED7, 0x8D2F, 0xBE06, 0x8592, 0xFB15, 0x3F44],
    [0x3F24, 0xE9A5, 0xA539, 0xEA27, 0xA87F, 0x3F2A],
    [0xAC7D, 0xE4A1, 0x64BC, 0x467C, 0xDDD0, 0x3E55],
    [0x7B63, 0xCC06, 0x5423, 0x8377, 0x91FF, 0x3D81],
    [0xFA91, 0x193A, 0x637A, 0x4325, 0xC031, 0x3CAC],
    [0x8921, 0x38D1, 0x4782, 0xB897, 0xFD00, 0x3BD7],
    [0x88DC, 0x0858, 0xB11B, 0xE3E8, 0xA686, 0x3B03],
    [0x84C6, 0x4245, 0xB607, 0x7599, 0xDB37, 0x3A2E],
    [0x7133, 0xD21C, 0xDB23, 0xEE32, 0x9049, 0x395A],
    [0x87A6, 0xC0BE, 0xDA57, 0x82A5, 0xA2A6, 0x32B5],
    [0x68E2, 0x11B2, 0x52A7, 0x449F, 0xB759, 0x2C10],
    [0x4925, 0x2DE4, 0x3436, 0x534F, 0xCEAE, 0x256B],
    [0x598F, 0xA404, 0xDEC0, 0x7DC2, 0xE8FB, 0x1EC6],
    [0xE79E, 0x5A88, 0x9157, 0xBF3C, 0x8350, 0x1822],
    [0x4B4E, 0x6265, 0x83FD, 0xAF8F, 0x9406, 0x117D],
    [0x2DE4, 0x9FDE, 0xD2CE, 0x04C8, 0xA6DD, 0x0AD8],
];

fn shift_left(words: &mut [u16]) {
    let mut carry = 0u32;
    for word in words {
        let value = (u32::from(*word) << 1) | carry;
        *word = low_u16(value);
        carry = value >> 16;
    }
}

/// The low 16 bits of a value.
fn low_u16(value: u32) -> u16 {
    let [low, high, _, _] = value.to_le_bytes();
    u16::from_le_bytes([low, high])
}

/// The low 16 bits of a 64-bit value.
fn low_u16_of_u64(value: u64) -> u16 {
    let bytes = value.to_le_bytes();
    u16::from_le_bytes([bytes[0], bytes[1]])
}

/// The CRT's extended multiply. Products below the top 64 bits of the
/// convolution are never summed, as in the original.
fn multiply(left: &mut Extended, right: &Extended) {
    let mut exponent = left.exponent + right.exponent - 0x3FFE;
    if left.exponent >= 0x7FFF || right.exponent >= 0x7FFF || exponent >= 0x7FFF {
        *left = Extended {
            mantissa: [0; 5],
            exponent: 0x7FFF,
        };
        return;
    }
    if exponent <= -63 {
        *left = ZERO;
        return;
    }
    // One spare word: the C++ carries into Product[6], which a product of
    // two 80-bit mantissas never reaches.
    let mut product = [0u16; 7];
    for column in 0..5 {
        for index in column..5 {
            let term =
                u32::from(left.mantissa[index]) * u32::from(right.mantissa[column + 4 - index]);
            let previous = u32::from(product[column]) | (u32::from(product[column + 1]) << 16);
            let sum = previous.wrapping_add(term);
            product[column] = low_u16(sum);
            product[column + 1] = low_u16(sum >> 16);
            if sum < previous {
                product[column + 2] = product[column + 2].wrapping_add(1);
            }
        }
    }
    while exponent > 0 && product[5] & 0x8000 == 0 {
        shift_left(&mut product[..6]);
        exponent -= 1;
    }
    if exponent <= 0 {
        *left = ZERO;
        return;
    }
    if product[0] > 0x8000 || (product[0] == 0x8000 && product[1] & 1 != 0) {
        let mut index = 1;
        while index < 6 {
            product[index] = product[index].wrapping_add(1);
            if product[index] != 0 {
                break;
            }
            index += 1;
        }
        if index == 6 {
            product[5] = 0x8000;
            exponent += 1;
        }
    }
    left.mantissa.copy_from_slice(&product[1..6]);
    left.exponent = exponent;
}

fn multiply_power(value: &mut Extended, power: i32) {
    let table = if power < 0 {
        &NEGATIVE_POWERS
    } else {
        &POSITIVE_POWERS
    };
    let mut remaining = power.unsigned_abs();
    let mut group = 0;
    while remaining != 0 {
        let digit = (remaining & 7) as usize;
        remaining >>= 3;
        if digit != 0 {
            let row = &table[group + digit - 1];
            let mut factor = Extended {
                mantissa: [row[0], row[1], row[2], row[3], row[4]],
                exponent: i32::from(row[5]),
            };
            if factor.mantissa[0] >= 0x8000 {
                let low = (u32::from(factor.mantissa[1]) | (u32::from(factor.mantissa[2]) << 16))
                    .wrapping_sub(1);
                factor.mantissa[1] = low_u16(low);
                factor.mantissa[2] = low_u16(low >> 16);
            }
            multiply(value, &factor);
        }
        group += 7;
    }
}

/// `_ld12tod`: rounds strictly above halfway; an exact midpoint is
/// truncated.
fn to_double(value: &Extended, negative: bool) -> f64 {
    let mut bits: u64 = if negative { 1 << 63 } else { 0 };
    let m = &value.mantissa;
    let high = u64::from(m[1])
        | (u64::from(m[2]) << 16)
        | (u64::from(m[3]) << 32)
        | (u64::from(m[4]) << 48);
    let mut exponent = value.exponent - 0x3FFF;
    if value.exponent >= 0x7FFF {
        bits |= 0x7FF0_0000_0000_0000;
    } else if high != 0 {
        let mut significand = high >> 11;
        let tail = high & 0x7FF;
        if tail > 0x400 || (tail == 0x400 && m[0] != 0) {
            significand += 1;
        }
        if significand == 1 << 53 {
            significand >>= 1;
            exponent += 1;
        }
        if exponent >= 1024 {
            bits |= 0x7FF0_0000_0000_0000;
        } else if exponent >= -1022 {
            let biased = u64::try_from(exponent + 1023).unwrap_or(0);
            bits |= (biased << 52) | (significand & 0x000F_FFFF_FFFF_FFFF);
        } else {
            // Subnormal: shifted relative to -1023 from the unrounded
            // mantissa, rounded once, and the extra low bit dropped.
            let shift = u32::try_from(-1023 - (value.exponent - 0x3FFF)).unwrap_or(u32::MAX);
            if shift < 64 {
                let shifted = high >> shift;
                let extra = if shift == 0 {
                    m[0]
                } else if shift < 16 {
                    low_u16_of_u64(high << (16 - shift)) | (m[0] >> shift)
                } else {
                    low_u16_of_u64(high >> (shift - 16))
                };
                let mut rounded = shifted >> 11;
                let remainder = shifted & 0x7FF;
                if remainder > 0x400 || (remainder == 0x400 && extra != 0) {
                    rounded += 1;
                }
                bits |= rounded >> 1;
            }
        }
    }
    f64::from_bits(bits)
}

/// MSVCRT `atof` in the "C" locale (`MsvcrtAtof`).
#[must_use]
pub fn atof(text: &[u8]) -> f64 {
    let text = c_str(text);
    let mut cursor = text
        .iter()
        .position(|&b| !is_c_space(b))
        .unwrap_or(text.len());
    let at = |index: usize| text.get(index).copied().unwrap_or(0);
    let negative = at(cursor) == b'-';
    if matches!(at(cursor), b'+' | b'-') {
        cursor += 1;
    }

    let mut digits = [0u8; 24];
    let mut stored = 0usize;
    let mut significant = 0i32;
    let mut fractional = 0i32;
    let mut point = false;
    let mut any_digit = false;
    let mut round_digit = 0u8;
    loop {
        let c = at(cursor);
        if c.is_ascii_digit() {
            any_digit = true;
            if point {
                fractional += 1;
            }
            if significant != 0 || c != b'0' {
                significant += 1;
                if stored < digits.len() {
                    digits[stored] = c - b'0';
                    stored += 1;
                } else if significant == 25 {
                    round_digit = c - b'0';
                }
            }
        } else if !point && c == b'.' {
            point = true;
        } else {
            break;
        }
        cursor += 1;
    }
    if !any_digit {
        return 0.0;
    }

    let mut explicit_exponent = 0i32;
    if matches!(at(cursor), b'e' | b'E' | b'd' | b'D') {
        cursor += 1;
        let exponent_negative = at(cursor) == b'-';
        if matches!(at(cursor), b'+' | b'-') {
            cursor += 1;
        }
        while at(cursor).is_ascii_digit() {
            if explicit_exponent < 10_000 {
                explicit_exponent = explicit_exponent * 10 + i32::from(at(cursor) - b'0');
            }
            cursor += 1;
        }
        if exponent_negative {
            explicit_exponent = -explicit_exponent;
        }
    }
    if stored == 0 {
        return if negative { -0.0 } else { 0.0 };
    }

    let stored_count = i32::try_from(stored).unwrap_or(24);
    let power = explicit_exponent - fractional + significant - stored_count;
    if round_digit >= 5 {
        digits[stored - 1] += 1;
    }
    let mut value = Extended::default();
    for &digit in &digits[..stored] {
        let mut carry = u32::from(digit);
        for word in &mut value.mantissa {
            let product = u32::from(*word) * 10 + carry;
            *word = low_u16(product);
            carry = product >> 16;
        }
    }
    value.exponent = 0x3FFF + 79;
    while value.mantissa[4] & 0x8000 == 0 {
        shift_left(&mut value.mantissa);
        value.exponent -= 1;
    }
    if power > 5200 {
        value.exponent = 0x7FFF;
    } else if power < -5200 {
        value = ZERO;
    } else {
        multiply_power(&mut value, power);
    }
    // Rounded to 64 bits first; an exact halfway value is truncated.
    if value.mantissa[0] > 0x8000 {
        let mut word = 1;
        while word < 5 {
            value.mantissa[word] = value.mantissa[word].wrapping_add(1);
            if value.mantissa[word] != 0 {
                break;
            }
            word += 1;
        }
        if word == 5 {
            value.mantissa[4] = 0x8000;
            value.exponent += 1;
        }
    }
    value.mantissa[0] = 0;
    to_double(&value, negative)
}

#[cfg(test)]
mod tests {
    use super::atof;

    #[test]
    fn simple_values_are_exact() {
        assert_eq!(atof(b"1"), 1.0);
        assert_eq!(atof(b"  -2.5e3xyz"), -2500.0);
        assert_eq!(atof(b"0.125"), 0.125);
        assert_eq!(atof(b"1d2"), 100.0);
        assert_eq!(atof(b".5"), 0.5);
    }

    #[test]
    fn no_digits_gives_positive_zero() {
        assert_eq!(atof(b"-").to_bits(), 0);
        assert_eq!(atof(b"-0").to_bits(), (-0.0f64).to_bits());
        assert_eq!(atof(b"abc"), 0.0);
    }

    #[test]
    fn range_ends() {
        assert!(atof(b"1e400").is_infinite());
        assert_eq!(atof(b"1e-400"), 0.0);
        assert!(atof(b"4.9406564584124654e-324") > 0.0);
    }

    #[test]
    fn common_decimals_round_to_nearest() {
        for text in [
            "0.1",
            "0.2",
            "0.3",
            "3.14159",
            "1234.5678",
            "1e22",
            "6.02214076e23",
        ] {
            let parsed: f64 = text.parse().unwrap_or_default();
            assert_eq!(atof(text.as_bytes()), parsed, "{text}");
        }
    }
}
