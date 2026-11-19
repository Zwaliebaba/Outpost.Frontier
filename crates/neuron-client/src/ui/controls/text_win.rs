//! `TextWin` (`text_win.cpp`): an object that draws one `Font::String` at
//! its position, and `UIStaticText` (`ui_static_text.cpp`), the page
//! caption built on it.
//!
//! The text box is `width` by `height` world units at the object's depth,
//! its top-left corner at the position; the hit test intersects the ray
//! with that depth. The draw projects the box and the position through the
//! Win3D camera into a screen box and paints the string's lines in it,
//! tinted with the idle colour, the hover pulse around the pulse colour, or
//! the inactive colour.

use std::any::Any;

use neuron_core::math::{Transform, Vector};

use crate::text::font::string::{DrawOrigin, FontString};
use crate::text::font::{
    FaceHandle, FontBackend, FontEnv, FontId, FontSystem, LogFont, TextMetrics,
};
use crate::ui::camera::Camera;
use crate::ui::page::{EntryPayload, project_extent};
use crate::ui::pulse::palette;
use crate::ui::tree::{ACTIVE, DrawHost, NodeId, Payload, Ui, VISIBLE, Widget};

/// The default text scale (bits 0x3B03126F).
pub const DEFAULT_TEXT_SCALE: f32 = 0.002;

/// `TextWinProjectExtent`: a world width and height at depth `z` in screen
/// pixels (a depth below 0.001 counts as 1).
#[must_use]
pub fn project_extent_to_screen(camera: &Camera, width: f32, height: f32, z: f32) -> (f32, f32) {
    let mut depth = f64::from(z).abs();
    if depth < 0.001 {
        depth = 1.0;
    }
    let near = f64::from(camera.z_near);
    let near_over_depth = near / depth;
    let inv_near = 1.0 / near;
    let w = (f64::from(camera.x_scale) * inv_near * near_over_depth * f64::from(width)) as f32;
    let h = (-(inv_near * f64::from(camera.y_scale) * near_over_depth * f64::from(height))) as f32;
    (w, h)
}

/// `TextWinProjectPoint`: the screen point of a world point, (0, 0) behind
/// the near plane.
#[must_use]
pub fn project_point(camera: &Camera, world: Vector) -> (f32, f32) {
    camera.project(world).map_or((0.0, 0.0), |(x, y, _)| (x, y))
}

/// The `TextWin` part of a text object.
#[derive(Clone, Debug, PartialEq)]
pub struct TextWin {
    pub text: FontString,
    pub text_scale: f32,
    /// The text box in world units at the object's depth.
    pub width: f32,
    pub height: f32,
    /// `{0, 0, width, -height}`, rebuilt by the draw.
    pub bounds: [f32; 4],
    pub color_idle: u32,
    pub color_pulse: u32,
    pub color_d4: u32,
    pub color_d8: u32,
    pub color_inactive: u32,
    pub flag_e0: u8,
    pub flag_e1: u8,
    pub flag_e2: u8,
    pub flag_e3: u8,
    /// Whether the idle and pulse colours are used (else the inactive one).
    pub use_palette: bool,
    /// The horizontal alignment of the draw (`+0xe8`: 1 centre, 2 right).
    pub align: u32,
}

impl Default for TextWin {
    fn default() -> Self {
        TextWin::new()
    }
}

impl TextWin {
    /// The inlined constructor: the string, the colours zeroed.
    #[must_use]
    pub fn new() -> TextWin {
        TextWin {
            text: FontString::new(),
            text_scale: 0.0,
            width: 0.0,
            height: 0.0,
            bounds: [0.0; 4],
            color_idle: 0,
            color_pulse: 0,
            color_d4: 0,
            color_d8: 0,
            color_inactive: 0,
            flag_e0: 0,
            flag_e1: 0,
            flag_e2: 0,
            flag_e3: 0,
            use_palette: false,
            align: 0,
        }
    }

    /// `InitTextDefaults`.
    pub fn init_text_defaults(&mut self) {
        self.flag_e0 = 0;
        self.flag_e3 = 0;
        self.flag_e1 = 0;
        self.flag_e2 = 1;
        self.use_palette = true;
        self.align = 0;
        self.text_scale = DEFAULT_TEXT_SCALE;
        self.color_idle = palette::IDLE;
        self.color_pulse = palette::AMBER;
        self.color_inactive = palette::GREY;
    }

    /// `TextWin::SetText` (slot 28). Without a font system the string only
    /// takes the text.
    pub fn set_text(&mut self, ui: &mut Ui, text: &[u16]) {
        match ui.fonts.as_mut() {
            Some(sys) => self.text.set_text(sys, Some(text)),
            None => {
                let mut scratch = scratch_fonts();
                self.text.set_text(&mut scratch, Some(text));
            }
        }
    }

    /// `TextWin::SetFont`: the font by name, every line rebound when it
    /// changed; the scale stored either way.
    pub fn set_font(&mut self, ui: &mut Ui, name: &[u8], scale: f32) {
        if let Some(sys) = ui.fonts.as_mut() {
            let font: Option<FontId> = sys.find_font(name);
            self.text.set_font(sys, font);
        }
        self.text_scale = scale;
    }

