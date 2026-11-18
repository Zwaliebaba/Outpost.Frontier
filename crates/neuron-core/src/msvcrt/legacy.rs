//! The legacy `printf` of `msvcrt_numeric_format.h`: `MsvcrtLegacyVsnprintf`,
//! that is the Universal C Runtime's `__stdio_common_vsprintf` with the
//! options `LEGACY_VSPRINTF_NULL_TERMINATION`, `LEGACY_MSVCRT_COMPATIBILITY`
//! and `LEGACY_THREE_DIGIT_EXPONENTS`, and without `STANDARD_ROUNDING`.
//!
//! What those options change, reasoned from the runtime's source
//! (`corecrt_internal_stdio_output.h`, `cvt.cpp`, `cfout.cpp`) since no
//! Microsoft runtime runs here:
//!
//! - Rounding is the legacy one (`__acrt_rounding_mode::legacy`): the exact
//!   decimal expansion is cut after the digits asked for and rounded up when
//!   the next digit is 5 or more, so an exact half rounds away from zero.
//!   [`super::format_g`] is the default (standard) rounding.
//! - Infinity and NaN go through `__acrt_fltout`'s legacy mantissas
//!   (`1#INF`, `1#QNAN`, `1#SNAN`, `1#IND`, decimal point after the first
//!   character) and are then rounded and laid out like digits, which gives
//!   the VC6 texts `1.#INF00`, `1.#J` (`%.2f`) and `-1.#IND`.
//! - Exponents have at least three digits (`1e+020`).
//! - A buffer of `count` bytes takes the output and a NUL when it is
//!   shorter; output of exactly `count` bytes is stored without the NUL;
//!   longer output stores `count` bytes and returns -1.
//!
//! The conversions are those a format of the VC6 number users can hold:
//! flags `-+ #0`, a width and a precision (digits or `*`), and the types
//! `d i u o x X c e E f F g G` and `%%`. Length modifiers, strings and
//! pointers have no argument type here and are reported as unsupported,
//! as are missing or mistyped arguments (undefined behaviour in the
//! reference; divergence D230).

use std::fmt;

use super::format::exact_decimal;
use crate::hash::c_str;

/// One variadic argument: an `int` (also what `%c` takes) or a `double`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum PrintfArg {
    /// An `int`.
    Int(i32),
    /// A `double`.
    Double(f64),
}

/// What `_vsnprintf` leaves behind.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Printed {
    /// The return value: the length of the output, or -1 when it was
    /// longer than the buffer.
    pub result: i32,
    /// The bytes stored at the start of the buffer, with the terminating
    /// NUL when one was stored. Bytes past these are left as they were.
    pub stored: Vec<u8>,
}

impl Printed {
    /// Stores `output` in a buffer of `count` bytes.
    #[must_use]
    pub fn store(count: usize, output: &[u8]) -> Self {
        let len = output.len();
        let result = i32::try_from(len).unwrap_or(i32::MAX);
        if len < count {
            let mut stored = output.to_vec();
            stored.push(0);
            Self { result, stored }
        } else {
            Self {
                result: if len == count { result } else { -1 },
                stored: output[..count].to_vec(),
            }
        }
    }
}

/// A format the port does not run. In the reference most of these are
/// undefined behaviour (a missing or mistyped argument) or go to the
/// invalid-parameter handler.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PrintfError {
    /// The format ends inside a conversion.
    Truncated,
    /// A conversion or length modifier this port does not handle.
    Unsupported(u8),
    /// Fewer arguments than conversions.
    MissingArgument,
    /// An `int` where a `double` was read, or the reverse.
    ArgumentType,
}

impl fmt::Display for PrintfError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Truncated => write!(f, "format ends inside a conversion"),
            Self::Unsupported(c) => write!(f, "unsupported conversion character 0x{c:02x}"),
            Self::MissingArgument => write!(f, "fewer arguments than conversions"),
            Self::ArgumentType => write!(f, "argument of the wrong type"),
        }
    }
}

impl std::error::Error for PrintfError {}

