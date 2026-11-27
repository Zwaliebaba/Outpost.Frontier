//! `font_string.cpp`: a line of text drawn from its own pages
//! (`FontString`, here [`FontStringChild`]), and the multi-line holder
//! UI controls embed (`Font::String`, here [`FontString`]) with its word
//! wrap, extents and aligned drawing.
//!
//! The reference counts references to children; only the holder's slots
//! keep them, so here the slots own them, and releasing a child (giving
//! its pages back, or its number face) is an explicit call.

use std::cmp::Ordering;

use neuron_core::num::trunc_to_i32;

use super::atlas::{PageGrid, PageId, SCRATCH_SIZE, round_up_pow2};
use super::number::NumberStringChild;
use super::quad::{BLEND_ALPHA, QuadSink, ScreenQuadRecord, clip_1d, pack_color};
use super::{FontId, FontSystem, wide_c_str};
use crate::render::texture::TexturePipeline;

/// The shadow colour both children start with.
pub const DEFAULT_SHADOW_COLOR: u32 = 0xFF08_0808;

/// `FFontStringDrawOrigin`: where a string is drawn and the box it is
/// aligned in.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DrawOrigin {
    pub x: f32,
    pub y: f32,
    /// The box width; truncated, it is also the wrap width.
    pub width: f32,
    /// The box height.
    pub height: f32,
    /// 1 centre, 2 right, else left.
    pub h_mode: u32,
    /// 1 centre, 2 bottom, else top.
    pub v_mode: u32,
}

/// What a paint draws with.
pub struct PaintCx<'a> {
    pub pipeline: &'a mut dyn TexturePipeline,
    pub sink: &'a mut dyn QuadSink,
    /// `GetGameOption(TEXT)`: string pages are drawn only when set.
    pub text_enabled: bool,
}

/// `FontString` (`.?AUFontString@@`): one line, rasterised whole into a
/// grid of pages.
#[derive(Clone, Debug, PartialEq)]
pub struct FontStringChild {
    /// `None` for the null text.
    text: Option<Vec<u16>>,
    font: Option<FontId>,
    grid: Option<PageGrid>,
    cached_w: i32,
    cached_h: i32,
    layout_gen: i32,
    /// The shadow colour.
    pub color: u32,
    /// Whether the shadow quads are drawn.
    pub shadow: bool,
}

impl Default for FontStringChild {
    fn default() -> Self {
        Self::new()
    }
}

impl FontStringChild {
    /// No text, no font, shadow on in the default colour.
    #[must_use]
    pub fn new() -> FontStringChild {
        FontStringChild {
            text: None,
            font: None,
            grid: None,
            cached_w: 0,
            cached_h: 0,
            layout_gen: 0,
            color: DEFAULT_SHADOW_COLOR,
            shadow: true,
        }
    }

    /// The pages back to the pool (the destructor and `Release`).
    pub fn release(&mut self, sys: &mut FontSystem) {
        sys.atlas.free_grid(&mut self.grid);
    }

    /// `BindFont`: on a change the pages go back and the size is stale.
    pub fn bind_font(&mut self, sys: &mut FontSystem, font: Option<FontId>) {
        if self.font == font {
            return;
        }
        sys.atlas.free_grid(&mut self.grid);
        self.layout_gen = 0;
        self.font = font;
    }

    /// `GetFont`.
    #[must_use]
    pub fn font(&self) -> Option<FontId> {
        self.font
    }

    /// `GetText`.
    #[must_use]
    pub fn text(&self) -> Option<&[u16]> {
        self.text.as_deref()
    }

    /// The page grid.
    #[must_use]
    pub fn grid(&self) -> Option<&PageGrid> {
        self.grid.as_ref()
    }

    /// `SetText`: nothing when equal to the current text (null reads as
    /// empty); otherwise the pages go back, and the text is copied (up to
    /// its NUL) or, for null, emptied. The size is stale either way.
    pub fn set_text(&mut self, sys: &mut FontSystem, text: Option<&[u16]>) {
        let incoming = text.map(wide_c_str);
        if incoming.unwrap_or(&[]) == self.text.as_deref().unwrap_or(&[]) {
            return;
        }
        sys.atlas.free_grid(&mut self.grid);
        match incoming {
            None => {
                if let Some(t) = &mut self.text {
                    t.clear();
                }
            }
            Some(t) => self.text = Some(t.to_vec()),
        }
        self.layout_gen = 0;
    }

