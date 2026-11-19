//! `BaseRichTextWin` and `RichTextWin` (`rich_text_win.cpp`): a control
//! that lays a display list out to its pixel width, draws it into a memory
//! surface through a `DeviceTRC`, copies the surface into a big image and
//! draws that over the control's projected screen rectangle; scrolling
//! moves the device's origin.
//!
//! The reference's GDI surface is a [`RichSurface`]; its `A1R5G5B5`
//! read-back goes into a [`BigImage`] whose tile draws are handed to the
//! host ([`RichTextCx::tiles`]). The vertical scroll bar the window makes
//! is a `UIScroll` child in the reference; the toolkit has no `UIScroll`
//! yet, so the window keeps the bar's state itself ([`ScrollBar`]) and the
//! bar's value changes come in through [`RichTextWin::on_scroll_bar`]
//! (divergence D422). The window's own mesh, its debug frame and the
//! tooltip feed are left to the toolkit's controls (P4.13).

use std::any::Any;

use neuron_core::image::{FormatMaps, PixelFormat};
use neuron_core::math::transform_xform;
use neuron_core::math::{Matrix, Transform, Vector};
use neuron_core::num::trunc_to_i32;

use crate::render::texture::big_image::{BigImage, BigImageDesc, Rect, TileDraw};
use crate::render::texture::pipeline::TexturePipeline;
use crate::text::font::FontSystem;
use crate::text::layout::device::{DeviceTrc, DeviceTrcCx, RichSurface};
use crate::text::layout::list::LayoutParams;
use crate::text::layout::{TextRenderContext, layout_rich_text};
use crate::text::rdl::{RdlNode, RenderDisplayList};
use crate::ui::control::{WinControl, base};
use crate::ui::tree::{DrawHost, NodeId, Payload, Property, Ui, VISIBLE, Widget};

/// What the host gives a rich-text window's draw: the pipeline's viewport
/// (`GetViewportRect`: x, y, width, height) and projection
/// (`GetProjection`, row by row as the reference reads it), the texture
/// pipeline and format maps the big image is made with, and where its
/// tile draws go (`ScreenQuadDrawBigImageTile`).
pub struct RichTextCx<'a> {
    pub viewport: [i32; 4],
    pub projection: [f32; 16],
    pub pipeline: &'a mut dyn TexturePipeline,
    pub formats: &'a FormatMaps,
    pub tiles: &'a mut Vec<TileDraw>,
}

/// The state of the vertical `UIScroll` the window makes (`ScrollV_%d`):
/// value, range, page and step.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ScrollBar {
    pub value: f32,
    pub min: f32,
    pub max: f32,
    pub page: f32,
    pub step: f32,
    /// The bar's width and height in the control's units.
    pub size: (f32, f32),
}

/// `RoundSurfaceSize`: a negative size becomes 1, 0 stays 0, a power of
/// two stays, anything else rounds up to the next one.
#[must_use]
pub fn round_surface_size(size: i32) -> i32 {
    if size < 0 {
        return 1;
    }
    let mut result: i32 = 1;
    let mut rest = size;
    let pow2 = size & size.wrapping_sub(1) == 0;
    while rest != 0 {
        rest >>= 1;
        result = result.wrapping_shl(1);
    }
    if pow2 { result >> 1 } else { result }
}

