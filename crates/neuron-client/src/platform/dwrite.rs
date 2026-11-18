//! The font backend on DirectWrite (decision T3, work package P4.12): what
//! the reference asked of GDI (`CreateFontIndirectA`, `GetTextMetricsA`,
//! `GetCharWidthW`, the string extent and drawing white text on black),
//! answered in DirectWrite's GDI-compatible measuring mode.
//!
//! A `LOGFONT` becomes a face through DirectWrite's GDI interop, which
//! matches family, weight and style as GDI's font mapper does for installed
//! faces. A face DirectWrite does not know (the game's `Agency FB` and
//! `Arial Unicode MS` come with Microsoft Office, not Windows; `FixedSys` is
//! a bitmap font DirectWrite cannot use) is replaced by Arial, where GDI's
//! mapper picks a substitute of its own (divergence D290).
//!
//! The pixel em size, the text metrics and the rounding of advances follow
//! GDI's mapping ([`crate::text::font::gdi`]); the advances themselves are
//! DirectWrite's GDI-compatible (hinted) advances at that em size, and the
//! glyphs are rasterised in the GDI-classic rendering mode with grayscale
//! antialiasing, as the reference's `ANTIALIASED_QUALITY` asked of GDI.

use std::collections::{BTreeMap, HashMap};
use std::mem::ManuallyDrop;

use windows::Win32::Foundation::RECT;
use windows::Win32::Graphics::DirectWrite::{
    DWRITE_FACTORY_TYPE_SHARED, DWRITE_GLYPH_METRICS, DWRITE_GLYPH_RUN,
    DWRITE_GRID_FIT_MODE_ENABLED, DWRITE_MEASURING_MODE_GDI_CLASSIC,
    DWRITE_RENDERING_MODE_GDI_CLASSIC, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE,
    DWRITE_TEXTURE_ALIASED_1x1, DWRITE_TEXTURE_CLEARTYPE_3x1, DWRITE_TEXTURE_TYPE,
    DWriteCreateFactory, IDWriteFactory2, IDWriteFont, IDWriteFontFace, IDWriteGdiInterop,
};
use windows::Win32::Graphics::Gdi::{
    FONT_CHARSET, FONT_CLIP_PRECISION, FONT_OUTPUT_PRECISION, FONT_QUALITY, LOGFONTW,
};
use windows::core::{BOOL, w};

use crate::text::font::gdi::FaceTables;
use crate::text::font::{FaceHandle, FontBackend, LogFont, TextMetrics};

/// The face that stands in for one DirectWrite does not have.
pub const FALLBACK_FACE: &[u8] = b"Arial";

/// An OpenType table tag as DirectWrite takes it (`DWRITE_MAKE_OPENTYPE_TAG`).
fn tag(name: &[u8; 4]) -> u32 {
    u32::from_le_bytes(*name)
}

struct Face {
    face: IDWriteFontFace,
    tables: FaceTables,
    ppem: u32,
    metrics: TextMetrics,
    family: String,
    substituted: bool,
    glyphs: HashMap<u16, (u16, i32)>,
}

/// DirectWrite as a [`FontBackend`].
pub struct DWriteBackend {
    factory: IDWriteFactory2,
    interop: IDWriteGdiInterop,
    faces: BTreeMap<u32, Face>,
    next: u32,
}

