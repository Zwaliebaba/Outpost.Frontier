//! Number strings (`FontNumberString` in `font_string.cpp`): text made of
//! the sixteen characters `-.0123456789MK%/`, drawn one quad per character
//! from a per-font "number face", a `FontString` of those characters
//! rasterised once, cut up by the characters' advance widths.
//!
//! The text itself is printed by [`crate::text::number`].

use std::cmp::Ordering;
use std::collections::BTreeMap;

use super::atlas::PageId;
use super::quad::{BLEND_ALPHA, ScreenQuadRecord, clip_1d, pack_color};
use super::string::{DEFAULT_SHADOW_COLOR, DrawOrigin, FontStringChild, PaintCx};
use super::{FontId, FontSystem};
use crate::render::texture::TexturePipeline;

/// The characters of a number face, in glyph-index order.
pub const GLYPH_CHARS: &[u8; 16] = b"-.0123456789MK%/";

/// The glyphs of a face: the sixteen characters and the terminator.
pub const GLYPH_COUNT: usize = 17;

/// One glyph's texture rectangle in its face's page.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct GlyphUv {
    pub u0: f32,
    pub v0: f32,
    pub u1: f32,
    pub v1: f32,
}

/// `FNumberFace`.
#[derive(Clone, Debug, PartialEq)]
pub struct NumberFace {
    pub(crate) glyph: Option<FontStringChild>,
    pub layout_gen: i32,
    pub font: FontId,
    pub ref_count: i32,
    pub advance: [i32; GLYPH_COUNT],
    pub uv: [GlyphUv; GLYPH_COUNT],
}

/// `G_NumberFaceMap`, by font.
pub(crate) type NumberFaces = BTreeMap<FontId, NumberFace>;

/// The glyph index `Printf` maps a byte to: the place in
/// [`GLYPH_CHARS`], 16 for NUL, 0 (the `-` glyph) for any other byte.
#[must_use]
pub fn glyph_index(byte: u8) -> u8 {
    if byte == 0 {
        return 16;
    }
    GLYPH_CHARS
        .iter()
        .position(|&c| c == byte)
        .map_or(0, |i| i as u8)
}

/// The face of a font (`NumberFaceGetOrCreate`): a new reference to it, or
/// a new face with one. `None` for no font.
pub(crate) fn get_or_create_face(sys: &mut FontSystem, font: Option<FontId>) -> Option<FontId> {
    let font = font?;
    sys.number_faces
        .entry(font)
        .and_modify(|f| f.ref_count += 1)
        .or_insert_with(|| NumberFace {
            glyph: None,
            layout_gen: 0,
            font,
            ref_count: 1,
            advance: [0; GLYPH_COUNT],
            uv: [GlyphUv::default(); GLYPH_COUNT],
        });
    Some(font)
}

/// `NumberFaceRelease`: one reference less; at none the glyph string is
/// released and the face removed.
pub(crate) fn release_face(sys: &mut FontSystem, key: FontId) {
    let Some(face) = sys.number_faces.get_mut(&key) else {
        return;
    };
    face.ref_count -= 1;
    if face.ref_count <= 0
        && let Some(mut face) = sys.number_faces.remove(&key)
        && let Some(mut glyph) = face.glyph.take()
    {
        glyph.release(sys);
    }
}

/// `NumberFaceResetAll`: every face's glyph string released, every face
/// stale.
pub(crate) fn reset_all_faces(sys: &mut FontSystem) {
    let keys: Vec<FontId> = sys.number_faces.keys().copied().collect();
    for key in keys {
        let glyph = sys.number_faces.get_mut(&key).and_then(|f| {
            f.layout_gen = 0;
            f.glyph.take()
        });
        if let Some(mut glyph) = glyph {
            glyph.release(sys);
        }
    }
}

/// The glyph text as the face's string holds it: the sixteen characters,
/// the terminator `AssignWideCount` copies with them and its own.
fn glyph_units() -> [u16; 18] {
    let mut units = [0u16; 18];
    for (u, &c) in units.iter_mut().zip(GLYPH_CHARS) {
        *u = u16::from(c);
    }
    units
}

/// `NumberFaceCreateGlyph`: a `FontString` of the glyph characters bound
/// to the face's font and rasterised with the size check; released again
/// when it has no page.
fn create_glyph(sys: &mut FontSystem, pipeline: &mut dyn TexturePipeline, key: FontId) {
    let Some(font) = sys.number_faces.get(&key).map(|f| f.font) else {
        return;
    };
    let mut glyph = FontStringChild::new();
    glyph.bind_font(sys, Some(font));
    glyph.assign_raw(&glyph_units());
    let kept = if glyph.ensure_pages(sys, pipeline, false).is_none() {
        glyph.release(sys);
        None
    } else {
        Some(glyph)
    };
    if let Some(face) = sys.number_faces.get_mut(&key) {
        face.glyph = kept;
    }
}

