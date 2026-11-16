//! The Universal C Runtime's `%g` conversion.
//!
//! The C++ formats BINI float parameters with `sprintf("%g")` from the
//! Universal C Runtime. It prints the exact binary value rounded to the
//! precision, an exact halfway case to even, with an exponent of at least
//! two digits. Rust's own formatting differs in layout, so the digits are
//! produced here from the exact decimal expansion.

/// A non-negative integer, 32-bit limbs, least significant first.
struct BigUint(Vec<u32>);

impl BigUint {
    fn from_u64(value: u64) -> Self {
        let [a, b, c, d, e, f, g, h] = value.to_le_bytes();
        let mut limbs = vec![
            u32::from_le_bytes([a, b, c, d]),
            u32::from_le_bytes([e, f, g, h]),
        ];
        while limbs.last() == Some(&0) {
            limbs.pop();
        }
        Self(limbs)
    }

    fn mul_small(&mut self, factor: u32) {
        let mut carry = 0u64;
        for limb in &mut self.0 {
            let product = u64::from(*limb) * u64::from(factor) + carry;
            *limb = (product & 0xFFFF_FFFF) as u32;
            carry = product >> 32;
        }
        if carry != 0 {
            self.0.push((carry & 0xFFFF_FFFF) as u32);
        }
    }

    fn shl(&mut self, bits: u32) {
        for _ in 0..bits / 31 {
            self.mul_small(1 << 31);
        }
        let rest = bits % 31;
        if rest != 0 {
            self.mul_small(1 << rest);
        }
    }

    /// Divides by `divisor` in place and returns the remainder.
    fn div_small(&mut self, divisor: u32) -> u32 {
        let mut remainder = 0u64;
        for limb in self.0.iter_mut().rev() {
            let current = (remainder << 32) | u64::from(*limb);
            *limb = (current / u64::from(divisor)) as u32;
            remainder = current % u64::from(divisor);
        }
        while self.0.last() == Some(&0) {
            self.0.pop();
        }
        remainder as u32
    }

    /// Decimal digits (values 0..=9), most significant first.
    fn into_decimal(mut self) -> Vec<u8> {
        let mut chunks = Vec::new();
        while !self.0.is_empty() {
            chunks.push(self.div_small(1_000_000_000));
        }
        let mut digits = Vec::new();
        for (index, chunk) in chunks.iter().rev().enumerate() {
            let text = if index == 0 {
                chunk.to_string()
            } else {
                format!("{chunk:09}")
            };
            digits.extend(text.bytes().map(|b| b - b'0'));
        }
        digits
    }
}

/// The exact decimal expansion of a positive finite value: the value is
/// `0.d1 d2 d3 ... * 10^point`, with no leading or trailing zero digits.
pub(crate) fn exact_decimal(value: f64) -> (Vec<u8>, i32) {
    let bits = value.to_bits();
    let biased = ((bits >> 52) & 0x7FF) as i32;
    let fraction = bits & 0x000F_FFFF_FFFF_FFFF;
    let (mantissa, exponent) = if biased == 0 {
        (fraction, -1074)
    } else {
        (fraction | (1 << 52), biased - 1075)
    };
    let mut number = BigUint::from_u64(mantissa);
    let scale = if exponent >= 0 {
        number.shl(exponent.unsigned_abs());
        0
    } else {
        // m * 2^-k = m * 5^k / 10^k
        for _ in 0..exponent.unsigned_abs() {
            number.mul_small(5);
        }
        -exponent
    };
    let mut digits = number.into_decimal();
    let point = i32::try_from(digits.len()).unwrap_or(i32::MAX) - scale;
    while digits.last() == Some(&0) {
        digits.pop();
    }
    (digits, point)
}

/// Rounds `digits` to `keep` significant digits, an exact half to even.
/// The result has exactly `keep` digits.
pub(crate) fn round_half_even(digits: &mut Vec<u8>, point: &mut i32, keep: usize) {
    if digits.len() > keep {
        let first_dropped = digits[keep];
        let rest_nonzero = digits[keep + 1..].iter().any(|&d| d != 0);
        let last_odd = keep > 0 && digits[keep - 1] % 2 == 1;
        let round_up = first_dropped > 5 || (first_dropped == 5 && (rest_nonzero || last_odd));
        digits.truncate(keep);
        if round_up {
            let mut index = keep;
            loop {
                if index == 0 {
                    digits.insert(0, 1);
                    digits.truncate(keep);
                    *point += 1;
                    break;
                }
                index -= 1;
                if digits[index] == 9 {
                    digits[index] = 0;
                } else {
                    digits[index] += 1;
                    break;
                }
            }
        }
    }
    digits.resize(keep, 0);
}

