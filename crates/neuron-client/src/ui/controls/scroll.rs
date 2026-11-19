//! `UIScroll` and `UICycle` (`ui_scroll.cpp`): the page descriptors' and
//! the rich-text windows' scroll bars, and the two-arrow value cycler.
//!
//! A scroll bar is a `WinControl` of `width` by `height` world units in its
//! own plane, laid out as five parts in local coordinates (y grows
//! downwards as negative values): the two arrows at the ends, the track
//! before and after the thumb, and the thumb, whose length is the page's
//! share of the range. The mouse ray is taken into the bar's plane through
//! the inverse of its transform. An arrow press steps the value and arms an
//! auto-repeat the draw runs; a track press pages; a thumb press drags; the
//! "step mode" owners set makes the thumb a fixed two-arrow-wide block over
//! a range one shorter. Every change tells the parent (slot 23 0xF005, then
//! property 0x3E with the value). The draw binds the bar's transform as the
//! model-view and draws textured triangles and quads of the
//! `UITextures.ini` shapes through the immediate-mode layer.
//!
//! `UICycle` is the same object drawing and taking only the arrows.

use std::any::Any;

use neuron_core::math::{Matrix, Transform, Vector, general_inverse, transform_xform};
use neuron_core::num::trunc_to_i32;

use crate::ui::control::{WinControl, base};
use crate::ui::immediate::{Immediate, primitive, render_state};
use crate::ui::message::MouseButton;
use crate::ui::page::EntryPayload;
use crate::ui::pulse::{Pulse, palette};
use crate::ui::shape::{ShapeDb, TexShape};
use crate::ui::tree::{
    ChildMessage, DrawHost, NodeId, Payload, Property, Ui, Widget, shape_handle,
};

/// The thumb's end-cap length along the track (bits 0x3B03126F).
pub const THUMB_CAP: f32 = 0.002;
/// The delay before a held arrow repeats.
pub const REPEAT_DELAY: f32 = 0.3;
/// `UICycle`'s repeat interval.
pub const CYCLE_REPEAT_INTERVAL: f32 = 0.1;
/// The step-mode track click: a tenth of the range.
pub const TRACK_STEP_FRACTION: f32 = 0.1;

/// The three shapes of `Interface\UI\UITextures.ini` the scroll bars draw.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ScrollShapes {
    /// "HUD_sliderarrow".
    pub arrow: Option<TexShape>,
    /// "HUD_slidertextopbottom".
    pub top_bottom: Option<TexShape>,
    /// "HUD_slidertexmid".
    pub mid: Option<TexShape>,
}

impl ScrollShapes {
    /// The shapes found in a loaded database (the first `UIScroll`
    /// constructor's lookups, divergence D432).
    #[must_use]
    pub fn from_db(db: &ShapeDb) -> ScrollShapes {
        ScrollShapes {
            arrow: db.find(b"HUD_sliderarrow").copied(),
            top_bottom: db.find(b"HUD_slidertextopbottom").copied(),
            mid: db.find(b"HUD_slidertexmid").copied(),
        }
    }
}

/// `FScrollHitRect`: a part in local coordinates.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct HitRect {
    pub x0: f32,
    pub y0: f32,
    pub x1: f32,
    pub y1: f32,
    pub hot: bool,
}

impl HitRect {
    /// `x0 <= x <= x1` and `y1 <= y <= y0`.
    #[must_use]
    pub fn contains(&self, x: f32, y: f32) -> bool {
        x >= self.x0 && x <= self.x1 && y <= self.y0 && y >= self.y1
    }
}

/// `0 <= x < width` and `-height < y <= 0`.
fn inside_box(x: f32, y: f32, width: f32, height: f32) -> bool {
    if !(x >= 0.0) || !(x < width) || !(y <= 0.0) {
        return false;
    }
    -height < y
}

