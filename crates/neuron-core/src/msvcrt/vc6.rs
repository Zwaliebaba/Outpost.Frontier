//! The VC6 number texts of `CppSource/include/msvcrt_numeric_format.h`
//! (work packages P4.13 and P4.14, decision T11).
//!
//! The shipped game was built with Visual C++ 6, whose runtime formatted
//! numbers by converting them to 17 significant digits first and rounding
//! those digits half up to the precision asked for. The reference links
//! the Universal C Runtime and rebuilds that behaviour from `_ecvt_s`:
//!
//! - [`format_lua_number`] is `MsvcrtFormatLuaNumber`, the `%.16g` text
//!   Lua 3.2 makes of a number (`luaV_tostring`, and `lua_getstring` on a
//!   number). `0.99999` becomes `0.9999900000000001`; exponents have three
//!   digits. The scene-script chunks store their numbers in this form
//!   (fact F15), so `xtask thn-compile` uses it too.
//! - [`numeric_vsnprintf`] is `MsvcrtNumericVsnprintf`, the `printf` of the
//!   client's number strings (`NumberStringChild::Printf`): a format that is
//!   one `%.Nf` and a suffix gets the VC6 fixed text, which pads zeros after
//!   the 17th significant digit; any other format goes to the legacy
//!   `printf` ([`super::legacy_vsnprintf`]).
//!
//! The decimal point is the "C" locale's `.`: neither the client nor the
//! scene scripts change the locale.

use super::legacy::{Printed, PrintfArg, PrintfError, ecvt, legacy_format, legacy_vsnprintf};
use crate::hash::c_str;

/// The digits `_ecvt_s` is asked for.
const DIGITS: usize = 17;

/// The `Output` buffer of `MsvcrtNumericVsnprintf`.
const OUTPUT_SIZE: usize = 512;

/// The largest precision the fixed path takes.
const MAX_PRECISION: usize = 100;

/// `MsvcrtRoundDigits`: rounds the 17 digits half up to `keep` digits. A
/// carry out of the first digit leaves `1` and sixteen zeros and moves the
/// point. Digits from `keep` on are not cleared otherwise.
fn round_digits(digits: &mut [u8], keep: i32, point: &mut i32) {
    let Ok(keep) = usize::try_from(keep) else {
        return;
    };
    if keep >= DIGITS || digits[keep] < b'5' {
        return;
    }
    let mut index = keep;
    while index > 0 && digits[index - 1] == b'9' {
        digits[index - 1] = b'0';
        index -= 1;
    }
    if index > 0 {
        digits[index - 1] += 1;
    } else {
        digits[0] = b'1';
        digits[1..DIGITS].fill(b'0');
        *point += 1;
    }
}

/// `MsvcrtFormatLuaNumber`: Lua 3.2's text for a number.
///
/// Infinity and NaN take the legacy `%.16g` (`1.#INF`, `-1.#IND`).
#[must_use]
pub fn format_lua_number(value: f64) -> String {
    let text = if value.is_finite() {
        lua_digits(value)
    } else {
        legacy_format(b"%.16g", &[PrintfArg::Double(value)]).unwrap_or_default()
    };
    // Every byte is ASCII: digits, signs, `.`, `e`, `#` and capitals.
    text.iter().map(|&b| char::from(b)).collect()
}

fn lua_digits(value: f64) -> Vec<u8> {
    let (mut digits, mut point, sign) = ecvt(value, DIGITS);
    if value == 0.0 {
        // `%g` writes 0 or -0; `_ecvt_s` puts the point of zero at 0.
        point = 1;
    }
    round_digits(&mut digits, 16, &mut point);
    let mut length = 16;
    while length > 1 && digits[length - 1] == b'0' {
        length -= 1;
    }
    let mut out = Vec::with_capacity(32);
    if sign {
        out.push(b'-');
    }
    let exponent = point - 1;
    if !(-4..16).contains(&exponent) {
        out.push(digits[0]);
        if length > 1 {
            out.push(b'.');
            out.extend_from_slice(&digits[1..length]);
        }
        let sign = if exponent < 0 { '-' } else { '+' };
        out.extend_from_slice(format!("e{sign}{:03}", exponent.unsigned_abs()).as_bytes());
    } else if point <= 0 {
        out.extend_from_slice(b"0.");
        out.resize(out.len() + point.unsigned_abs() as usize, b'0');
        out.extend_from_slice(&digits[..length]);
    } else {
        let whole = point.unsigned_abs() as usize;
        out.extend((0..whole).map(|i| if i < length { digits[i] } else { b'0' }));
        if length > whole {
            out.push(b'.');
            out.extend_from_slice(&digits[whole..length]);
        }
    }
    out
}