/// `BaseRichTextWin` (`+0x32c` to `+0x488`).
#[derive(Debug)]
pub struct BaseRichTextWin {
    pub control: WinControl,
    /// `+0x330`, `+0x334`: the size in the control's units.
    pub norm_width: f32,
    pub norm_height: f32,
    /// `+0x338`: the two corners in world space (x0, y0, x1, y1).
    pub world_rect: [f32; 4],
    /// `+0x348`: their screen rectangle.
    pub screen_rect: [i32; 4],
    pub pixel_width: i32,
    pub pixel_height: i32,
    pub scroll_x: i32,
    pub scroll_y: i32,
    /// `+0x374`: what the layout made, executed by the draw.
    pub layout_list: RenderDisplayList,
    /// `+0x380`.
    pub layout_height: i32,
    /// `+0x384`.
    pub trc: DeviceTrc,
    /// `+0x3d4`: the memory DC and its bitmap.
    pub surface: Option<RichSurface>,
    /// `+0x3e8`: the read-back's row pitch in bytes.
    pub pitch: i32,
    /// `+0x3ec`.
    pub image: Option<BigImage>,
    /// `+0x3f0`; the horizontal bar of `+0x3f4` is never made.
    pub scroll_bar_v: Option<ScrollBar>,
    pub scroll_bar_width: f32,
    pub scroll_bar_gap: f32,
    pub layout_dirty: bool,
    pub surface_dirty: bool,
    pub draw_frame: bool,
    pub frame_color: [u8; 4],
    pub auto_scrollbar: bool,
    /// `+0x408`: 0 scrolled, 1 top, 2 centred, 3 bottom.
    pub align_mode: i32,
    pub tooltip_enabled: bool,
    pub never_hit: bool,
    pub skip_upload: bool,
    pub alpha: f32,
    pub keep_scrolled_to_end: bool,
    pub attach_offset: Vector,
}

impl Default for BaseRichTextWin {
    fn default() -> Self {
        BaseRichTextWin::new()
    }
}

impl BaseRichTextWin {
    /// The constructor.
    #[must_use]
    pub fn new() -> BaseRichTextWin {
        BaseRichTextWin {
            control: WinControl::new(),
            norm_width: 0.0,
            norm_height: 0.0,
            world_rect: [0.0; 4],
            screen_rect: [0; 4],
            pixel_width: 0,
            pixel_height: 0,
            scroll_x: 0,
            scroll_y: 0,
            layout_list: RenderDisplayList::new(),
            layout_height: 0,
            trc: DeviceTrc::new(),
            surface: None,
            pitch: 0,
            image: None,
            scroll_bar_v: None,
            scroll_bar_width: 0.02,
            scroll_bar_gap: 0.002,
            layout_dirty: true,
            surface_dirty: true,
            draw_frame: false,
            frame_color: [0x00, 0xFF, 0x00, 0xFF],
            auto_scrollbar: false,
            align_mode: 0,
            tooltip_enabled: false,
            never_hit: false,
            skip_upload: false,
            alpha: 1.0,
            keep_scrolled_to_end: false,
            attach_offset: Vector::ZERO,
        }
    }

    /// `Create`'s resets after `Win3D::Create`: the rectangles, the attach
    /// offset, both dirty flags and a 0.4 by 0.3 size. The reference then
    /// projects the control (`EnsureSurface`), which here waits for the
    /// first draw (divergence D423).
    fn create_reset(&mut self) {
        self.world_rect = [0.0; 4];
        self.pixel_width = 0;
        self.pixel_height = 0;
        self.screen_rect = [0; 4];
        self.attach_offset = Vector::ZERO;
        self.layout_dirty = true;
        self.surface_dirty = true;
        self.norm_width = 0.4;
        self.norm_height = 0.3;
    }

    /// `HitTest`: the ray's point at the control's depth within the world
    /// rectangle below and right of the position.
    #[must_use]
    pub fn hit_test(&self, position: Vector, ray: Vector) -> bool {
        if self.never_hit {
            return false;
        }
        let t = f64::from(position.z) / f64::from(ray.z);
        let point_y = (t * f64::from(ray.y)) as f32;
        let delta_x = t * f64::from(ray.x) - f64::from(position.x);
        if delta_x < 0.0 {
            return false;
        }
        if !(delta_x <= f64::from(self.world_rect[2]) - f64::from(self.world_rect[0])) {
            return false;
        }
        let delta_y = f64::from(position.y) - f64::from(point_y);
        if delta_y < 0.0 {
            return false;
        }
        delta_y <= f64::from(self.world_rect[1]) - f64::from(self.world_rect[3])
    }

