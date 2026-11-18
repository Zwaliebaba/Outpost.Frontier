//! GDI's font mapping arithmetic, for backends that read a face's tables
//! (the DirectWrite backend, `platform::dwrite`).
//!
//! GDI picks the em size of a TrueType face from `lfHeight`: a positive
//! height is the cell height, `usWinAscent + usWinDescent` scaled; a
//! negative one is the em height. The em size is a whole number of pixels,
//! and the text metrics are the font's design values scaled to it and
//! rounded. Windows does not document this; the arithmetic here is Wine's
//! reimplementation of it (`dlls/win32u/freetype.c`, FreeType's 16.16
//! fixed point), which matches Windows for faces without a `VDMX` table.
//! The DirectWrite backend applies it to the em size and to the
//! GDI-compatible advances DirectWrite gives (divergence D290).

use super::TextMetrics;

/// The design values of a face that GDI's mapping reads: from `head`,
/// `hhea` and `OS/2`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FaceTables {
    pub units_per_em: u16,
    pub x_min: i16,
    pub x_max: i16,
    pub hhea_ascender: i16,
    pub hhea_descender: i16,
    pub hhea_line_gap: i16,
    pub advance_width_max: u16,
    pub win_ascent: u16,
    pub win_descent: u16,
    pub x_avg_char_width: i16,
}

fn be_u16(bytes: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_be_bytes([*bytes.get(at)?, *bytes.get(at + 1)?]))
}

fn be_i16(bytes: &[u8], at: usize) -> Option<i16> {
    be_u16(bytes, at).map(u16::cast_signed)
}

impl FaceTables {
    /// The values from the raw `head`, `hhea` and `OS/2` tables; `None`
    /// when a table is too short or the em is 0. An `OS/2` table without
    /// the Windows metrics (version 0, under 78 bytes) counts as missing,
    /// and the `hhea` values stand in for them.
    #[must_use]
    pub fn parse(head: &[u8], hhea: &[u8], os2: Option<&[u8]>) -> Option<FaceTables> {
        let units_per_em = be_u16(head, 18)?;
        if units_per_em == 0 {
            return None;
        }
        let hhea_ascender = be_i16(hhea, 4)?;
        let hhea_descender = be_i16(hhea, 6)?;
        let mut tables = FaceTables {
            units_per_em,
            x_min: be_i16(head, 36)?,
            x_max: be_i16(head, 40)?,
            hhea_ascender,
            hhea_descender,
            hhea_line_gap: be_i16(hhea, 8)?,
            advance_width_max: be_u16(hhea, 10)?,
            win_ascent: hhea_ascender.max(0).cast_unsigned(),
            win_descent: hhea_descender.saturating_neg().max(0).cast_unsigned(),
            x_avg_char_width: 0,
        };
        if let Some(os2) = os2.filter(|t| t.len() >= 78) {
            tables.x_avg_char_width = be_i16(os2, 2)?;
            tables.win_ascent = be_u16(os2, 74)?;
            tables.win_descent = be_u16(os2, 76)?;
        }
        Some(tables)
    }

    /// The whole-pixel em size GDI realises `lf_height` at: `MulDiv(em,
    /// height, winAscent + winDescent)` for a positive height (the `hhea`
    /// extent when those are 0), the height itself for a negative one, as
    /// for 16 for 0; at least 1.
    #[must_use]
    pub fn ppem_for_height(&self, lf_height: i32) -> u32 {
        let height = if lf_height == 0 { 16 } else { lf_height };
        let ppem = if height > 0 {
            let win = i32::from(self.win_ascent) + i32::from(self.win_descent);
            let extent = if win == 0 {
                i32::from(self.hhea_ascender) - i32::from(self.hhea_descender)
            } else {
                win
            };
            mul_div(i32::from(self.units_per_em), height, extent)
        } else {
            height.saturating_neg()
        };
        ppem.max(1).cast_unsigned()
    }

    /// FreeType's scale for `ppem`: 26.6 pixels per design unit in 16.16.
    #[must_use]
    pub fn scale(&self, ppem: u32) -> i64 {
        ft_div_fix(i64::from(ppem) << 6, i64::from(self.units_per_em))
    }

    /// A design length in whole pixels at `ppem`, rounded as GDI rounds
    /// metrics: `(FT_MulFix(value, scale) + 32) >> 6`.
    #[must_use]
    pub fn pixels(&self, ppem: u32, design: i32) -> i32 {
        let v = (ft_mul_fix(i64::from(design), self.scale(ppem)) + 32) >> 6;
        i32::try_from(v).unwrap_or(i32::MAX)
    }