/// `NumberFaceBuildTable`: the advances of the 17 glyphs (`GetCharWidths`
/// over the string's 18 units) and their texture rectangles: a running
/// float sum of the advances over the page width.
fn build_table(sys: &mut FontSystem, key: FontId, glyph: &FontStringChild, page: PageId) {
    let mut widths = [0i32; 18];
    if let Some(font) = glyph.font()
        && let Some((f, mut cx)) = sys.font_cx(font)
    {
        f.char_widths(&mut cx, &glyph_units(), &mut widths);
    }
    let page = sys.atlas.page(page).clone();
    let Some(face) = sys.number_faces.get_mut(&key) else {
        return;
    };
    face.advance.copy_from_slice(&widths[..GLYPH_COUNT]);
    let v_ratio = f64::from(page.used_h) / f64::from(page.page_h.cast_signed());
    let mut previous = 0.0f32;
    let mut sum = 0.0f32;
    for i in 0..GLYPH_COUNT {
        sum = (f64::from(face.advance[i]) + f64::from(sum)) as f32;
        let u1 = (f64::from(sum) / f64::from(page.page_w)) as f32;
        face.uv[i] = GlyphUv {
            u0: previous,
            v0: 0.0,
            u1,
            v1: v_ratio as f32,
        };
        previous = u1;
    }
}

/// The face's pages made current and its table rebuilt, with the
/// generation stamped first (the steps `NumberFaceRebuild` and the size
/// loop share once the glyph string exists).
fn refresh_table(sys: &mut FontSystem, pipeline: &mut dyn TexturePipeline, key: FontId) {
    let Some(face) = sys.number_faces.get_mut(&key) else {
        return;
    };
    let Some(mut glyph) = face.glyph.take() else {
        return;
    };
    face.layout_gen = sys.env.layout_generation;
    if let Some(page) = glyph.ensure_pages(sys, pipeline, false) {
        build_table(sys, key, &glyph, page);
    }
    if let Some(face) = sys.number_faces.get_mut(&key) {
        face.glyph = Some(glyph);
    } else {
        glyph.release(sys);
    }
}

/// `NumberFaceRebuild`: when stale, the glyph string made if missing, then
/// the table.
fn rebuild_face(sys: &mut FontSystem, pipeline: &mut dyn TexturePipeline, key: FontId) {
    let Some(face) = sys.number_faces.get(&key) else {
        return;
    };
    if face.layout_gen == sys.env.layout_generation {
        return;
    }
    if face.glyph.is_none() {
        create_glyph(sys, pipeline, key);
    }
    refresh_table(sys, pipeline, key);
}

fn face_stale(sys: &FontSystem, key: FontId) -> bool {
    sys.number_faces
        .get(&key)
        .is_some_and(|f| f.layout_gen != sys.env.layout_generation)
}

fn has_glyph(sys: &FontSystem, key: FontId) -> bool {
    sys.number_faces
        .get(&key)
        .is_some_and(|f| f.glyph.is_some())
}

/// `FontNumberString`.
#[derive(Clone, Debug, PartialEq)]
pub struct NumberStringChild {
    glyphs: Vec<u8>,
    font: Option<FontId>,
    /// The shadow colour.
    pub color: u32,
    /// Whether the shadow quads are drawn.
    pub shadow: bool,
    layout_gen: i32,
    width: i32,
    height: i32,
    face: Option<FontId>,
}

impl Default for NumberStringChild {
    fn default() -> Self {
        Self::new()
    }
}

impl NumberStringChild {
    /// No glyphs, no font, shadow on.
    #[must_use]
    pub fn new() -> NumberStringChild {
        NumberStringChild {
            glyphs: Vec::new(),
            font: None,
            color: DEFAULT_SHADOW_COLOR,
            shadow: true,
            layout_gen: 0,
            width: 0,
            height: 0,
            face: None,
        }
    }

    /// The face reference given back (the destructor).
    pub fn release(&mut self, sys: &mut FontSystem) {
        if let Some(key) = self.face.take() {
            release_face(sys, key);
        }
    }

    /// `BindFont`: on a change the face is given back; the size is stale.
    pub fn bind_font(&mut self, sys: &mut FontSystem, font: Option<FontId>) {
        if self.font == font {
            return;
        }
        self.font = font;
        self.release(sys);
        self.layout_gen = 0;
    }

