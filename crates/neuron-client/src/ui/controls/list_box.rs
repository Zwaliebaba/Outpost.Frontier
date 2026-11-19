//! `ListBox` (`list_box.cpp`) and `UIListBox` (`ui_list_box.cpp`).
//!
//! A list box is a `Win3D` box of `width` by `height` world units at its
//! depth, holding up to 256 single-line strings of the "ListText" font,
//! each with a data word. It shows `visible_rows` rows from `first_visible`
//! in the projected box: the selected row in the amber, the row an owner
//! marks (`field_80`) in the hover pulse and every other row in the HUD
//! chrome colour, a row wider than the box cut on the right. A bevelled
//! frame (dark top and left, light right and bottom) and optional row rules
//! are drawn through the immediate-mode layer in a 2D projection. The
//! update finds the row under the software cursor; a press or double click
//! selects it and tells the parent (0xF001, 0xF002).
//!
//! `UIListBox` is the page descriptor's list: its payload size and the
//! extent projection of the page widgets. `NN_LoadSave` embeds two list
//! boxes (P5).

use std::any::Any;

use neuron_core::math::{Transform, Vector};
use neuron_core::num::trunc_to_i32;

use crate::text::font::string::{DrawOrigin, FontStringChild};
use crate::text::font::{FontId, FontSystem};
use crate::ui::controls::text_win::{DEFAULT_TEXT_SCALE, project_extent_to_screen, project_point};
use crate::ui::immediate::{Immediate, capability, primitive};
use crate::ui::message::MouseButton;
use crate::ui::page::{EntryPayload, project_extent};
use crate::ui::pulse::{HUD_CHROME_RGBA, Pulse, palette};
use crate::ui::tree::{ChildMessage, DrawHost, NodeId, Payload, Ui, Widget};

/// The rows a list box holds.
pub const ROW_CAPACITY: usize = 0x100;

/// `G_ListBoxShadeRgba`: the frame's top and left edges.
pub const SHADE_RGBA: [u8; 4] = [0x37, 0x49, 0x5A, 0xFF];
/// `G_ListBoxLightRgba`: the frame's right and bottom edges and the rules.
pub const LIGHT_RGBA: [u8; 4] = [0x9B, 0xCD, 0xFF, 0xFF];

/// The font queries of a list box (the `IFont` slots it calls).
pub trait ListMetrics {
    /// `GetLineHeight` (slot 6).
    fn line_height(&mut self) -> i32;
    /// The width `MeasureString` (slot 8) gives a whole text.
    fn text_width(&mut self, text: &[u16]) -> i32;
}

/// [`ListMetrics`] of a font of the font system.
pub struct FontMetrics<'a> {
    pub sys: &'a mut FontSystem,
    pub font: Option<FontId>,
}

impl ListMetrics for FontMetrics<'_> {
    fn line_height(&mut self) -> i32 {
        match self.font.and_then(|id| self.sys.font_cx(id)) {
            Some((font, mut cx)) => font.line_height(&mut cx),
            None => 0,
        }
    }

    fn text_width(&mut self, text: &[u16]) -> i32 {
        match self.font.and_then(|id| self.sys.font_cx(id)) {
            Some((font, mut cx)) => font.measure_string(&mut cx, Some(text)).0,
            None => 0,
        }
    }
}

/// One row of the draw: its index, the draw colour it sets (little-endian
/// `0xAABBGGRR`), the origin and, for a row with text, the right trim (the
/// width past the box) it is painted with.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RowPaint {
    pub index: i32,
    pub color: u32,
    pub x: f32,
    pub y: f32,
    pub trim_right: Option<f32>,
}

/// A screen box: the projected position and extent.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ScreenBox {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
}

/// The `ListBox` part of a list.
#[derive(Clone, Debug, PartialEq)]
pub struct ListBox {
    /// `+0x7c`: the row under the cursor (-1: none).
    pub hover_index: i32,
    /// `+0x80`: the row drawn in the pulse; the owner sets it.
    pub field_80: i32,
    /// `+0x84`: "ListText".
    pub font: Option<FontId>,
    pub text_scale: f32,
    pub item_data: Vec<i32>,
    pub items: Vec<Option<FontStringChild>>,
    pub first_visible: i32,
    pub timer_890: f32,
    pub timer_894: f32,
    pub width: f32,
    pub height: f32,
    pub draw_frame: bool,
    pub draw_row_rules: bool,
    pub selectable: bool,
    pub item_count: i32,
    pub visible_rows: i32,
    pub selected_index: i32,
}