/// `MsvcrtLegacyVsnprintf(Buffer, count, format, args)`. The format ends at
/// its first NUL.
///
/// # Errors
///
/// A format outside the supported set, or arguments that do not match it.
pub fn legacy_vsnprintf(
    count: usize,
    format: &[u8],
    args: &[PrintfArg],
) -> Result<Printed, PrintfError> {
    let output = legacy_format(format, args)?;
    Ok(Printed::store(count, &output))
}

/// The whole output of the legacy `printf` for `format`, without a
/// terminator.
///
/// # Errors
///
/// As [`legacy_vsnprintf`].
pub fn legacy_format(format: &[u8], args: &[PrintfArg]) -> Result<Vec<u8>, PrintfError> {
    let format = c_str(format);
    let mut args = args.iter();
    let mut out = Vec::new();
    let mut at = 0;
    while at < format.len() {
        let byte = format[at];
        at += 1;
        if byte != b'%' {
            out.push(byte);
            continue;
        }
        let mut spec = Spec::default();
        // Flags.
        while let Some(&flag) = format.get(at) {
            match flag {
                b'-' => spec.left = true,
                b'+' => spec.plus = true,
                b' ' => spec.space = true,
                b'#' => spec.alternate = true,
                b'0' => spec.zero = true,
                _ => break,
            }
            at += 1;
        }
        // Width.
        if format.get(at) == Some(&b'*') {
            at += 1;
            let width = int_arg(args.next())?;
            if width < 0 {
                spec.left = true;
            }
            spec.width = width.unsigned_abs() as usize;
        } else {
            while let Some(d) = format.get(at).filter(|d| d.is_ascii_digit()) {
                spec.width = spec
                    .width
                    .saturating_mul(10)
                    .saturating_add(usize::from(d - b'0'));
                at += 1;
            }
        }
        // Precision.
        if format.get(at) == Some(&b'.') {
            at += 1;
            if format.get(at) == Some(&b'*') {
                at += 1;
                let precision = int_arg(args.next())?;
                // A negative precision is taken as omitted.
                spec.precision = usize::try_from(precision).ok();
            } else {
                let mut precision = 0usize;
                while let Some(d) = format.get(at).filter(|d| d.is_ascii_digit()) {
                    precision = precision
                        .saturating_mul(10)
                        .saturating_add(usize::from(d - b'0'));
                    at += 1;
                }
                spec.precision = Some(precision);
            }
        }
        let Some(&kind) = format.get(at) else {
            return Err(PrintfError::Truncated);
        };
        at += 1;
        match kind {
            b'%' => out.push(b'%'),
            b'd' | b'i' => {
                let value = int_arg(args.next())?;
                let sign = if value < 0 {
                    b"-".as_slice()
                } else {
                    spec.positive_sign()
                };
                let digits = integer_digits(u64::from(value.unsigned_abs()), 10, false);
                spec.pad_integer(&mut out, sign, b"", &digits);
            }
            b'u' | b'o' | b'x' | b'X' => {
                let value = u64::from(u32::from_ne_bytes(int_arg(args.next())?.to_ne_bytes()));
                let (radix, upper) = match kind {
                    b'u' => (10, false),
                    b'o' => (8, false),
                    b'x' => (16, false),
                    _ => (16, true),
                };
                let mut digits = integer_digits(value, radix, upper);
                let mut prefix: &[u8] = b"";
                if spec.alternate && value != 0 {
                    match kind {
                        b'x' => prefix = b"0x",
                        b'X' => prefix = b"0X",
                        _ => {}
                    }
                }
                if spec.alternate && kind == b'o' && digits.first() != Some(&b'0') {
                    digits.insert(0, b'0');
                }
                spec.pad_integer(&mut out, b"", prefix, &digits);
            }
            b'c' => {
                let value = int_arg(args.next())?;
                let [byte, ..] = value.to_le_bytes();
                spec.pad(&mut out, b"", &[byte], false);
            }
            b'e' | b'E' | b'f' | b'F' | b'g' | b'G' => {
                let value = double_arg(args.next())?;
                let (negative, body) = format_float(value, kind, &spec);
                let sign = if negative {
                    b"-".as_slice()
                } else {
                    spec.positive_sign()
                };
                spec.pad(&mut out, sign, &body, spec.zero);
            }
            other => return Err(PrintfError::Unsupported(other)),
        }
    }
    Ok(out)
}