/// The fixed path of `MsvcrtNumericVsnprintf`: `value` with `precision`
/// decimals as VC6 printed it, then `suffix`. `None` where the text and
/// the suffix do not fit the 512-byte work buffer.
fn vc6_fixed(value: f64, precision: usize, suffix: &[u8]) -> Option<Vec<u8>> {
    let (mut digits, mut point, sign) = ecvt(value, DIGITS);
    let precision_i32 = i32::try_from(precision).unwrap_or(i32::MAX);
    let mut keep = point + precision_i32;
    round_digits(&mut digits, keep, &mut point);
    if keep >= 0 && point + precision_i32 > keep {
        keep = point + precision_i32;
    }
    let digit = |index: i32| {
        usize::try_from(index)
            .ok()
            .filter(|&i| i < DIGITS && index < keep)
            .map_or(b'0', |i| digits[i])
    };
    let mut out = Vec::new();
    if sign {
        out.push(b'-');
    }
    for index in 0..point.max(1) {
        out.push(if point > 0 { digit(index) } else { b'0' });
    }
    if precision > 0 {
        out.push(b'.');
    }
    for index in 0..precision_i32 {
        out.push(digit(point + index));
    }
    if out.len() + suffix.len() >= OUTPUT_SIZE {
        return None;
    }
    out.extend_from_slice(suffix);
    Some(out)
}

/// `MsvcrtNumericVsnprintf(Buffer, count, format, args)`. The format ends
/// at its first NUL.
///
/// A format of `%.` with 1 to 100 as the precision, `f`, and a suffix
/// without `%` (or exactly `%`, which prints nothing, or `%%`, which
/// prints `%`) reads one `double` and gets the VC6 fixed text; infinity and
/// NaN, and every other format, go to the legacy `printf`. When the fixed
/// text does not fit its 512-byte work buffer the result is -1 and nothing
/// is stored.
///
/// # Errors
///
/// A format the legacy `printf` does not run, or arguments that do not
/// match the format.
pub fn numeric_vsnprintf(
    count: usize,
    format: &[u8],
    args: &[PrintfArg],
) -> Result<Printed, PrintfError> {
    let format = c_str(format);
    if let Some((precision, rest)) = fixed_format(format) {
        let value = match args.first() {
            Some(PrintfArg::Double(value)) => *value,
            Some(PrintfArg::Int(_)) => return Err(PrintfError::ArgumentType),
            None => return Err(PrintfError::MissingArgument),
        };
        if !value.is_finite() {
            return legacy_vsnprintf(count, format, &[PrintfArg::Double(value)]);
        }
        let suffix: &[u8] = match rest {
            b"%" => b"",
            b"%%" => b"%",
            other => other,
        };
        return Ok(match vc6_fixed(value, precision, suffix) {
            Some(output) => Printed::store(count, &output),
            None => Printed {
                result: -1,
                stored: Vec::new(),
            },
        });
    }
    legacy_vsnprintf(count, format, args)
}