    /// `AssignWideCount` of the number face's glyph text: the units as
    /// given.
    pub(crate) fn assign_raw(&mut self, text: &[u16]) {
        self.text = Some(wide_c_str(text).to_vec());
    }

    /// `GetSize`: measured again when the generation moved (a changed size
    /// gives the pages back); without a font the cached size stays.
    pub fn get_size(&mut self, sys: &mut FontSystem) -> (i32, i32) {
        if self.layout_gen != sys.env.layout_generation {
            self.layout_gen = sys.env.layout_generation;
            if let Some(id) = self.font {
                let text = self.text.clone();
                let measured = match sys.font_cx(id) {
                    Some((font, mut cx)) => font.measure_string(&mut cx, text.as_deref()),
                    None => (0, 0),
                };
                if measured != (self.cached_w, self.cached_h) {
                    sys.atlas.free_grid(&mut self.grid);
                    self.cached_w = measured.0;
                    self.cached_h = measured.1;
                }
            }
        }
        (self.cached_w, self.cached_h)
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

    /// `FontStringChildEnsurePages`: a stale grid goes back, an empty one is
    /// rasterised; the head.
    pub fn ensure_pages(
        &mut self,
        sys: &mut FontSystem,
        pipeline: &mut dyn TexturePipeline,
        skip_size_check: bool,
    ) -> Option<PageId> {
        if self.layout_gen != sys.env.layout_generation && self.grid.is_some() {
            sys.atlas.free_grid(&mut self.grid);
        }
        if self.grid.is_none() {
            self.rasterize(sys, pipeline, skip_size_check);
        }
        self.grid.as_ref().and_then(PageGrid::head)
    }

    /// `FontAtlasRasterize`: size the string, build or reuse its grid, draw
    /// each tile and upload it. Without `skip_size_check` both sides go to
    /// a power of two and must fit the 256 x 256 scratch. True when the
    /// grid is current or every tile was drawn.
    pub fn rasterize(
        &mut self,
        sys: &mut FontSystem,
        pipeline: &mut dyn TexturePipeline,
        skip_size_check: bool,
    ) -> bool {
        let Some(font_id) = self.font else {
            return false;
        };
        let (raw_w, raw_h) = self.get_size(sys);
        let mut check_w = raw_w.cast_unsigned();
        let mut check_h = raw_h.cast_unsigned();
        if !skip_size_check {
            check_w = round_up_pow2(check_w);
            check_h = round_up_pow2(check_h);
            if check_w > SCRATCH_SIZE || check_h > SCRATCH_SIZE {
                return false;
            }
        }
        if let Some(grid) = &self.grid {
            if sys.atlas.grid_covers(grid, check_w, check_h) {
                let head = grid.head().map(|id| sys.atlas.page(id).content_h);
                if head == Some(raw_h.cast_unsigned()) {
                    return true;
                }
            } else {
                sys.atlas.free_grid(&mut self.grid);
            }
        }
        if self.grid.is_none() {
            self.grid = sys.atlas.build_grid(pipeline, check_w, check_h);
            if self.grid.is_none() {
                return false;
            }
        }
        let grid = self.grid.clone().unwrap_or_default();
        let face = sys
            .font_cx(font_id)
            .and_then(|(font, mut cx)| font.face(&mut cx));
        let text: Vec<u16> = self.text.clone().unwrap_or_default();
        let mut ok = true;
        let mut left_h = raw_h;
        let mut row_index = 0;
        'rows: while left_h > 0 {
            let tile_h = left_h.cast_unsigned().min(SCRATCH_SIZE);
            let Some(row) = grid.rows.get(row_index) else {
                break;
            };
            let mut left_w = raw_w;
            let mut col_index = 0;
            while left_w > 0 {
                let tile_w = left_w.cast_unsigned().min(SCRATCH_SIZE);
                let Some(&id) = row.get(col_index) else {
                    break 'rows;
                };
                let page = sys.atlas.page_mut(id);
                let (src_x, src_y) = (page.src_x, page.src_y);
                page.used_w = tile_w;
                page.used_h = tile_h;
                let drawn = face.and_then(|face| {
                    sys.backend.draw_text(
                        face,
                        src_x.wrapping_neg(),
                        src_y.wrapping_neg(),
                        tile_w,
                        tile_h,
                        &text,
                    )
                });
                match drawn {
                    None => ok = false,
                    Some(coverage) if ok => {
                        if sys
                            .atlas
                            .upload_tile(pipeline, id, &coverage, tile_w, tile_h)
                            != 0
                        {
                            ok = false;
                        }
                    }
                    Some(_) => {}
                }
                col_index += 1;
                left_w -= SCRATCH_SIZE.cast_signed();
            }
            row_index += 1;
            left_h -= SCRATCH_SIZE.cast_signed();
        }
        if ok && let Some(head) = grid.head() {
            sys.atlas.page_mut(head).content_h = raw_h.cast_unsigned();
        }
        ok
    }