    /// `SetScrollOrigin`: each coordinate moves only to a new, non-negative
    /// value; a move puts the origin at minus the scroll, dirties the
    /// surface and sets the bars.
    pub fn set_scroll_origin(&mut self, x: i32, y: i32) {
        let mut changed = false;
        if x != self.scroll_x && x >= 0 {
            self.scroll_x = x;
            changed = true;
        }
        if y != self.scroll_y && y >= 0 {
            self.scroll_y = y;
        } else if !changed {
            return;
        }
        self.trc.state.origin_v = self.scroll_y.wrapping_neg();
        self.trc.state.origin_h = self.scroll_x.wrapping_neg();
        self.surface_dirty = true;
        if let Some(bar) = self.scroll_bar_v.as_mut() {
            bar.value = self.scroll_y as f32;
        }
    }

    /// `GetScrollOrigin`: the horizontal scroll, and the vertical one as
    /// the alignment reports it (`None` for a mode above 3, where the
    /// reference leaves its output alone).
    #[must_use]
    pub fn scroll_origin(&self) -> (i32, Option<i32>) {
        let y = match self.align_mode {
            0 => Some(self.scroll_y),
            1 => Some(0),
            2 => Some(-((self.pixel_height.wrapping_sub(self.layout_height)) / 2)),
            3 => Some(self.layout_height.wrapping_sub(self.pixel_height)),
            _ => None,
        };
        (self.scroll_x, y)
    }

    /// `SetAutoScrollbar`.
    pub fn set_auto_scrollbar(&mut self, auto: bool) {
        self.auto_scrollbar = auto;
        self.layout_dirty = true;
        self.surface_dirty = true;
    }

    /// `ScrollToEnd`.
    pub fn scroll_to_end(&mut self) {
        let y = if self.layout_height > self.pixel_height {
            self.layout_height - self.pixel_height
        } else {
            0
        };
        self.set_scroll_origin(self.scroll_x, y);
    }

    /// `ScrollToTop`.
    pub fn scroll_to_top(&mut self) {
        self.set_scroll_origin(self.scroll_x, 0);
    }

    /// `SetAlignMode`: the surface dirtied.
    pub fn set_align_mode(&mut self, mode: i32) {
        self.align_mode = mode;
        self.surface_dirty = true;
    }

    /// `MarkDirty`: a dirty layout always dirties the surface.
    pub fn mark_dirty(&mut self, surface: bool, layout: bool) {
        self.layout_dirty = self.layout_dirty || layout;
        if self.layout_dirty {
            self.surface_dirty = true;
            return;
        }
        self.surface_dirty = self.surface_dirty || surface;
    }

    /// `CreateScrollBarV`: the bar beside the text, its range the larger of
    /// the laid-out and the pixel height, a page of the pixel height and a
    /// step of a tenth of it.
    fn create_scroll_bar_v(&self) -> ScrollBar {
        let range = self.layout_height.max(self.pixel_height);
        ScrollBar {
            value: 0.0,
            min: 0.0,
            max: range as f32,
            page: self.pixel_height as f32,
            step: (self.pixel_height / 10) as f32,
            size: (self.scroll_bar_width, self.norm_height),
        }
    }

    /// `ShowScrollBarV`.
    pub fn show_scroll_bar_v(&mut self, show: bool) {
        if show {
            if self.scroll_bar_v.is_none() {
                let mut bar = self.create_scroll_bar_v();
                bar.value = self.scroll_y as f32;
                self.scroll_bar_v = Some(bar);
            }
        } else {
            self.scroll_bar_v = None;
        }
    }

    /// `SetNormSize`: no dirty flags (the next draw's projection notices),
    /// the bar resized.
    pub fn set_norm_size(&mut self, width: f32, height: f32) {
        self.norm_height = height;
        self.norm_width = width;
        if let Some(bar) = self.scroll_bar_v.as_mut() {
            bar.size = (self.scroll_bar_width, height);
        }
    }

