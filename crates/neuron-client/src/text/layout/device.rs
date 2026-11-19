//! `DeviceTRC`, the context the client draws rich text with
//! (`common_textrender.cpp`): metrics from the rich-text face table
//! ([`RichFonts`]), drawing into a memory surface in place of a GDI memory
//! DC, and the bitmaps of image nodes.
//!
//! The reference selects the face of the font byte into its DC and asks
//! GDI. Here the faces are the backend's; the DC is a [`RichSurface`]: an
//! `0x00RRGGBB` pixel buffer with a clip region. A device without a
//! surface measures nothing, as GDI does on a null DC. Text is drawn
//! through [`FontBackend::draw_text`]'s coverage and blended into the
//! surface in the text colour; images are stretched by nearest pixel
//! (divergence D421).

use neuron_core::image::{RgbImage, bmp};
use neuron_core::num::trunc_to_i32;

use super::{TextRenderContext, TrcState, VisualSize};
use crate::text::font::FontBackend;
use crate::text::font::rich::RichFonts;
use crate::text::rdl::{ClipRect, RichImage};

/// The shadow colour of highlighted text (`0x0A0A0A`).
pub const SHADOW_COLOR: u32 = 0x000A_0A0A;

/// The memory DC a rich-text window draws into: its bitmap and its clip
/// region. Pixels are `0x00RRGGBB`, rows top to bottom.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RichSurface {
    pub width: i32,
    pub height: i32,
    pub pixels: Vec<u32>,
    /// The selected clip region, in surface pixels; `None` for none.
    pub clip: Option<ClipRect>,
}

impl RichSurface {
    /// A black surface (`CreateCompatibleBitmap`); a size below 1 gives an
    /// empty one.
    #[must_use]
    pub fn new(width: i32, height: i32) -> RichSurface {
        let (w, h) = (width.max(0), height.max(0));
        RichSurface {
            width: w,
            height: h,
            pixels: vec![0; (w as usize) * (h as usize)],
            clip: None,
        }
    }

    /// The draw's `SelectClipRgn(dc, NULL)` and `FillRect` in black.
    pub fn clear(&mut self) {
        self.clip = None;
        self.pixels.fill(0);
    }

    /// The rectangle drawing reaches: the bitmap, within the clip region.
    fn bounds(&self) -> ClipRect {
        let mut r = ClipRect {
            left: 0,
            top: 0,
            right: self.width,
            bottom: self.height,
        };
        if let Some(clip) = self.clip {
            r.left = r.left.max(clip.left);
            r.top = r.top.max(clip.top);
            r.right = r.right.min(clip.right);
            r.bottom = r.bottom.min(clip.bottom);
        }
        r
    }

    /// Blends `color` (`0x00BBGGRR`, a `COLORREF`) over the pixel by a
    /// coverage of 0 to 255.
    fn blend(&mut self, x: i32, y: i32, color: u32, coverage: u8) {
        let index = (y as usize) * (self.width as usize) + x as usize;
        let dst = self.pixels[index];
        let a = u32::from(coverage);
        let mix = |src: u32, dst: u32| (src * a + dst * (255 - a) + 127) / 255;
        let (sr, sg, sb) = (color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF);
        let (dr, dg, db) = ((dst >> 16) & 0xFF, (dst >> 8) & 0xFF, dst & 0xFF);
        self.pixels[index] = (mix(sr, dr) << 16) | (mix(sg, dg) << 8) | mix(sb, db);
    }

    /// `ExtTextOutW` in `TRANSPARENT` mode with the cell's top left at
    /// (`x`, `y`): the backend's coverage of the text blended in `color`.
    #[allow(clippy::too_many_arguments)]
    fn draw_text(
        &mut self,
        backend: &mut dyn FontBackend,
        face: crate::text::font::FaceHandle,
        x: i32,
        y: i32,
        advance: i32,
        cell: i32,
        text: &[u16],
        color: u32,
    ) {
        // A box around the run with a cell's room on either side for
        // overhangs.
        let pad = cell.max(1);
        let w = advance.saturating_add(pad.saturating_mul(2)).max(1);
        let h = cell.saturating_add(pad).max(1);
        let Some(coverage) = backend.draw_text(face, pad, 0, w as u32, h as u32, text) else {
            return;
        };
        let bounds = self.bounds();
        for row in 0..h {
            let sy = y.saturating_add(row);
            if sy < bounds.top || sy >= bounds.bottom {
                continue;
            }
            for col in 0..w {
                let sx = x.saturating_sub(pad).saturating_add(col);
                if sx < bounds.left || sx >= bounds.right {
                    continue;
                }
                let c = coverage[(row as usize) * (w as usize) + col as usize];
                if c != 0 {
                    self.blend(sx, sy, color, c);
                }
            }
        }
    }