    /// `Paint`: one quad per page at the floored origin, each preceded by
    /// its shadow quad, clipped on x to the surface or to the window the
    /// two trims leave, and on y to the surface (a page clipped away on y
    /// ends the paint).
    pub fn paint(
        &mut self,
        sys: &mut FontSystem,
        cx: &mut PaintCx<'_>,
        origin: &DrawOrigin,
        trim_left: f32,
        trim_right: f32,
    ) {
        if !cx.text_enabled {
            return;
        }
        if self.text.as_deref().is_none_or(<[u16]>::is_empty) {
            return;
        }
        let surface_w = sys.env.surface_width;
        let surface_h = sys.env.surface_height;
        let scale = sys.env.pixel_scale() as f32;
        let mut origin_x = f64::from(origin.x).floor() as f32;
        let origin_y = f64::from(origin.y).floor() as f32;
        let (w, _) = self.get_size(sys);

        let mut clip_left = 0i32;
        let mut clip_right = surface_w - 1;
        let trim_left_int = trunc_to_i32(f64::from(trim_left).floor());
        let trim_right_int = trunc_to_i32(f64::from(trim_right).floor());
        if trim_right_int > 0 || trim_left_int > 0 {
            let shifted = f64::from(origin_x) - f64::from(trim_left_int);
            clip_right =
                trunc_to_i32(f64::from(w.cast_unsigned()) + shifted - f64::from(trim_right_int));
            clip_right = clip_right.max(0).min(surface_w - 1);
            if trim_left_int > 0 {
                clip_left = trunc_to_i32(f64::from(origin_x));
                origin_x = shifted as f32;
                clip_left = clip_left.max(0);
            }
        }

        cx.sink.set_material_key(sys.atlas.material_key);
        if self.ensure_pages(sys, cx.pipeline, true).is_none() {
            return;
        }
        let Some(grid) = self.grid.clone() else {
            return;
        };
        let clip_left_f = clip_left as f32;
        let clip_right_f = clip_right as f32;
        let surface_bottom = (surface_h - 1) as f32;
        for row in &grid.rows {
            for &id in row {
                let page = sys.atlas.page(id).clone();
                let x0 = (f64::from(page.src_x.cast_unsigned()) + f64::from(origin_x) - 0.5) as f32;
                let y0 = (f64::from(page.src_y.cast_unsigned()) + f64::from(origin_y) - 0.5) as f32;
                let u_ratio =
                    (f64::from(page.used_w) / f64::from(page.page_w.cast_signed())) as f32;
                let v_ratio =
                    (f64::from(page.used_h) / f64::from(page.page_h.cast_signed())) as f32;
                let (mut u0, mut v0, mut u1, mut v1) = (0.0f32, 0.0f32, u_ratio, v_ratio);
                cx.sink.begin_batch_frame(page.texture, BLEND_ALPHA, false);
                let mut clip_x0 = x0;
                let mut clip_y0 = y0;
                let mut x1 = (f64::from(page.used_w) + f64::from(x0)) as f32;
                let mut y1 = (f64::from(page.used_h) + f64::from(y0)) as f32;
                let mut shadow_x0 = (f64::from(x0) + f64::from(scale)) as f32;
                let mut shadow_y0 = (f64::from(y0) + f64::from(scale)) as f32;
                let mut shadow_x1 = (f64::from(x1) + f64::from(scale)) as f32;
                let mut shadow_y1 = (f64::from(y1) + f64::from(scale)) as f32;
                let span_x = (f64::from(x1) - f64::from(x0) + 1.0) as f32;
                if clip_1d(
                    &mut clip_x0,
                    &mut x1,
                    &mut u0,
                    &mut u1,
                    clip_left_f,
                    clip_right_f,
                    span_x,
                ) == 0
                {
                    continue;
                }
                let span_y = (f64::from(y1) - f64::from(clip_y0) + 1.0) as f32;
                if clip_1d(
                    &mut clip_y0,
                    &mut y1,
                    &mut v0,
                    &mut v1,
                    0.0,
                    surface_bottom,
                    span_y,
                ) == 0
                {
                    return;
                }
                if self.shadow {
                    let (mut su0, mut sv0, mut su1, mut sv1) = (0.0f32, 0.0f32, u_ratio, v_ratio);
                    let span_sx = (f64::from(shadow_x1) - f64::from(shadow_x0) + 1.0) as f32;
                    let span_sy = (f64::from(shadow_y1) - f64::from(shadow_y0) + 1.0) as f32;
                    if clip_1d(
                        &mut shadow_x0,
                        &mut shadow_x1,
                        &mut su0,
                        &mut su1,
                        clip_left_f,
                        clip_right_f,
                        span_sx,
                    ) != 0
                        && clip_1d(
                            &mut shadow_y0,
                            &mut shadow_y1,
                            &mut sv0,
                            &mut sv1,
                            0.0,
                            surface_bottom,
                            span_sy,
                        ) != 0
                    {
                        let alpha = (sys.atlas.draw_color >> 24) as u8;
                        cx.sink.append(ScreenQuadRecord {
                            x0: shadow_x0,
                            y0: shadow_y0,
                            x1: shadow_x1,
                            y1: shadow_y1,
                            depth_a: 1.0,
                            depth_b: 1.0,
                            u0: su0,
                            v0: sv0,
                            u1: su1,
                            v1: sv1,
                            color: pack_color(alpha, self.color),
                        });
                    }
                }
                let tint = sys.atlas.draw_color;
                cx.sink.append(ScreenQuadRecord {
                    x0: clip_x0,
                    y0: clip_y0,
                    x1,
                    y1,
                    depth_a: 1.0,
                    depth_b: 1.0,
                    u0,
                    v0,
                    u1,
                    v1,
                    color: pack_color((tint >> 24) as u8, tint),
                });
            }
        }
    }
}