    /// `EnsureSurface`: the control's corners (0, 0, 0) and (width,
    /// -height, 0) through its transform and the projection into the
    /// screen rectangle; a surface of the rectangle's size rounded up to
    /// powers of two, with a big image of zeros of that size, when the
    /// rounded size changed; both dirty flags when the pixel size changed.
    /// Nothing changes when a corner projects to w = 0.
    pub fn ensure_surface(&mut self, transform: &Transform, cx: &mut RichTextCx<'_>) {
        let m = &cx.projection;
        let [view_x, view_y, view_w, view_h] = cx.viewport;
        let row = |r: usize, w: Vector| {
            let mut sum = f64::from(m[r * 4 + 2]) * f64::from(w.z);
            sum += f64::from(m[r * 4 + 1]) * f64::from(w.y);
            sum += f64::from(m[r * 4]) * f64::from(w.x);
            sum += f64::from(m[r * 4 + 3]);
            sum as f32
        };
        let world0 = transform_xform(transform, Vector::ZERO);
        let clip0 = [
            row(0, world0),
            row(1, world0),
            row(2, world0),
            row(3, world0),
        ];
        if clip0[3] == 0.0 {
            return;
        }
        let inv_w0 = 1.0 / f64::from(clip0[3]);
        let ndc_x0 = (f64::from(clip0[0]) * inv_w0) as f32;
        let ndc_y0 = (inv_w0 * f64::from(clip0[1])) as f32;
        let corner1 = Vector {
            x: self.norm_width,
            y: -self.norm_height,
            z: 0.0,
        };
        let world1 = transform_xform(transform, corner1);
        let clip1_x = row(0, world1);
        let clip1_y = row(1, world1);
        let _clip1_z = row(2, world1);
        let clip1_w = {
            let mut sum = f64::from(m[13]) * f64::from(world1.y);
            sum += f64::from(m[12]) * f64::from(world1.x);
            sum += f64::from(m[14]) * f64::from(world1.z);
            sum += f64::from(m[15]);
            sum as f32
        };
        if clip1_w == 0.0 {
            return;
        }
        let inv_w1 = 1.0 / f64::from(clip1_w);
        self.world_rect = [world0.x, world0.y, world1.x, world1.y];
        let ndc_x1 = f64::from(clip1_x) * inv_w1;
        let ndc_y1 = (inv_w1 * f64::from(clip1_y)) as f32;
        let width_d = f64::from(view_w);
        let x_d = f64::from(view_x);
        let height_f = f64::from(view_h as f32);
        let y_f = f64::from(view_y as f32);
        let left = trunc_to_i32((1.0 - f64::from(ndc_x0)) * width_d * 0.5 + x_d);
        let top = trunc_to_i32((f64::from(ndc_y0) + 1.0) * height_f * 0.5 + y_f);
        let right = trunc_to_i32((1.0 - ndc_x1) * width_d * 0.5 + x_d);
        let bottom = trunc_to_i32((f64::from(ndc_y1) + 1.0) * height_f * 0.5 + y_f);
        let (old_w, old_h) = (self.pixel_width, self.pixel_height);
        self.screen_rect = [left, top, right, bottom];
        self.pixel_width = right.wrapping_sub(left);
        self.pixel_height = bottom.wrapping_sub(top);

        let surface_w = round_surface_size(self.pixel_width);
        let surface_h = round_surface_size(self.pixel_height);
        let current = self.surface.as_ref().map(|s| (s.width, s.height));
        if surface_h > 0 && surface_w > 0 && current != Some((surface_w, surface_h)) {
            self.surface = Some(RichSurface::new(surface_w, surface_h));
            self.pitch = surface_w.wrapping_mul(2);
            self.surface_dirty = true;
            if self.pitch & 3 != 0 {
                self.pitch = self.pitch - (self.pitch & 3) + 4;
            }
            let pixels = vec![0u8; (self.pitch as usize) * (surface_h as usize)];
            let desc = BigImageDesc {
                src_format: PixelFormat::A1R5G5B5,
                dst_format: PixelFormat::A1R5G5B5,
                pixels: &pixels,
                width: surface_w as u32,
                height: surface_h as u32,
                pitch: self.pitch as u32,
            };
            if let Ok(image) = BigImage::create(&desc, cx.formats, cx.pipeline) {
                if let Some(mut old) = self.image.take() {
                    old.destroy(cx.pipeline);
                }
                self.image = Some(image);
            }
        }
        if old_w != self.pixel_width || old_h != self.pixel_height {
            self.layout_dirty = true;
            self.surface_dirty = true;
        }
    }