impl Default for ListBox {
    fn default() -> Self {
        ListBox::new()
    }
}

/// The right and bottom of any page payload (the reference reads the first
/// two floats whatever the class).
fn payload_extent(payload: &EntryPayload) -> (f32, f32) {
    match *payload {
        EntryPayload::Text { right, bottom, .. }
        | EntryPayload::TextMsgButton { right, bottom, .. }
        | EntryPayload::Bounds { right, bottom }
        | EntryPayload::WinObject { right, bottom, .. }
        | EntryPayload::Table { right, bottom, .. }
        | EntryPayload::ScrollCycle { right, bottom, .. } => (right, bottom),
    }
}

impl ListBox {
    /// The constructor: no rows, the state `Create` also resets, the text
    /// scale; the font is looked up by `Create` (divergence D431).
    #[must_use]
    pub fn new() -> ListBox {
        ListBox {
            hover_index: -1,
            field_80: -1,
            font: None,
            text_scale: DEFAULT_TEXT_SCALE,
            item_data: vec![0; ROW_CAPACITY],
            items: vec![None; ROW_CAPACITY],
            first_visible: 0,
            timer_890: 0.0,
            timer_894: 0.0,
            width: 0.0,
            height: 0.0,
            draw_frame: true,
            draw_row_rules: false,
            selectable: true,
            item_count: 0,
            visible_rows: 0x10,
            selected_index: -1,
        }
    }

    /// The state `Create` resets.
    pub fn reset(&mut self) {
        self.selected_index = -1;
        self.hover_index = -1;
        self.field_80 = -1;
        self.item_count = 0;
        self.first_visible = 0;
        self.draw_row_rules = false;
        self.visible_rows = 0x10;
        self.draw_frame = true;
        self.selectable = true;
    }

    /// `ListBox::Create`: the resets, then `Win3D::Create`; the "ListText"
    /// font found when there is none yet.
    pub fn create(&mut self, ui: &mut Ui, id: NodeId, parent: Option<NodeId>) -> bool {
        if self.font.is_none() {
            self.font = ui.fonts.as_ref().and_then(|f| f.find_font(b"ListText"));
        }
        self.reset();
        ui.base_create(id, parent)
    }

    /// `ListBox::HitTest`: the ray's point at the object's depth inside
    /// `[x, x + width]` by `[y - height, y]`.
    #[must_use]
    pub fn hit_test(&self, position: Vector, ray: Vector) -> bool {
        let t = f64::from(position.z) / f64::from(ray.z);
        let point_y = (t * f64::from(ray.y)) as f32;
        let delta_x = t * f64::from(ray.x) - f64::from(position.x);
        if !(delta_x >= 0.0) || !(delta_x <= f64::from(self.width)) {
            return false;
        }
        let delta_y = f64::from(position.y) - f64::from(point_y);
        delta_y >= 0.0 && delta_y <= f64::from(self.height)
    }

    /// The box in screen pixels through the Win3D camera.
    #[must_use]
    pub fn screen_box(&self, ui: &Ui, position: Vector) -> ScreenBox {
        let (x, y) = project_point(&ui.camera, position);
        let (width, height) =
            project_extent_to_screen(&ui.camera, self.width, self.height, position.z);
        ScreenBox {
            x,
            y,
            width,
            height,
        }
    }

    /// The frame time added to both timers.
    pub fn advance_timers(&mut self, delta: f64) {
        self.timer_890 = (delta + f64::from(self.timer_890)) as f32;
        self.timer_894 = (delta + f64::from(self.timer_894)) as f32;
    }