/// `ScrollThumbSpan`: the thumb's position (unrounded) and length along a
/// track.
fn thumb_span(
    track: f32,
    range_min: f32,
    range_max: f32,
    value: f32,
    page: f32,
    step_mode: bool,
) -> (f64, f32) {
    if range_max == range_min {
        return (0.0, track);
    }
    let range = (f64::from(range_max) - f64::from(range_min)) as f32;
    let mut pos = (f64::from(value) - f64::from(range_min)) * f64::from(track) / f64::from(range);
    let mut thumb = (f64::from(track) * f64::from(page) / f64::from(range)) as f32;
    if thumb < 0.01 {
        thumb = 0.01;
    }
    if step_mode {
        thumb = 0.0;
    }
    if f64::from(thumb) + pos > f64::from(track) {
        pos = f64::from(track) - f64::from(thumb);
    }
    (pos, thumb)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// The bytes `Win3DColor4ubv` reads from a colour word.
fn rgba(color: u32) -> [u8; 4] {
    color.to_le_bytes()
}

/// The `UIScroll` object (`UICycle` too, with `cycle` set).
#[derive(Debug)]
pub struct UiScroll {
    pub control: WinControl,
    /// Whether this is a `UICycle` (its own draw and input).
    pub cycle: bool,
    /// `+0x32c`: gates the hover tint and the drag.
    pub active: bool,
    /// `+0x32d`: every part in `custom_color`.
    pub custom_color_on: bool,
    /// `+0x32e`: the grey from the constructor.
    pub custom_color: u32,
    pub width: f32,
    pub height: f32,
    pub top_arrow: HitRect,
    pub bottom_arrow: HitRect,
    pub track_before: HitRect,
    pub track_after: HitRect,
    pub thumb: HitRect,
    /// `min(width, height) * 0.2`.
    pub arrow_size: f32,
    /// `+0x3b4`: owners write it directly.
    pub step_mode: bool,
    /// `+0x3b8`: 0 the top arrow held, 1 the bottom one, -1 none.
    pub drag_part: i32,
    pub repeat_timer: f32,
    pub range_min: f32,
    pub range_max: f32,
    pub value: f32,
    pub page: f32,
    pub step: f32,
    pub dragging: bool,
    pub drag_offset: f32,
    /// `+0x3dc`: the inverse of the transform, from the last placement.
    pub inverse_layout: Transform,
    pub arrow_idle_color: u32,
    pub arrow_hot_color: u32,
    pub bar_idle_color: u32,
    pub bar_hot_color: u32,
    pub scroll_offset: Vector,
    /// `+0x428`: the page property 0x37 goes back to.
    pub default_page: f32,
    pub horizontal: bool,
}

impl Default for UiScroll {
    fn default() -> Self {
        UiScroll::new(false)
    }
}

impl UiScroll {
    /// The constructor (the `UITextures.ini` load is the client's,
    /// [`ScrollShapes`]).
    #[must_use]
    pub fn new(cycle: bool) -> UiScroll {
        UiScroll {
            control: WinControl::new(),
            cycle,
            active: true,
            custom_color_on: false,
            custom_color: palette::GREY,
            width: 0.0,
            height: 0.0,
            top_arrow: HitRect::default(),
            bottom_arrow: HitRect::default(),
            track_before: HitRect::default(),
            track_after: HitRect::default(),
            thumb: HitRect::default(),
            arrow_size: 0.0,
            step_mode: false,
            drag_part: -1,
            repeat_timer: 0.0,
            range_min: 0.0,
            range_max: 0.0,
            value: 0.0,
            page: 0.0,
            step: 0.0,
            dragging: false,
            drag_offset: 0.0,
            inverse_layout: Transform::IDENTITY,
            arrow_idle_color: palette::IDLE,
            arrow_hot_color: palette::IDLE,
            bar_idle_color: palette::IDLE,
            bar_hot_color: palette::IDLE,
            scroll_offset: Vector::ZERO,
            default_page: 0.0,
            horizontal: false,
        }
    }

    /// `Relayout`: the five parts from the size, range, value and page.
    pub fn relayout(&mut self) {
        let step = self.step_mode;
        let (w, h, arrow) = (
            f64::from(self.width),
            f64::from(self.height),
            f64::from(self.arrow_size),
        );
        if self.horizontal {
            let lead = r(h + arrow);
            let tail = w - h - arrow;
            let arrows = if step {
                self.range_max = r(f64::from(self.range_max) - 1.0);
                h * 4.0
            } else {
                h + h
            };
            let track = r((w - arrows) - (arrow + arrow));
            let (pos, thumb) = thumb_span(
                track,
                self.range_min,
                self.range_max,
                self.value,
                self.page,
                step,
            );
            let lead64 = f64::from(lead);
            if step {
                self.thumb.x1 = r(h + h + pos + lead64);
                self.thumb.x0 = r(pos + lead64);
            } else {
                self.thumb.x0 = r(lead64 + pos);
                self.thumb.x1 = r(f64::from(thumb) + pos + lead64);
            }
            self.thumb.y0 = 0.0;
            self.thumb.y1 = -self.height;
            self.top_arrow = HitRect {
                x0: 0.0,
                y0: 0.0,
                x1: self.height,
                y1: -self.height,
                hot: self.top_arrow.hot,
            };
            self.bottom_arrow = HitRect {
                x0: r(w - h),
                y0: 0.0,
                x1: self.width,
                y1: -self.height,
                hot: self.bottom_arrow.hot,
            };
            self.track_before = HitRect {
                x0: lead,
                y0: 0.0,
                x1: self.thumb.x0,
                y1: -self.height,
                hot: self.track_before.hot,
            };
            self.track_after = HitRect {
                x0: self.thumb.x1,
                y0: 0.0,
                x1: r(tail),
                y1: -self.height,
                hot: self.track_after.hot,
            };
        } else {
            let lead = r(w + arrow);
            let tail = h - w - arrow;
            let arrows = if step {
                self.range_max = r(f64::from(self.range_max) - 1.0);
                w * 4.0
            } else {
                w + w
            };
            let track = r((h - arrows) - (arrow + arrow));
            let (pos, thumb) = thumb_span(
                track,
                self.range_min,
                self.range_max,
                self.value,
                self.page,
                step,
            );
            let lead64 = f64::from(lead);
            if step {
                self.thumb.x1 = self.width;
                self.thumb.y1 = r(-(w + w + pos + lead64));
                self.thumb.y0 = r(-(pos + lead64));
            } else {
                self.thumb.y0 = r(-(pos + lead64));
                self.thumb.x1 = self.width;
                self.thumb.y1 = r(-(f64::from(thumb) + pos + lead64));
            }
            self.thumb.x0 = 0.0;
            self.top_arrow = HitRect {
                x0: 0.0,
                y0: 0.0,
                x1: self.width,
                y1: -self.width,
                hot: self.top_arrow.hot,
            };
            self.bottom_arrow = HitRect {
                x0: 0.0,
                y0: r(-(h - w)),
                x1: self.width,
                y1: -self.height,
                hot: self.bottom_arrow.hot,
            };
            self.track_before = HitRect {
                x0: 0.0,
                y0: -lead,
                x1: self.width,
                y1: self.thumb.y0,
                hot: self.track_before.hot,
            };
            self.track_after = HitRect {
                x0: 0.0,
                y0: self.thumb.y1,
                x1: self.width,
                y1: r(-tail),
                hot: self.track_after.hot,
            };
        }
        if self.step_mode {
            self.range_max = r(f64::from(self.range_max) + 1.0);
        }
    }

    /// `ClampValues`: min at most max, the page within the span (1 in step
    /// mode) and not negative, the value within `[min, max - page]`.
    pub fn clamp_values(&mut self) {
        if self.range_min > self.range_max {
            self.range_min = self.range_max;
        }
        let span = f64::from(self.range_max) - f64::from(self.range_min);
        if span < f64::from(self.page) {
            self.page = if self.step_mode { 1.0 } else { r(span) };
        }
        if self.page < 0.0 {
            self.page = 0.0;
        }
        if self.value < self.range_min {
            self.value = self.range_min;
        }
        let last = f64::from(self.range_max) - f64::from(self.page);
        if last < f64::from(self.value) {
            self.value = r(last);
        }
    }

    /// `SetRange`.
    pub fn set_range(&mut self, min: f32, max: f32) {
        self.range_min = min;
        self.range_max = max;
        self.clamp_values();
        self.relayout();
    }

    /// `SetValue`.
    pub fn set_value(&mut self, value: f32) {
        self.value = value;
        self.clamp_values();
        self.relayout();
    }

    /// `SetPage`: also the default page.
    pub fn set_page(&mut self, page: f32) {
        self.page = page;
        self.default_page = page;
        self.clamp_values();
        self.relayout();
    }

    /// `SetStep`.
    pub fn set_step(&mut self, step: f32) {
        self.step = step;
        self.clamp_values();
        self.relayout();
    }

    /// `SetSize`: the arrows a fifth of the shorter side.
    pub fn set_size(&mut self, width: f32, height: f32) {
        self.width = width;
        self.height = height;
        let shorter = if height > width { width } else { height };
        self.arrow_size = r(f64::from(shorter) * 0.2);
        self.relayout();
    }

    /// `SetArrowColors`.
    pub fn set_arrow_colors(&mut self, idle: u32, hot: u32) {
        self.arrow_idle_color = idle;
        self.arrow_hot_color = hot;
    }

    /// `SetBarColors`.
    pub fn set_bar_colors(&mut self, idle: u32, hot: u32) {
        self.bar_idle_color = idle;
        self.bar_hot_color = hot;
    }

    /// The inverse of the node's transform into `+0x3dc`.
    fn refresh_inverse(&mut self, ui: &Ui, id: NodeId) {
        if let Some(n) = ui.node(id) {
            self.inverse_layout =
                general_inverse(&Transform::new(n.orientation, n.position)).transform;
        }
    }

    /// `SetScrollOffset`: moved by the offset's change through
    /// `WinControl::SetPosition` (the inverse is not refreshed).
    pub fn set_scroll_offset(&mut self, ui: &mut Ui, id: NodeId, offset: Vector) {
        let old = self.scroll_offset;
        self.scroll_offset = offset;
        let p = ui.node(id).map_or(Vector::ZERO, |n| n.position);
        let delta_z = r(f64::from(p.z) - f64::from(old.z));
        let moved = Vector::new(
            r(f64::from(p.x) - f64::from(old.x) + f64::from(offset.x)),
            r(f64::from(p.y) - f64::from(old.y) + f64::from(offset.y)),
            r(f64::from(delta_z) + f64::from(offset.z)),
        );
        base::set_position(ui, id, &mut self.control, moved);
    }

    /// Property 6: the scroll offset, each difference and sum rounded,
    /// then the inverse refreshed.
    fn property_scroll_offset(&mut self, ui: &mut Ui, id: NodeId, offset: Vector) {
        let old = self.scroll_offset;
        self.scroll_offset = offset;
        let p = ui.node(id).map_or(Vector::ZERO, |n| n.position);
        let base_x = r(f64::from(p.x) - f64::from(old.x));
        let base_y = r(f64::from(p.y) - f64::from(old.y));
        let base_z = r(f64::from(p.z) - f64::from(old.z));
        let moved = Vector::new(
            r(f64::from(base_x) + f64::from(offset.x)),
            r(f64::from(base_y) + f64::from(offset.y)),
            r(f64::from(base_z) + f64::from(offset.z)),
        );
        base::set_position(ui, id, &mut self.control, moved);
        self.refresh_inverse(ui, id);
    }

    /// The ray on the bar's plane in local coordinates.
    #[must_use]
    pub fn ray_to_local(&self, position: Vector, ray: Vector) -> Vector {
        let scale = f64::from(position.z) / f64::from(ray.z);
        let scaled = Vector::new(
            r(f64::from(ray.x) * scale),
            r(f64::from(ray.y) * scale),
            r(f64::from(ray.z) * scale),
        );
        transform_xform(&self.inverse_layout, scaled)
    }

    /// The parent told of a change: slot 23 0xF005, then property 0x3E with
    /// the truncated value when it is a `WinControl`.
    fn notify_parent(&self, ui: &mut Ui, id: NodeId) {
        let Some(parent) = ui.parent(id) else { return };
        ui.send_child_message(parent, &ChildMessage::ScrollChanged { from: id });
        if ui.win_control(parent).is_some() {
            let value = trunc_to_i32(f64::from(self.value));
            ui.property(parent, Property::ScrollChanged(id, value));
        }
    }

    /// The value moved, clamped, laid out and announced.
    fn apply_value(&mut self, ui: &mut Ui, id: NodeId, value: f64) {
        self.value = r(value);
        self.clamp_values();
        self.relayout();
        self.notify_parent(ui, id);
    }

    fn position(ui: &Ui, id: NodeId) -> Vector {
        ui.node(id).map_or(Vector::ZERO, |n| n.position)
    }

    /// `UIScroll::Update`: the children, then the hot flags of both arrows
    /// and the thumb from the mouse ray; a dragged thumb stays hot.
    pub fn update_hot(&mut self, ui: &Ui, id: NodeId) {
        let local = self.ray_to_local(Self::position(ui, id), ui.mouse_ray());
        for rect in [&mut self.top_arrow, &mut self.bottom_arrow, &mut self.thumb] {
            rect.hot = rect.contains(local.x, local.y);
        }
        if self.dragging {
            self.thumb.hot = true;
        }
    }

    /// The model-view and the seven render states both draws open with.
    fn begin_draw(&self, imm: &mut dyn Immediate, transform: &Transform) {
        imm.bind_modelview(transform);
        imm.set_render_state(render_state::ZENABLE, 0);
        imm.set_render_state(render_state::ZWRITEENABLE, 0);
        imm.set_render_state(render_state::CULLMODE, 1);
        imm.set_render_state(render_state::LIGHTING, 0);
        imm.set_render_state(render_state::SRCBLEND, 5);
        imm.set_render_state(render_state::DESTBLEND, 6);
        imm.set_render_state(render_state::ALPHABLENDENABLE, 1);
    }

    /// The arrow's fill: the bar's hot colour pulsed while hot, else the
    /// arrow's hot colour.
    fn arrow_fill(&self, rect: &HitRect, pulse: &Pulse) -> u32 {
        if rect.hot {
            pulse.blend_from(self.bar_hot_color)
        } else {
            self.arrow_hot_color
        }
    }

    /// `DrawArrowVertical` and `DrawArrowHorizontal`: the arrow triangle
    /// (pointing up or left when `first`), leaving the bar's idle colour
    /// (hot) or the arrow's idle colour current.
    fn draw_arrow(
        &self,
        imm: &mut dyn Immediate,
        texture: u32,
        rect: &HitRect,
        first: bool,
        pulse: &Pulse,
    ) {
        imm.bind_texture(texture);
        imm.color4ubv(rgba(self.arrow_fill(rect, pulse)));
        imm.begin(primitive::TRIANGLES);
        if self.horizontal {
            let mid_y = r((f64::from(rect.y0) + f64::from(rect.y1)) * 0.5);
            if first {
                imm.tex_coord(1.0, 1.0);
                imm.vertex(rect.x1, rect.y0, 0.0);
                imm.tex_coord(0.0, 1.0);
                imm.vertex(rect.x1, rect.y1, 0.0);
                imm.tex_coord(1.0, 0.0);
                imm.vertex(rect.x0, mid_y, 0.0);
            } else {
                imm.tex_coord(1.0, 0.0);
                imm.vertex(rect.x0, rect.y0, 0.0);
                imm.tex_coord(1.0, 1.0);
                imm.vertex(rect.x0, rect.y1, 0.0);
                imm.tex_coord(0.0, 1.0);
                imm.vertex(rect.x1, mid_y, 0.0);
            }
        } else {
            let mid_x = r((f64::from(rect.x1) + f64::from(rect.x0)) * 0.5);
            if first {
                imm.tex_coord(1.0, 1.0);
                imm.vertex(rect.x1, rect.y1, 0.0);
                imm.tex_coord(0.0, 1.0);
                imm.vertex(rect.x0, rect.y1, 0.0);
                imm.tex_coord(1.0, 0.0);
                imm.vertex(mid_x, rect.y0, 0.0);
            } else {
                imm.tex_coord(0.0, 1.0);
                imm.vertex(rect.x0, rect.y0, 0.0);
                imm.tex_coord(1.0, 0.0);
                imm.vertex(rect.x1, rect.y0, 0.0);
                imm.tex_coord(1.0, 1.0);
                imm.vertex(mid_x, rect.y1, 0.0);
            }
        }
        imm.end();
        let after = if rect.hot {
            self.bar_idle_color
        } else {
            self.arrow_idle_color
        };
        imm.color4ubv(rgba(after));
    }

    fn quad(imm: &mut dyn Immediate, texture: u32, corners: [(f32, f32, f32, f32); 4]) {
        imm.bind_texture(texture);
        imm.begin(primitive::QUADS);
        for (u, v, x, y) in corners {
            imm.tex_coord(u, v);
            imm.vertex(x, y, 0.0);
        }
        imm.end();
    }

    fn line_loop(imm: &mut dyn Immediate, color: u32, corners: [(f32, f32); 4]) {
        imm.color4ubv(rgba(color));
        imm.begin(primitive::LINE_LOOP);
        for (x, y) in corners {
            imm.vertex(x, y, 0.0);
        }
        imm.end();
    }

    /// The textures of the draw: the arrow, the caps and the middle.
    fn textures(ui: &Ui, host: &mut dyn DrawHost) -> [u32; 3] {
        let shapes = ui.ui_shapes;
        [
            shape_handle(host, shapes.arrow.as_ref()),
            shape_handle(host, shapes.top_bottom.as_ref()),
            shape_handle(host, shapes.mid.as_ref()),
        ]
    }

    /// The immediate-mode part of `UIScroll::Draw` (after the option gate
    /// and the colour refresh).
    pub fn draw_bar(
        &self,
        imm: &mut dyn Immediate,
        transform: &Transform,
        textures: [u32; 3],
        pulse: &Pulse,
    ) {
        let [arrow, caps, mid] = textures;
        self.begin_draw(imm, transform);
        let (f0, f1) = (self.track_before, self.track_after);
        self.draw_arrow(imm, arrow, &self.bottom_arrow, false, pulse);
        self.draw_arrow(imm, arrow, &self.top_arrow, true, pulse);
        if !self.horizontal {
            Self::line_loop(
                imm,
                self.arrow_idle_color,
                [
                    (f1.x0, f0.y0),
                    (f1.x1, f0.y0),
                    (f1.x1, f1.y1),
                    (f1.x0, f1.y1),
                ],
            );
        }
        let fill = if self.thumb.hot && self.active {
            pulse.blend_from(self.bar_hot_color)
        } else {
            self.arrow_hot_color
        };
        imm.color4ubv(rgba(fill));
        let t = self.thumb;
        if self.horizontal {
            let lead = r(f64::from(t.x0) + f64::from(THUMB_CAP));
            let tail = r(f64::from(t.x1) - f64::from(THUMB_CAP));
            Self::quad(
                imm,
                mid,
                [
                    (1.0, 0.0, lead, t.y0),
                    (1.0, 1.0, tail, t.y0),
                    (0.0, 1.0, tail, t.y1),
                    (0.0, 0.0, lead, t.y1),
                ],
            );
            Self::quad(
                imm,
                caps,
                [
                    (1.0, 1.0, lead, t.y0),
                    (1.0, 1.0, t.x0, t.y0),
                    (1.0, 0.75, t.x0, t.y1),
                    (1.0, 0.75, lead, t.y1),
                ],
            );
            Self::quad(
                imm,
                caps,
                [
                    (0.0, 0.25, tail, t.y0),
                    (1.0, 0.25, t.x1, t.y0),
                    (1.0, 0.0, t.x1, t.y1),
                    (0.0, 0.0, tail, t.y1),
                ],
            );
            Self::line_loop(
                imm,
                self.arrow_idle_color,
                [
                    (f0.x0, f1.y0),
                    (f1.x1, f1.y0),
                    (f1.x1, f1.y1),
                    (f0.x0, f1.y1),
                ],
            );
        } else {
            let lead = r(f64::from(t.y0) - f64::from(THUMB_CAP));
            let tail = r(f64::from(t.y1) + f64::from(THUMB_CAP));
            Self::quad(
                imm,
                mid,
                [
                    (0.0, 0.0, t.x0, lead),
                    (1.0, 0.0, t.x1, lead),
                    (1.0, 1.0, t.x1, tail),
                    (0.0, 1.0, t.x0, tail),
                ],
            );
            Self::quad(
                imm,
                caps,
                [
                    (1.0, 1.0, t.x0, lead),
                    (1.0, 1.0, t.x1, lead),
                    (1.0, 0.75, t.x1, t.y0),
                    (1.0, 0.75, t.x0, t.y0),
                ],
            );
            Self::quad(
                imm,
                caps,
                [
                    (0.0, 0.25, t.x0, t.y1),
                    (1.0, 0.25, t.x1, t.y1),
                    (1.0, 0.0, t.x1, tail),
                    (0.0, 0.0, t.x0, tail),
                ],
            );
        }
    }

    /// `UICycle::Draw`'s arrows.
    pub fn draw_cycle_arrows(
        &self,
        imm: &mut dyn Immediate,
        transform: &Transform,
        arrow: u32,
        pulse: &Pulse,
    ) {
        self.begin_draw(imm, transform);
        self.draw_arrow(imm, arrow, &self.bottom_arrow, false, pulse);
        self.draw_arrow(imm, arrow, &self.top_arrow, true, pulse);
    }

    /// The colours `UIScroll::Draw` rewrites each frame.
    pub fn refresh_colors(&mut self) {
        let c = if self.custom_color_on {
            self.custom_color
        } else {
            palette::IDLE
        };
        self.arrow_idle_color = c;
        self.arrow_hot_color = c;
        self.bar_idle_color = c;
        self.bar_hot_color = c;
    }

    /// The held-arrow auto-repeat at the end of `UIScroll::Draw`: below
    /// zero the value steps each frame; the timer counts down either way.
    pub fn scroll_repeat(&mut self, ui: &mut Ui, id: NodeId, frame_time: f64) {
        if self.drag_part == -1 {
            return;
        }
        if self.repeat_timer < 0.0 {
            match self.drag_part {
                0 => self.apply_value(ui, id, f64::from(self.value) - f64::from(self.step)),
                1 => self.apply_value(ui, id, f64::from(self.step) + f64::from(self.value)),
                _ => {}
            }
        }
        self.repeat_timer = r(f64::from(self.repeat_timer) - frame_time);
    }

    /// The value stepped with the range one shorter around the clamp in
    /// step mode (`UICycle`).
    fn cycle_apply(&mut self, ui: &mut Ui, id: NodeId, value: f64) {
        self.value = r(value);
        if self.step_mode {
            self.range_max = r(f64::from(self.range_max) - 1.0);
        }
        self.clamp_values();
        if self.step_mode {
            self.range_max = r(f64::from(self.range_max) + 1.0);
        }
        self.relayout();
        self.notify_parent(ui, id);
    }

    /// `UICycle::Draw`'s repeat: every 0.1 seconds once the delay ran out.
    pub fn cycle_repeat(&mut self, ui: &mut Ui, id: NodeId, frame_time: f64) {
        if self.drag_part == -1 {
            return;
        }
        if !(self.repeat_timer < 0.0) {
            self.repeat_timer = r(f64::from(self.repeat_timer) - frame_time);
            return;
        }
        let value = match self.drag_part {
            0 => f64::from(self.value) - f64::from(self.step),
            1 => f64::from(self.step) + f64::from(self.value),
            _ => {
                self.repeat_timer = CYCLE_REPEAT_INTERVAL;
                return;
            }
        };
        self.cycle_apply(ui, id, value);
        self.repeat_timer = CYCLE_REPEAT_INTERVAL;
    }

    /// `UIScroll::MouseOver`: the dragged thumb maps the mouse along the
    /// track to a value.
    pub fn drag(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) {
        if !self.dragging || !self.active {
            return;
        }
        let local = self.ray_to_local(Self::position(ui, id), ray);
        let range = f64::from(self.range_max) - f64::from(self.range_min);
        let (numerator, denominator) = if self.horizontal {
            let along = if self.step_mode {
                f64::from(local.x)
            } else {
                f64::from(local.x) + f64::from(self.drag_offset)
            };
            (
                range * (f64::from(self.track_before.x0) - along),
                f64::from(self.track_before.x0) - f64::from(self.track_after.x1),
            )
        } else {
            let along = if self.step_mode {
                f64::from(local.y)
            } else {
                f64::from(local.y) + f64::from(self.drag_offset)
            };
            (
                range * (f64::from(self.track_before.y0) - along),
                f64::from(self.track_before.y0) - f64::from(self.track_after.y1),
            )
        };
        let mut new_value = numerator / denominator + f64::from(self.range_min);
        if self.step_mode {
            new_value = f64::from(trunc_to_i32(new_value));
        }
        if new_value == f64::from(self.value) {
            return;
        }
        self.apply_value(ui, id, new_value);
    }

    /// `UIScroll::MouseEvent`.
    fn scroll_mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        if !self.active || event.field0() != -1 {
            return false;
        }
        let local = self.ray_to_local(Self::position(ui, id), ui.mouse_ray());
        self.drag_part = -1;
        let kind = event.field1();
        let press = kind == 2 || kind == 3;
        if !press
            && let Some(parent) = ui.parent(id)
            && ui.win_control(parent).is_some()
        {
            ui.property(parent, Property::ChildButtonUp(id));
        }
        let mut handled = false;
        if inside_box(local.x, local.y, self.width, self.height) {
            if self.thumb.contains(local.x, local.y) {
                if press {
                    self.dragging = true;
                    self.drag_offset = if self.horizontal {
                        r(f64::from(self.thumb.x0) - f64::from(local.x))
                    } else {
                        r(f64::from(self.thumb.y0) - f64::from(local.y))
                    };
                    if ui.capture() != Some(id) {
                        ui.push_capture(id);
                    }
                }
            } else if press {
                let value = f64::from(self.value);
                let span = f64::from(self.range_max) - f64::from(self.range_min);
                let fraction = f64::from(TRACK_STEP_FRACTION);
                let new_value = if self.top_arrow.contains(local.x, local.y) {
                    self.drag_part = 0;
                    self.repeat_timer = REPEAT_DELAY;
                    Some(value - f64::from(self.step))
                } else if self.bottom_arrow.contains(local.x, local.y) {
                    self.drag_part = 1;
                    self.repeat_timer = REPEAT_DELAY;
                    Some(f64::from(self.step) + value)
                } else if self.track_before.contains(local.x, local.y) {
                    Some(if self.step_mode {
                        let jump = f64::from(trunc_to_i32(value - span * fraction));
                        if jump != value { jump } else { value - 1.0 }
                    } else {
                        value - f64::from(self.page)
                    })
                } else if self.track_after.contains(local.x, local.y) {
                    Some(if self.step_mode {
                        let jump = f64::from(trunc_to_i32(span * fraction + value));
                        if jump != value { jump } else { value + 1.0 }
                    } else {
                        f64::from(self.page) + value
                    })
                } else {
                    None
                };
                if let Some(v) = new_value {
                    self.apply_value(ui, id, v);
                }
            }
            handled = true;
        }
        if !press && self.dragging {
            self.dragging = false;
            if ui.capture() == Some(id) {
                ui.remove_capture(id);
            }
            return true;
        }
        if handled {
            return true;
        }
        base::mouse_event(ui, id, self, event)
    }

    /// `UICycle::MouseEvent`: an arrow press steps and arms the repeat;
    /// anything else inside is swallowed.
    fn cycle_mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        if !self.active || event.field0() != -1 {
            return false;
        }
        let local = self.ray_to_local(Self::position(ui, id), ui.mouse_ray());
        self.drag_part = -1;
        if !inside_box(local.x, local.y, self.width, self.height) {
            return base::mouse_event(ui, id, self, event);
        }
        if event.field1() != 2 {
            return true;
        }
        let value = if self.top_arrow.contains(local.x, local.y) {
            self.drag_part = 0;
            f64::from(self.value) - f64::from(self.step)
        } else if self.bottom_arrow.contains(local.x, local.y) {
            self.drag_part = 1;
            f64::from(self.step) + f64::from(self.value)
        } else {
            return true;
        };
        self.repeat_timer = REPEAT_DELAY;
        self.cycle_apply(ui, id, value);
        true
    }
}