fn int_arg(arg: Option<&PrintfArg>) -> Result<i32, PrintfError> {
    match arg {
        Some(PrintfArg::Int(value)) => Ok(*value),
        Some(PrintfArg::Double(_)) => Err(PrintfError::ArgumentType),
        None => Err(PrintfError::MissingArgument),
    }
}

fn double_arg(arg: Option<&PrintfArg>) -> Result<f64, PrintfError> {
    match arg {
        Some(PrintfArg::Double(value)) => Ok(*value),
        Some(PrintfArg::Int(_)) => Err(PrintfError::ArgumentType),
        None => Err(PrintfError::MissingArgument),
    }
}

/// A parsed conversion specification.
#[derive(Default)]
struct Spec {
    left: bool,
    plus: bool,
    space: bool,
    alternate: bool,
    zero: bool,
    width: usize,
    precision: Option<usize>,
}

impl Spec {
    fn positive_sign(&self) -> &'static [u8] {
        if self.plus {
            b"+"
        } else if self.space {
            b" "
        } else {
            b""
        }
    }

    /// Sign, prefix, at least `precision` digits (none for a zero value
    /// with precision 0), padded to the width; `0` pads only without a
    /// precision.
    fn pad_integer(&self, out: &mut Vec<u8>, sign: &[u8], prefix: &[u8], digits: &[u8]) {
        let mut body = Vec::with_capacity(digits.len());
        let digits: &[u8] = if self.precision == Some(0) && digits == b"0" {
            b""
        } else {
            digits
        };
        let minimum = self.precision.unwrap_or(1);
        body.resize(minimum.saturating_sub(digits.len()), b'0');
        body.extend_from_slice(digits);
        let mut head = sign.to_vec();
        head.extend_from_slice(prefix);
        self.pad(out, &head, &body, self.zero && self.precision.is_none());
    }

    /// `head` then `body`, padded to the width with spaces (on the right
    /// for `-`) or with zeros between them.
    fn pad(&self, out: &mut Vec<u8>, head: &[u8], body: &[u8], zeros: bool) {
        let fill = self.width.saturating_sub(head.len() + body.len());
        if self.left {
            out.extend_from_slice(head);
            out.extend_from_slice(body);
            out.resize(out.len() + fill, b' ');
        } else if zeros {
            out.extend_from_slice(head);
            out.resize(out.len() + fill, b'0');
            out.extend_from_slice(body);
        } else {
            out.resize(out.len() + fill, b' ');
            out.extend_from_slice(head);
            out.extend_from_slice(body);
        }
    }
}

fn integer_digits(mut value: u64, radix: u64, upper: bool) -> Vec<u8> {
    let table: &[u8; 16] = if upper {
        b"0123456789ABCDEF"
    } else {
        b"0123456789abcdef"
    };
    let mut digits = Vec::new();
    loop {
        digits.push(table[(value % radix) as usize]);
        value /= radix;
        if value == 0 {
            break;
        }
    }
    digits.reverse();
    digits
}

/// `_strflt` as `__acrt_fltout` fills it: the sign, the position of the
/// decimal point and the mantissa characters (the exact digits, or a
/// legacy text for infinity and NaN).
pub(crate) struct Strflt {
    pub negative: bool,
    pub decpt: i32,
    pub mantissa: Vec<u8>,
}

/// `__acrt_fltout`.
pub(crate) fn fltout(value: f64) -> Strflt {
    let bits = value.to_bits();
    let negative = bits >> 63 != 0;
    let special = |text: &[u8]| Strflt {
        negative,
        decpt: 1,
        mantissa: text.to_vec(),
    };
    if value == 0.0 {
        return Strflt {
            negative,
            decpt: 0,
            mantissa: b"0".to_vec(),
        };
    }
    if value.is_infinite() {
        return special(b"1#INF");
    }
    if value.is_nan() {
        let quiet = bits & 0x0008_0000_0000_0000 != 0;
        let payload = bits & 0x0007_FFFF_FFFF_FFFF;
        return match (quiet, negative && payload == 0) {
            (false, _) => special(b"1#SNAN"),
            (true, true) => special(b"1#IND"),
            (true, false) => special(b"1#QNAN"),
        };
    }
    let (digits, point) = exact_decimal(value.abs());
    Strflt {
        negative,
        decpt: point,
        mantissa: digits.iter().map(|d| b'0' + d).collect(),
    }
}