/// The precision and the suffix when `format` takes the fixed path.
fn fixed_format(format: &[u8]) -> Option<(usize, &[u8])> {
    let rest = format.strip_prefix(b"%.")?;
    if !rest.first().is_some_and(u8::is_ascii_digit) {
        return None;
    }
    let mut precision = 0usize;
    let mut at = 0;
    // The reference stops reading digits once the precision passes 100.
    while let Some(d) = rest.get(at).filter(|d| d.is_ascii_digit()) {
        if precision > MAX_PRECISION {
            break;
        }
        precision = precision * 10 + usize::from(d - b'0');
        at += 1;
    }
    let suffix = rest.get(at + 1..)?;
    let fixed = rest[at] == b'f'
        && precision <= MAX_PRECISION
        && (!suffix.contains(&b'%') || suffix == b"%" || suffix == b"%%");
    fixed.then_some((precision, suffix))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn printed(count: usize, format: &str, args: &[PrintfArg]) -> (i32, String) {
        let p = numeric_vsnprintf(count, format.as_bytes(), args).unwrap();
        let text = p
            .stored
            .iter()
            .take_while(|&&b| b != 0)
            .map(|&b| char::from(b));
        (p.result, text.collect())
    }

    fn fixed(format: &str, value: f64) -> String {
        printed(512, format, &[PrintfArg::Double(value)]).1
    }

    #[test]
    fn lua_numbers_round_twice() {
        let cases: &[(f64, &str)] = &[
            (0.0, "0"),
            (-0.0, "-0"),
            (1.0, "1"),
            (29.875, "29.875"),
            (-0.99999, "-0.9999900000000001"),
            (0.1, "0.1"),
            (9e-6, "9e-006"),
            (0.0001, "0.0001"),
            (0.00001, "1e-005"),
            (1e20, "1e+020"),
            (1.5e-300, "1.5e-300"),
            (123_456.789, "123456.789"),
            (1_234_567_890_123_456.0, "1234567890123456"),
            (1e16, "1e+016"),
            (12_345_678_901_234_567.0, "1.234567890123457e+016"),
            (65535.5, "65535.5"),
            (0.999_999_999_999_999_9, "0.9999999999999999"),
            (0.000_778, "0.0007780000000000001"),
            (f64::MAX, "1.797693134862316e+308"),
            (5e-324, "4.940656458412465e-324"),
            (f64::INFINITY, "1.#INF"),
            (f64::NEG_INFINITY, "-1.#INF"),
        ];
        for &(x, text) in cases {
            assert_eq!(format_lua_number(x), text, "{x:e}");
        }
    }

    #[test]
    fn fixed_text_pads_after_seventeen_digits() {
        assert_eq!(fixed("%.2f", 1.005), "1.00");
        // 0.10499999999999999611... is 0.10500000000000000 in 17 digits.
        assert_eq!(fixed("%.2f", 0.105), "0.11");
        assert_eq!(fixed("%.0f", 0.5), "1");
        assert_eq!(fixed("%.1f", 0.05), "0.1");
        assert_eq!(fixed("%.2f", -0.001), "-0.00");
        assert_eq!(fixed("%.2f", -0.0), "-0.00");
        assert_eq!(fixed("%.2f", 0.0), "0.00");
        assert_eq!(fixed("%.1f", 9.96), "10.0");
        assert_eq!(fixed("%.3fK", 12.5), "12.500K");
        assert_eq!(fixed("%.1f%", 50.0), "50.0");
        assert_eq!(fixed("%.1f%%", 50.0), "50.0%");
        assert_eq!(fixed("%.20f", 0.1), "0.10000000000000001000");
        assert_eq!(fixed("%.1f", 1e20), "100000000000000000000.0");
        assert_eq!(
            fixed("%.1f", 123_456_789_012_345_678.0),
            "123456789012345680.0"
        );
        // 1125899906842624.25: the 17-digit conversion takes the half up.
        assert_eq!(
            fixed("%.1f", f64::from_bits(0x4310_0000_0000_0001)),
            "1125899906842624.3"
        );
        // Single float values widened to double, as FormatFloat passes them.
        assert_eq!(fixed("%.2f", f64::from(0.1f32)), "0.10");
        assert_eq!(fixed("%.9f", f64::from(0.1f32)), "0.100000001");
    }

    #[test]
    fn other_formats_go_to_the_legacy_printf() {
        assert_eq!(printed(32, "75%%", &[]), (3, "75%".into()));
        assert_eq!(
            printed(32, "%d%c", &[PrintfArg::Int(7), PrintfArg::Int(0)]),
            (2, "7".into())
        );
        assert_eq!(fixed("%.2f", f64::INFINITY), "1.#J");
        // Precision past 100, a second conversion, `%.f`.
        assert_eq!(fixed("%.101f", 1.0).len(), 103);
        assert_eq!(
            printed(64, "%.1f%d", &[PrintfArg::Double(0.25), PrintfArg::Int(3)]),
            (4, "0.33".into())
        );
        assert_eq!(fixed("%.f", 2.5), "3");
    }

    #[test]
    fn short_buffers() {
        let p = numeric_vsnprintf(4, b"%.2f", &[PrintfArg::Double(12.5)]).unwrap();
        assert_eq!(
            p,
            Printed {
                result: -1,
                stored: b"12.5".to_vec()
            }
        );
        let p = numeric_vsnprintf(5, b"%.2f", &[PrintfArg::Double(12.5)]).unwrap();
        assert_eq!(
            p,
            Printed {
                result: 5,
                stored: b"12.50".to_vec()
            }
        );
        // 1e300 with 100 decimals and a 200-byte suffix passes 512 bytes.
        let format = format!("%.100f{}", "K".repeat(200));
        let p = numeric_vsnprintf(32, format.as_bytes(), &[PrintfArg::Double(1e300)]).unwrap();
        assert_eq!(
            p,
            Printed {
                result: -1,
                stored: Vec::new()
            }
        );
    }
}