fn push_digits(out: &mut String, digits: &[u8]) {
    out.extend(digits.iter().map(|&d| char::from(b'0' + d)));
}

fn nan_text(bits: u64) -> &'static str {
    let negative = bits >> 63 != 0;
    let quiet = bits & 0x0008_0000_0000_0000 != 0;
    let payload = bits & 0x0007_FFFF_FFFF_FFFF;
    match (negative, quiet) {
        (false, true) => "nan",
        (true, true) if payload == 0 => "-nan(ind)",
        (true, true) => "-nan",
        (false, false) => "nan(snan)",
        (true, false) => "-nan(snan)",
    }
}

/// `sprintf("%.*g", precision, value)` as the Universal C Runtime prints
/// it in the "C" locale.
#[must_use]
pub fn format_g(value: f64, precision: usize) -> String {
    if value.is_nan() {
        return nan_text(value.to_bits()).to_owned();
    }
    let mut out = String::new();
    if value.is_sign_negative() {
        out.push('-');
    }
    if value.is_infinite() {
        out.push_str("inf");
        return out;
    }
    let precision = precision.max(1);
    if value == 0.0 {
        out.push('0');
        return out;
    }
    let (mut digits, mut point) = exact_decimal(value.abs());
    round_half_even(&mut digits, &mut point, precision);
    let exponent = point - 1;
    let precision_i32 = i32::try_from(precision).unwrap_or(i32::MAX);
    while digits.len() > 1 && digits.last() == Some(&0) {
        digits.pop();
    }
    if exponent < -4 || exponent >= precision_i32 {
        push_digits(&mut out, &digits[..1]);
        if digits.len() > 1 {
            out.push('.');
            push_digits(&mut out, &digits[1..]);
        }
        let sign = if exponent < 0 { '-' } else { '+' };
        out.push_str(&format!("e{sign}{:02}", exponent.unsigned_abs()));
    } else if point <= 0 {
        out.push_str("0.");
        for _ in 0..point.unsigned_abs() {
            out.push('0');
        }
        push_digits(&mut out, &digits);
    } else {
        let whole = point.unsigned_abs() as usize;
        if digits.len() <= whole {
            push_digits(&mut out, &digits);
            for _ in digits.len()..whole {
                out.push('0');
            }
        } else {
            push_digits(&mut out, &digits[..whole]);
            out.push('.');
            push_digits(&mut out, &digits[whole..]);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exact_expansion() {
        assert_eq!(exact_decimal(0.5), (vec![5], 0));
        assert_eq!(exact_decimal(1280.0), (vec![1, 2, 8], 4));
        let (digits, point) = exact_decimal(0.1);
        assert_eq!(point, 0);
        assert_eq!(
            &digits[..19],
            &[1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 5]
        );
    }

    #[test]
    fn layout_follows_the_c_standard() {
        assert_eq!(format_g(0.0, 6), "0");
        assert_eq!(format_g(-0.0, 6), "-0");
        assert_eq!(format_g(1.0, 6), "1");
        assert_eq!(format_g(0.1f32.into(), 6), "0.1");
        assert_eq!(format_g(123_456.0, 6), "123456");
        assert_eq!(format_g(1_234_567.0, 6), "1.23457e+06");
        assert_eq!(format_g(0.0001, 6), "0.0001");
        assert_eq!(format_g(0.000_012_5, 6), "1.25e-05");
        assert_eq!(format_g(1e100, 6), "1e+100");
        assert_eq!(format_g(999_999.5, 6), "1e+06");
        assert_eq!(format_g(f64::INFINITY, 6), "inf");
        assert_eq!(
            format_g(f64::from_bits(0xFFF8_0000_0000_0000), 6),
            "-nan(ind)"
        );
    }

    #[test]
    fn exact_halves_round_to_even() {
        assert_eq!(format_g(1_234_565.0, 6), "1.23456e+06");
        assert_eq!(format_g(1_234_575.0, 6), "1.23458e+06");
        assert_eq!(format_g(0.5, 0), "0.5");
        assert_eq!(format_g(2.5, 1), "2");
    }
}