    /// `RebuildLayoutIfDirty`: with a surface and a dirty layout, the list
    /// laid out again (`relayout`, from the default attributes at the pen
    /// (0, 0), the height starting at the pixel height), the automatic bar
    /// made or dropped, the bar's range set, the view kept at the end when
    /// it was within 60 pixels of it.
    pub fn rebuild_layout_if_dirty(
        &mut self,
        fonts: &mut FontSystem,
        content: Option<(&RenderDisplayList, bool)>,
    ) {
        if self.surface.is_none() || !self.layout_dirty {
            return;
        }
        self.trc.state.attr_stack.clear();
        self.trc.state.current = self.trc.state.default;
        self.trc.state.pos_v = 0;
        self.trc.state.pos_h = 0;
        self.layout_list.nodes.clear();
        self.layout_height = self.pixel_height;
        if let Some((source, wrap)) = content {
            // RichTextWin::Relayout.
            let mut out = RenderDisplayList::new();
            let mut ctx = DeviceTrcCx {
                trc: &mut self.trc,
                fonts: &fonts.rich,
                backend: fonts.backend.as_mut(),
                surface: self.surface.as_mut(),
            };
            let result = layout_rich_text(
                &mut ctx,
                &fonts.styles,
                source,
                &mut out,
                LayoutParams {
                    width: self.pixel_width.wrapping_sub(4),
                    origin_h: 2,
                    origin_v: 0,
                    single_line: false,
                    wrap,
                },
            );
            self.layout_list = out;
            self.layout_height = result.height;
        }
        if self.auto_scrollbar {
            if self.scroll_bar_v.is_some() {
                if self.layout_height <= self.pixel_height {
                    self.scroll_bar_v = None;
                }
            } else if self.layout_height > self.pixel_height {
                let mut bar = self.create_scroll_bar_v();
                bar.value = self.scroll_y as f32;
                self.scroll_bar_v = Some(bar);
            }
        }
        if let Some(bar) = self.scroll_bar_v.as_mut() {
            bar.min = 0.0;
            bar.max = self.layout_height.max(self.pixel_height) as f32;
        }
        if self.keep_scrolled_to_end {
            let overflow = self.layout_height.wrapping_sub(self.pixel_height);
            if f64::from(overflow) - f64::from(self.scroll_y) <= 60.0 {
                let y = if self.layout_height > self.pixel_height {
                    overflow
                } else {
                    0
                };
                self.set_scroll_origin(self.scroll_x, y);
            }
        }
        self.layout_dirty = false;
    }

