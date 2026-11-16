//! Number and field parsing for the game INI reader (work package P1.2).
//!
//! Ported from `StringIndex`, `ParseNumber`, `ValueNum` and
//! `INI_Reader::is_number` in `CppSource/Common/common_ini.cpp`. Inputs are
//! C string bytes.

use crate::hash::c_str;
use crate::msvcrt::atof;

/// The character-class table (128 bytes) followed by the digit-value table
/// (128 bytes). Class bits: 0x08 decimal digit, 0x10 hex digit, 0x40 white
/// space. The class table is indexed with `c & 0x7F`, the digit table with
/// the signed char, so bytes 0x80 and above read the class table.
const PARSE_TABLES: [u8; 256] = [
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x40, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x80, 0x00, 0x20, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x80, 0x00, 0x80, 0x00, 0x80,
    0x38, 0x38, 0x38, 0x38, 0x38, 0x38, 0x38, 0x38, 0x38, 0x38, 0x00, 0x00, 0x80, 0x80, 0x80, 0x00,
    0x00, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x23, 0x23, 0x23, 0x23, 0x23, 0x23, 0x23, 0x23, 0x23,
    0x23, 0x23, 0x23, 0x23, 0x23, 0x23, 0x23, 0x23, 0x23, 0x23, 0x23, 0x00, 0x00, 0x00, 0x80, 0x20,
    0x00, 0x35, 0x35, 0x35, 0x35, 0x35, 0x35, 0x25, 0x25, 0x25, 0x25, 0x25, 0x25, 0x25, 0x25, 0x25,
    0x25, 0x25, 0x25, 0x25, 0x25, 0x25, 0x25, 0x25, 0x25, 0x25, 0x25, 0x00, 0x80, 0x00, 0x80, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
];

const CLASS_SPACE: u8 = 0x40;
const CLASS_DIGIT: u8 = 0x08;
const CLASS_HEX: u8 = 0x10;

/// `StringIndex` copies at most this many bytes of a field.
const FIELD_MAX_BYTES: usize = 0x3FF;

fn has_class(byte: u8, bits: u8) -> bool {
    PARSE_TABLES[usize::from(byte & 0x7F)] & bits != 0
}

fn digit_value(byte: u8) -> u8 {
    PARSE_TABLES[usize::from(byte ^ 0x80)]
}

/// `StringIndex`: field `index` of a comma-separated list, with white
/// space trimmed at both ends. A missing field is empty.
#[must_use]
pub fn string_index(text: &[u8], index: u32) -> &[u8] {
    let text = c_str(text);
    let mut cursor = 0;
    let mut remain = index;
    loop {
        while cursor < text.len() && has_class(text[cursor], CLASS_SPACE) {
            cursor += 1;
        }
        let comma = text[cursor..]
            .iter()
            .position(|&b| b == b',')
            .map(|p| cursor + p);
        if remain != 0 {
            match comma {
                None => return &[],
                Some(at) => {
                    cursor = at + 1;
                    remain -= 1;
                    continue;
                }
            }
        }
        let mut end = comma.unwrap_or(text.len());
        while end > cursor && has_class(text[end - 1], CLASS_SPACE) {
            end -= 1;
        }
        let copy = (end - cursor).min(FIELD_MAX_BYTES);
        return &text[cursor..cursor + copy];
    }
}

/// A [`parse_number`] result.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ParsedNumber {
    /// The value.
    pub value: f64,
    /// The C++ logged "illegal BASE-N digit(s)" and stopped at that digit.
    pub illegal_digit: bool,
}

fn lower(byte: u8) -> u8 {
    byte.to_ascii_lowercase()
}

fn parse_base_n(span: &[u8], base: u8) -> ParsedNumber {
    let base_f = f64::from(base);
    let mut value = 0.0f64;
    let mut frac = 0.0f64;
    for &byte in span {
        if byte == b'.' {
            frac = base_f;
            continue;
        }
        let digit = digit_value(byte);
        if digit >= base {
            return ParsedNumber {
                value,
                illegal_digit: true,
            };
        }
        if frac > 0.0 {
            value += f64::from(digit) / frac;
            frac *= base_f;
        } else {
            value = value * base_f + f64::from(digit);
        }
    }
    ParsedNumber {
        value,
        illegal_digit: false,
    }
}

