//! The rich-text font tables of `common_textrender.cpp`: the 32 standard
//! fonts (`SetStandardFont`), the 256 realised faces `DeviceTRC::mFont`
//! (32 styles times the eight bold, italic and underline combinations,
//! indexed by the font byte of the text attributes), the rich-text scale
//! and the metric queries of `DeviceTRC`.
//!
//! Drawing rich text (`execute_text`, images, clipping) belongs to the
//! rich-text layout and execution, which are not ported yet.

use neuron_core::num::trunc_to_i32;

use super::config::{StandardFont, default_standard_fonts};
use super::{FaceHandle, FontBackend, LogFont, TextMetrics, wide_c_str};

/// Styles, and faces per style.
const STYLES: usize = 32;
const COMBINATIONS: usize = 8;

/// One entry of `mFont` (`DeviceTRC::WinFont`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RichFont {
    pub log_font: LogFont,
    /// The size before scaling.
    pub unscaled_size: i32,
    pub handle: Option<FaceHandle>,
    pub metrics: TextMetrics,
}

/// `TextRenderContext::VisualSize`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct VisualSize {
    pub width: i32,
    pub height: i32,
    pub ascent: i32,
    pub descent: i32,
}

/// The rich-text font state.
#[derive(Clone, Debug)]
pub struct RichFonts {
    pub standard: Vec<StandardFont>,
    pub fonts: Vec<RichFont>,
    /// `GRichTextScaleX`: surface height over 768; the font scale.
    pub scale_x: f32,
    /// `GRichTextScaleY`: surface height over 600; the image scale.
    pub scale_y: f32,
    /// `mDropShadowX`, `mDropShadowY`.
    pub drop_shadow: (i32, i32),
    initialised: bool,
}

impl Default for RichFonts {
    fn default() -> Self {
        Self::new()
    }
}

impl RichFonts {
    /// The standard fonts at their defaults, no faces, scale 1, shadow 2.
    #[must_use]
    pub fn new() -> RichFonts {
        let empty = RichFont {
            log_font: LogFont::new(b"", 0, false, false, false),
            unscaled_size: 0,
            handle: None,
            metrics: TextMetrics::default(),
        };
        RichFonts {
            standard: default_standard_fonts(),
            fonts: vec![empty; STYLES * COMBINATIONS],
            scale_x: 1.0,
            scale_y: 1.0,
            drop_shadow: (2, 2),
            initialised: false,
        }
    }

    fn release_all(&mut self, backend: &mut dyn FontBackend) {
        for font in &mut self.fonts {
            if let Some(h) = font.handle.take() {
                backend.delete_font(h);
            }
        }
    }

    /// `InitializeRichText`: faces released when already initialised, the
    /// scales from the surface height, the drop shadow `trunc(2 * scale_x)`.
    pub fn initialize(&mut self, backend: &mut dyn FontBackend, screen_height: i32) -> bool {
        if self.initialised {
            self.release_all(backend);
            self.initialised = false;
        }
        self.scale_x = (f64::from(screen_height) / 768.0) as f32;
        self.scale_y = (f64::from(screen_height) / 600.0) as f32;
        let shadow = trunc_to_i32(f64::from(self.scale_x) + f64::from(self.scale_x));
        self.drop_shadow = (shadow, shadow);
        self.initialised = true;
        true
    }

    /// `SetStandardFont`; an index outside 0 to 31 is ignored (the
    /// reference writes past its table).
    pub fn set_standard_font(&mut self, index: usize, desc: StandardFont) {
        if let Some(slot) = self.standard.get_mut(index) {
            let mut desc = desc;
            desc.name.truncate(0x7F);
            *slot = desc;
        }
    }

    /// `RebuildRichTextStyleFonts`: the eight faces of a style from a name
    /// and size at `scale` (`lfHeight = trunc(size * scale)`), each old face
    /// deleted first.
    fn rebuild_style(
        &mut self,
        backend: &mut dyn FontBackend,
        style: usize,
        name: &[u8],
        size: u32,
        scale: f32,
    ) {
        for sub in 0..COMBINATIONS {
            let (bold, italic, underline) = (sub & 1 != 0, sub & 2 != 0, sub & 4 != 0);
            let Some(entry) = self.fonts.get_mut(style * COMBINATIONS + sub) else {
                return;
            };
            entry.unscaled_size = size.cast_signed();
            entry.log_font = LogFont::new(name, entry.unscaled_size, bold, italic, underline);
            if let Some(h) = entry.handle.take() {
                backend.delete_font(h);
            }
            entry.log_font.height = trunc_to_i32(f64::from(entry.unscaled_size) * f64::from(scale));
            entry.handle = backend.create_font(&entry.log_font);
            if let Some(h) = entry.handle {
                entry.metrics = backend.text_metrics(h);
            }
        }
    }