impl DWriteBackend {
    /// The shared DirectWrite factory (Windows 8.1 or later) and its GDI
    /// interop.
    ///
    /// # Errors
    ///
    /// DirectWrite's, when the factory cannot be made.
    pub fn new() -> windows::core::Result<DWriteBackend> {
        // SAFETY: a factory type constant; the result is an owned interface.
        let factory: IDWriteFactory2 = unsafe { DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED)? };
        // SAFETY: a live factory; the result is an owned interface.
        let interop = unsafe { factory.GetGdiInterop()? };
        Ok(DWriteBackend {
            factory,
            interop,
            faces: BTreeMap::new(),
            next: 0,
        })
    }

    /// The family name of a face (English where the font has it).
    #[must_use]
    pub fn family(&self, face: FaceHandle) -> Option<&str> {
        self.faces.get(&face.0).map(|f| f.family.as_str())
    }

    /// Whether the face is the stand-in for one DirectWrite does not have.
    #[must_use]
    pub fn substituted(&self, face: FaceHandle) -> bool {
        self.faces.get(&face.0).is_some_and(|f| f.substituted)
    }

    /// The whole-pixel em size the face is realised at.
    #[must_use]
    pub fn ppem(&self, face: FaceHandle) -> Option<u32> {
        self.faces.get(&face.0).map(|f| f.ppem)
    }

    fn logfont_w(log_font: &LogFont, face: &[u8]) -> LOGFONTW {
        let mut name = [0u16; 32];
        for (dst, &src) in name.iter_mut().zip(face.iter().take(31)) {
            *dst = u16::from(src);
        }
        LOGFONTW {
            lfHeight: log_font.height,
            lfWidth: log_font.width,
            lfEscapement: log_font.escapement,
            lfOrientation: log_font.orientation,
            lfWeight: log_font.weight,
            lfItalic: log_font.italic,
            lfUnderline: log_font.underline,
            lfStrikeOut: log_font.strike_out,
            lfCharSet: FONT_CHARSET(log_font.char_set),
            lfOutPrecision: FONT_OUTPUT_PRECISION(log_font.out_precision),
            lfClipPrecision: FONT_CLIP_PRECISION(log_font.clip_precision),
            lfQuality: FONT_QUALITY(log_font.quality),
            lfPitchAndFamily: log_font.pitch_and_family,
            lfFaceName: name,
        }
    }

    fn font_from_logfont(&self, log_font: &LogFont, face: &[u8]) -> Option<IDWriteFont> {
        let lf = Self::logfont_w(log_font, face);
        // SAFETY: `lf` is a complete LOGFONTW on the stack for the call.
        unsafe { self.interop.CreateFontFromLOGFONT(&lf) }.ok()
    }

    fn family_name(font: &IDWriteFont) -> String {
        // SAFETY: owned interfaces from a live font; the buffer is sized
        // from the reported length plus the terminator.
        unsafe {
            let Ok(names) = font.GetFontFamily().and_then(|f| f.GetFamilyNames()) else {
                return String::new();
            };
            let mut index = 0u32;
            let mut exists = BOOL(0);
            if names
                .FindLocaleName(w!("en-us"), &mut index, &mut exists)
                .is_err()
                || !exists.as_bool()
            {
                index = 0;
            }
            let Ok(len) = names.GetStringLength(index) else {
                return String::new();
            };
            let mut buf = vec![0u16; len as usize + 1];
            if names.GetString(index, &mut buf).is_err() {
                return String::new();
            }
            String::from_utf16_lossy(&buf[..len as usize])
        }
    }

    fn table(face: &IDWriteFontFace, name: &[u8; 4]) -> Option<Vec<u8>> {
        let mut data = std::ptr::null_mut();
        let mut size = 0u32;
        let mut context = std::ptr::null_mut();
        let mut exists = BOOL(0);
        // SAFETY: out-pointers to locals; on success with `exists` the data
        // pointer is valid for `size` bytes until ReleaseFontTable, and is
        // copied before that call.
        unsafe {
            face.TryGetFontTable(tag(name), &mut data, &mut size, &mut context, &mut exists)
                .ok()?;
            if !exists.as_bool() || data.is_null() {
                if !context.is_null() {
                    face.ReleaseFontTable(context);
                }
                return None;
            }
            let bytes = std::slice::from_raw_parts(data.cast::<u8>(), size as usize).to_vec();
            face.ReleaseFontTable(context);
            Some(bytes)
        }
    }

    /// The glyph index and whole-pixel advance of one code unit.
    fn glyph(&mut self, handle: FaceHandle, ch: u16) -> Option<(u16, i32)> {
        let face = self.faces.get_mut(&handle.0)?;
        if let Some(&g) = face.glyphs.get(&ch) {
            return Some(g);
        }
        let code_point = if (0xD800..0xE000).contains(&ch) {
            0xFFFF
        } else {
            u32::from(ch)
        };
        let mut index = 0u16;
        // SAFETY: one code point in, one glyph index out, both locals.
        unsafe { face.face.GetGlyphIndices(&code_point, 1, &mut index) }.ok()?;
        let mut metrics = DWRITE_GLYPH_METRICS::default();
        // SAFETY: one glyph index in, one metrics record out, both locals;
        // no transform.
        unsafe {
            face.face.GetGdiCompatibleGlyphMetrics(
                face.ppem as f32,
                1.0,
                None,
                false,
                &index,
                1,
                &mut metrics,
                false,
            )
        }
        .ok()?;
        let advance = face.tables.pixels(
            face.ppem,
            i32::try_from(metrics.advanceWidth).unwrap_or(i32::MAX),
        );
        face.glyphs.insert(ch, (index, advance));
        Some((index, advance))
    }

    /// The coverage of a glyph run analysis in `texture_type`, blitted into
    /// a `width` x `height` target; false when the texture is empty.
    fn blit(
        analysis: &windows::Win32::Graphics::DirectWrite::IDWriteGlyphRunAnalysis,
        texture_type: DWRITE_TEXTURE_TYPE,
        out: &mut [u8],
        width: u32,
        height: u32,
    ) -> bool {
        // SAFETY: a live analysis; the bounds come back by value.
        let Ok(bounds) = (unsafe { analysis.GetAlphaTextureBounds(texture_type) }) else {
            return false;
        };
        let bw = (bounds.right - bounds.left).max(0) as usize;
        let bh = (bounds.bottom - bounds.top).max(0) as usize;
        if bw == 0 || bh == 0 {
            return false;
        }
        let per_pixel = if texture_type == DWRITE_TEXTURE_CLEARTYPE_3x1 {
            3
        } else {
            1
        };
        let mut texture = vec![0u8; bw * bh * per_pixel];
        let rect = RECT { ..bounds };
        // SAFETY: the buffer holds exactly the bytes the bounds and texture
        // type call for.
        if unsafe { analysis.CreateAlphaTexture(texture_type, &rect, &mut texture) }.is_err() {
            return false;
        }
        for row in 0..bh {
            let y = bounds.top + row as i32;
            if y < 0 || y >= height as i32 {
                continue;
            }
            for col in 0..bw {
                let x = bounds.left + col as i32;
                if x < 0 || x >= width as i32 {
                    continue;
                }
                let at = (row * bw + col) * per_pixel;
                let coverage = if per_pixel == 3 {
                    let sum: u32 = texture[at..at + 3].iter().map(|&b| u32::from(b)).sum();
                    (sum / 3) as u8
                } else {
                    texture[at]
                };
                let dst = &mut out[y as usize * width as usize + x as usize];
                *dst = (*dst).max(coverage);
            }
        }
        true
    }
}