    /// The surface half of `Draw`: when either flag is up and the window
    /// has pixels, the surface cleared, the bar's page and step set, the
    /// layout rebuilt, the origin set by the alignment, the laid list
    /// executed with drawing from the default attributes at (0, 0), and the
    /// read-back uploaded into the big image (which clears the surface
    /// flag unless the upload is skipped or fails).
    pub fn render(
        &mut self,
        fonts: &mut FontSystem,
        content: Option<(&RenderDisplayList, bool)>,
        pipeline: &mut dyn TexturePipeline,
    ) {
        if !(self.surface_dirty || self.layout_dirty)
            || self.pixel_width <= 0
            || self.pixel_height <= 0
        {
            return;
        }
        let Some(surface) = self.surface.as_mut() else {
            return;
        };
        surface.clear();
        if let Some(bar) = self.scroll_bar_v.as_mut() {
            bar.page = self.pixel_height as f32;
            bar.step = (self.pixel_height / 10) as f32;
        }
        self.rebuild_layout_if_dirty(fonts, content);
        match self.align_mode {
            1 => {
                self.trc.state.origin_v = 0;
                self.trc.state.origin_h = self.scroll_x.wrapping_neg();
            }
            2 => {
                self.trc.state.origin_v = self.pixel_height.wrapping_sub(self.layout_height) / 2;
                self.trc.state.origin_h = self.scroll_x.wrapping_neg();
            }
            3 => {
                self.trc.state.origin_v = self.pixel_height.wrapping_sub(self.layout_height);
                self.trc.state.origin_h = self.scroll_x.wrapping_neg();
            }
            _ => {}
        }
        let mut ctx = DeviceTrcCx {
            trc: &mut self.trc,
            fonts: &fonts.rich,
            backend: fonts.backend.as_mut(),
            surface: self.surface.as_mut(),
        };
        ctx.reset_to_default();
        ctx.trc_mut().pos_v = 0;
        ctx.trc_mut().pos_h = 0;
        self.layout_list.execute(&mut ctx, &fonts.styles, true);
        let Some(surface) = self.surface.as_ref() else {
            return;
        };
        let buffer = surface.to_a1r5g5b5(self.pitch.max(0) as usize);
        if !self.skip_upload
            && let Some(image) = self.image.as_mut()
            && image.set_from_buffer(
                &buffer,
                PixelFormat::A1R5G5B5,
                surface.width as u32,
                surface.height as u32,
                self.pitch as u32,
                pipeline,
            ) == 0
        {
            self.surface_dirty = false;
        }
    }

    /// The image half of `Draw`: the big image at the window's alpha in
    /// white, its top-left `width + 1` by `height + 1` pixels onto the
    /// screen rectangle grown by one.
    pub fn draw_image(&mut self) -> Vec<TileDraw> {
        let Some(image) = self.image.as_mut() else {
            return Vec::new();
        };
        image.set_alpha(self.alpha);
        image.set_color([1.0, 1.0, 1.0]);
        let [l, t, r, b] = self.screen_rect;
        let src = Rect {
            left: 0,
            top: 0,
            right: r.wrapping_sub(l).wrapping_add(1),
            bottom: b.wrapping_sub(t).wrapping_add(1),
        };
        let dest = Rect {
            left: l,
            top: t,
            right: r.wrapping_add(1),
            bottom: b.wrapping_add(1),
        };
        image.draw(Some(src), dest).1
    }
}

/// `RichTextWin`: a [`BaseRichTextWin`] over a source list of its own.
#[derive(Debug)]
pub struct RichTextWin {
    pub base: BaseRichTextWin,
    /// `+0x488`.
    pub content: RenderDisplayList,
    /// `+0x494`: the layout wraps at words (1 from the constructor).
    pub layout_flag: bool,
}

impl Default for RichTextWin {
    fn default() -> Self {
        RichTextWin::new()
    }
}

impl RichTextWin {
    /// The constructor: both dirty flags cleared, wrapping on.
    #[must_use]
    pub fn new() -> RichTextWin {
        let mut base = BaseRichTextWin::new();
        base.surface_dirty = false;
        base.layout_dirty = false;
        RichTextWin {
            base,
            content: RenderDisplayList::new(),
            layout_flag: true,
        }
    }

    /// `ClearContent`: only a non-empty list dirties the window.
    pub fn clear_content(&mut self) {
        if self.content.nodes.is_empty() {
            return;
        }
        self.content.nodes.clear();
        self.base.layout_dirty = true;
        self.base.surface_dirty = true;
    }

    /// `AppendContent`: a clone of every node of `source`; an empty
    /// source changes nothing.
    pub fn append_content(&mut self, source: &RenderDisplayList) {
        if source.nodes.is_empty() {
            return;
        }
        self.content
            .nodes
            .extend(source.nodes.iter().map(RdlNode::cloned));
        self.base.layout_dirty = true;
        self.base.surface_dirty = true;
    }