/// `__acrt_fp_strflt_to_string` with legacy rounding: the first `count`
/// mantissa characters (zero-padded), rounded up when the next character
/// is `'5'` or above. A carry out of the first digit keeps the extra
/// leading `'1'` (one character more) and moves the decimal point.
/// Nothing for a negative `count`.
pub(crate) fn strflt_digits(flt: &mut Strflt, count: i32) -> Vec<u8> {
    let n = usize::try_from(count).unwrap_or(0);
    let mut buffer = Vec::with_capacity(n + 1);
    buffer.push(b'0');
    buffer.extend((0..n).map(|i| flt.mantissa.get(i).copied().unwrap_or(b'0')));
    if count >= 0 && flt.mantissa.get(n).copied().unwrap_or(0) >= b'5' {
        let mut index = buffer.len() - 1;
        while buffer[index] == b'9' {
            buffer[index] = b'0';
            index -= 1;
        }
        buffer[index] += 1;
    }
    if buffer[0] == b'1' {
        flt.decpt += 1;
    } else {
        buffer.remove(0);
    }
    buffer
}

/// `_ecvt_s(buffer, size, value, count, &point, &sign)` of the runtime:
/// `count` digits, the decimal point position and the sign. Zero gives
/// zeros with the point at 0.
pub(crate) fn ecvt(value: f64, count: usize) -> (Vec<u8>, i32, bool) {
    let mut flt = fltout(value);
    let mut digits = strflt_digits(&mut flt, i32::try_from(count).unwrap_or(i32::MAX));
    digits.truncate(count);
    (digits, flt.decpt, flt.negative)
}

/// A floating conversion without sign and padding: whether the value is
/// negative, and the text.
fn format_float(value: f64, kind: u8, spec: &Spec) -> (bool, Vec<u8>) {
    let precision = spec.precision.unwrap_or(6);
    let precision_i32 = i32::try_from(precision).unwrap_or(i32::MAX);
    let upper = kind.is_ascii_uppercase();
    let mut flt = fltout(value);
    let zero = flt.mantissa == b"0";
    let body = match kind {
        b'f' | b'F' => {
            let count = flt.decpt.saturating_add(precision_i32);
            let digits = strflt_digits(&mut flt, count);
            fixed_layout(&digits, flt.decpt, precision, spec.alternate)
        }
        b'e' | b'E' => {
            let digits = strflt_digits(&mut flt, precision_i32.saturating_add(1));
            exponent_layout(&digits, flt.decpt, precision, spec.alternate, upper, zero)
        }
        _ => {
            let significant = precision.max(1);
            let significant_i32 = i32::try_from(significant).unwrap_or(i32::MAX);
            let before = flt.decpt;
            let mut digits = strflt_digits(&mut flt, significant_i32);
            let magnitude = flt.decpt - 1;
            let mut text = if magnitude < -4 || magnitude >= significant_i32 {
                if flt.decpt != before {
                    digits.pop();
                }
                exponent_layout(
                    &digits,
                    flt.decpt,
                    significant - 1,
                    spec.alternate,
                    upper,
                    zero,
                )
            } else {
                let decimals = usize::try_from(significant_i32 - flt.decpt).unwrap_or(0);
                fixed_layout(&digits, flt.decpt, decimals, spec.alternate)
            };
            if !spec.alternate {
                crop_zeros(&mut text);
            }
            text
        }
    };
    (flt.negative, body)
}

/// `fp_format_f_internal`: the digits with the decimal point placed
/// `decpt` digits in, a leading `0` for a value below one, and zeros
/// between the point and the first digit.
fn fixed_layout(digits: &[u8], decpt: i32, precision: usize, alternate: bool) -> Vec<u8> {
    let mut out = Vec::with_capacity(digits.len() + precision + 3);
    let rest = if decpt > 0 {
        let whole = (decpt.unsigned_abs() as usize).min(digits.len());
        out.extend_from_slice(&digits[..whole]);
        &digits[whole..]
    } else {
        out.push(b'0');
        digits
    };
    if precision > 0 || alternate {
        out.push(b'.');
    }
    if precision > 0 && decpt < 0 {
        let zeros = (decpt.unsigned_abs() as usize).min(precision);
        out.resize(out.len() + zeros, b'0');
    }
    out.extend_from_slice(rest);
    out
}