/// A child of a [`FontString`].
#[derive(Clone, Debug, PartialEq)]
pub enum Child {
    Text(FontStringChild),
    Number(NumberStringChild),
}

impl Child {
    /// `Release` of the last reference: pages or number face given back.
    pub fn release(&mut self, sys: &mut FontSystem) {
        match self {
            Child::Text(c) => c.release(sys),
            Child::Number(c) => c.release(sys),
        }
    }

    /// Slot 2, `BindFont`.
    pub fn bind_font(&mut self, sys: &mut FontSystem, font: Option<FontId>) {
        match self {
            Child::Text(c) => c.bind_font(sys, font),
            Child::Number(c) => c.bind_font(sys, font),
        }
    }

    /// Slot 6, `GetSize`.
    pub fn get_size(
        &mut self,
        sys: &mut FontSystem,
        pipeline: &mut dyn TexturePipeline,
    ) -> (i32, i32) {
        match self {
            Child::Text(c) => c.get_size(sys),
            Child::Number(c) => c.get_size(sys, pipeline),
        }
    }

    /// Slot 5, `SetText` (nothing on a number string).
    pub fn set_text(&mut self, sys: &mut FontSystem, text: Option<&[u16]>) {
        if let Child::Text(c) = self {
            c.set_text(sys, text);
        }
    }