    /// The hover part of `ListBox::Update`: the row under the cursor, -1
    /// outside the box or past the last item. The line height is asked for
    /// only with the cursor inside.
    pub fn update_hover(
        &mut self,
        screen: ScreenBox,
        cursor: (i32, i32),
        metrics: &mut dyn ListMetrics,
    ) {
        self.hover_index = -1;
        let point_x = f64::from(cursor.0);
        if point_x < f64::from(screen.x) {
            return;
        }
        if !(point_x <= f64::from(screen.width) + f64::from(screen.x)) {
            return;
        }
        let point_y = f64::from(cursor.1);
        if point_y < f64::from(screen.y) {
            return;
        }
        if !(point_y <= f64::from(screen.height) + f64::from(screen.y)) {
            return;
        }
        let line_height = metrics.line_height();
        let row = trunc_to_i32((point_y - f64::from(screen.y)) / f64::from(line_height));
        if row < 0 || row >= self.item_count {
            return;
        }
        self.hover_index = self.first_visible.wrapping_add(row);
    }

    /// The rows of `ListBox::Draw`: their colours, origins advancing by the
    /// line height (taken as unsigned), and the trim past the box's floored
    /// width; a row without text sets its colour and advances unpainted.
    pub fn row_paints(
        &self,
        screen: ScreenBox,
        pulse: &Pulse,
        metrics: &mut dyn ListMetrics,
    ) -> Vec<RowPaint> {
        let box_width = trunc_to_i32(f64::from(screen.width).floor());
        let mut y = screen.y;
        let mut out = Vec::new();
        let end = self
            .first_visible
            .wrapping_add(self.visible_rows)
            .min(self.item_count);
        let mut index = self.first_visible;
        while index < end {
            let color = if self.selected_index == index {
                palette::AMBER
            } else if self.field_80 == index {
                pulse.blend()
            } else {
                u32::from_le_bytes(HUD_CHROME_RGBA)
            };
            let text = usize::try_from(index)
                .ok()
                .and_then(|i| self.items.get(i))
                .and_then(Option::as_ref)
                .and_then(FontStringChild::text);
            let trim_right = text.map(|text| {
                let measured = metrics.text_width(text);
                let overflow = if measured > box_width {
                    measured.wrapping_sub(box_width)
                } else {
                    0
                };
                overflow as f32
            });
            out.push(RowPaint {
                index,
                color,
                x: screen.x,
                y,
                trim_right,
            });
            let line_height = metrics.line_height().cast_unsigned();
            y = (f64::from(line_height) + f64::from(y)) as f32;
            index += 1;
        }
        out
    }

    /// The 2D part of `ListBox::Draw`: the frame and the rules in the
    /// projected box, in a pushed 2D projection.
    pub fn draw_frame(
        &self,
        imm: &mut dyn Immediate,
        frame: ScreenBox,
        metrics: &mut dyn ListMetrics,
    ) {
        imm.push_projection();
        imm.set_ortho_2d();
        let left = frame.x;
        let top = frame.y;
        let right = (f64::from(frame.width) + f64::from(left)) as f32;
        let bottom = (f64::from(frame.height) + f64::from(top)) as f32;
        if self.draw_frame {
            imm.begin(primitive::LINES);
            imm.color3ub(SHADE_RGBA[0], SHADE_RGBA[1], SHADE_RGBA[2]);
            imm.vertex(left, top, 0.0);
            imm.vertex(right, top, 0.0);
            imm.vertex(left, top, 0.0);
            imm.vertex(left, bottom, 0.0);
            imm.color3ub(LIGHT_RGBA[0], LIGHT_RGBA[1], LIGHT_RGBA[2]);
            imm.vertex(right, top, 0.0);
            imm.vertex(right, bottom, 0.0);
            imm.vertex(left, bottom, 0.0);
            imm.vertex(right, bottom, 0.0);
            imm.end();
        }
        if self.draw_row_rules {
            let line_height = metrics.line_height();
            imm.color3ub(LIGHT_RGBA[0], LIGHT_RGBA[1], LIGHT_RGBA[2]);
            imm.begin(primitive::LINES);
            if self.visible_rows >= 0 {
                let mut offset = 0i32;
                let mut rule = 0;
                loop {
                    let y = (f64::from(offset) + f64::from(top)) as f32;
                    imm.vertex(left, y, 0.0);
                    imm.vertex(right, y, 0.0);
                    rule += 1;
                    offset = offset.wrapping_add(line_height);
                    if rule > self.visible_rows {
                        break;
                    }
                }
            }
            imm.end();
        }
        imm.pop_projection();
    }

