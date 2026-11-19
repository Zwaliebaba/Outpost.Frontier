//! The client's font code (work package P4.12): `font_imp.cpp`,
//! `font_atlas.cpp` and `font_string.cpp` of
//! `CppSource/OutpostFrontier/Client`, the font tables of
//! `CppSource/Common/common_textrender.cpp`, and the font configuration
//! that `fonts.ini` and `rich_fonts.ini` give.
//!
//! # How the reference draws text
//!
//! There is no glyph atlas in the usual sense. Every font of `fonts.ini`
//! is a GDI face (`FontImp`) whose pixel height follows the surface: the
//! configured `fixed_height` times the surface height, corrected by a
//! search that fits a calibration pangram to the share of the surface
//! width it takes at 800 x 600 ([`face`]). Text is measured with GDI's
//! integer advance widths. A string (`FontString`) is drawn whole, white
//! on black and antialiased, into a 256 x 256 scratch bitmap, one tile
//! at a time, and each tile is copied into its own power-of-two
//! `A1R5G5B5` texture (a "page"); pages are pooled and reused
//! ([`atlas`]). The string is then drawn as one screen quad per page,
//! with an optional shadow quad ([`string`]). Number strings
//! (`FontNumberString`) draw each character as a quad cut out of one
//! rasterised string, `-.0123456789MK%/`, from the advance widths
//! ([`number`]). The quads go to the screen-quad batcher, which expands
//! each into four pre-transformed vertices ([`quad`]).
//!
//! # The port
//!
//! The model is platform free. What GDI did is behind [`FontBackend`]:
//! creating a face from a `LOGFONT`, its text metrics, advance widths,
//! string extents and drawing a string into a coverage bitmap. The
//! Windows backend is DirectWrite in GDI-compatible measuring mode
//! (`platform::dwrite`, decision T3); tests use a fake. Everything else,
//! the autoscale search, measurement, line breaking, the page grid and
//! the quads, is the reference's arithmetic in its order (decision T11).
//!
//! State the reference keeps in globals (the font list, the page pool,
//! the number faces, the layout generation, the tint) belongs to a
//! [`FontSystem`] (decision T10). Strings belong to their owners and take
//! the system by reference; their pages go back to the pool only through
//! their `release` methods.

pub mod atlas;
pub mod config;
pub mod face;
pub mod gdi;
pub mod number;
pub mod quad;
pub mod rich;
pub mod string;
pub mod substitute;

use atlas::Atlas;
use face::{Font, FontDesc};
use number::NumberFaces;

/// A face the backend created (`HFONT`). Never 0.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct FaceHandle(pub u32);

/// The `LOGFONTA` fields the reference sets. Heights follow GDI: positive
/// is the cell height (ascent plus descent), negative the em height.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LogFont {
    pub height: i32,
    pub width: i32,
    pub escapement: i32,
    pub orientation: i32,
    pub weight: i32,
    pub italic: u8,
    pub underline: u8,
    pub strike_out: u8,
    pub char_set: u8,
    pub out_precision: u8,
    pub clip_precision: u8,
    pub quality: u8,
    pub pitch_and_family: u8,
    /// `lfFaceName`, NUL-terminated within its 32 bytes.
    pub face_name: [u8; 32],
}

/// `DEFAULT_CHARSET`.
pub const DEFAULT_CHARSET: u8 = 1;
/// `OUT_TT_PRECIS`.
pub const OUT_TT_PRECIS: u8 = 4;
/// `ANTIALIASED_QUALITY`.
pub const ANTIALIASED_QUALITY: u8 = 4;
/// `FW_NORMAL`.
pub const FW_NORMAL: i32 = 400;
/// `FW_BOLD`.
pub const FW_BOLD: i32 = 700;

impl LogFont {
    /// The `LOGFONTA` that `FontImp::Init` and `BuildRichTextLogFont` both
    /// build: zeroed, weight 700 or 400, `DEFAULT_CHARSET`,
    /// `OUT_TT_PRECIS`, `ANTIALIASED_QUALITY`, the face cut to 31 bytes
    /// (`strncpy` 0x1f and a terminator) and the height given.
    #[must_use]
    pub fn new(face: &[u8], height: i32, bold: bool, italic: bool, underline: bool) -> LogFont {
        let mut face_name = [0u8; 32];
        for (dst, &src) in face_name
            .iter_mut()
            .zip(face.iter().take_while(|&&b| b != 0).take(31))
        {
            *dst = src;
        }
        LogFont {
            height,
            width: 0,
            escapement: 0,
            orientation: 0,
            weight: if bold { FW_BOLD } else { FW_NORMAL },
            italic: u8::from(italic),
            underline: u8::from(underline),
            strike_out: 0,
            char_set: DEFAULT_CHARSET,
            out_precision: OUT_TT_PRECIS,
            clip_precision: 0,
            quality: ANTIALIASED_QUALITY,
            pitch_and_family: 0,
            face_name,
        }
    }