    /// Slot 9, `Paint` (a number string reads only the origin).
    pub fn paint(
        &mut self,
        sys: &mut FontSystem,
        cx: &mut PaintCx<'_>,
        origin: &DrawOrigin,
        trim_left: f32,
        trim_right: f32,
    ) {
        match self {
            Child::Text(c) => c.paint(sys, cx, origin, trim_left, trim_right),
            Child::Number(c) => c.paint(sys, cx, origin),
        }
    }
}

/// `FFontStringChildSlot`: a child and the kind byte written when the slot
/// was appended (0 text, else number); replacing the child in place leaves
/// the byte as it was.
#[derive(Clone, Debug, PartialEq)]
pub struct ChildSlot {
    pub child: Option<Child>,
    pub kind: u8,
}

/// The greedy word wrap of `Font::String` (`FontStringWordWrap`), over
/// `GetGlyphAdvance`: chunks end at a space or tab (kept, the line goes
/// on), CR or LF (dropped, the line ends), the text's end, or a character
/// that would pass the width left (the chunk goes to the next line unless
/// it is the line's first). Every line is made; only the first
/// `max_lines` are returned.
///
/// The reference loops for ever when a line's first character alone is
/// wider than the width; here that character takes the line by itself
/// (divergence D293).
pub fn word_wrap(
    sys: &mut FontSystem,
    font: FontId,
    text: &[u16],
    max_width: f32,
    max_lines: i32,
) -> Vec<Vec<u16>> {
    let text = wide_c_str(text);
    let mut lines = Vec::new();
    let Some((face, mut cx)) = sys.font_cx(font) else {
        return lines;
    };
    let mut remain = text.len().cast_signed();
    if remain <= 0 {
        return lines;
    }
    let at = |i: usize| text.get(i).copied().unwrap_or(0);
    let mut cursor = 0usize;
    let mut line_count = 0i32;
    loop {
        let mut out: Vec<u16> = Vec::new();
        let mut first_chunk = true;
        let mut width_left = max_width;
        let line_start = cursor;
        loop {
            let mut taken = 0usize;
            let mut accum = 0.0f32;
            // 0 space or tab, 1 CR or LF, 2 NUL, 3 overflow.
            let break_kind;
            loop {
                let ch = at(cursor + taken);
                let adv = face.glyph_advance(&mut cx, ch);
                if ch == 0 {
                    break_kind = 2;
                    break;
                }
                let hit = if ch == u16::from(b'\r') || ch == u16::from(b'\n') {
                    Some(1)
                } else if ch == u16::from(b' ') || ch == u16::from(b'\t') {
                    Some(0)
                } else {
                    None
                };
                let fits = matches!(
                    (f64::from(accum) + f64::from(adv)).partial_cmp(&f64::from(width_left)),
                    Some(Ordering::Less | Ordering::Equal)
                );
                if hit.is_none() && !fits {
                    break_kind = 3;
                    break;
                }
                taken += 1;
                accum = (f64::from(adv) + f64::from(accum)) as f32;
                if let Some(kind) = hit {
                    break_kind = kind;
                    break;
                }
            }
            if first_chunk && break_kind == 3 && taken == 0 && cursor == line_start {
                taken = 1;
            }
            if first_chunk || break_kind != 3 {
                for _ in 0..taken {
                    let ch = at(cursor);
                    if ch != u16::from(b'\r') && ch != u16::from(b'\n') {
                        out.push(ch);
                    }
                    cursor += 1;
                }
                width_left = (f64::from(width_left) - f64::from(accum)) as f32;
                remain -= taken.cast_signed();
            }
            first_chunk = false;
            if break_kind != 0 {
                break;
            }
        }
        if line_count < max_lines {
            lines.push(out);
            line_count += 1;
        }
        if remain <= 0 {
            break;
        }
    }
    lines
}

/// `Font::String`: text, its lines as children, and the extents.
#[derive(Clone, Debug, PartialEq)]
pub struct FontString {
    text: Option<Vec<u16>>,
    /// `+0x08`: the most lines a multi-line string keeps (0x7FFF; at most
    /// 0x400 are made).
    pub max_lines: i32,
    children: Vec<ChildSlot>,
    font: Option<FontId>,
    extent_w: i32,
    extent_h: i32,
    layout_gen: i32,
    scale_key: i32,
    /// Word-wrapped into lines when set; one line otherwise.
    pub multi_line: bool,
}