    /// `GetFont`.
    #[must_use]
    pub fn font(&self) -> Option<FontId> {
        self.font
    }

    /// The glyph indices.
    #[must_use]
    pub fn glyphs(&self) -> &[u8] {
        &self.glyphs
    }

    /// The glyphs of printed text, one per byte through [`glyph_index`].
    pub fn set_printed(&mut self, bytes: &[u8]) {
        self.glyphs = bytes.iter().map(|&b| glyph_index(b)).collect();
        self.layout_gen = 0;
    }

    /// `Printf` of a format without arguments, as `SetNumberText` calls
    /// it; a format the printer refuses leaves no glyphs.
    pub fn printf(&mut self, format: &[u8]) {
        let bytes = crate::text::number::printf(format, &[]).unwrap_or_default();
        self.set_printed(&bytes);
    }

    /// `FormatFloat`.
    pub fn format_float(&mut self, value: f32, decimals: i32, suffix: u8) {
        let bytes = crate::text::number::format_float(value, decimals, suffix).unwrap_or_default();
        self.set_printed(&bytes);
    }

    /// `FormatInt`.
    pub fn format_int(&mut self, value: i32, suffix: u8) {
        let bytes = crate::text::number::format_int(value, suffix).unwrap_or_default();
        self.set_printed(&bytes);
    }

    /// `SetColor`: the shadow colour, and the shadow on.
    pub fn set_color(&mut self, argb: u32) {
        self.color = argb;
        self.shadow = true;
    }

    /// `SetShadow`.
    pub fn set_shadow(&mut self, on: bool) {
        self.shadow = on;
    }

    /// `GetSize`: when stale, the face's glyph height and the sum of the
    /// glyphs' advances (a stale face's table rebuilt first).
    pub fn get_size(
        &mut self,
        sys: &mut FontSystem,
        pipeline: &mut dyn TexturePipeline,
    ) -> (i32, i32) {
        if self.layout_gen != sys.env.layout_generation {
            self.layout_gen = sys.env.layout_generation;
            self.width = 0;
            self.height = 0;
            if self.face.is_none() {
                self.face = get_or_create_face(sys, self.font);
            }
            if let Some(key) = self.face {
                if !has_glyph(sys, key) {
                    create_glyph(sys, pipeline, key);
                }
                let glyph = sys.number_faces.get_mut(&key).and_then(|f| f.glyph.take());
                if let Some(mut glyph) = glyph {
                    self.height = glyph.get_size(sys).1;
                    if let Some(face) = sys.number_faces.get_mut(&key) {
                        face.glyph = Some(glyph);
                    }
                    for &index in &self.glyphs {
                        if face_stale(sys, key) {
                            if !has_glyph(sys, key) {
                                create_glyph(sys, pipeline, key);
                            }
                            refresh_table(sys, pipeline, key);
                        }
                        let advance = sys
                            .number_faces
                            .get(&key)
                            .map_or(0, |f| f.advance[usize::from(index) % GLYPH_COUNT]);
                        self.width = self.width.wrapping_add(advance);
                    }
                }
            }
        }
        (self.width, self.height)
    }