/// `fp_format_e_internal`: one digit, the point and `precision` digits,
/// then the exponent with a sign and at least three digits (`e+000` for
/// zero).
fn exponent_layout(
    digits: &[u8],
    decpt: i32,
    precision: usize,
    alternate: bool,
    upper: bool,
    zero: bool,
) -> Vec<u8> {
    let digit = |i: usize| digits.get(i).copied().unwrap_or(b'0');
    let mut out = vec![digit(0)];
    if precision > 0 || alternate {
        out.push(b'.');
    }
    out.extend((1..=precision).map(digit));
    out.push(if upper { b'E' } else { b'e' });
    let exponent = if zero { 0 } else { decpt - 1 };
    out.push(if exponent < 0 { b'-' } else { b'+' });
    out.extend_from_slice(format!("{:03}", exponent.unsigned_abs()).as_bytes());
    out
}

/// `%g` without `#`: trailing zeros of the fraction go, and the point
/// with them when nothing is left after it; an exponent stays.
fn crop_zeros(text: &mut Vec<u8>) {
    let Some(point) = text.iter().position(|&b| b == b'.') else {
        return;
    };
    let end = text[point..]
        .iter()
        .position(|&b| b == b'e' || b == b'E')
        .map_or(text.len(), |e| point + e);
    let mut keep = end;
    while keep > point + 1 && text[keep - 1] == b'0' {
        keep -= 1;
    }
    if keep == point + 1 {
        keep = point;
    }
    text.drain(keep..end);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn text(format: &str, args: &[PrintfArg]) -> String {
        String::from_utf8(legacy_format(format.as_bytes(), args).unwrap()).unwrap()
    }

    fn d(value: f64) -> PrintfArg {
        PrintfArg::Double(value)
    }

    #[test]
    fn integers_and_literals() {
        assert_eq!(text("75%%", &[]), "75%");
        assert_eq!(
            text("%d%c", &[PrintfArg::Int(-12), PrintfArg::Int(75)]),
            "-12K"
        );
        assert_eq!(
            text("%5d|%-5d|%05d", &[PrintfArg::Int(42); 3]),
            "   42|42   |00042"
        );
        assert_eq!(
            text("%+d % d", &[PrintfArg::Int(1), PrintfArg::Int(1)]),
            "+1  1"
        );
        assert_eq!(
            text("%.0d|%.3d", &[PrintfArg::Int(0), PrintfArg::Int(7)]),
            "|007"
        );
        assert_eq!(
            text(
                "%#x %X %o %u",
                &[
                    PrintfArg::Int(255),
                    PrintfArg::Int(255),
                    PrintfArg::Int(8),
                    PrintfArg::Int(-1)
                ]
            ),
            "0xff FF 10 4294967295"
        );
        assert_eq!(text("%*d", &[PrintfArg::Int(-3), PrintfArg::Int(1)]), "1  ");
    }

    #[test]
    fn legacy_rounding_takes_halves_up() {
        assert_eq!(text("%.0f", &[d(0.5)]), "1");
        assert_eq!(text("%.0f", &[d(2.5)]), "3");
        assert_eq!(text("%.1f", &[d(0.25)]), "0.3");
        assert_eq!(text("%.2f", &[d(0.125)]), "0.13");
        assert_eq!(text("%.1f", &[d(0.05)]), "0.1");
        assert_eq!(text("%.1f", &[d(0.006)]), "0.0");
        assert_eq!(text("%.0f", &[d(0.4)]), "0");
        assert_eq!(text("%.1f", &[d(9.96)]), "10.0");
        assert_eq!(text("%f", &[d(-0.0)]), "-0.000000");
        assert_eq!(text("%f", &[d(1e20)]), "100000000000000000000.000000");
    }

    #[test]
    fn exponents_have_three_digits() {
        assert_eq!(text("%e", &[d(0.0)]), "0.000000e+000");
        assert_eq!(text("%.2E", &[d(12345.0)]), "1.23E+004");
        assert_eq!(text("%g", &[d(1e20)]), "1e+020");
        assert_eq!(text("%g", &[d(0.0001)]), "0.0001");
        assert_eq!(text("%g", &[d(0.00001)]), "1e-005");
        assert_eq!(text("%g", &[d(999_999.5)]), "1e+006");
        assert_eq!(text("%g", &[d(9.999_995)]), "10");
        assert_eq!(text("%#g", &[d(1.0)]), "1.00000");
        assert_eq!(text("%.16g", &[d(0.1)]), "0.1");
        assert_eq!(text("%.17g", &[d(0.1)]), "0.10000000000000001");
        assert_eq!(text("%g", &[d(0.0)]), "0");
        assert_eq!(
            text("%8.3f|%-8.1e|%+08.2f", &[d(3.141_75), d(5.0), d(-1.5)]),
            "   3.142|5.0e+000|-0001.50"
        );
    }

    #[test]
    fn specials_use_the_vc6_texts() {
        let inf = f64::INFINITY;
        let ind = f64::from_bits(0xFFF8_0000_0000_0000);
        let qnan = f64::from_bits(0x7FF8_0000_0000_0000);
        let snan = f64::from_bits(0x7FF0_0000_0000_0001);
        assert_eq!(text("%f", &[d(inf)]), "1.#INF00");
        assert_eq!(text("%.2f", &[d(inf)]), "1.#J");
        assert_eq!(text("%.1f", &[d(inf)]), "1.$");
        assert_eq!(text("%.0f", &[d(-inf)]), "-1");
        assert_eq!(text("%.3f", &[d(ind)]), "-1.#IO");
        assert_eq!(text("%.16g", &[d(inf)]), "1.#INF");
        assert_eq!(text("%.16g", &[d(ind)]), "-1.#IND");
        assert_eq!(text("%.16g", &[d(qnan)]), "1.#QNAN");
        assert_eq!(text("%.16g", &[d(snan)]), "1.#SNAN");
        assert_eq!(text("%e", &[d(inf)]), "1.#INF00e+000");
    }

    #[test]
    fn stores_like_vsnprintf() {
        assert_eq!(
            Printed::store(4, b"abc"),
            Printed {
                result: 3,
                stored: b"abc\0".to_vec()
            }
        );
        assert_eq!(
            Printed::store(3, b"abc"),
            Printed {
                result: 3,
                stored: b"abc".to_vec()
            }
        );
        assert_eq!(
            Printed::store(2, b"abc"),
            Printed {
                result: -1,
                stored: b"ab".to_vec()
            }
        );
    }

    #[test]
    fn rejects_what_has_no_argument_type() {
        assert_eq!(
            legacy_format(b"%s", &[]),
            Err(PrintfError::Unsupported(b's'))
        );
        assert_eq!(
            legacy_format(b"%ld", &[]),
            Err(PrintfError::Unsupported(b'l'))
        );
        assert_eq!(legacy_format(b"%d", &[]), Err(PrintfError::MissingArgument));
        assert_eq!(
            legacy_format(b"%f", &[PrintfArg::Int(1)]),
            Err(PrintfError::ArgumentType)
        );
        assert_eq!(legacy_format(b"abc%", &[]), Err(PrintfError::Truncated));
        assert_eq!(legacy_format(b"a\0%d", &[]), Ok(b"a".to_vec()));
    }

    #[test]
    fn ecvt_rounds_seventeen_digits_half_up() {
        let (digits, point, sign) = ecvt(0.0, 17);
        assert_eq!(
            (digits, point, sign),
            (b"00000000000000000".to_vec(), 0, false)
        );
        assert!(ecvt(-0.0, 17).2);
        // 1125899906842624.25 has 18 digits: the exact half rounds up.
        let (digits, point, _) = ecvt(f64::from_bits(0x4310_0000_0000_0001), 17);
        assert_eq!((digits, point), (b"11258999068426243".to_vec(), 16));
        let (digits, point, _) = ecvt(0.1, 17);
        assert_eq!((digits, point), (b"10000000000000001".to_vec(), 0));
    }
}