impl Default for FontString {
    fn default() -> Self {
        Self::new()
    }
}

impl FontString {
    /// `FontStringConstruct`.
    #[must_use]
    pub fn new() -> FontString {
        FontString {
            text: None,
            max_lines: 0x7FFF,
            children: Vec::new(),
            font: None,
            extent_w: 0,
            extent_h: 0,
            layout_gen: 0,
            scale_key: 0x7FFF,
            multi_line: false,
        }
    }

    /// The children's slots.
    #[must_use]
    pub fn children(&self) -> &[ChildSlot] {
        &self.children
    }

    /// The child of a slot.
    pub fn child_mut(&mut self, index: usize) -> Option<&mut Child> {
        self.children.get_mut(index).and_then(|s| s.child.as_mut())
    }

    /// The text.
    #[must_use]
    pub fn text(&self) -> Option<&[u16]> {
        self.text.as_deref()
    }

    /// The font (`+0x1c`).
    #[must_use]
    pub fn font(&self) -> Option<FontId> {
        self.font
    }

    /// The extents of the last layout (`+0x20`, `+0x24`).
    #[must_use]
    pub fn extents(&self) -> (i32, i32) {
        (self.extent_w, self.extent_h)
    }

    /// `+0x30` with the layout made stale when it changes.
    pub fn set_multi_line(&mut self, on: bool) {
        if self.multi_line != on {
            self.multi_line = on;
            self.layout_gen = 0;
        }
    }

    /// The wrap width key.
    #[must_use]
    pub fn scale_key(&self) -> i32 {
        self.scale_key
    }

    fn release_children(&mut self, sys: &mut FontSystem, from: usize) {
        for slot in self.children.iter_mut().skip(from) {
            if let Some(mut child) = slot.child.take() {
                child.release(sys);
            }
        }
    }

    /// `FontStringClearContents`: the text freed, the children released and
    /// dropped, the layout stale; the extents stay.
    pub fn clear_contents(&mut self, sys: &mut FontSystem) {
        self.text = None;
        self.release_children(sys, 0);
        self.children.clear();
        self.layout_gen = 0;
    }

    /// `FontStringDestroy`: everything given back.
    pub fn release(&mut self, sys: &mut FontSystem) {
        self.clear_contents(sys);
    }

    /// `FontStringSetFont`: every child rebound, the layout stale.
    pub fn set_font(&mut self, sys: &mut FontSystem, font: Option<FontId>) {
        if self.font == font {
            return;
        }
        self.font = font;
        for slot in &mut self.children {
            if let Some(child) = &mut slot.child {
                child.bind_font(sys, font);
            }
        }
        self.layout_gen = 0;
    }

    /// `FontStringSetText`: nothing when equal (null reads as empty); a
    /// non-empty old text is freed with every child; then the text copied,
    /// or emptied for null. The layout is stale.
    pub fn set_text(&mut self, sys: &mut FontSystem, text: Option<&[u16]>) {
        let incoming = text.map(wide_c_str);
        if incoming.unwrap_or(&[]) == self.text.as_deref().unwrap_or(&[]) {
            return;
        }
        if self.text.as_deref().is_some_and(|t| !t.is_empty()) {
            self.text = None;
            self.release_children(sys, 0);
            self.children.clear();
            self.layout_gen = 0;
        }
        match incoming {
            Some(t) => self.text = Some(t.to_vec()),
            None => {
                if let Some(t) = &mut self.text {
                    t.clear();
                }
            }
        }
        self.layout_gen = 0;
    }

    /// `FontStringSetScale`: the wrap key from a depth (`param2`, its
    /// magnitude, 1 below 0.001) and a world width (`param1`) through the
    /// scene camera's x scale and near plane:
    /// `floor((x_scale / z_near) * (z_near / depth) * param1 + 0.5)`.
    pub fn set_scale(&mut self, param1: f32, param2: f32, camera_x_scale: f32, camera_z_near: f32) {
        let mut depth = param2.abs();
        if depth < 0.001 {
            depth = 1.0;
        }
        let term = (f64::from(camera_x_scale) / f64::from(camera_z_near))
            * (f64::from(camera_z_near) / f64::from(depth))
            * f64::from(param1)
            + 0.5;
        let key = trunc_to_i32(term.floor());
        if key != self.scale_key {
            self.scale_key = key;
            self.layout_gen = 0;
        }
    }