/// `ParseNumber`: decimal through the legacy `atof`, `0x` hex, and the
/// `d` (decimal), `b` (binary) and single-digit `h` (hex) suffixes. A
/// leading `'` gives the code of the next character.
#[must_use]
pub fn parse_number(text: &[u8]) -> ParsedNumber {
    let text = c_str(text);
    let at = |index: usize| text.get(index).copied().unwrap_or(0);
    let mut negative = false;
    let mut base = 10u8;
    let mut start = 0usize;
    if at(start) == b'+' {
        start += 1;
    }
    if at(start) == b'-' {
        negative = true;
        start += 1;
    }
    if at(start) == b'0' {
        while at(start + 1) == b'0' {
            start += 1;
        }
        start += 1;
        if lower(at(start)) == b'x' {
            start += 1;
            base = 16;
        }
    }

    let mut end = start;
    let result = if base == 16 {
        while has_class(at(end), CLASS_HEX) {
            end += 1;
        }
        if at(end) == b'.' {
            end += 1;
        }
        while has_class(at(end), CLASS_HEX) {
            end += 1;
        }
        parse_base_n(&text[start..end], base)
    } else if at(start) == b'\'' {
        // The character code, signed, and without the sign seen before.
        return ParsedNumber {
            value: f64::from(i8::from_ne_bytes([at(start + 1)])),
            illegal_digit: false,
        };
    } else if at(start) == 0 {
        decimal(&text[start..])
    } else {
        while has_class(at(end), CLASS_HEX) {
            end += 1;
        }
        let run = end - start;
        if run > 1 {
            match lower(at(end - 1)) {
                b'd' => {
                    end -= 1;
                    base = 10;
                }
                b'b' => {
                    end -= 1;
                    base = 2;
                }
                _ => {}
            }
        } else if run == 1 && at(end) == b'h' {
            base = 16;
        }
        if at(end) != 0 && at(end) == b'.' {
            end += 1;
            while has_class(at(end), CLASS_DIGIT) {
                end += 1;
            }
        }
        if base == 10 {
            decimal(&text[start..])
        } else {
            parse_base_n(&text[start..end], base)
        }
    };
    if negative {
        ParsedNumber {
            value: -result.value,
            ..result
        }
    } else {
        result
    }
}

fn decimal(text: &[u8]) -> ParsedNumber {
    ParsedNumber {
        value: atof(text),
        illegal_digit: false,
    }
}

/// `ValueNum`: field `index` through the legacy `atof`. Unlike
/// [`parse_number`] it knows no hex or suffixes.
#[must_use]
pub fn value_num(text: &[u8], index: u32) -> f64 {
    atof(string_index(text, index))
}

/// `INI_Reader::is_number`.
#[must_use]
pub fn is_number(candidate: &[u8]) -> bool {
    let text = c_str(candidate);
    let at = |index: usize| text.get(index).copied().unwrap_or(0);
    let mut cursor = 0usize;
    let mut saw_exp = false;
    let mut saw_dot = false;
    let mut sign_locked = true;
    if matches!(at(cursor), b'-' | b'+') {
        cursor += 1;
    }
    if at(cursor) == b'.' {
        cursor += 1;
        saw_dot = true;
    }
    if at(cursor) == b'\'' {
        cursor += 1;
        if lower(at(cursor)).is_ascii_lowercase() && at(cursor + 1) == b'\'' {
            return true;
        }
    }
    if at(cursor) == b'0' && lower(at(cursor + 1)) == b'x' {
        return text[cursor + 2..]
            .iter()
            .all(|&b| b.is_ascii_digit() || (b'a'..=b'f').contains(&lower(b)));
    }
    if !at(cursor).is_ascii_digit() {
        return false;
    }
    for &byte in &text[cursor + 1..] {
        if byte.is_ascii_digit() {
            continue;
        }
        if !sign_locked && matches!(byte, b'+' | b'-') {
            sign_locked = true;
            continue;
        }
        if !saw_dot && byte == b'.' {
            saw_dot = true;
            continue;
        }
        if saw_exp || lower(byte) != b'e' {
            return false;
        }
        saw_exp = true;
        sign_locked = false;
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    fn value(text: &[u8]) -> f64 {
        parse_number(text).value
    }

    #[test]
    fn fields_are_trimmed_and_counted_by_commas() {
        let text = b"  a , b b\t,, \r";
        assert_eq!(string_index(text, 0), b"a");
        assert_eq!(string_index(text, 1), b"b b");
        assert_eq!(string_index(text, 2), b"");
        assert_eq!(string_index(text, 3), b"");
        assert_eq!(string_index(text, 4), b"");
        // 0x89 and 0xA0 mask to tab and space.
        assert_eq!(string_index(b"\x89x\xA0", 0), b"x");
    }

    #[test]
    fn number_forms() {
        assert_eq!(value(b"12"), 12.0);
        assert_eq!(value(b"-1.5"), -1.5);
        assert_eq!(value(b"0x1F"), 31.0);
        assert_eq!(value(b"0x1.8"), 1.5);
        assert_eq!(value(b"101b"), 5.0);
        assert_eq!(value(b"12d"), 12.0);
        assert_eq!(value(b"Ah"), 10.0);
        assert_eq!(value(b"1e3"), 1000.0);
        assert_eq!(value(b"ff"), 0.0);
        assert_eq!(value(b"-'A"), 65.0);
        assert_eq!(value(b"000"), 0.0);
        assert!(parse_number(b"0x").value == 0.0);
        let illegal = parse_number(b"1021b");
        assert!(illegal.illegal_digit);
        assert_eq!(illegal.value, 2.0);
    }

    #[test]
    fn value_num_is_plain_atof() {
        assert_eq!(value_num(b"1, 0x10", 1), 0.0);
        assert_eq!(value_num(b"1, 2.5", 1), 2.5);
    }

    #[test]
    fn number_detection() {
        for yes in [&b"1"[..], b"-1.5e+3", b"0x1f", b"'a'", b".5", b"1e5"] {
            assert!(is_number(yes), "{yes:?}");
        }
        for no in [&b""[..], b"a", b"1e5e", b"0xg", b"1..2", b"1+2"] {
            assert!(!is_number(no), "{no:?}");
        }
    }
}
