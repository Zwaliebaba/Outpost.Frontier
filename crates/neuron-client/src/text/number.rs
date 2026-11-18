//! The text of a number string (`NumberStringChild` in
//! `CppSource/OutpostFrontier/Client/font_string.cpp`, work packages P4.12
//! and P4.13): what its `printf` leaves for the glyph map, which draws each
//! byte from the font's number face (`-.0123456789MK%/`).
//!
//! The HUD hands it text that is already formatted (`SetNumberText`), so
//! the text is a format without arguments: `75%%` shows `75%`.
//! `FormatFloat` and `FormatInt` format a value first. All three go
//! through `MsvcrtNumericVsnprintf` into a 32-byte buffer
//! ([`neuron_core::msvcrt::numeric_vsnprintf`]).

use neuron_core::msvcrt::{PrintfArg, PrintfError, numeric_vsnprintf};

/// The buffer `Printf` formats into.
const BUFFER_SIZE: usize = 0x20;

/// The `_snprintf` count `FormatFloat` builds its format with.
const FORMAT_COUNT: usize = 10;

/// `NumberStringChild::Printf`: the bytes the glyphs are built from, the
/// output up to its first NUL and at most 32 bytes.
///
/// When the output is longer than the buffer, the reference stores 32
/// bytes without a terminator and reads them up to the NUL its port adds
/// after the buffer, which is what this returns. Where the fixed path
/// stores nothing (a text past its 512-byte work buffer) the reference
/// reads an uninitialised buffer; this returns no bytes (divergence D231).
///
/// # Errors
///
/// A format the legacy `printf` does not run, or arguments that do not
/// match it.
pub fn printf(format: &[u8], args: &[PrintfArg]) -> Result<Vec<u8>, PrintfError> {
    let printed = numeric_vsnprintf(BUFFER_SIZE, format, args)?;
    Ok(printed.stored.into_iter().take_while(|&b| b != 0).collect())
}

/// `NumberStringChild::FormatFloat`: `value` with `decimals` decimals and
/// the suffix byte (none for 0), through the format `%.<decimals>f<suffix>`
/// and [`printf`].
///
/// The reference builds the format with `_snprintf` into a count of 10;
/// a format longer than that (decimals of eight digits or more) is cut to
/// 10 bytes there and read on past them, and is cut to 10 bytes here
/// (divergence D231).
///
/// # Errors
///
/// As [`printf`].
pub fn format_float(value: f32, decimals: i32, suffix: u8) -> Result<Vec<u8>, PrintfError> {
    let mut format = format!("%.{decimals}f").into_bytes();
    format.push(suffix);
    format.truncate(FORMAT_COUNT);
    printf(&format, &[PrintfArg::Double(f64::from(value))])
}

/// `NumberStringChild::FormatInt`: `%d%c` with the value and the suffix
/// byte, sign-extended as the reference passes it.
///
/// # Errors
///
/// As [`printf`].
pub fn format_int(value: i32, suffix: u8) -> Result<Vec<u8>, PrintfError> {
    let suffix = i32::from(i8::from_ne_bytes([suffix]));
    printf(b"%d%c", &[PrintfArg::Int(value), PrintfArg::Int(suffix)])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hud_texts_print_as_themselves() {
        assert_eq!(printf(b"1234", &[]).unwrap(), b"1234");
        assert_eq!(printf(b"75%%", &[]).unwrap(), b"75%");
        assert_eq!(printf(b"12 M", &[]).unwrap(), b"12 M");
        assert_eq!(printf(b"3.4K", &[]).unwrap(), b"3.4K");
    }

    #[test]
    fn long_text_keeps_thirty_two_bytes() {
        let text = [b'7'; 40];
        assert_eq!(printf(&text, &[]).unwrap(), [b'7'; 32]);
    }

    #[test]
    fn values_with_suffixes() {
        assert_eq!(format_float(12.345, 1, b'K').unwrap(), b"12.3K");
        // 0.105f is 0.10499999761581420898...
        assert_eq!(format_float(0.105, 2, 0).unwrap(), b"0.10");
        assert_eq!(format_float(50.0, 0, b'%').unwrap(), b"50");
        assert_eq!(format_int(-7, b'M').unwrap(), b"-7M");
        assert_eq!(format_int(7, 0).unwrap(), b"7");
        // A format cut to ten bytes: `%.12345678` has no conversion type.
        assert!(format_float(1.0, 12_345_678, b'K').is_err());
    }
}