    /// `FontStringEnsureChild`: the child at `index` of `kind`, reused when
    /// the slot's kind byte matches and it has a child, else made new and
    /// bound to the font. A slot in range takes the new child (its kind
    /// byte unchanged); otherwise a slot is appended at the end, whatever
    /// the index. Returns the child's slot.
    pub fn ensure_child(&mut self, sys: &mut FontSystem, index: usize, kind: u8) -> usize {
        if let Some(slot) = self.children.get_mut(index) {
            if slot.kind == kind && slot.child.is_some() {
                return index;
            }
            if slot.kind != kind
                && let Some(mut old) = slot.child.take()
            {
                old.release(sys);
            }
        }
        let mut child = if kind != 0 {
            Child::Number(NumberStringChild::new())
        } else {
            Child::Text(FontStringChild::new())
        };
        child.bind_font(sys, self.font);
        if let Some(slot) = self.children.get_mut(index) {
            slot.child = Some(child);
            return index;
        }
        self.children.push(ChildSlot {
            child: Some(child),
            kind,
        });
        self.children.len() - 1
    }

    /// `FontStringRebuild`: the layout stamped; with text and a font, the
    /// lines (word-wrapped at the wrap key when multi-line, trailing spaces
    /// cut to one character; else the whole text when at least one
    /// character fits the key) set on children 0.., the slots cut or padded
    /// to the line count, and the extents (widest width, summed heights).
    pub fn rebuild(&mut self, sys: &mut FontSystem, pipeline: &mut dyn TexturePipeline) {
        self.layout_gen = sys.env.layout_generation;
        let Some(text) = self.text.clone().filter(|t| !t.is_empty()) else {
            return;
        };
        let Some(font) = self.font else {
            return;
        };
        let mut line_count = 0usize;
        if self.multi_line {
            let max_lines = self.max_lines.cast_unsigned().min(0x400);
            let wrap_width = f64::from(self.scale_key.cast_unsigned()) as f32;
            let lines = word_wrap(sys, font, &text, wrap_width, max_lines.cast_signed());
            line_count = lines.len();
            for (i, mut line) in lines.into_iter().enumerate() {
                let at = self.ensure_child(sys, i, 0);
                while line.len() > 1 && line.last() == Some(&u16::from(b' ')) {
                    line.pop();
                }
                if let Some(child) = &mut self.children[at].child {
                    child.set_text(sys, Some(&line));
                }
            }
        } else {
            let fits = match sys.font_cx(font) {
                Some((f, mut cx)) => f.probe_string(&mut cx, self.scale_key, &text),
                None => 0,
            };
            if fits > 0 {
                line_count = 1;
                let at = self.ensure_child(sys, 0, 0);
                if let Some(child) = &mut self.children[at].child {
                    child.set_text(sys, Some(&text));
                }
            }
        }
        if self.children.len() < line_count {
            self.children.resize(
                line_count,
                ChildSlot {
                    child: None,
                    kind: 0,
                },
            );
        } else if line_count < self.children.len() {
            self.release_children(sys, line_count);
            self.children.truncate(line_count);
        }
        self.compute_extents(sys, pipeline);
    }

    fn compute_extents(&mut self, sys: &mut FontSystem, pipeline: &mut dyn TexturePipeline) {
        self.extent_w = 0;
        self.extent_h = 0;
        for slot in &mut self.children {
            let (w, h) = match &mut slot.child {
                Some(child) => child.get_size(sys, pipeline),
                None => (0, 0),
            };
            if self.extent_w.cast_unsigned() < w.cast_unsigned() {
                self.extent_w = w;
            }
            self.extent_h = self.extent_h.wrapping_add(h);
        }
    }

