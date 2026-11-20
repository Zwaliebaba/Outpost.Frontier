//! The Universal C Runtime `atof` (moved from `neuron-client::audio::config`
//! with the scene-script reader, decision O44): what `SplinePath` and the
//! audio configuration parse their numbers with.

/// The Universal C Runtime `atof`: white space, a sign, then `inf`,
/// `infinity`, `nan` (with an optional parenthesised tail), a hex float or
/// a decimal float, correctly rounded; no number gives +0 (the sign is
/// not applied).
#[must_use]
pub fn ucrt_atof(text: &[u8]) -> f64 {
    let byte = |at: usize| text.get(at).copied().unwrap_or(0);
    let mut at = 0;
    while matches!(byte(at), b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r') {
        at += 1;
    }
    let negative = byte(at) == b'-';
    if matches!(byte(at), b'-' | b'+') {
        at += 1;
    }
    let rest = &text[at.min(text.len())..];
    let starts =
        |word: &[u8]| rest.len() >= word.len() && rest[..word.len()].eq_ignore_ascii_case(word);
    let magnitude = if starts(b"inf") {
        Some(f64::INFINITY)
    } else if starts(b"nan") {
        Some(f64::NAN)
    } else if rest.len() > 2 && rest[0] == b'0' && matches!(rest[1], b'x' | b'X') {
        // Without a hex digit the number is the "0" before the x.
        Some(hex_float(&rest[2..]).unwrap_or(0.0))
    } else {
        decimal_float(rest)
    };
    match magnitude {
        Some(m) if negative => -m,
        Some(m) => m,
        None => 0.0,
    }
}

/// The longest decimal float at the start of `text`, correctly rounded;
/// `None` without a digit.
fn decimal_float(text: &[u8]) -> Option<f64> {
    let digits = |from: usize| {
        text[from..]
            .iter()
            .take_while(|b| b.is_ascii_digit())
            .count()
    };
    let int = digits(0);
    let mut end = int;
    let mut frac = 0;
    if text.get(end) == Some(&b'.') {
        frac = digits(end + 1);
        end += 1 + frac;
    }
    if int + frac == 0 {
        return None;
    }
    if matches!(text.get(end), Some(b'e' | b'E')) {
        let mut exp = end + 1;
        if matches!(text.get(exp), Some(b'+' | b'-')) {
            exp += 1;
        }
        let n = digits(exp);
        if n != 0 {
            end = exp + n;
        }
    }
    let mut number = String::with_capacity(end + 1);
    if int == 0 {
        number.push('0');
    }
    number.extend(text[..end].iter().map(|&b| char::from(b)));
    number.parse().ok()
}

/// A hex float after its `0x`: hex digits, an optional point and digits,
/// an optional binary exponent; `None` without a digit (the reference
/// then reads the `0`).
fn hex_float(text: &[u8]) -> Option<f64> {
    let mut at = 0;
    let mut mantissa: u64 = 0;
    let mut exponent: i64 = 0;
    let mut sticky = false;
    let mut any = false;
    let mut point = false;
    loop {
        let b = text.get(at).copied().unwrap_or(0);
        if b == b'.' && !point {
            point = true;
            at += 1;
            continue;
        }
        let Some(d) = char::from(b).to_digit(16) else {
            break;
        };
        any = true;
        if mantissa >> 59 == 0 {
            mantissa = mantissa << 4 | u64::from(d);
            if point {
                exponent -= 4;
            }
        } else {
            sticky |= d != 0;
            if !point {
                exponent += 4;
            }
        }
        at += 1;
    }
    if !any {
        return None;
    }
    if matches!(text.get(at), Some(b'p' | b'P')) {
        let mut e = at + 1;
        let negative = text.get(e) == Some(&b'-');
        if matches!(text.get(e), Some(b'+' | b'-')) {
            e += 1;
        }
        let start = e;
        let mut value: i64 = 0;
        while let Some(d) = text.get(e).filter(|b| b.is_ascii_digit()) {
            value = (value * 10 + i64::from(d - b'0')).min(1 << 20);
            e += 1;
        }
        if e > start {
            exponent += if negative { -value } else { value };
        }
    }
    Some(scale_binary(mantissa, sticky, exponent))
}

/// `(mantissa + sticky) * 2^exponent` rounded to nearest, ties to even.
fn scale_binary(mantissa: u64, sticky: bool, exponent: i64) -> f64 {
    if mantissa == 0 {
        return 0.0;
    }
    let shift = mantissa.leading_zeros();
    let m = mantissa << shift;
    // The value lies in [2^top, 2^(top + 1)).
    let mut top = exponent + 63 - i64::from(shift);
    if top > 1023 {
        return f64::INFINITY;
    }
    // Significant bits kept: 53 for a normal result, fewer for a subnormal.
    let keep = if top >= -1022 { 53 } else { 53 - (-1022 - top) };
    if keep <= 0 {
        // At most half the smallest subnormal: exactly half is a tie to 0.
        let above_half = top == -1075 && (m << 1 != 0 || sticky);
        return if above_half { f64::from_bits(1) } else { 0.0 };
    }
    let drop = 64 - keep as u32;
    let mut kept = m >> drop;
    let rest = m << (64 - drop);
    let half = 1u64 << 63;
    if rest > half || (rest == half && (sticky || kept & 1 == 1)) {
        kept += 1;
    }
    if top < -1022 {
        // A subnormal is kept * 2^-1074; a carry into bit 52 is the
        // smallest normal, which the same bits spell.
        return f64::from_bits(kept);
    }
    if kept == 1 << 53 {
        kept >>= 1;
        top += 1;
        if top > 1023 {
            return f64::INFINITY;
        }
    }
    f64::from_bits(((top + 1023) as u64) << 52 | (kept & ((1 << 52) - 1)))
}
