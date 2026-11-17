//! `TextureFormatMaps`: the rules that pick the format a texture is
//! created in from its source format (`FormatMapAppend`,
//! `FormatMapLoadDefaults`, `FormatMapParseLine`, `FormatMapRowMatches`
//! and `PickGpuFormat` in `TextureLibrary.cpp`).
//!
//! A row holds six optional fields (bits per pixel, red, green, blue and
//! alpha widths, and an alpha test) and a FourCC naming the result, one of
//! the format classes 15 to 21. A field that is set must equal the
//! source's value; an unset one (`*` in the INI) matches anything. The
//! first matching row wins.

use super::format::{PixelFormat, fourcc};
use crate::msvcrt::atoi;

/// One rule: `value[i]` must equal `mask[i] & field`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FormatMapRow {
    /// The field values (0 where unset).
    pub value: [u32; 6],
    /// `0xFFFFFFFF` where set, 0 where unset.
    pub mask: [u32; 6],
    /// The FourCC of the resulting format class.
    pub fourcc: u32,
}

impl FormatMapRow {
    /// `FormatMapAppend`: a field below 0 is unset.
    #[must_use]
    pub fn new(fields: [i32; 6], fourcc: u32) -> Self {
        let mut row = Self {
            value: [0; 6],
            mask: [0; 6],
            fourcc,
        };
        for (i, &field) in fields.iter().enumerate() {
            if field >= 0 {
                row.value[i] = field.cast_unsigned();
                row.mask[i] = 0xFFFF_FFFF;
            }
        }
        row
    }

    /// `FormatMapRowMatches`. For [`PixelFormat::P8`] the first four
    /// fields are tested against the constant 8 and the alpha width field
    /// must be 0. The sixth field is tested against the alpha width, as the
    /// fifth is.
    #[must_use]
    pub fn matches(&self, source: PixelFormat) -> bool {
        let fields = source.match_fields();
        if source == PixelFormat::P8 {
            return (0..4).all(|i| self.mask[i] & 8 == self.value[i])
                && self.value[4] == 0
                && self.mask[5] & fields[4] == self.value[5];
        }
        (0..5).all(|i| self.mask[i] & fields[i] == self.value[i])
            && self.mask[5] & fields[4] == self.value[5]
    }
}

/// The rows in order.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FormatMaps {
    /// The rules, first match wins.
    pub rows: Vec<FormatMapRow>,
}

impl Default for FormatMaps {
    fn default() -> Self {
        Self::defaults()
    }
}

/// Bytes past a line buffer read as 0.
fn at(buffer: &[u8], index: usize) -> u8 {
    buffer.get(index).copied().unwrap_or(0)
}

fn set(buffer: &mut [u8], index: usize, byte: u8) {
    if let Some(slot) = buffer.get_mut(index) {
        *slot = byte;
    }
}

fn skip(buffer: &[u8], cursor: &mut usize, characters: &[u8]) {
    while at(buffer, *cursor) != 0 && characters.contains(&at(buffer, *cursor)) {
        *cursor += 1;
    }
}

/// The C string at `start`, ending at a NUL or the end of the buffer.
fn c_str_at(buffer: &[u8], start: usize) -> &[u8] {
    let rest = buffer.get(start..).unwrap_or_default();
    &rest[..rest.iter().position(|&b| b == 0).unwrap_or(rest.len())]
}

/// `ParseFormatMapField`: the token starts before the skipped separators;
/// the byte after the digits (and `*`) becomes NUL and, with `advance`,
/// is stepped over, even when it was the line's terminator.
fn parse_field(buffer: &mut [u8], cursor: &mut usize, separators: &[u8], advance: bool) -> i32 {
    let start = *cursor;
    skip(buffer, cursor, separators);
    skip(buffer, cursor, b"1234567890*");
    set(buffer, *cursor, 0);
    if advance {
        *cursor += 1;
    }
    let token = c_str_at(buffer, start);
    if token.contains(&b'*') {
        -1
    } else {
        atoi(token)
    }
}