    /// `ReinitializeRichTextFonts`: every style's faces rebuilt from its
    /// standard font at `scale_x`.
    pub fn reinitialize_fonts(&mut self, backend: &mut dyn FontBackend) {
        for style in 0..STYLES {
            let standard = self.standard[style].clone();
            self.rebuild_style(backend, style, &standard.name, standard.size, self.scale_x);
        }
    }

    /// `DeviceTRC::static_set_font`: one style's faces from `desc` at
    /// `scale`; the standard table is not touched.
    pub fn static_set_font(
        &mut self,
        backend: &mut dyn FontBackend,
        style: usize,
        desc: &StandardFont,
        scale: f32,
    ) {
        self.rebuild_style(backend, style, &desc.name, desc.size, scale);
    }

    /// `DeviceTRC::release_font`: one style's faces deleted.
    pub fn release_font(&mut self, backend: &mut dyn FontBackend, style: usize) {
        for sub in 0..COMBINATIONS {
            if let Some(h) = self
                .fonts
                .get_mut(style * COMBINATIONS + sub)
                .and_then(|f| f.handle.take())
            {
                backend.delete_font(h);
            }
        }
    }

    /// `ShutdownRichText`.
    pub fn shutdown(&mut self, backend: &mut dyn FontBackend) {
        self.release_all(backend);
        self.initialised = false;
    }

    /// The face of a font byte (the low byte of the packed attributes).
    #[must_use]
    pub fn font(&self, font_byte: u8) -> &RichFont {
        &self.fonts[usize::from(font_byte)]
    }

    /// `get_cell_width`: `tmMaxCharWidth`.
    #[must_use]
    pub fn cell_width(&self, font_byte: u8) -> i32 {
        self.font(font_byte).metrics.max_char_width
    }

    /// `get_cell_height`: `tmHeight`.
    #[must_use]
    pub fn cell_height(&self, font_byte: u8) -> i32 {
        self.font(font_byte).metrics.height
    }

    /// `get_font_metrics`: ascent and descent.
    #[must_use]
    pub fn font_metrics(&self, font_byte: u8) -> (i32, i32) {
        let m = self.font(font_byte).metrics;
        (m.ascent, m.descent)
    }

    /// `get_text_visual_size` with the length -1: the extent (0 x 0 when it
    /// fails) and the face's ascent and descent. A null text writes only
    /// the ascent and descent (as 0) into `size`.
    pub fn text_visual_size(
        &self,
        backend: &mut dyn FontBackend,
        font_byte: u8,
        text: Option<&[u16]>,
        size: &mut VisualSize,
    ) {
        let Some(text) = text else {
            size.ascent = 0;
            size.descent = 0;
            return;
        };
        let font = self.font(font_byte);
        let extent = font
            .handle
            .and_then(|h| backend.text_extent(h, wide_c_str(text)));
        (size.width, size.height) = extent.unwrap_or((0, 0));
        size.ascent = font.metrics.ascent;
        size.descent = font.metrics.descent;
    }

    /// `get_text_fit` with the length -1: how many leading characters fit
    /// in `max_width` (`GetTextExtentExPointW`: their summed advances at
    /// most the width). 0 for a null or empty text or on failure.
    pub fn text_fit(
        &self,
        backend: &mut dyn FontBackend,
        font_byte: u8,
        text: Option<&[u16]>,
        max_width: i32,
    ) -> i32 {
        let Some(text) = text.map(wide_c_str).filter(|t| !t.is_empty()) else {
            return 0;
        };
        let Some(face) = self.font(font_byte).handle else {
            return 0;
        };
        let mut sum = 0i64;
        let mut fit = 0;
        for &ch in text {
            let Some(w) = backend.char_width(face, ch) else {
                return 0;
            };
            sum += i64::from(w);
            if sum > i64::from(max_width) {
                break;
            }
            fit += 1;
        }
        fit
    }

    /// The pen advance of `execute_text`: the text's extent width (0 when
    /// it fails).
    pub fn text_advance(&self, backend: &mut dyn FontBackend, font_byte: u8, text: &[u16]) -> i32 {
        self.font(font_byte)
            .handle
            .and_then(|h| backend.text_extent(h, wide_c_str(text)))
            .map_or(0, |(w, _)| w)
    }
}