impl FontBackend for DWriteBackend {
    fn create_font(&mut self, log_font: &LogFont) -> Option<FaceHandle> {
        let (font, substituted) = match self.font_from_logfont(log_font, log_font.face()) {
            Some(font) => (font, false),
            None => (self.font_from_logfont(log_font, FALLBACK_FACE)?, true),
        };
        // SAFETY: a live font; the result is an owned interface.
        let face = unsafe { font.CreateFontFace() }.ok()?;
        let head = Self::table(&face, b"head")?;
        let hhea = Self::table(&face, b"hhea")?;
        let os2 = Self::table(&face, b"OS/2");
        let tables = FaceTables::parse(&head, &hhea, os2.as_deref())?;
        let ppem = tables.ppem_for_height(log_font.height);
        let metrics = tables.text_metrics(ppem);
        self.next += 1;
        self.faces.insert(
            self.next,
            Face {
                face,
                tables,
                ppem,
                metrics,
                family: Self::family_name(&font),
                substituted,
                glyphs: HashMap::new(),
            },
        );
        Some(FaceHandle(self.next))
    }

    fn delete_font(&mut self, face: FaceHandle) {
        self.faces.remove(&face.0);
    }

    fn text_metrics(&mut self, face: FaceHandle) -> TextMetrics {
        self.faces
            .get(&face.0)
            .map(|f| f.metrics)
            .unwrap_or_default()
    }

    fn char_width(&mut self, face: FaceHandle, ch: u16) -> Option<i32> {
        self.glyph(face, ch).map(|(_, advance)| advance)
    }

    fn text_extent(&mut self, face: FaceHandle, text: &[u16]) -> Option<(i32, i32)> {
        let mut width = 0i32;
        for &ch in text {
            width = width.wrapping_add(self.glyph(face, ch)?.1);
        }
        Some((width, self.faces.get(&face.0)?.metrics.height))
    }