    /// `ListBox::MouseOver`.
    pub fn mouse_over(&mut self) {
        self.timer_894 = 10.0;
        self.timer_890 = 10.0;
    }

    /// The text of a row, `None` without one.
    #[must_use]
    pub fn item_text(&self, index: i32) -> Option<&[u16]> {
        if index < 0 || index >= self.item_count {
            return None;
        }
        self.items
            .get(usize::try_from(index).ok()?)?
            .as_ref()?
            .text()
    }

    /// `ListBox::MouseEvent`: a left press or double click of a selectable
    /// list selects the hovered row and tells the parent.
    pub fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        if !self.selectable || event.field1() == 1 || event.field0() != -1 {
            return false;
        }
        self.selected_index = self.hover_index;
        if self.hover_index >= self.item_count {
            self.selected_index = -1;
        }
        let Some(parent) = ui.parent(id) else {
            return false;
        };
        let selected = self.selected_index;
        let text = if selected >= 0 && (selected as usize) < ROW_CAPACITY {
            self.item_text(selected).map(<[u16]>::to_vec)
        } else {
            None
        };
        let double_click = match event.field1() {
            2 => false,
            3 => true,
            _ => return false,
        };
        ui.send_child_message(
            parent,
            &ChildMessage::ListPick {
                from: id,
                double_click,
                text,
                index: selected,
            },
        );
        true
    }

    /// The row object made on demand, bound to the list's font, given the
    /// text.
    fn set_row_text(&mut self, ui: &mut Ui, index: usize, text: &[u16]) {
        let row = self.items[index].get_or_insert_with(FontStringChild::new);
        match ui.fonts.as_mut() {
            Some(sys) => {
                row.bind_font(sys, self.font);
                row.set_text(sys, Some(text));
            }
            None => {
                let mut scratch = super::text_win::scratch_fonts();
                row.set_text(&mut scratch, Some(text));
            }
        }
    }

    /// `ListBox::SetItemText`: an existing row's text.
    pub fn set_item_text(&mut self, ui: &mut Ui, index: i32, text: &[u16]) -> bool {
        if index >= self.item_count || index >= ROW_CAPACITY as i32 {
            return false;
        }
        let Ok(i) = usize::try_from(index) else {
            return false;
        };
        self.set_row_text(ui, i, text);
        true
    }

    /// `ListBox::GetItemData`: 0 for -1 and out of range.
    #[must_use]
    pub fn item_data(&self, index: i32) -> i32 {
        if index == -1 || index >= self.item_count || index >= ROW_CAPACITY as i32 {
            return 0;
        }
        usize::try_from(index).map_or(0, |i| self.item_data[i])
    }

    /// `ListBox::AddItem`: the new row's index, -1 when full.
    pub fn add_item(&mut self, ui: &mut Ui, text: &[u16], data: i32) -> i32 {
        let index = self.item_count;
        if index >= ROW_CAPACITY as i32 {
            return -1;
        }
        self.item_count = index + 1;
        if let Ok(i) = usize::try_from(index) {
            self.set_row_text(ui, i, text);
            self.item_data[i] = data;
        }
        index
    }

    /// The destructor: every row released.
    pub fn release_rows(&mut self, ui: &mut Ui) {
        for row in self.items.iter_mut().rev() {
            if let Some(mut r) = row.take()
                && let Some(sys) = ui.fonts.as_mut()
            {
                r.release(sys);
            }
        }
    }

    /// `ListBox::Update`.
    pub fn update(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update(id);
        self.advance_timers(ui.frame_time);
        let position = ui.node(id).map_or(Vector::ZERO, |n| n.position);
        let screen = self.screen_box(ui, position);
        let cursor = ui.cursor;
        let font = self.font;
        match ui.fonts.as_mut() {
            Some(sys) => self.update_hover(screen, cursor, &mut FontMetrics { sys, font }),
            None => self.update_hover(screen, cursor, &mut NoMetrics),
        }
    }

    /// `ListBox::Draw`.
    pub fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        if let Some(imm) = host.immediate() {
            imm.disable(capability::DEPTH_TEST);
            imm.disable(capability::TEXTURE_2D);
        }
        let position = ui.node(id).map_or(Vector::ZERO, |n| n.position);
        let screen = self.screen_box(ui, position);
        let pulse = ui.pulse;
        let font = self.font;
        if let Some(sys) = ui.fonts.as_mut() {
            let rows = self.row_paints(screen, &pulse, &mut FontMetrics { sys, font });
            for row in rows {
                sys.atlas.draw_color = row.color;
                let Some(trim_right) = row.trim_right else {
                    continue;
                };
                let Some(mut cx) = host.paint_cx() else {
                    continue;
                };
                let origin = DrawOrigin {
                    x: row.x,
                    y: row.y,
                    width: screen.x,
                    height: screen.y,
                    h_mode: 0,
                    v_mode: 0,
                };
                if let Some(Some(item)) = usize::try_from(row.index)
                    .ok()
                    .and_then(|i| self.items.get_mut(i))
                {
                    item.paint(sys, &mut cx, &origin, 0.0, trim_right);
                }
            }
        }
        if let Some(imm) = host.immediate() {
            match ui.fonts.as_mut() {
                Some(sys) => self.draw_frame(imm, screen, &mut FontMetrics { sys, font }),
                None => self.draw_frame(imm, screen, &mut NoMetrics),
            }
        }
        ui.draw_children(id, host);
    }
}