    /// The face name up to its NUL.
    #[must_use]
    pub fn face(&self) -> &[u8] {
        let end = self
            .face_name
            .iter()
            .position(|&b| b == 0)
            .unwrap_or(self.face_name.len());
        &self.face_name[..end]
    }
}

/// The `TEXTMETRICA` fields the font code reads.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TextMetrics {
    /// `tmHeight`: ascent plus descent.
    pub height: i32,
    pub ascent: i32,
    pub descent: i32,
    pub internal_leading: i32,
    pub external_leading: i32,
    pub ave_char_width: i32,
    pub max_char_width: i32,
}

/// What the reference asked of GDI, for one face at a time.
///
/// Text is UTF-16 code units; each unit is one character, as GDI's
/// non-complex text calls take it (no shaping, no kerning, no fallback).
pub trait FontBackend {
    /// `CreateFontIndirectA`; `None` when no face can be made.
    fn create_font(&mut self, log_font: &LogFont) -> Option<FaceHandle>;
    /// `DeleteObject` on a face.
    fn delete_font(&mut self, face: FaceHandle);
    /// `GetTextMetricsA` with the face selected.
    fn text_metrics(&mut self, face: FaceHandle) -> TextMetrics;
    /// `GetCharWidthW` for one code unit: its advance in whole pixels.
    fn char_width(&mut self, face: FaceHandle, ch: u16) -> Option<i32>;
    /// `GetTextExtentPointW`: the summed advances and the cell height. An
    /// empty text measures as zero wide and one cell high.
    fn text_extent(&mut self, face: FaceHandle, text: &[u16]) -> Option<(i32, i32)>;
    /// `ExtTextOutW` with `ETO_OPAQUE` into a black `width` x `height`
    /// bitmap: the text drawn white and antialiased with its cell's top
    /// left corner at (`x`, `y`), clipped to the bitmap. Returns the
    /// coverage, `width * height` bytes row by row, 0 for background and
    /// 255 for full coverage.
    fn draw_text(
        &mut self,
        face: FaceHandle,
        x: i32,
        y: i32,
        width: u32,
        height: u32,
        text: &[u16],
    ) -> Option<Vec<u8>>;
}

/// The surface the fonts are scaled for and the layout generation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FontEnv {
    /// `G_RenderSurfaceWidth`.
    pub surface_width: i32,
    /// `G_RenderSurfaceHeight`.
    pub surface_height: i32,
    /// `G_FontLayoutGeneration`: every cached size and page grid is valid
    /// only for the generation it was made in. Starts at 1, never 0.
    pub layout_generation: i32,
    /// `OUTPOST_FRONTIER_WIDESCREEN_SUPPORT`: the autoscale search fits
    /// the pangram to the width of a 4:3 surface of the same height.
    pub widescreen: bool,
}

impl FontEnv {
    /// An 800 x 600 surface in the first generation, widescreen support on
    /// as the reference build has it.
    #[must_use]
    pub fn new(surface_width: i32, surface_height: i32) -> FontEnv {
        FontEnv {
            surface_width,
            surface_height,
            layout_generation: 1,
            widescreen: true,
        }
    }

    /// The shadow offset and number-glyph inset of both paints: 2 above
    /// an 800-wide surface, else 1.
    #[must_use]
    pub fn pixel_scale(&self) -> i32 {
        if self.surface_width > 0x320 { 2 } else { 1 }
    }
}

/// What the font calls of one face need: the backend and the surface.
pub struct FontCx<'a> {
    pub backend: &'a mut dyn FontBackend,
    pub env: &'a FontEnv,
}

/// A font of the list, by its place in it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct FontId(pub usize);