    fn draw_text(
        &mut self,
        face: FaceHandle,
        x: i32,
        y: i32,
        width: u32,
        height: u32,
        text: &[u16],
    ) -> Option<Vec<u8>> {
        let mut out = vec![0u8; (width as usize) * (height as usize)];
        let mut indices = Vec::with_capacity(text.len());
        let mut advances = Vec::with_capacity(text.len());
        for &ch in text {
            let (index, advance) = self.glyph(face, ch)?;
            indices.push(index);
            advances.push(advance as f32);
        }
        if indices.is_empty() || out.is_empty() {
            return Some(out);
        }
        let entry = self.faces.get(&face.0)?;
        let run = DWRITE_GLYPH_RUN {
            fontFace: ManuallyDrop::new(Some(entry.face.clone())),
            fontEmSize: entry.ppem as f32,
            glyphCount: u32::try_from(indices.len()).ok()?,
            glyphIndices: indices.as_ptr(),
            glyphAdvances: advances.as_ptr(),
            glyphOffsets: std::ptr::null(),
            isSideways: BOOL(0),
            bidiLevel: 0,
        };
        let baseline = (y + entry.metrics.ascent) as f32;
        // SAFETY: the run's arrays live until the call returns; the font
        // face reference in the run is released below.
        let analysis = unsafe {
            self.factory.CreateGlyphRunAnalysis(
                &run,
                None,
                DWRITE_RENDERING_MODE_GDI_CLASSIC,
                DWRITE_MEASURING_MODE_GDI_CLASSIC,
                DWRITE_GRID_FIT_MODE_ENABLED,
                DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE,
                x as f32,
                baseline,
            )
        };
        drop(ManuallyDrop::into_inner(run.fontFace));
        let analysis = analysis.ok()?;
        if !Self::blit(
            &analysis,
            DWRITE_TEXTURE_CLEARTYPE_3x1,
            &mut out,
            width,
            height,
        ) {
            Self::blit(
                &analysis,
                DWRITE_TEXTURE_ALIASED_1x1,
                &mut out,
                width,
                height,
            );
        }
        Some(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn backend() -> Option<DWriteBackend> {
        match DWriteBackend::new() {
            Ok(b) => Some(b),
            Err(e) => {
                println!("DirectWrite is not available here ({e}); nothing to test");
                None
            }
        }
    }

    fn w(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    #[test]
    fn arial_metrics_are_sane() {
        let Some(mut b) = backend() else {
            return;
        };
        let Some(face) = b.create_font(&LogFont::new(b"Arial", 16, false, false, false)) else {
            println!("no Arial here; nothing to test");
            return;
        };
        assert!(!b.substituted(face));
        assert_eq!(b.family(face), Some("Arial"));
        let tm = b.text_metrics(face);
        assert!((15..=17).contains(&tm.height), "{tm:?}");
        assert_eq!(tm.ascent + tm.descent, tm.height);
        assert!(tm.ascent > tm.descent && tm.descent > 0, "{tm:?}");
        let wide = b.char_width(face, u16::from(b'W')).unwrap();
        let narrow = b.char_width(face, u16::from(b'i')).unwrap();
        assert!(wide > narrow && narrow > 0, "W {wide}, i {narrow}");
        let text = w("Hello");
        let (cx, cy) = b.text_extent(face, &text).unwrap();
        let sum: i32 = text.iter().map(|&c| b.char_width(face, c).unwrap()).sum();
        assert_eq!((cx, cy), (sum, tm.height));
        assert_eq!(b.text_extent(face, &[]), Some((0, tm.height)));
    }

    #[test]
    fn glyphs_rasterise_into_coverage() {
        let Some(mut b) = backend() else {
            return;
        };
        let Some(face) = b.create_font(&LogFont::new(b"Arial", 24, false, false, false)) else {
            return;
        };
        let text = w("Hg");
        let (cx, cy) = b.text_extent(face, &text).unwrap();
        let (width, height) = (cx.cast_unsigned(), cy.cast_unsigned());
        let coverage = b.draw_text(face, 0, 0, width, height, &text).unwrap();
        assert_eq!(coverage.len(), (width * height) as usize);
        let lit = coverage.iter().filter(|&&c| c > 0).count();
        assert!(lit > 20, "{lit} pixels lit");
        assert!(coverage.contains(&255), "no full coverage");
        // Antialiased: some partial coverage.
        assert!(
            coverage.iter().any(|&c| c > 0 && c < 255),
            "no partial coverage"
        );
        // Drawn half outside on the left, the right part only.
        let shifted = b
            .draw_text(face, -(cx / 2), 0, width, height, &text)
            .unwrap();
        let lit_shifted = shifted.iter().filter(|&&c| c > 0).count();
        assert!(lit_shifted < lit);
    }

    #[test]
    fn unknown_faces_fall_back() {
        let Some(mut b) = backend() else {
            return;
        };
        let Some(face) = b.create_font(&LogFont::new(
            b"No Such Face Outpost",
            20,
            false,
            false,
            false,
        )) else {
            println!("no fallback face here; nothing to test");
            return;
        };
        assert!(b.substituted(face));
        assert!(b.char_width(face, u16::from(b'A')).unwrap() > 0);
    }

    /// The configured faces: each realises (or falls back) and measures.
    #[test]
    fn configured_faces_measure() {
        let Some(mut b) = backend() else {
            return;
        };
        for name in [
            &b"Agency FB"[..],
            b"Arial Unicode MS",
            b"FixedSys",
            b"Vibrocentric",
        ] {
            let Some(face) = b.create_font(&LogFont::new(name, 26, false, false, false)) else {
                continue;
            };
            let tm = b.text_metrics(face);
            assert!(tm.height > 20 && tm.height < 32, "{tm:?}");
            let (cx, _) = b
                .text_extent(
                    face,
                    &w("In the beginning the Earth was without form and void."),
                )
                .unwrap();
            assert!(cx > 200 && cx < 900, "{cx}");
            println!(
                "{} -> {} (substituted {}): {tm:?}, pangram {cx}",
                String::from_utf8_lossy(name),
                b.family(face).unwrap_or(""),
                b.substituted(face)
            );
        }
    }
}