    /// `AppendNode`.
    pub fn append_node(&mut self, node: &RdlNode) {
        self.content.push(node.cloned());
        self.base.layout_dirty = true;
        self.base.surface_dirty = true;
    }

    /// `SetLayoutFlag`.
    pub fn set_layout_flag(&mut self, wrap: bool) {
        self.layout_flag = wrap;
    }

    /// `CreateWithText` after `Create`: the size, then one text node of the
    /// text up to its first NUL. No dirty flags beyond `Create`'s.
    pub fn set_text_content(&mut self, text: &[u16], width: f32, height: f32) {
        self.base.norm_width = width;
        self.base.norm_height = height;
        self.content
            .push(RdlNode::Text(crate::text::font::wide_c_str(text).to_vec()));
    }

    /// The scroll bar's value changed (slot 23, message `0xF005` from the
    /// bar): the vertical scroll follows it, truncated.
    pub fn on_scroll_bar(&mut self, value: f32) {
        let x = self.base.scroll_x;
        self.base
            .set_scroll_origin(x, trunc_to_i32(f64::from(value)));
    }

    /// `Draw` without the toolkit: the projection, the surface, the image.
    /// Returns nothing when the window has no surface yet.
    pub fn draw_with(
        &mut self,
        transform: &Transform,
        fonts: &mut FontSystem,
        cx: &mut RichTextCx<'_>,
    ) {
        self.base.ensure_surface(transform, cx);
        let content = (&self.content, self.layout_flag);
        self.base.render(fonts, Some(content), cx.pipeline);
        cx.tiles.extend(self.base.draw_image());
    }
}

impl Widget for RichTextWin {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    fn win_control(&self) -> Option<&WinControl> {
        Some(&self.base.control)
    }
    fn win_control_mut(&mut self) -> Option<&mut WinControl> {
        Some(&mut self.base.control)
    }

    /// `RichTextWin::Create`: `Win3D::Create` with no payload, the resets
    /// and the content emptied.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        let result = ui.base_create(id, parent);
        self.base.create_reset();
        self.content.nodes.clear();
        result
    }

    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        let position = ui.node(id).map_or(Vector::ZERO, |n| n.position);
        self.base.hit_test(position, ray)
    }

    /// `SetPosition`: the attach offset added.
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        let o = self.base.attach_offset;
        let moved = Vector {
            x: o.x + position.x,
            y: o.y + position.y,
            z: o.z + position.z,
        };
        base::set_position(ui, id, &mut self.base.control, moved);
    }

    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        base::set_orientation(ui, id, &mut self.base.control, orientation);
    }

    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        base::destroy(ui, id, &mut self.base.control);
    }

    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        base::on_delete(ui, id, &mut self.base.control);
    }

    /// `Draw`: visible windows only; the host must offer a rich-text
    /// context and the toolkit a font system, else only the children draw.
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        let Some(node) = ui.node(id) else { return };
        if node.flags & VISIBLE == 0 {
            return;
        }
        let transform = Transform {
            rotation: node.orientation,
            translation: node.position,
        };
        if let Some(fonts) = ui.fonts.as_mut()
            && let Some(mut cx) = host.rich_text_cx()
        {
            self.draw_with(&transform, fonts, &mut cx);
        }
        ui.draw_children(id, host);
    }

    /// Property 6 is `SetAttachOffset`: moved by the change of the offset.
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        if let Property::Offset(offset) = property {
            let old = self.base.attach_offset;
            self.base.attach_offset = offset;
            let position = ui.node(id).map_or(Vector::ZERO, |n| n.position);
            let dz = position.z - old.z;
            let moved = Vector {
                x: (f64::from(position.x) - f64::from(old.x) + f64::from(offset.x)) as f32,
                y: (f64::from(position.y) - f64::from(old.y) + f64::from(offset.y)) as f32,
                z: dz + offset.z,
            };
            base::set_position(ui, id, &mut self.base.control, moved);
            return 1;
        }
        base::property(ui, id, self, property)
    }
}

#[cfg(test)]
mod tests;