    /// `Paint`: the string box at the floored origin less half a pixel,
    /// clipped to the surface; then each glyph from the pen position, its
    /// shadow first, until a glyph reaches the clipped right edge.
    pub fn paint(&mut self, sys: &mut FontSystem, cx: &mut PaintCx<'_>, origin: &DrawOrigin) {
        if sys.number_paint_disabled || self.glyphs.is_empty() {
            return;
        }
        if self.face.is_none() {
            self.face = get_or_create_face(sys, self.font);
        }
        let Some(key) = self.face else {
            return;
        };
        if !has_glyph(sys, key) {
            create_glyph(sys, cx.pipeline, key);
        }
        let Some(mut glyph) = sys.number_faces.get_mut(&key).and_then(|f| f.glyph.take()) else {
            return;
        };
        let page = glyph.ensure_pages(sys, cx.pipeline, false);
        if let Some(face) = sys.number_faces.get_mut(&key) {
            face.glyph = Some(glyph);
        }
        let Some(page) = page else {
            return;
        };
        let texture = sys.atlas.page(page).texture;

        cx.sink.set_material_key(sys.atlas.material_key);
        let scale = sys.env.pixel_scale() as f32;
        let x0 = (f64::from(origin.x).floor() - 0.5) as f32;
        let y0 = (f64::from(origin.y).floor() - 0.5) as f32;
        cx.sink.begin_batch_frame(texture, BLEND_ALPHA, true);
        if face_stale(sys, key) {
            rebuild_face(sys, cx.pipeline, key);
        }
        let (w, h) = self.get_size(sys, cx.pipeline);
        let x1 = (f64::from(w.cast_unsigned()) + f64::from(x0) + f64::from(scale)) as f32;
        let y1 = (f64::from(h.cast_unsigned()) + f64::from(y0) + f64::from(scale)) as f32;
        let first = sys
            .number_faces
            .get(&key)
            .map(|f| f.uv[0])
            .unwrap_or_default();
        let (mut clip_x0, mut clip_y0, mut clip_x1, mut clip_y1) = (x0, y0, x1, y1);
        let (mut clip_u0, mut clip_v0, mut clip_u1, mut clip_v1) =
            (0.0f32, first.v0, 1.0f32, first.v1);
        let screen_w = sys.env.surface_width;
        let screen_h = sys.env.surface_height;
        if clip_1d(
            &mut clip_y0,
            &mut clip_y1,
            &mut clip_v0,
            &mut clip_v1,
            0.0,
            (screen_h - 1) as f32,
            (f64::from(y1) - f64::from(y0) + 1.0) as f32,
        ) == 0
        {
            return;
        }
        if clip_1d(
            &mut clip_x0,
            &mut clip_x1,
            &mut clip_u0,
            &mut clip_u1,
            0.0,
            (screen_w - 1) as f32,
            (f64::from(x1) - f64::from(x0) + 1.0) as f32,
        ) == 0
        {
            return;
        }

        let mut pen_x = x0;
        for &index in &self.glyphs {
            let index = usize::from(index) % GLYPH_COUNT;
            if face_stale(sys, key) {
                rebuild_face(sys, cx.pipeline, key);
            }
            let advance = sys.number_faces.get(&key).map_or(0, |f| f.advance[index]) as f32;
            if face_stale(sys, key) {
                rebuild_face(sys, cx.pipeline, key);
            }
            let glyph_x0 = pen_x;
            pen_x = (f64::from(advance) + f64::from(pen_x)) as f32;
            let uv = sys
                .number_faces
                .get(&key)
                .map(|f| f.uv[index])
                .unwrap_or_default();
            if glyph_x0 < clip_x0 {
                continue;
            }
            if pen_x >= clip_x1 {
                return;
            }
            let tint = sys.atlas.draw_color;
            if self.shadow {
                let mut sx0 = (f64::from(glyph_x0) + f64::from(scale)) as f32;
                let sy0 = (f64::from(clip_y0) + f64::from(scale)) as f32;
                let mut sx1 = (f64::from(pen_x) + f64::from(scale)) as f32;
                let (mut su0, mut su1) = (uv.u0, uv.u1);
                let span = (f64::from(sx1) - f64::from(sx0) + 1.0) as f32;
                if clip_1d(
                    &mut sx0,
                    &mut sx1,
                    &mut su0,
                    &mut su1,
                    0.0,
                    (screen_w - 1) as f32,
                    span,
                ) != 0
                {
                    cx.sink.append(ScreenQuadRecord {
                        x0: sx0,
                        y0: sy0,
                        x1: sx1,
                        y1: clip_y1,
                        depth_a: 1.0,
                        depth_b: 1.0,
                        u0: su0,
                        v0: clip_v0,
                        u1: su1,
                        v1: clip_v1,
                        color: pack_color((tint >> 24) as u8, self.color),
                    });
                }
            }
            let glyph_y1 = (f64::from(clip_y1) - f64::from(scale)) as f32;
            if !matches!(
                glyph_y1.partial_cmp(&clip_y0),
                Some(Ordering::Greater | Ordering::Equal)
            ) {
                continue;
            }
            cx.sink.append(ScreenQuadRecord {
                x0: glyph_x0,
                y0: clip_y0,
                x1: pen_x,
                y1: glyph_y1,
                depth_a: 1.0,
                depth_b: 1.0,
                u0: uv.u0,
                v0: clip_v0,
                u1: uv.u1,
                v1: clip_v1,
                color: pack_color((tint >> 24) as u8, tint),
            });
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn printed_bytes_map_to_glyphs() {
        assert_eq!(glyph_index(b'-'), 0);
        assert_eq!(glyph_index(b'.'), 1);
        assert_eq!(glyph_index(b'0'), 2);
        assert_eq!(glyph_index(b'9'), 11);
        assert_eq!(glyph_index(b'/'), 15);
        assert_eq!(glyph_index(0), 16);
        assert_eq!(glyph_index(b' '), 0);
        let mut n = NumberStringChild::new();
        n.printf(b"75%%");
        assert_eq!(n.glyphs(), &[9, 7, 14]);
    }
}