/// The font subsystem: the backend, the font list (`G_FontList`), the page
/// pool, the number faces, the generation, and the rich-text faces and
/// styles.
pub struct FontSystem {
    pub backend: Box<dyn FontBackend>,
    pub env: FontEnv,
    pub fonts: Vec<Font>,
    pub atlas: Atlas,
    pub(crate) number_faces: NumberFaces,
    /// `G_NumberStringPaintDisable` (0 in the reference, never written).
    pub number_paint_disabled: bool,
    /// The rich-text face table, scales and image root (`DeviceTRC`'s
    /// statics and the globals of `common_textrender.cpp`).
    pub rich: rich::RichFonts,
    /// The rich-text style registry (`StyleCollection`).
    pub styles: crate::text::layout::StyleCollection,
}

impl FontSystem {
    #[must_use]
    pub fn new(backend: Box<dyn FontBackend>, env: FontEnv) -> FontSystem {
        FontSystem {
            backend,
            env,
            fonts: Vec::new(),
            atlas: Atlas::new(),
            number_faces: NumberFaces::default(),
            number_paint_disabled: false,
            rich: rich::RichFonts::new(),
            styles: crate::text::layout::StyleCollection::new(),
        }
    }

    /// The calls of one font with this system's backend and surface.
    pub fn font_cx(&mut self, id: FontId) -> Option<(&mut Font, FontCx<'_>)> {
        let font = self.fonts.get_mut(id.0)?;
        Some((
            font,
            FontCx {
                backend: self.backend.as_mut(),
                env: &self.env,
            },
        ))
    }

    /// `LoadFonts` over the entries of a fonts INI
    /// ([`config::read_fonts_ini`]): one font per entry, appended to the
    /// list; returns how many initialised (all of them: `FontImp::Init`
    /// always succeeds).
    pub fn load_fonts(&mut self, entries: &[config::TrueTypeFont]) -> usize {
        let mut count = 0;
        for entry in entries {
            let mut font = Font::new();
            font.set_name(&entry.nickname);
            let desc = FontDesc {
                face: entry.font.clone(),
                fixed_height: entry.fixed_height,
                bold: false,
                italic: false,
                underline: false,
            };
            let mut cx = FontCx {
                backend: self.backend.as_mut(),
                env: &self.env,
            };
            if font.init(&mut cx, &desc) {
                count += 1;
            }
            self.fonts.push(font);
        }
        count
    }

    /// `LoadRichFontsFromIni` over the file's contents: the `[TrueType]`
    /// lines into the standard fonts ([`config::apply_rich_fonts_ini`]) and
    /// the `[Style]` sections into the styles
    /// ([`crate::text::layout::style::apply_rich_styles`]). The two kinds of
    /// section touch nothing in common, so reading them in two passes is
    /// reading them in file order.
    pub fn load_rich_fonts(&mut self, contents: &[u8]) {
        config::apply_rich_fonts_ini(contents, &mut self.rich.standard);
        crate::text::layout::style::apply_rich_styles(contents, &mut self.styles);
    }

    /// `FindMenuFontImp`: the first font whose name equals `nickname`
    /// under `stricmp`.
    #[must_use]
    pub fn find_font(&self, nickname: &[u8]) -> Option<FontId> {
        self.fonts
            .iter()
            .position(|f| neuron_core::hash::eq_ignore_case(f.name(), nickname))
            .map(FontId)
    }

    /// `FontDeviceReset` from the render-manager init: the next layout
    /// generation (0 skipped), every number face's glyph string released.
    /// The GDI scratch it rebuilds has no counterpart here.
    pub fn device_reset(&mut self) {
        self.env.layout_generation = self.env.layout_generation.wrapping_add(1);
        if self.env.layout_generation == 0 {
            self.env.layout_generation = 1;
        }
        number::reset_all_faces(self);
    }

    /// `FontShutdown`: the font list emptied (faces forgotten, as the
    /// reference leaks them), the page pool's textures destroyed, every
    /// number face's glyph string released. The screen-quad batch cache it
    /// also clears belongs to the batcher.
    pub fn shutdown(&mut self, pipeline: &mut dyn crate::render::texture::TexturePipeline) {
        self.fonts.clear();
        self.atlas.release_pool(pipeline);
        number::reset_all_faces(self);
    }
}

/// The UTF-16 text up to its first NUL, as `wcslen` sees it.
#[must_use]
pub fn wide_c_str(text: &[u16]) -> &[u16] {
    let end = text.iter().position(|&c| c == 0).unwrap_or(text.len());
    &text[..end]
}

#[cfg(test)]
pub(crate) mod fake;

#[cfg(test)]
mod tests;