    /// `StretchBlt` with `SRCCOPY`: `bitmap` scaled to `width` by `height`
    /// at (`x`, `y`) by nearest pixel.
    fn stretch(&mut self, bitmap: &RgbImage, x: i32, y: i32, width: i32, height: i32) {
        if width <= 0 || height <= 0 || bitmap.width == 0 || bitmap.height == 0 {
            return;
        }
        let bounds = self.bounds();
        for dy in 0..height {
            let sy = y.saturating_add(dy);
            if sy < bounds.top || sy >= bounds.bottom {
                continue;
            }
            let src_y = (i64::from(dy) * i64::from(bitmap.height) / i64::from(height)) as u32;
            for dx in 0..width {
                let sx = x.saturating_add(dx);
                if sx < bounds.left || sx >= bounds.right {
                    continue;
                }
                let src_x = (i64::from(dx) * i64::from(bitmap.width) / i64::from(width)) as u32;
                let [r, g, b] = bitmap.pixel(src_x, src_y);
                let index = (sy as usize) * (self.width as usize) + sx as usize;
                self.pixels[index] = (u32::from(r) << 16) | (u32::from(g) << 8) | u32::from(b);
            }
        }
    }

    /// The draw's read-back: `GetDIBits` into 16-bit `BI_RGB` (5:5:5, each
    /// channel's top five bits), rows of `pitch` bytes, then the alpha bit
    /// on every pixel that is not black: the `A1R5G5B5` buffer the big
    /// image takes.
    #[must_use]
    pub fn to_a1r5g5b5(&self, pitch: usize) -> Vec<u8> {
        let mut out = vec![0u8; pitch * self.height as usize];
        for y in 0..self.height as usize {
            for x in 0..self.width as usize {
                let p = self.pixels[y * self.width as usize + x];
                let r = (p >> 19) & 0x1F;
                let g = (p >> 11) & 0x1F;
                let b = (p >> 3) & 0x1F;
                let mut v = ((r << 10) | (g << 5) | b) as u16;
                if v != 0 {
                    v |= 0x8000;
                }
                let at = y * pitch + x * 2;
                if at + 2 <= out.len() {
                    out[at..at + 2].copy_from_slice(&v.to_le_bytes());
                }
            }
        }
        out
    }
}

/// The state a `DeviceTRC` object keeps: the base fields and the cached
/// clip rectangle (`+0x40`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DeviceTrc {
    pub state: TrcState,
    pub clip: ClipRect,
}

impl DeviceTrc {
    #[must_use]
    pub fn new() -> DeviceTrc {
        DeviceTrc::default()
    }

    /// `create_image` (`BMPImage`'s constructor): a name that is a UNC path
    /// or has a drive is read as given, anything else after the image root
    /// (`set_image_root`, kept with its trailing backslash). `read` gives a
    /// file's bytes. A file that does not read or decode gives an image of
    /// size 0 without pixels; the reference also gives an image then.
    pub fn create_image(
        fonts: &RichFonts,
        name: &[u8],
        read: &mut dyn FnMut(&[u8]) -> Option<Vec<u8>>,
    ) -> RichImage {
        let unc = name.starts_with(b"\\\\");
        let path = if !unc && name.get(1) != Some(&b':') {
            let mut path = fonts.image_root.clone();
            path.extend_from_slice(name);
            path
        } else {
            name.to_vec()
        };
        match read(&path).and_then(|bytes| bmp::decode(&bytes).ok()) {
            Some(bitmap) => RichImage {
                width: bitmap.width as f32,
                height: bitmap.height as f32,
                bitmap: Some(bitmap),
            },
            None => RichImage::sized(0.0, 0.0),
        }
    }
}

/// A [`DeviceTrc`] at work: with the face table, the backend and, when it
/// is bound to one, the surface (`bind_to_dc`).
pub struct DeviceTrcCx<'a> {
    pub trc: &'a mut DeviceTrc,
    pub fonts: &'a RichFonts,
    pub backend: &'a mut dyn FontBackend,
    pub surface: Option<&'a mut RichSurface>,
}

impl DeviceTrcCx<'_> {
    /// The extent of `text` in the current face: `None` without a surface
    /// or a face, or when the backend fails.
    fn extent(&mut self, text: &[u16]) -> Option<(i32, i32)> {
        self.surface.as_ref()?;
        let face = self.fonts.font(self.trc.state.font_byte()).handle?;
        self.backend.text_extent(face, text)
    }
}