    /// `TextWin::HitTest`: the ray's point at the object's depth inside
    /// `[x, x + width]` by `[y - height, y]`.
    #[must_use]
    pub fn hit_test(&self, position: Vector, ray: Vector) -> bool {
        let t = f64::from(position.z) / f64::from(ray.z);
        let point_y = (t * f64::from(ray.y)) as f32;
        let delta_x = t * f64::from(ray.x) - f64::from(position.x);
        if delta_x < 0.0 || !(delta_x <= f64::from(self.width)) {
            return false;
        }
        let delta_y = f64::from(position.y) - f64::from(point_y);
        delta_y >= 0.0 && delta_y <= f64::from(self.height)
    }

    /// `TextWin::Destroy` and the destructor: the text cleared, then the
    /// `Win3D` part.
    pub fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        if let Some(sys) = ui.fonts.as_mut() {
            self.text.clear_contents(sys);
        }
        ui.base_destroy(id);
    }

    /// `TextWin::Draw`.
    pub fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        if !host.interface_enabled() {
            return;
        }
        let Some(node) = ui.node(id) else { return };
        if node.flags & VISIBLE == 0 {
            return;
        }
        let position = node.position;
        self.bounds = [0.0, 0.0, self.width, -self.height];
        let pulsed = ui.pulse.blend_from(self.color_pulse);
        let color = if self.use_palette {
            if ui.hover() == Some(id) {
                pulsed
            } else {
                self.color_idle
            }
        } else {
            self.color_inactive
        };
        let (w, h) = project_extent_to_screen(&ui.camera, self.width, self.height, position.z);
        let (x, y) = project_point(&ui.camera, position);
        let mut origin = DrawOrigin {
            x,
            y,
            width: w,
            height: h,
            h_mode: self.align,
            v_mode: 0,
        };
        let Some(sys) = ui.fonts.as_mut() else { return };
        sys.atlas.draw_color = color;
        let Some(mut cx) = host.paint_cx() else {
            return;
        };
        self.text.draw_children(sys, &mut cx, &mut origin, 0.0, 0.0);
    }
}

/// A font system without fonts, for strings handled while the manager has
/// none.
pub(crate) fn scratch_fonts() -> FontSystem {
    FontSystem::new(Box::new(NoBackend), FontEnv::new(0, 0))
}

/// A backend for strings handled without a font system: it makes nothing.
struct NoBackend;

impl FontBackend for NoBackend {
    fn create_font(&mut self, _: &LogFont) -> Option<FaceHandle> {
        None
    }
    fn delete_font(&mut self, _: FaceHandle) {}
    fn text_metrics(&mut self, _: FaceHandle) -> TextMetrics {
        TextMetrics::default()
    }
    fn char_width(&mut self, _: FaceHandle, _: u16) -> Option<i32> {
        None
    }
    fn text_extent(&mut self, _: FaceHandle, _: &[u16]) -> Option<(i32, i32)> {
        None
    }
    fn draw_text(
        &mut self,
        _: FaceHandle,
        _: i32,
        _: i32,
        _: u32,
        _: u32,
        _: &[u16],
    ) -> Option<Vec<u8>> {
        None
    }
}

/// `UIStaticText`: a page caption.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct UiStaticText {
    pub text_win: TextWin,
    /// The payload's width and height (`+0xec`, `+0xf0`).
    pub local_width: f32,
    pub local_height: f32,
}

impl Widget for UiStaticText {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }

    /// `UIStaticText::Create`: the `Win3D` part, the text defaults,
    /// inactive, a 0.1 box and the text "StaticText"; from a payload the
    /// box, the caption and `+0xd8` black.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> bool {
        ui.base_create(id, parent);
        let tw = &mut self.text_win;
        tw.init_text_defaults();
        if let Some(n) = ui.node_mut(id) {
            n.flags &= !ACTIVE;
        }
        tw.width = 0.1;
        tw.height = 0.1;
        tw.flag_e1 = 0;
        let default: Vec<u16> = "StaticText".encode_utf16().collect();
        tw.set_text(ui, &default);
        if let Payload::Page(EntryPayload::Text {
            right,
            bottom,
            caption,
        }) = payload
        {
            self.local_width = *right;
            self.text_win.width = *right;
            self.local_height = *bottom;
            self.text_win.height = *bottom;
            self.text_win.set_text(ui, caption);
            self.text_win.color_d8 = 0xFF00_0000;
        }
        true
    }

    /// `UIStaticText::SetTransform` (slot 8): orientation and position,
    /// then the box from the payload size through the transform's matrix at
    /// its depth.
    fn set_transform(&mut self, ui: &mut Ui, id: NodeId, transform: &Transform) {
        self.set_orientation(ui, id, transform.rotation);
        self.set_position(ui, id, transform.translation);
        let (width, height) = project_extent(
            &transform.rotation,
            transform.translation.z,
            self.local_width,
            self.local_height,
        );
        self.text_win.width = width;
        self.text_win.height = height;
    }

    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        let position = ui.node(id).map_or(Vector::ZERO, |n| n.position);
        self.text_win.hit_test(position, ray)
    }

    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        self.text_win.destroy(ui, id);
    }

    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        self.text_win.destroy(ui, id);
    }

    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        self.text_win.draw(ui, id, host);
    }
}