impl FormatMaps {
    /// No rows.
    #[must_use]
    pub fn empty() -> Self {
        Self { rows: Vec::new() }
    }

    /// `FormatMapLoadDefaults`: the eight built-in rows.
    #[must_use]
    pub fn defaults() -> Self {
        let rows = vec![
            FormatMapRow::new([8, 8, 8, 8, -1, 0], fourcc(*b"DAOP")),
            FormatMapRow::new([-1, -1, -1, -1, 1, 0], fourcc(*b"DAA1")),
            FormatMapRow::new([-1, -1, -1, -1, 4, 0], fourcc(*b"DAA4")),
            FormatMapRow::new([-1, -1, -1, -1, 8, 0], fourcc(*b"DAA8")),
            FormatMapRow::new([-1, -1, -1, -1, -1, 0], fourcc(*b"DAOT")),
            FormatMapRow::new([-1, -1, -1, -1, -1, 1], fourcc(*b"DAA1")),
            FormatMapRow::new([-1, -1, -1, -1, -1, 4], fourcc(*b"DAA4")),
            FormatMapRow::new([-1, -1, -1, -1, -1, -1], fourcc(*b"DAA8")),
        ];
        Self { rows }
    }

    /// `FormatMapParseLine` on a line buffer that holds a C string.
    ///
    /// A `;` ends the line. A key starting with `clear` (three characters
    /// compared, case-insensitive) removes every row. Otherwise the text
    /// before the first `=` holds up to six fields separated by `:`, space
    /// or tab, and the first four bytes after the `=` and its blanks are
    /// the FourCC; a line without a value adds nothing. The reference
    /// writes NUL bytes into the buffer and can step past the line's end
    /// into the rest of the buffer; `buffer` is modified the same way, and
    /// bytes past its end read as 0.
    pub fn parse_line(&mut self, buffer: &mut [u8]) {
        let length = c_str_at(buffer, 0).len();
        if let Some(semi) = buffer[..length].iter().position(|&b| b == b';') {
            buffer[semi] = 0;
        }
        let mut key = 0;
        skip(buffer, &mut key, b" \t");
        let prefix = [at(buffer, key), at(buffer, key + 1), at(buffer, key + 2)];
        if prefix.eq_ignore_ascii_case(b"cle") {
            self.rows.clear();
            return;
        }
        let mut value = key;
        while at(buffer, value) != 0 && at(buffer, value) != b'=' {
            value += 1;
        }
        set(buffer, value, 0);
        value += 1;
        skip(buffer, &mut value, b" \t");
        if at(buffer, value) == 0 {
            return;
        }
        let mut field = key;
        let f0 = parse_field(buffer, &mut field, b" \t", true);
        let f1 = parse_field(buffer, &mut field, b" \t:", true);
        let f2 = parse_field(buffer, &mut field, b" \t:", true);
        let f3 = parse_field(buffer, &mut field, b" \t:", true);
        let f4 = parse_field(buffer, &mut field, b" \t:", true);
        let f5 = parse_field(buffer, &mut field, b" \t:", false);
        let code = u32::from_le_bytes([
            at(buffer, value),
            at(buffer, value + 1),
            at(buffer, value + 2),
            at(buffer, value + 3),
        ]);
        self.rows
            .push(FormatMapRow::new([f0, f1, f2, f3, f4, f5], code));
    }

    /// `PickGpuFormat`: DXT1 to DXT5 stay as they are; otherwise the
    /// format class of the first matching row (through
    /// [`PixelFormat::from_fourcc`]). `None` on a miss, where the reference
    /// leaves its output unchanged.
    #[must_use]
    pub fn pick(&self, source: PixelFormat) -> Option<PixelFormat> {
        if source.is_dxt() {
            return Some(source);
        }
        self.rows
            .iter()
            .find(|row| row.matches(source))
            .map(|row| PixelFormat::from_fourcc(row.fourcc))
    }
}