impl TextRenderContext for DeviceTrcCx<'_> {
    fn trc(&self) -> &TrcState {
        &self.trc.state
    }

    fn trc_mut(&mut self) -> &mut TrcState {
        &mut self.trc.state
    }

    fn cell_width(&mut self) -> i32 {
        self.fonts.cell_width(self.trc.state.font_byte())
    }

    fn cell_height(&mut self) -> i32 {
        self.fonts.cell_height(self.trc.state.font_byte())
    }

    /// The extent (0 x 0 without a surface, a face, or when it fails) and
    /// the face's ascent and descent; a null text writes only those two,
    /// as 0.
    fn text_visual_size(&mut self, text: Option<&[u16]>, size: &mut VisualSize) {
        let Some(text) = text else {
            size.ascent = 0;
            size.descent = 0;
            return;
        };
        (size.width, size.height) = self.extent(text).unwrap_or((0, 0));
        let (ascent, descent) = self.fonts.font_metrics(self.trc.state.font_byte());
        size.ascent = ascent;
        size.descent = descent;
    }

    fn font_metrics(&mut self) -> (i32, i32) {
        self.fonts.font_metrics(self.trc.state.font_byte())
    }

    /// `GetTextExtentExPointW`: the leading characters whose summed
    /// advances are at most `max_width`; 0 for a null or empty text, or
    /// without a surface or a face.
    fn text_fit(&mut self, text: Option<&[u16]>, max_width: i32) -> i32 {
        let Some(text) = text.filter(|t| !t.is_empty()) else {
            return 0;
        };
        if self.surface.is_none() {
            return 0;
        }
        let Some(face) = self.fonts.font(self.trc.state.font_byte()).handle else {
            return 0;
        };
        let mut sum = 0i64;
        let mut fit = 0;
        for &unit in text {
            let Some(w) = self.backend.char_width(face, unit) else {
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

    /// With `highlight`, the text in the shadow colour at the shadow
    /// offset, then in the colour of the current attributes, the cell's top
    /// left at the pen plus the origin. Drawn or not, the pen advances by
    /// the extent (0 when it fails).
    fn execute_text(&mut self, text: Option<&[u16]>, highlight: bool) {
        let Some(text) = text else { return };
        let advance = self.extent(text).map_or(0, |(w, _)| w);
        if highlight {
            let font = self.fonts.font(self.trc.state.font_byte());
            let s = &self.trc.state;
            let x = s.pos_h.wrapping_add(s.origin_h);
            let y = s.pos_v.wrapping_add(s.origin_v);
            let color = (s.current >> 8) & 0x00FF_FFFF;
            let (dx, dy) = self.fonts.drop_shadow;
            if let (Some(face), Some(surface)) = (font.handle, self.surface.as_deref_mut()) {
                let cell = font.metrics.height;
                surface.draw_text(
                    self.backend,
                    face,
                    x.wrapping_add(dx),
                    y.wrapping_add(dy),
                    advance,
                    cell,
                    text,
                    SHADOW_COLOR,
                );
                surface.draw_text(self.backend, face, x, y, advance, cell, text, color);
            }
        }
        let s = &mut self.trc.state;
        s.pos_h = s.pos_h.wrapping_add(advance);
    }

    /// With `draw`, the bitmap stretched to its size times the image scale
    /// at the pen plus the origin; the pen then advances by the scaled
    /// width. A missing image does nothing (the reference reads through the
    /// null pointer; divergence D299).
    fn execute_image(&mut self, image: Option<&RichImage>, draw: bool) {
        let Some(image) = image else { return };
        let scale = f64::from(self.fonts.scale_y);
        let s = &self.trc.state;
        if draw && let (Some(bitmap), Some(surface)) = (&image.bitmap, self.surface.as_deref_mut())
        {
            surface.stretch(
                bitmap,
                s.origin_h.wrapping_add(s.pos_h),
                s.origin_v.wrapping_add(s.pos_v),
                trunc_to_i32(scale * f64::from(image.width)),
                trunc_to_i32(scale * f64::from(image.height)),
            );
        }
        let s = &mut self.trc.state;
        s.pos_h = trunc_to_i32(scale * f64::from(image.width) + f64::from(s.pos_h));
    }

    /// With a surface: an empty rectangle removes the clip region, any
    /// other becomes it, moved by the origin and normalised as GDI makes
    /// regions. The rectangle is kept either way.
    fn set_clip_rect(&mut self, rect: ClipRect) {
        if let Some(surface) = self.surface.as_deref_mut() {
            if rect.bottom == rect.top || rect.left == rect.right {
                surface.clip = None;
            } else {
                let (oh, ov) = (self.trc.state.origin_h, self.trc.state.origin_v);
                let (l, r) = (rect.left.wrapping_add(oh), rect.right.wrapping_add(oh));
                let (t, b) = (rect.top.wrapping_add(ov), rect.bottom.wrapping_add(ov));
                surface.clip = Some(ClipRect {
                    left: l.min(r),
                    top: t.min(b),
                    right: l.max(r),
                    bottom: t.max(b),
                });
            }
        }
        self.trc.clip = rect;
    }

    /// With a surface: no clip region zeroes the kept rectangle, else it
    /// becomes the region's box moved back by the origin. The kept
    /// rectangle is returned.
    fn clip_rect(&mut self) -> ClipRect {
        if let Some(surface) = self.surface.as_deref() {
            self.trc.clip = match surface.clip {
                None => ClipRect::default(),
                Some(c) => {
                    let (oh, ov) = (self.trc.state.origin_h, self.trc.state.origin_v);
                    ClipRect {
                        left: c.left.wrapping_sub(oh),
                        top: c.top.wrapping_sub(ov),
                        right: c.right.wrapping_sub(oh),
                        bottom: c.bottom.wrapping_sub(ov),
                    }
                }
            };
        }
        self.trc.clip
    }

    fn image_scale(&self) -> f32 {
        self.fonts.scale_y
    }
}

#[cfg(test)]
mod tests;
