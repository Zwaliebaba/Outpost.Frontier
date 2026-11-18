//! Lua 3.2 number text, as the reference's copy of Lua reads it
//! (`CppSource/SceneScript/lua32/src/lobject.c`, `lundump.c`, `lvm.c`).
//!
//! The reference does not read numerals with `strtod`: `luaO_str2d`
//! accumulates the digits in a double and applies the decimal exponent with
//! one division or multiplication by a power of ten (`Expten`). The result
//! is not correctly rounded. The reference runs it in 53-bit x87 mode, which
//! `f64` arithmetic reproduces (design/decisions.md T11).

/// `isspace` in the C locale.
fn is_space(b: u8) -> bool {
    matches!(b, b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r')
}

/// `Expten` of `lobject.c`: 10 to the power `n` by repeated squaring.
fn expten(mut n: u32) -> f64 {
    let mut base = 10.0_f64;
    let mut result = 1.0_f64;
    while n != 0 {
        if n & 1 != 0 {
            result *= base;
        }
        base *= base;
        n >>= 1;
    }
    result
}

/// `luaO_str2d`: the value of an unsigned numeral, or -1.0 when the text is
/// not one. Trailing white space is allowed; a numeral without digits (such
/// as `.`) reads as 0, as in the reference.
pub fn str2d(text: &[u8]) -> f64 {
    let mut at = 0;
    let peek = |at: usize| text.get(at).copied().unwrap_or(0);
    let mut mantissa = 0.0_f64;
    let mut point: u32 = 0;
    while peek(at).is_ascii_digit() {
        mantissa = 10.0 * mantissa + f64::from(peek(at) - b'0');
        at += 1;
    }
    if peek(at) == b'.' {
        at += 1;
        while peek(at).is_ascii_digit() {
            mantissa = 10.0 * mantissa + f64::from(peek(at) - b'0');
            at += 1;
            point = point.wrapping_add(1);
        }
    }
    if peek(at).eq_ignore_ascii_case(&b'e') {
        at += 1;
        let mut negative_exponent = false;
        if peek(at) == b'-' {
            at += 1;
            negative_exponent = true;
        } else if peek(at) == b'+' {
            at += 1;
        }
        if !peek(at).is_ascii_digit() {
            return -1.0;
        }
        let mut exponent: u32 = 0;
        while peek(at).is_ascii_digit() {
            exponent = exponent
                .wrapping_mul(10)
                .wrapping_add(u32::from(peek(at) - b'0'));
            at += 1;
        }
        // The reference adjusts in wrapping 32-bit registers.
        point = point.wrapping_sub(if negative_exponent {
            exponent.wrapping_neg()
        } else {
            exponent
        });
    }
    while is_space(peek(at)) {
        at += 1;
    }
    // A NUL ends the C string; text after it is never seen.
    if at < text.len() && text[at] != 0 {
        return -1.0;
    }
    let signed_point = point.cast_signed();
    if signed_point > 0 {
        mantissa / expten(point)
    } else if signed_point < 0 {
        mantissa * expten(point.wrapping_neg())
    } else {
        mantissa
    }
}

/// `luaU_str2d`: a number constant of a compiled chunk, stored as text with
/// an optional leading `-`. `None` where the reference raises "cannot
/// convert number".
pub fn undump(text: &[u8]) -> Option<f64> {
    let (negative, body) = match text.split_first() {
        Some((b'-', body)) => (true, body),
        _ => (false, text),
    };
    // `LuaParseStoredNumber` accepts what is not below zero.
    let x = str2d(body);
    // `Parsed >= 0.0`: NaN (such as 0e400, 0 times infinity) fails too.
    if x.is_nan() || x < 0.0 {
        return None;
    }
    Some(if negative { -x } else { x })
}

/// `luaV_tonumber` on a string: the number arithmetic converts it to, or
/// `None` where the conversion fails.
///
/// The reference tests the byte after the sign and the one after it for a
/// digit; where the text ends at the sign that second test reads past the
/// string. Here a byte past the end is not a digit (divergence D201).
pub fn to_number(text: &[u8]) -> Option<f64> {
    let byte = |at: usize| text.get(at).copied().unwrap_or(0);
    let mut at = 0;
    while is_space(byte(at)) {
        at += 1;
    }
    let mut sign = 1.0_f64;
    if byte(at) == b'-' {
        at += 1;
        sign = -1.0;
    } else if byte(at) == b'+' {
        at += 1;
    }
    let second = if at < text.len() { byte(at + 1) } else { 0 };
    if !byte(at).is_ascii_digit() && !second.is_ascii_digit() {
        return None;
    }
    // `LuaParseSignedNumberObject`.
    let x = str2d(&text[at.min(text.len())..]);
    // `Parsed >= 0.0`: NaN (such as 0e400, 0 times infinity) fails too.
    if x.is_nan() || x < 0.0 {
        return None;
    }
    Some(x * sign)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_like_lua_not_correctly_rounded() {
        assert_eq!(str2d(b"1.5"), 1.5);
        assert_eq!(str2d(b"9e-006"), 9.0 / 1e6);
        assert_eq!(str2d(b"1e+020"), 1e20);
        assert_eq!(str2d(b"12 \t"), 12.0);
        assert_eq!(str2d(b"."), 0.0);
        assert_eq!(str2d(b""), 0.0);
        assert_eq!(str2d(b"1e"), -1.0);
        assert_eq!(str2d(b"1x"), -1.0);
        assert_eq!(str2d(b"1\0x"), 1.0);
        // 0.1 accumulated then divided: 1 / 10.
        assert_eq!(str2d(b"0.1").to_bits(), (1.0_f64 / 10.0).to_bits());
        // 17 digits accumulate past 2^53 and round on the way.
        let x = str2d(b"0.9999900000000001");
        assert_eq!(x.to_bits(), (9_999_900_000_000_001.0_f64 / 1e16).to_bits());
    }

    #[test]
    fn undumps_signed_constants() {
        assert_eq!(undump(b"-2.5"), Some(-2.5));
        assert_eq!(undump(b"3"), Some(3.0));
        assert_eq!(undump(b"--3"), None);
        assert_eq!(undump(b"abc"), None);
        assert_eq!(undump(b"-"), Some(-0.0));
    }

    #[test]
    fn converts_strings_for_arithmetic() {
        assert_eq!(to_number(b"  -12.5"), Some(-12.5));
        assert_eq!(to_number(b"+4"), Some(4.0));
        assert_eq!(to_number(b".5"), Some(0.5));
        assert_eq!(to_number(b"x"), None);
        assert_eq!(to_number(b"-"), None);
        assert_eq!(to_number(b""), None);
        assert_eq!(to_number(b"1e"), None);
    }
}