impl Widget for UiScroll {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    fn win_control(&self) -> Option<&WinControl> {
        Some(&self.control)
    }
    fn win_control_mut(&mut self) -> Option<&mut WinControl> {
        Some(&mut self.control)
    }

    /// `UIScroll::HitTest`: inside the box; a `WinControl` parent hears
    /// property 0x25.
    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        let local = self.ray_to_local(Self::position(ui, id), ray);
        if !inside_box(local.x, local.y, self.width, self.height) {
            return false;
        }
        if let Some(parent) = ui.parent(id)
            && ui.win_control(parent).is_some()
        {
            ui.property(parent, Property::ChildHit(id));
        }
        true
    }

    /// `UIScroll::SetPosition`: placed at the position plus the offset and
    /// the scroll offset, then the inverse refreshed.
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        let o = self.control.offset;
        let s = self.scroll_offset;
        let sum_x = f64::from(o.x) + f64::from(s.x);
        let sum_y = f64::from(o.y) + f64::from(s.y);
        let sum_z = r(f64::from(o.z) + f64::from(s.z));
        let local = Vector::new(
            r(sum_x + f64::from(position.x)),
            r(f64::from(position.y) + sum_y),
            r(f64::from(position.z) + f64::from(sum_z)),
        );
        base::set_position(ui, id, &mut self.control, local);
        self.refresh_inverse(ui, id);
    }

    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        base::set_orientation(ui, id, &mut self.control, orientation);
    }

    /// `UIScroll::Create`: `Win3D::Create`, the payload, the arrow size,
    /// the layout.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> bool {
        let ok = ui.base_create(id, parent);
        if let Payload::Page(EntryPayload::ScrollCycle {
            right,
            bottom,
            extra,
            flag,
        }) = payload
        {
            self.width = *right;
            self.height = *bottom;
            self.range_min = extra[0];
            self.range_max = extra[1];
            self.page = extra[2];
            self.step = extra[3];
            self.horizontal = *flag;
            self.default_page = extra[2];
        }
        let shorter = if self.height > self.width {
            self.width
        } else {
            self.height
        };
        self.arrow_size = r(f64::from(shorter) * 0.2);
        self.relayout();
        ok
    }

    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        base::destroy(ui, id, &mut self.control);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        base::on_delete(ui, id, &mut self.control);
    }

    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update(id);
        self.update_hot(ui, id);
    }

    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        let transform = ui.node(id).map_or(Transform::IDENTITY, |n| {
            Transform::new(n.orientation, n.position)
        });
        let pulse = ui.pulse;
        let frame_time = ui.frame_time;
        if self.cycle {
            let [arrow, _, _] = Self::textures(ui, host);
            if let Some(imm) = host.immediate() {
                self.draw_cycle_arrows(imm, &transform, arrow, &pulse);
            }
            self.cycle_repeat(ui, id, frame_time);
            return;
        }
        if !host.scroll_bars_enabled() {
            return;
        }
        self.refresh_colors();
        let textures = Self::textures(ui, host);
        if let Some(imm) = host.immediate() {
            self.draw_bar(imm, &transform, textures, &pulse);
        }
        self.scroll_repeat(ui, id, frame_time);
    }

    fn mouse_over(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) {
        self.drag(ui, id, ray);
    }

    fn mouse_leave(&mut self, _ui: &mut Ui, _id: NodeId, _ray: Vector) {
        self.drag_part = -1;
    }

    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        if self.cycle {
            self.cycle_mouse_event(ui, id, event)
        } else {
            self.scroll_mouse_event(ui, id, event)
        }
    }

    fn broadcast(&mut self, ui: &mut Ui, id: NodeId, msg: u32, _a: u32, _b: u32) -> bool {
        base::broadcast(ui, id, self, msg)
    }

    /// `UIScroll::SetProperty`: 6 the scroll offset, 0x37 the integer range
    /// (a range shorter than the default page hides the bar), 0x38 the
    /// value, 0x3B the value back; the rest `WinControl`'s.
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        match property {
            Property::Offset(offset) => {
                self.property_scroll_offset(ui, id, offset);
                1
            }
            Property::ScrollRange(min, max) => {
                let mut max = max;
                self.page = self.default_page;
                let show = !(f64::from(max.wrapping_sub(min)) < f64::from(self.page));
                if let Some(n) = ui.node_mut(id) {
                    if show {
                        n.flags |= 3;
                    } else {
                        n.flags &= 0xFC;
                    }
                }
                if show {
                    max = trunc_to_i32(f64::from(max) + 1.0);
                }
                self.range_min = min as f32;
                self.range_max = max as f32;
                self.clamp_values();
                self.relayout();
                1
            }
            Property::ScrollValue(value) => {
                self.value = value as f32;
                self.clamp_values();
                self.relayout();
                1
            }
            Property::GetScrollValue => trunc_to_i32(f64::from(self.value)) as isize,
            other => base::property(ui, id, self, other),
        }
    }
}