    /// The `TEXTMETRIC` of the face at `ppem`.
    #[must_use]
    pub fn text_metrics(&self, ppem: u32) -> TextMetrics {
        let ascent = self.pixels(ppem, i32::from(self.win_ascent));
        let descent = self.pixels(ppem, i32::from(self.win_descent));
        let height = ascent + descent;
        let hhea_extent = i32::from(self.hhea_ascender) - i32::from(self.hhea_descender);
        let win_extent = i32::from(self.win_ascent) + i32::from(self.win_descent);
        let external = self.pixels(
            ppem,
            i32::from(self.hhea_line_gap) - (win_extent - hhea_extent),
        );
        TextMetrics {
            height,
            ascent,
            descent,
            internal_leading: height - ppem.cast_signed(),
            external_leading: external.max(0),
            ave_char_width: self.pixels(ppem, i32::from(self.x_avg_char_width)),
            max_char_width: self.pixels(ppem, i32::from(self.x_max) - i32::from(self.x_min)),
        }
    }
}

/// `MulDiv`: `a * b / c` in 64 bits, rounded half away from zero; -1 for
/// a zero divisor.
#[must_use]
pub fn mul_div(a: i32, b: i32, c: i32) -> i32 {
    if c == 0 {
        return -1;
    }
    let product = i64::from(a) * i64::from(b);
    let c = i64::from(c);
    let half = c.abs() / 2;
    let q = if (product < 0) == (c < 0) {
        (product.abs() + half) / c.abs()
    } else {
        -((product.abs() + half) / c.abs())
    };
    i32::try_from(q).unwrap_or(-1)
}

/// `FT_DivFix`: `(a << 16) / b`, rounded, sign kept.
fn ft_div_fix(a: i64, b: i64) -> i64 {
    if b == 0 {
        return 0x7FFF_FFFF;
    }
    let negative = (a < 0) != (b < 0);
    let q = ((a.abs() << 16) + b.abs() / 2) / b.abs();
    if negative { -q } else { q }
}

/// `FT_MulFix`: `(a * b) >> 16`, rounded, sign kept.
fn ft_mul_fix(a: i64, b: i64) -> i64 {
    let negative = (a < 0) != (b < 0);
    let q = (a.abs() * b.abs() + 0x8000) >> 16;
    if negative { -q } else { q }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Arial's design values (version 7 of `arial.ttf`).
    fn arial() -> FaceTables {
        FaceTables {
            units_per_em: 2048,
            x_min: -1361,
            x_max: 4096,
            hhea_ascender: 1854,
            hhea_descender: -434,
            hhea_line_gap: 67,
            advance_width_max: 4096,
            win_ascent: 1854,
            win_descent: 434,
            x_avg_char_width: 904,
        }
    }

    #[test]
    fn mul_div_rounds_half_away_from_zero() {
        assert_eq!(mul_div(3, 1, 2), 2);
        assert_eq!(mul_div(-3, 1, 2), -2);
        assert_eq!(mul_div(5, 5, 0), -1);
        assert_eq!(mul_div(2048, 16, 2288), 14);
    }

    #[test]
    fn arial_at_cell_heights() {
        let a = arial();
        // lfHeight 16 is a 14-pixel em: GDI's tmHeight 16, ascent 13.
        assert_eq!(a.ppem_for_height(16), 14);
        let tm = a.text_metrics(14);
        assert_eq!((tm.height, tm.ascent, tm.descent), (16, 13, 3));
        assert_eq!(tm.internal_leading, 2);
        assert_eq!(tm.ave_char_width, 6);
        // A negative height is the em itself.
        assert_eq!(a.ppem_for_height(-12), 12);
        let tm = a.text_metrics(12);
        assert_eq!((tm.height, tm.ascent, tm.descent), (14, 11, 3));
    }

    #[test]
    fn tables_parse_from_bytes() {
        let mut head = vec![0u8; 54];
        head[18..20].copy_from_slice(&2048u16.to_be_bytes());
        head[36..38].copy_from_slice(&(-1361i16).to_be_bytes());
        head[40..42].copy_from_slice(&4096i16.to_be_bytes());
        let mut hhea = vec![0u8; 36];
        hhea[4..6].copy_from_slice(&1854i16.to_be_bytes());
        hhea[6..8].copy_from_slice(&(-434i16).to_be_bytes());
        hhea[8..10].copy_from_slice(&67i16.to_be_bytes());
        hhea[10..12].copy_from_slice(&4096u16.to_be_bytes());
        let mut os2 = vec![0u8; 96];
        os2[2..4].copy_from_slice(&904i16.to_be_bytes());
        os2[74..76].copy_from_slice(&1854u16.to_be_bytes());
        os2[76..78].copy_from_slice(&434u16.to_be_bytes());
        assert_eq!(FaceTables::parse(&head, &hhea, Some(&os2)), Some(arial()));
        let no_os2 = FaceTables::parse(&head, &hhea, Some(&os2[..68])).unwrap();
        assert_eq!((no_os2.win_ascent, no_os2.win_descent), (1854, 434));
        assert_eq!(FaceTables::parse(&head[..20], &hhea, None), None);
    }
}
