//! Number text as the Lua 3.2 compiler stored it: `sprintf("%.16g")`
//! (`NUMBER_FMT` in `lundump.h`) from the Microsoft C runtime the shipped
//! chunks were made with, whose exponent has at least three digits
//! (`9e-006`, `1e+020`).
//!
//! That runtime rounds twice: it converts the value to 17 significant
//! digits, then rounds those half up to the precision asked for. So
//! 0.99999, whose double is 0.99999000000000004551..., becomes
//! `0.99999000000000005` and then `0.9999900000000001`, where a correctly
//! rounding `printf` gives `0.99999`.

/// Formats `x` as MSVC's `%.16g` does.
pub fn format_g16(x: f64) -> String {
    const PRECISION: i32 = 16;
    if !x.is_finite() {
        // Never stored by the shipped chunks; MSVC would print `1.#INF`.
        return format!("{x}");
    }
    // 17 digits rounded from the exact binary value, then half up to 16.
    let sci = format!("{:.16e}", x.abs());
    let (mantissa, exponent) = sci.split_once('e').expect("exponent form");
    let mut exponent: i32 = exponent.parse().expect("exponent digits");
    let mut digits: Vec<u8> = mantissa.bytes().filter(u8::is_ascii_digit).collect();
    let round_up = digits.pop().is_some_and(|d| d >= b'5');
    if round_up {
        let mut i = digits.len();
        loop {
            if i == 0 {
                digits.insert(0, b'1');
                digits.pop();
                exponent += 1;
                break;
            }
            i -= 1;
            if digits[i] == b'9' {
                digits[i] = b'0';
            } else {
                digits[i] += 1;
                break;
            }
        }
    }
    let digits = String::from_utf8(digits).expect("ASCII digits");
    let sign = if x.is_sign_negative() { "-" } else { "" };

    if (-4..PRECISION).contains(&exponent) {
        let (int, frac) = if exponent >= 0 {
            let split = (exponent + 1) as usize;
            (digits[..split].to_owned(), digits[split..].to_owned())
        } else {
            let zeros = "0".repeat((-exponent - 1) as usize);
            ("0".to_owned(), format!("{zeros}{digits}"))
        };
        let frac = frac.trim_end_matches('0');
        if frac.is_empty() {
            format!("{sign}{int}")
        } else {
            format!("{sign}{int}.{frac}")
        }
    } else {
        let frac = digits[1..].trim_end_matches('0');
        let point = if frac.is_empty() { "" } else { "." };
        let esign = if exponent < 0 { '-' } else { '+' };
        format!(
            "{sign}{}{point}{frac}e{esign}{:03}",
            &digits[..1],
            exponent.abs()
        )
    }
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

/// Reads a Lua 3.2 numeral as the reference does (`luaO_str2d` in
/// `lobject.c`, which `LuaParseStoredNumber` calls for source and stored
/// text alike): the digits accumulate in a double, then one division or
/// multiplication by a power of ten. This is not correctly rounded. The
/// reference runs it in 53-bit x87 mode, which `f64` reproduces for the
/// exponents numerals have (design/decisions.md T11). A leading `-`, as
/// stored constants have, negates the result (`luaU_str2d`).
pub fn parse(text: &str) -> Option<f64> {
    let (negative, body) = match text.strip_prefix('-') {
        Some(body) => (true, body),
        None => (false, text),
    };
    let mut bytes = body.bytes().peekable();
    let mut mantissa = 0.0_f64;
    let mut digits = 0;
    let mut point: u32 = 0;
    while let Some(d) = bytes.next_if(u8::is_ascii_digit) {
        mantissa = 10.0 * mantissa + f64::from(d - b'0');
        digits += 1;
    }
    if bytes.next_if_eq(&b'.').is_some() {
        while let Some(d) = bytes.next_if(u8::is_ascii_digit) {
            mantissa = 10.0 * mantissa + f64::from(d - b'0');
            digits += 1;
            point = point.wrapping_add(1);
        }
    }
    if digits == 0 {
        return None;
    }
    if bytes.next_if(|&c| c == b'e' || c == b'E').is_some() {
        let negative_exponent = bytes.next_if_eq(&b'-').is_some();
        if !negative_exponent {
            bytes.next_if_eq(&b'+');
        }
        let mut exponent: u32 = 0;
        let mut exponent_digits = 0;
        while let Some(d) = bytes.next_if(u8::is_ascii_digit) {
            exponent = exponent.wrapping_mul(10).wrapping_add(u32::from(d - b'0'));
            exponent_digits += 1;
        }
        if exponent_digits == 0 {
            return None;
        }
        // The reference adjusts in wrapping 32-bit arithmetic.
        point = if negative_exponent {
            point.wrapping_add(exponent)
        } else {
            point.wrapping_sub(exponent)
        };
    }
    if bytes.next().is_some() {
        return None;
    }
    let signed_point = point as i32;
    let x = if signed_point > 0 {
        mantissa / expten(point)
    } else if signed_point < 0 {
        mantissa * expten(point.wrapping_neg())
    } else {
        mantissa
    };
    Some(if negative { -x } else { x })
}

/// The numeral to write in source for a number constant stored as
/// `stored`: `stored` itself when it compiles back to that text, which is
/// nearly always. The stored text has 16 digits after double rounding, so
/// reading it can land one unit in the last place away from the value
/// that was printed; then the numeral is the shortest one near that value
/// that compiles to `stored`. `None` if there is none.
pub fn source_numeral(stored: &str) -> Option<String> {
    let x = parse(stored)?;
    if format_g16(x) == stored {
        return Some(stored.to_owned());
    }
    let mut candidates = Vec::new();
    let (mut down, mut up) = (x, x);
    for _ in 0..8 {
        down = down.next_down();
        up = up.next_up();
        for y in [down, up] {
            if format_g16(y) == stored {
                candidates.push(format!("{y}"));
                candidates.push(format!("{y:e}"));
            }
        }
    }
    // Plain decimals like the rest of the scripts, unless they get long.
    candidates.sort_by_key(|c| (c.len() > 24, c.contains('e'), c.len()));
    candidates.into_iter().find(|c| {
        parse(c)
            .is_some_and(|y| format_g16(y) == stored && !(y.abs() <= 65535.0 && y.fract() == 0.0))
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn formats_like_msvc_g16() {
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
            (123456.789, "123456.789"),
            (1234567890123456.0, "1234567890123456"),
            (1e16, "1e+016"),
            (12345678901234567.0, "1.234567890123457e+016"),
            (65535.5, "65535.5"),
            (0.999_999_999_999_999_9, "0.9999999999999999"),
        ];
        for &(x, text) in cases {
            assert_eq!(format_g16(x), text, "{x:e}");
        }
    }

    #[test]
    fn parses_numerals_only() {
        assert_eq!(parse("9e-006"), Some(9e-6));
        assert_eq!(parse("-0.5"), Some(-0.5));
        assert_eq!(parse(".5"), Some(0.5));
        assert_eq!(parse("5."), Some(5.0));
        assert_eq!(parse("1E+3"), Some(1000.0));
        assert_eq!(parse("1e-0"), Some(1.0));
        for bad in [
            "", "-", ".", "e5", "1e", "1e+", "inf", "nan", "1x", "0x10", "+1",
        ] {
            assert_eq!(parse(bad), None, "{bad:?}");
        }
    }

    #[test]
    fn stored_text_survives_a_parse() {
        for text in [
            "-0.9999900000000001",
            "9e-006",
            "0.003906",
            "70.197",
            "1e+020",
        ] {
            assert_eq!(format_g16(parse(text).unwrap()), text);
            assert_eq!(source_numeral(text).as_deref(), Some(text));
        }
    }

    #[test]
    fn reads_like_lua_not_correctly_rounded() {
        // 9999900000000001 exceeds 2^53: accumulating it rounds to even.
        assert_eq!(parse("0.9999900000000001"), Some(0.99999));
        // The stored text of 0.000778 reads back one unit higher.
        let stored = format_g16(0.000778);
        assert_eq!(stored, "0.0007780000000000001");
        assert_eq!(parse(&stored), Some(0.000778_f64.next_up()));
        assert_eq!(source_numeral(&stored).as_deref(), Some("0.000778"));
        assert_eq!(
            source_numeral("-0.0007780000000000001").as_deref(),
            Some("-0.000778")
        );
    }
}