    /// `FontStringGetExtent`: rebuilt when stale.
    pub fn extent(
        &mut self,
        sys: &mut FontSystem,
        pipeline: &mut dyn TexturePipeline,
    ) -> (i32, i32) {
        if self.layout_gen != sys.env.layout_generation {
            self.rebuild(sys, pipeline);
        }
        (self.extent_w, self.extent_h)
    }

    /// `FontStringChildCount`: rebuilt when stale.
    pub fn child_count(
        &mut self,
        sys: &mut FontSystem,
        pipeline: &mut dyn TexturePipeline,
    ) -> usize {
        if self.layout_gen != sys.env.layout_generation {
            self.rebuild(sys, pipeline);
        }
        self.children.len()
    }

    /// `FontStringDrawChildren`: the wrap key from the box width
    /// (truncated), rebuilt when stale, then line `i` at `y + h * i`,
    /// aligned in the box (`DrawChildAligned`).
    pub fn draw_children(
        &mut self,
        sys: &mut FontSystem,
        cx: &mut PaintCx<'_>,
        origin: &mut DrawOrigin,
        trim_left: f32,
        trim_right: f32,
    ) {
        let saved_x = origin.x;
        let saved_y = origin.y;
        let key = trunc_to_i32(f64::from(origin.width));
        if key != self.scale_key {
            self.scale_key = key;
            self.layout_gen = 0;
        }
        if self.layout_gen != sys.env.layout_generation {
            self.rebuild(sys, cx.pipeline);
        }
        let count = self.children.len();
        for i in 0..count {
            let Some(slot) = self.children.get_mut(i) else {
                continue;
            };
            let Some(child) = &mut slot.child else {
                continue;
            };
            let (_, h) = child.get_size(sys, cx.pipeline);
            let line_offset = h.wrapping_mul(i as i32).cast_unsigned();
            origin.x = saved_x;
            origin.y = (f64::from(line_offset) + f64::from(saved_y)) as f32;
            draw_child_aligned(sys, cx, origin, child, trim_left, trim_right);
        }
    }

    /// `FontStringSetNumberText`'s first step: a non-empty text dropped
    /// with its children.
    pub fn drop_text_for_number(&mut self, sys: &mut FontSystem) {
        if self.text.as_deref().is_some_and(|t| !t.is_empty()) {
            self.text = None;
            self.release_children(sys, 0);
            self.children.clear();
            self.layout_gen = 0;
        }
    }

    /// `FontStringSetNumberText`: a non-empty text dropped with its
    /// children, then child 0 made a number string printing `format`, and
    /// the extents from every child.
    pub fn set_number_text(
        &mut self,
        sys: &mut FontSystem,
        pipeline: &mut dyn TexturePipeline,
        format: &[u8],
    ) {
        self.drop_text_for_number(sys);
        let at = self.ensure_child(sys, 0, 1);
        if let Some(Child::Number(child)) = &mut self.children[at].child {
            child.printf(format);
        }
        self.compute_extents(sys, pipeline);
    }
}

/// `DrawChildAligned`: the child's size placed in the origin's box, then
/// its paint at that point.
fn draw_child_aligned(
    sys: &mut FontSystem,
    cx: &mut PaintCx<'_>,
    origin: &DrawOrigin,
    child: &mut Child,
    trim_left: f32,
    trim_right: f32,
) {
    let (w, h) = child.get_size(sys, cx.pipeline);
    let x = match origin.h_mode {
        1 => {
            let span = f64::from(origin.width) - f64::from(w.cast_unsigned());
            (span * 0.5 + f64::from(origin.x)) as f32
        }
        2 => (f64::from(origin.width) + f64::from(origin.x) - f64::from(w)) as f32,
        _ => origin.x,
    };
    let y = match origin.v_mode {
        1 => {
            let span = f64::from(origin.height) - f64::from(h.cast_unsigned());
            (span * 0.5 + f64::from(origin.y)) as f32
        }
        2 => (f64::from(origin.height) + f64::from(origin.y) - f64::from(h)) as f32,
        _ => origin.y,
    };
    let local = DrawOrigin {
        x,
        y,
        width: f32::from_bits(w.cast_unsigned()),
        height: f32::from_bits(h.cast_unsigned()),
        h_mode: 0,
        v_mode: 0,
    };
    child.paint(sys, cx, &local, trim_left, trim_right);
}