/// Metrics without a font: no line height, no width.
struct NoMetrics;

impl ListMetrics for NoMetrics {
    fn line_height(&mut self) -> i32 {
        0
    }
    fn text_width(&mut self, _: &[u16]) -> i32 {
        0
    }
}

/// The "ListBox" object as other controls embed it.
impl Widget for ListBox {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        let position = ui.node(id).map_or(Vector::ZERO, |n| n.position);
        ListBox::hit_test(self, position, ray)
    }
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        ListBox::create(self, ui, id, parent)
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        self.release_rows(ui);
        ui.base_on_delete(id);
    }
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        ListBox::update(self, ui, id);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        ListBox::draw(self, ui, id, host);
    }
    fn mouse_over(&mut self, _ui: &mut Ui, _id: NodeId, _ray: Vector) {
        ListBox::mouse_over(self);
    }
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        ListBox::mouse_event(self, ui, id, event)
    }
}

/// `UIListBox`: the page descriptor's list.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct UiListBox {
    pub list: ListBox,
    /// `+0x8b0`, `+0x8b4`: the payload size.
    pub local_width: f32,
    pub local_height: f32,
}

impl Widget for UiListBox {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        let position = ui.node(id).map_or(Vector::ZERO, |n| n.position);
        self.list.hit_test(position, ray)
    }

    /// `UIListBox::Create`: the list's resets and `Win3D::Create`, then the
    /// payload's size as both the local and the projected extent.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> bool {
        let ok = self.list.create(ui, id, parent);
        if let Payload::Page(p) = payload {
            let (right, bottom) = payload_extent(p);
            self.local_width = right;
            self.list.width = right;
            self.local_height = bottom;
            self.list.height = bottom;
        }
        ok
    }

    /// `UIListBox::SetTransform`: orientation, position, then the extent
    /// projected through the transform's matrix at its depth.
    fn set_transform(&mut self, ui: &mut Ui, id: NodeId, transform: &Transform) {
        self.set_orientation(ui, id, transform.rotation);
        self.set_position(ui, id, transform.translation);
        let (width, height) = project_extent(
            &transform.rotation,
            transform.translation.z,
            self.local_width,
            self.local_height,
        );
        self.list.width = width;
        self.list.height = height;
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        self.list.release_rows(ui);
        ui.base_on_delete(id);
    }
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        self.list.update(ui, id);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        self.list.draw(ui, id, host);
    }
    fn mouse_over(&mut self, _ui: &mut Ui, _id: NodeId, _ray: Vector) {
        self.list.mouse_over();
    }
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        self.list.mouse_event(ui, id, event)
    }
}
