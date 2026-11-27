//! `TextControl` and `EditControl` (`nn_text_control.cpp`): the NeuroNet
//! text label of a `[BaseFrame]` window and the field the player types a
//! quantity into.
//!
//! A text control is a `WinControl` holding a `Font::String`, a box of
//! `width` by `height` world units whose top-left corner is the position
//! plus the offset (or whose centre is the anchor, `+0x378` clear), an id
//! or text, a justification, a colour (the idle cyan, a property's
//! override, the hover pulse while a clickable control is under the
//! cursor, grey while inactive), a typewriter that reveals the text one
//! character per interval with a UI sound, and a colour blink. A
//! clickable control (`+0x35C`) holds the mouse between a press and its
//! release and tells its parent 0x26 and 0x27.
//!
//! The edit control types into the string itself: a blinking `|` is
//! appended and removed every half second, Enter, Backspace and the
//! characters change the text, and every change goes to the parent as
//! property 0x45. (It does not use the IME's edit boxes of
//! `text_input`; the reference's `EditControl` does not either.)
//!
//! The `Font::String` is behind [`ControlText`], so the string's layout
//! and paints can be the font system's ([`FontText`]) or a stand-in's.

use std::any::Any;

use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Transform, Vector, mul_matrix};
use neuron_core::num::trunc_to_i32;

use crate::text::font::FontId;
use crate::text::font::string::{DrawOrigin, FontString};
use crate::ui::control::{WinControl, base, cursor_ray_inline, ray_hits_box, tell_parent};
use crate::ui::controls::text_win::{text_extent, text_point};
use crate::ui::immediate::capability;
use crate::ui::message::MouseButton;
use crate::ui::pulse::palette;
use crate::ui::services::{HoverKey, ToolTipHover};
use crate::ui::tree::{ACTIVE, DrawHost, NodeId, Payload, Property, Ui, VISIBLE, Widget};

/// `kGameOptInterfaceGroup`.
pub const OPT_INTERFACE: u32 = 6;
/// `kGameOptNeuronet`.
pub const OPT_NEURONET: u32 = 9;
/// `kGameOptDrawTextControls`.
pub const OPT_TEXT_CONTROLS: u32 = 0x13;
/// `kGameOptDrawButtonControls`.
pub const OPT_BUTTON_CONTROLS: u32 = 0x14;
/// `kGameOptDrawStaticControls`.
pub const OPT_STATIC_CONTROLS: u32 = 0x15;

/// The UI sound slot of the typewriter.
pub const TYPEWRITER_SOUND: u32 = 0xB;

/// The `Font::String` calls of the text controls, the tooltip and the
/// rollover (`font_string.h`, and `FontAtlasSetDrawColor`).
pub trait ControlText {
    /// `FontStringSetScale(width, depth)`.
    fn set_scale(&mut self, ui: &mut Ui, width: f32, depth: f32);
    /// `FontStringSetText`.
    fn set_text(&mut self, ui: &mut Ui, text: Option<&[u16]>);
    /// The string's text (`+0x00`).
    fn text(&self) -> Option<&[u16]>;
    /// `FontStringSetNumberText(0, format)`.
    fn set_number_text(&mut self, ui: &mut Ui, format: &[u8]);
    /// The string's font (`+0x1c`).
    fn font(&self) -> Option<FontId>;
    /// `FontStringSetFont` (and `TextControl::SetFont`, the same body).
    fn set_font(&mut self, ui: &mut Ui, font: Option<FontId>);
    /// `FindMenuFontImp`.
    fn find_font(&self, ui: &Ui, name: &[u8]) -> Option<FontId>;
    /// Whether the string has child slots (`ChildBegin != ChildEnd`, read
    /// without a layout).
    fn has_child_slots(&self) -> bool;
    /// `FontStringChildCount`.
    fn child_count(&mut self, ui: &mut Ui, host: &mut dyn DrawHost) -> usize;
    /// `FontStringGetExtent`; without a host the extents of the last
    /// layout (divergence D437).
    fn extent(&mut self, ui: &mut Ui, host: Option<&mut dyn DrawHost>) -> (i32, i32);
    /// `FontStringDrawChildren(origin, this, left, right)`.
    fn draw_children(
        &mut self,
        ui: &mut Ui,
        host: &mut dyn DrawHost,
        origin: DrawOrigin,
        trim_left: f32,
        trim_right: f32,
    );
    /// `FontStringClearContents`.
    fn clear_contents(&mut self, ui: &mut Ui);
    /// `FontAtlasSetDrawColor`.
    fn set_draw_color(&mut self, ui: &mut Ui, argb: u32);
    /// `+0x30`: word-wrapped lines.
    fn set_multi_line(&mut self, on: bool);
    /// `+0x08`: the most lines kept.
    fn set_max_lines(&mut self, lines: i32);
    fn as_any(&self) -> &dyn Any;
    fn as_any_mut(&mut self) -> &mut dyn Any;
}

/// [`ControlText`] over the font system's `Font::String`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct FontText {
    pub string: FontString,
    /// A number text set without a host, applied at the next draw
    /// (divergence D437).
    pub pending_number: Option<Vec<u8>>,
}

impl FontText {
    fn apply_pending(&mut self, ui: &mut Ui, host: &mut dyn DrawHost) {
        let Some(format) = self.pending_number.take() else {
            return;
        };
        if let (Some(sys), Some(cx)) = (ui.fonts.as_mut(), host.paint_cx()) {
            self.string.set_number_text(sys, cx.pipeline, &format);
        }
    }
}

impl ControlText for FontText {
    fn set_scale(&mut self, ui: &mut Ui, width: f32, depth: f32) {
        let (x_scale, z_near) = (ui.camera.x_scale, ui.camera.z_near);
        self.string.set_scale(width, depth, x_scale, z_near);
    }
    fn set_text(&mut self, ui: &mut Ui, text: Option<&[u16]>) {
        match ui.fonts.as_mut() {
            Some(sys) => self.string.set_text(sys, text),
            None => {
                let mut scratch = super::text_win::scratch_fonts();
                self.string.set_text(&mut scratch, text);
            }
        }
    }
    fn text(&self) -> Option<&[u16]> {
        self.string.text()
    }
    /// The text dropped at once (it needs no layout), the number child
    /// made at the next draw (D437).
    fn set_number_text(&mut self, ui: &mut Ui, format: &[u8]) {
        match ui.fonts.as_mut() {
            Some(sys) => self.string.drop_text_for_number(sys),
            None => {
                let mut scratch = super::text_win::scratch_fonts();
                self.string.drop_text_for_number(&mut scratch);
            }
        }
        self.pending_number = Some(format.to_vec());
    }
    fn font(&self) -> Option<FontId> {
        self.string_font()
    }
    fn set_font(&mut self, ui: &mut Ui, font: Option<FontId>) {
        match ui.fonts.as_mut() {
            Some(sys) => self.string.set_font(sys, font),
            None => {
                let mut scratch = super::text_win::scratch_fonts();
                self.string.set_font(&mut scratch, font);
            }
        }
    }
    fn find_font(&self, ui: &Ui, name: &[u8]) -> Option<FontId> {
        ui.fonts.as_ref().and_then(|f| f.find_font(name))
    }
    fn has_child_slots(&self) -> bool {
        !self.string.children().is_empty()
    }
    fn child_count(&mut self, ui: &mut Ui, host: &mut dyn DrawHost) -> usize {
        self.apply_pending(ui, host);
        match (ui.fonts.as_mut(), host.paint_cx()) {
            (Some(sys), Some(cx)) => self.string.child_count(sys, cx.pipeline),
            _ => self.string.children().len(),
        }
    }
    fn extent(&mut self, ui: &mut Ui, host: Option<&mut dyn DrawHost>) -> (i32, i32) {
        if let Some(host) = host {
            self.apply_pending(ui, host);
            if let (Some(sys), Some(cx)) = (ui.fonts.as_mut(), host.paint_cx()) {
                return self.string.extent(sys, cx.pipeline);
            }
        }
        self.string.extents()
    }
    fn draw_children(
        &mut self,
        ui: &mut Ui,
        host: &mut dyn DrawHost,
        mut origin: DrawOrigin,
        trim_left: f32,
        trim_right: f32,
    ) {
        self.apply_pending(ui, host);
        if let (Some(sys), Some(mut cx)) = (ui.fonts.as_mut(), host.paint_cx()) {
            self.string
                .draw_children(sys, &mut cx, &mut origin, trim_left, trim_right);
        }
    }
    fn clear_contents(&mut self, ui: &mut Ui) {
        if let Some(sys) = ui.fonts.as_mut() {
            self.string.clear_contents(sys);
        }
    }
    fn set_draw_color(&mut self, ui: &mut Ui, argb: u32) {
        if let Some(sys) = ui.fonts.as_mut() {
            sys.atlas.draw_color = argb;
        }
    }
    fn set_multi_line(&mut self, on: bool) {
        self.string.set_multi_line(on);
    }
    fn set_max_lines(&mut self, lines: i32) {
        self.string.max_lines = lines;
    }
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
}

impl FontText {
    fn string_font(&self) -> Option<FontId> {
        self.string.font()
    }
}

fn add(a: f32, b: f32) -> f32 {
    (f64::from(a) + f64::from(b)) as f32
}

/// The `TextControl` part of a control.
pub struct TextControl {
    pub control: WinControl,
    /// `+0x32c`, `+0x330`: the box in world units.
    pub width: f32,
    pub height: f32,
    /// `+0x334`.
    pub text: Box<dyn ControlText>,
    /// `+0x338`, `+0x33c` (property 0x22).
    pub field_338: i32,
    pub field_33c: i32,
    /// `+0x340`: the line count of the last draw.
    pub line_count: i32,
    /// `+0x344`: the tooltip id.
    pub tooltip: i32,
    /// `+0x348`.
    pub text_colour: u32,
    /// `+0x34c`, `+0x350` (property 0x23): the draw's trims, as the bits
    /// of floats.
    pub trim_left_bits: i32,
    pub trim_right_bits: i32,
    /// `+0x354` (property 0xc): drawn in the amber.
    pub highlighted: bool,
    /// `+0x358`: the horizontal alignment.
    pub justification: i32,
    /// `+0x35c` (property 0x24): clickable.
    pub clickable: bool,
    /// `+0x35d`, `+0x35e`.
    pub colour_override: bool,
    pub override_colour: [u8; 4],
    /// `+0x362`.
    pub typewriter: bool,
    /// `+0x364`.
    pub typed_count: i32,
    /// `+0x368`.
    pub type_elapsed: f32,
    /// `+0x36c`.
    pub type_interval: f32,
    /// `+0x370`: the typewriter's whole text (empty: none).
    pub pending: Vec<u16>,
    /// `+0x378`: the position is the box's corner (else its centre).
    pub corner_anchor: bool,
    /// `+0x379`, `+0x37a`, `+0x37c`.
    pub blink: bool,
    pub blink_phase: bool,
    pub blink_elapsed: f32,
    /// `+0x380`: a hovered clickable control pulses.
    pub pulse_on_hover: bool,
    /// `+0x381`, `+0x385`.
    pub blink_on: [u8; 4],
    pub blink_off: [u8; 4],
    /// `+0x389`: the control holds a typewriter run.
    pub typing: bool,
    /// `+0x38c`, `+0x390`.
    pub cached_x: f32,
    pub cached_y: f32,
    /// `+0x394`, `+0x395`.
    pub cache_valid: bool,
    pub cached_draw: bool,
    /// `+0x398`.
    pub cached_surface_width: i32,
    /// `+0x39c`.
    pub in_capture: bool,
}

impl std::fmt::Debug for TextControl {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("TextControl")
            .field("width", &self.width)
            .field("height", &self.height)
            .field("text", &self.text.text())
            .finish_non_exhaustive()
    }
}

impl Default for TextControl {
    fn default() -> Self {
        TextControl::new()
    }
}

impl TextControl {
    /// The constructor.
    #[must_use]
    pub fn new() -> TextControl {
        TextControl {
            control: WinControl::new(),
            width: 0.0,
            height: 0.0,
            text: Box::new(FontText::default()),
            field_338: -1,
            field_33c: -1,
            line_count: 0,
            tooltip: 0,
            text_colour: (palette::IDLE & 0x00FF_FFFF) | 0xFF00_0000,
            trim_left_bits: 0,
            trim_right_bits: 0,
            highlighted: false,
            justification: 0,
            clickable: false,
            colour_override: false,
            override_colour: [0; 4],
            typewriter: false,
            typed_count: 1,
            type_elapsed: 0.0,
            type_interval: 0.05,
            pending: Vec::new(),
            corner_anchor: false,
            blink: false,
            blink_phase: false,
            blink_elapsed: 0.0,
            pulse_on_hover: true,
            blink_on: [0; 4],
            blink_off: [0; 4],
            typing: false,
            cached_x: 0.0,
            cached_y: 0.0,
            cache_valid: false,
            cached_draw: false,
            cached_surface_width: 0,
            in_capture: false,
        }
    }

    fn position(ui: &Ui, id: NodeId) -> Vector {
        ui.node(id).map_or(Vector::ZERO, |n| n.position)
    }

    /// `Offset.z + Position.z`, the depth of every scale call.
    fn depth(&self, ui: &Ui, id: NodeId) -> f32 {
        self.control.offset.z + Self::position(ui, id).z
    }

    /// The scale at the control's depth, then the text.
    fn put_text(&mut self, ui: &mut Ui, id: NodeId, text: Option<&[u16]>) {
        let depth = self.depth(ui, id);
        self.text.set_scale(ui, self.width, depth);
        self.text.set_text(ui, text);
    }

    /// The end of a typewriter run: the count down, the sound stopped by
    /// the last run, the whole pending text shown.
    fn end_typing(&mut self, ui: &mut Ui, id: NodeId) {
        self.typing = false;
        ui.text_typing_count -= 1;
        if ui.text_typing_count <= 0 {
            ui.services.stop_ui_sound_slot(TYPEWRITER_SOUND);
        }
        let pending = self.pending.clone();
        self.put_text(ui, id, Some(&pending));
    }

    /// The pending text emptied when it is not.
    fn clear_pending(&mut self) {
        if let Some(first) = self.pending.first_mut() {
            *first = 0;
            self.pending.clear();
        }
    }

    /// `TextControl::SetFont`: a different font stored, the layout stale.
    pub fn set_font(&mut self, ui: &mut Ui, font: Option<FontId>) {
        if self.text.font() == font {
            return;
        }
        self.text.set_font(ui, font);
    }

    /// `SetFontByName`.
    pub fn set_font_by_name(&mut self, ui: &mut Ui, name: &[u8]) {
        if let Some(font) = self.text.find_font(ui, name) {
            self.set_font(ui, Some(font));
        }
    }

    /// `SetIds`: the id's string, `IDS?` when it is missing, empty for 0.
    pub fn set_ids(&mut self, ui: &mut Ui, id: NodeId, ids: u32) {
        if ids != 0 {
            let mut s = ui.ids_string(ids);
            s.truncate(0xFFF);
            if s.is_empty() {
                s = "IDS?".encode_utf16().collect();
            }
            self.put_text(ui, id, Some(&s));
            return;
        }
        self.put_text(ui, id, Some(&[]));
    }

    /// `SetText`.
    pub fn set_text(&mut self, ui: &mut Ui, id: NodeId, text: Option<&[u16]>) {
        self.put_text(ui, id, text);
    }

    /// `SetNumberText`: a printf format through the string's number child.
    pub fn set_number_text(&mut self, ui: &mut Ui, id: NodeId, format: &[u8]) {
        let depth = self.depth(ui, id);
        self.text.set_scale(ui, self.width, depth);
        self.text.set_number_text(ui, format);
    }

    /// `SetWidth`.
    pub fn set_width(&mut self, ui: &mut Ui, id: NodeId, width: f32) {
        self.width = width;
        let depth = self.depth(ui, id);
        self.text.set_scale(ui, width, depth);
    }

    /// `SetCachedDraw`.
    pub fn set_cached_draw(&mut self, cached: bool, invalidate: bool) {
        self.cached_draw = cached;
        if invalidate {
            self.cache_valid = false;
        }
    }

    /// The attach transform reset and asked of `parent`'s hardpoint (its
    /// answer is not kept).
    fn ask_hardpoint(&mut self, ui: &Ui, parent: Option<NodeId>, name: &[u8]) {
        self.control.attach_orientation = Matrix::IDENTITY;
        self.control.attach_position = Vector::ZERO;
        if let Some(p) = parent.and_then(|p| ui.win_control(p)) {
            let (_, t) = p.hardpoint_transform(name);
            self.control.attach_orientation = t.rotation;
            self.control.attach_position = t.translation;
        }
    }

    /// `TextControl::Init`.
    #[allow(clippy::too_many_arguments)]
    pub fn init(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        style: Option<&[u8]>,
        position: Vector,
        hardpoint: Option<&[u8]>,
        width: f32,
        height: f32,
        ids: u32,
        justification: i32,
        tooltip: i32,
        corner_anchor: bool,
    ) {
        self.corner_anchor = corner_anchor;
        if let Some(style) = style
            && let Some(font) = self.text.find_font(ui, style)
        {
            self.set_font(ui, Some(font));
        }
        self.control.offset = position;
        if let Some(name) = hardpoint {
            self.control.hardpoint = name.to_vec();
            let parent = ui.parent(id);
            self.ask_hardpoint(ui, parent, name);
        }
        self.height = height;
        self.width = width;
        self.set_ids(ui, id, ids);
        self.justification = justification;
        self.tooltip = tooltip;
    }

    /// `TextControl::Create`: `Win3D::Create`, the keys of the INI
    /// payload, then the parent placed at its own position.
    pub fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> bool {
        ui.base_create(id, parent);
        if let Payload::Ini(ini) = payload {
            self.read_ini(ui, id, parent, ini);
        }
        if let Some(p) = ui.parent(id) {
            let position = Self::position(ui, p);
            ui.set_position(p, position);
            if let Some(pp) = crate::ui::control::parent_slot5_position(ui, id) {
                self.set_position(ui, id, pp);
            }
        }
        true
    }

    fn read_ini(&mut self, ui: &mut Ui, id: NodeId, parent: Option<NodeId>, ini: &mut IniReader) {
        while ini.read_value() {
            if ini.is_value(b"font") {
                let name = ini.value_string();
                if let Some(font) = self.text.find_font(ui, &name) {
                    self.text.set_font(ui, Some(font));
                }
            } else if ini.is_value(b"offset") {
                let y = ini.value_float(1) as f32;
                let x = ini.value_float(0) as f32;
                let z = ini.value_float(2) as f32;
                self.control.offset = Vector::new(x, y, z);
            } else if ini.is_value(b"hardpoint") {
                let name = ini.value_string();
                self.ask_hardpoint(ui, parent, &name);
            } else if ini.is_value(b"width") {
                self.width = ini.value_float(0) as f32;
            } else if ini.is_value(b"height") {
                self.height = ini.value_float(0) as f32;
            } else if ini.is_value(b"font_scale") {
            } else if ini.is_value(b"stringID") {
                let ids = trunc_to_i32(ini.value_float(0)).cast_unsigned();
                self.set_ids(ui, id, ids);
            } else if ini.is_value(b"justification") {
                self.justification = trunc_to_i32(ini.value_float(0));
            } else if ini.is_value(b"tooltip") {
                self.tooltip = trunc_to_i32(ini.value_float(0));
            }
        }
    }

    /// The text box's half extents and centre (`HitTest`).
    fn hit_box(&self, ui: &Ui, id: NodeId) -> (Vector, Vector) {
        let p = Self::position(ui, id);
        let o = self.control.offset;
        let sum_x = f64::from(p.x) + f64::from(o.x);
        let sum_y = f64::from(p.y) + f64::from(o.y);
        let centre_z = add(p.z, o.z);
        let half_w = self.width * 0.5;
        let half_h = self.height * 0.5;
        let centre = Vector::new(
            (sum_x + f64::from(half_w)) as f32,
            (sum_y - f64::from(half_h)) as f32,
            centre_z,
        );
        (Vector::new(half_w, half_h, 0.0), centre)
    }

    /// `TextControl::HitTest`: only with a tooltip or clickable; the text
    /// box against the ray, a hit fed to the tooltip and told to the
    /// parent (0x46).
    pub fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        if self.tooltip == 0 && !self.clickable {
            return false;
        }
        let (half, centre) = self.hit_box(ui, id);
        if !ray_hits_box(half, &Transform::new(Matrix::IDENTITY, centre), ray) {
            return false;
        }
        if self.tooltip != 0 {
            let hover = ToolTipHover {
                key: HoverKey::Ids(self.tooltip),
                ids_alt: self.tooltip,
                live: true,
                world_pos: centre,
                half_extents: half,
                ..Default::default()
            };
            ui.feed_tooltip_hover(&hover);
        }
        tell_parent(ui, id, 0x46, 0);
        true
    }

    /// `TextControl::MouseEvent`.
    pub fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        if !self.clickable {
            return false;
        }
        if event.field0() == -1 && event.field1() == 3 {
            tell_parent(ui, id, 0x27, 0);
        }
        if self.in_capture {
            self.in_capture = false;
            ui.remove_capture(id);
            if event.field0() != -1 {
                return false;
            }
            if event.field1() == 1 {
                tell_parent(ui, id, 0x26, 0);
                return true;
            }
        }
        if event.field0() != -1 || event.field1() == 1 {
            return false;
        }
        self.in_capture = true;
        ui.push_capture(id);
        true
    }

    /// `TextControl::SetOrientation`: times the attach orientation; no
    /// mesh, no children.
    pub fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        self.control.attached = true;
        let attach = self.control.attach_orientation(ui, id);
        let product = mul_matrix(&orientation, &attach);
        if let Some(n) = ui.node_mut(id) {
            n.orientation = product;
        }
    }

    /// `TextControl::SetPosition`: the anchor kept, the box's corner (or
    /// the anchor moved by half the box) placed through `WinControl`.
    pub fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        self.control.field_2e0 = position;
        self.control.attached = true;
        let mut attach = self.control.attach_position(ui, id);
        if !self.corner_anchor {
            attach.x -= self.width * 0.5;
            attach.y += self.height * 0.5;
        }
        let mut placed = position;
        if self.control.position_includes_offset {
            placed = Vector::new(
                placed.x + attach.x,
                placed.y + attach.y,
                placed.z + attach.z,
            );
        }
        base::set_position(ui, id, &mut self.control, placed);
        if self.width > 0.0 {
            let depth = self.depth(ui, id);
            self.text.set_scale(ui, self.width, depth);
        }
    }

    /// `TextControl::UpdateInactive`: the inactive children, then a
    /// running typewriter shows its whole text.
    pub fn update_inactive(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update_inactive(id);
        if self.typing {
            self.end_typing(ui, id);
        }
    }

    /// `TextControl::Update`: the children, the typewriter, the blink.
    pub fn update(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update(id);
        let has_children = self.text.has_child_slots();
        let visible = ui.node(id).is_some_and(|n| n.flags & VISIBLE != 0);
        if self.typewriter && has_children && visible && !ui.services.skip_local_server_tick() {
            if self.pending.is_empty() {
                match self.text.text() {
                    Some(source) => {
                        self.pending = crate::text::font::wide_c_str(source).to_vec();
                    }
                    None => self.clear_pending(),
                }
            }
            if !self.pending.is_empty() {
                if self.type_elapsed == 0.0 {
                    ui.services.play_ui_sound_slot_flag1(TYPEWRITER_SOUND);
                    if !self.typing {
                        self.typing = true;
                        ui.text_typing_count += 1;
                    }
                }
                self.type_elapsed = (ui.frame_time + f64::from(self.type_elapsed)) as f32;
                let length = i32::try_from(self.pending.len()).unwrap_or(i32::MAX);
                while self.type_elapsed > self.type_interval {
                    self.type_elapsed -= self.type_interval;
                    self.typed_count += 1;
                    if self.typed_count >= length {
                        self.typewriter = false;
                        if self.typing {
                            self.end_typing(ui, id);
                        }
                    }
                }
            }
        } else if self.typing && (!visible || !has_children) {
            self.end_typing(ui, id);
        }
        if !self.blink {
            return;
        }
        let mut sum = ui.frame_time + f64::from(self.blink_elapsed);
        self.blink_elapsed = sum as f32;
        if sum > 1.0 {
            let mut phase = self.blink_phase;
            loop {
                sum -= 1.0;
                phase = !phase;
                if !(sum > 1.0) {
                    break;
                }
            }
            self.blink_elapsed = sum as f32;
            self.blink_phase = phase;
        }
        self.override_colour = if self.blink_phase {
            self.blink_on
        } else {
            self.blink_off
        };
    }

    /// `TextControl::Draw`.
    pub fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        if host.game_option(OPT_INTERFACE) == 0
            || host.game_option(OPT_NEURONET) == 0
            || host.game_option(OPT_TEXT_CONTROLS) == 0
        {
            return;
        }
        ui.draw_children(id, host);
        let position = Self::position(ui, id);
        let offset = self.control.offset;
        if self.typewriter {
            if self.pending.is_empty() {
                self.pending = self
                    .text
                    .text()
                    .map(|t| crate::text::font::wide_c_str(t).to_vec())
                    .unwrap_or_default();
            }
            if !self.pending.is_empty() {
                if self.typed_count.cast_unsigned() > 0x3FF {
                    self.typed_count = 0x3FF;
                    self.typewriter = false;
                    if self.typing {
                        self.end_typing(ui, id);
                    }
                }
                let n = usize::try_from(self.typed_count).unwrap_or(0);
                let typed: Vec<u16> = self.pending.iter().copied().take(n).collect();
                self.text.set_scale(ui, self.width, position.z + offset.z);
                self.text.set_text(ui, Some(&typed));
            }
        }
        let world = Vector::new(
            position.x + offset.x,
            position.y + offset.y,
            position.z + offset.z,
        );
        self.text.set_scale(ui, self.width, world.z);
        if self.text.child_count(ui, host) == 0 {
            return;
        }
        if self.text.font().is_none() {
            let normal = self.text.find_font(ui, b"Normal");
            self.text.set_font(ui, normal);
        }
        if self.text.font().is_none() {
            if let Some(imm) = host.immediate() {
                imm.disable(capability::DEPTH_TEST);
            }
            return;
        }
        if -f64::from(world.z) < 0.001 {
            return;
        }
        let active = ui.node(id).is_some_and(|n| n.flags & ACTIVE != 0);
        if self.colour_override {
            let packed = u32::from_le_bytes(self.override_colour);
            self.text.set_draw_color(ui, packed);
        } else if self.highlighted {
            self.text.set_draw_color(ui, palette::AMBER);
        } else if !active {
            self.text.set_draw_color(ui, 0xFF80_8080);
        } else {
            let colour = self.text_colour;
            self.text.set_draw_color(ui, colour);
            if self.clickable {
                let ray = cursor_ray_inline(ui);
                if self.hit_test(ui, id, ray) && self.pulse_on_hover {
                    let pulse = ui.pulse.blend();
                    self.text.set_draw_color(ui, pulse);
                }
            }
        }
        self.line_count = i32::try_from(self.text.child_count(ui, host)).unwrap_or(i32::MAX);
        let (screen_w, screen_h) = text_extent(ui, self.width, self.height, world.z);
        let mut origin = DrawOrigin {
            x: 0.0,
            y: 0.0,
            width: screen_w,
            height: screen_h,
            h_mode: 0,
            v_mode: 0,
        };
        if self.cached_draw {
            let surface_width = ui.camera.surface.width;
            if self.cached_surface_width != surface_width {
                self.cache_valid = false;
            }
            if !self.cache_valid {
                self.cache_valid = true;
                let (x, y) = text_point(ui, world);
                self.cached_y = y;
                let (text_w, _) = self.text.extent(ui, Some(host));
                let text_w = f64::from(text_w.cast_unsigned());
                self.cached_x = ((f64::from(screen_w) - text_w) * 0.5 + f64::from(x)) as f32;
                self.cached_surface_width = surface_width;
            }
            origin.h_mode = 0;
            origin.x = self.cached_x;
            origin.y = self.cached_y;
        } else {
            origin.h_mode = self.justification.cast_unsigned();
            let (x, y) = text_point(ui, world);
            origin.x = x;
            origin.y = y;
        }
        let left = f32::from_bits(self.trim_left_bits.cast_unsigned());
        let right = f32::from_bits(self.trim_right_bits.cast_unsigned());
        self.text.draw_children(ui, host, origin, left, right);
        if let Some(imm) = host.immediate() {
            imm.disable(capability::DEPTH_TEST);
        }
    }

    /// `TextControl::SetProperty`: its own messages, the rest handed back
    /// for `WinControl::SetProperty`.
    ///
    /// # Errors
    ///
    /// The property when it is not the text control's.
    pub fn property<'a>(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        property: Property<'a>,
    ) -> Result<isize, Property<'a>> {
        Ok(match property {
            Property::Offset(offset) => {
                self.control.offset = offset;
                if self.control.mesh.is_none() {
                    return Ok(0);
                }
                let mut local = self.control.attach_position(ui, id);
                if !self.corner_anchor {
                    local.x -= self.width * 0.5;
                    local.y += self.height * 0.5;
                }
                let o = self.control.offset;
                let copy = if self.control.position_includes_offset {
                    Vector::new(local.x + o.x, local.y + o.y, local.z + o.z)
                } else {
                    o
                };
                base::set_position(ui, id, &mut self.control, copy);
                0
            }
            Property::Other(0xC, a, _) => {
                self.highlighted = a != 0;
                0
            }
            Property::SetText(text) => {
                self.put_text(ui, id, text);
                self.clear_pending();
                0
            }
            Property::Other(0x1E, a, _) => {
                self.set_ids(ui, id, a as u32);
                self.clear_pending();
                0
            }
            Property::Other(0x1F, _, _) => isize::from(self.text.text().is_some()),
            Property::Other(0x20, _, _) => self
                .text
                .text()
                .map_or(0, |t| crate::text::font::wide_c_str(t).len() as isize),
            Property::Other(0x21, _, _) => self.line_count as isize,
            Property::Other(0x22, a, b) => {
                self.field_33c = b as i32;
                self.field_338 = a as i32;
                0
            }
            Property::Other(0x23, a, b) => {
                self.trim_left_bits = a as i32;
                self.trim_right_bits = b as i32;
                0
            }
            Property::Other(0x24, a, _) => {
                self.clickable = a != 0;
                0
            }
            Property::Colour(rgba, on) => {
                self.colour_override = on;
                self.override_colour = rgba;
                0
            }
            Property::Other(0x50, a, b) => {
                if self.typing {
                    self.end_typing(ui, id);
                }
                self.typewriter = false;
                self.typed_count = 1;
                self.type_elapsed = 0.0;
                self.clear_pending();
                if a == 1 {
                    self.typewriter = true;
                    self.type_interval = (f64::from(b as u32) * f64::from(0.001_f32)) as f32;
                }
                0
            }
            Property::Other(0x51, a, _) => {
                self.blink = false;
                if a != 0 {
                    self.blink = true;
                    self.colour_override = true;
                }
                0
            }
            other => return Err(other),
        })
    }
}

/// The "TextControl" factory product.
#[derive(Debug, Default)]
pub struct TextControlWidget {
    pub text: TextControl,
}

macro_rules! text_control_slots {
    ($field:ident) => {
        fn as_any(&self) -> &dyn Any {
            self
        }
        fn as_any_mut(&mut self) -> &mut dyn Any {
            self
        }
        fn win_control(&self) -> Option<&WinControl> {
            Some(&self.$field.control)
        }
        fn win_control_mut(&mut self) -> Option<&mut WinControl> {
            Some(&mut self.$field.control)
        }
        fn text_control(&self) -> Option<&TextControl> {
            Some(&self.$field)
        }
        fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
            self.$field.hit_test(ui, id, ray)
        }
        fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
            self.$field.set_position(ui, id, position);
        }
        fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
            self.$field.set_orientation(ui, id, orientation);
        }
        fn create(
            &mut self,
            ui: &mut Ui,
            id: NodeId,
            parent: Option<NodeId>,
            payload: &mut Payload<'_>,
        ) -> bool {
            self.$field.create(ui, id, parent, payload)
        }
        fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
            base::destroy(ui, id, &mut self.$field.control);
        }
        fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
            let t = &mut self.$field;
            if t.typing {
                t.end_typing(ui, id);
            }
            t.text.clear_contents(ui);
            if t.in_capture {
                ui.remove_capture(id);
            }
            base::on_delete(ui, id, &mut t.control);
        }
        fn update_inactive(&mut self, ui: &mut Ui, id: NodeId) {
            self.$field.update_inactive(ui, id);
        }
        fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
            self.$field.draw(ui, id, host);
        }
        fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
            self.$field.mouse_event(ui, id, event)
        }
        fn broadcast(&mut self, ui: &mut Ui, id: NodeId, msg: u32, _a: u32, _b: u32) -> bool {
            base::broadcast(ui, id, self, msg)
        }
    };
}

impl Widget for TextControlWidget {
    text_control_slots!(text);
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        self.text.update(ui, id);
    }
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        match self.text.property(ui, id, property) {
            Ok(r) => r,
            Err(p) => base::property(ui, id, self, p),
        }
    }
}

/// `EditControl`: a text control the player types into.
#[derive(Debug)]
pub struct EditControl {
    pub text: TextControl,
    /// `+0x3a0`.
    pub cursor_timer: f32,
    /// `+0x3a4`: the text ends in the cursor.
    pub cursor_shown: bool,
    /// `+0x3a8`: above 0 the text's length plus one is capped by it.
    pub max_length: i32,
    /// `+0x3ac`.
    pub editable: bool,
    /// `+0x3ad`.
    pub digits_only: bool,
}

impl Default for EditControl {
    fn default() -> Self {
        EditControl {
            text: TextControl::new(),
            cursor_timer: 0.0,
            cursor_shown: false,
            max_length: -1,
            editable: true,
            digits_only: false,
        }
    }
}

/// The blink period of the edit cursor.
const EDIT_BLINK_SECONDS: f32 = 0.5;

impl EditControl {
    /// The string's text, as the edit control copies it (up to its NUL).
    fn current(&self) -> Option<Vec<u16>> {
        self.text
            .text
            .text()
            .map(|t| crate::text::font::wide_c_str(t).to_vec())
    }

    /// `RemoveCursor`: the text without its last character.
    pub fn remove_cursor(&mut self, ui: &mut Ui, id: NodeId) {
        if !self.cursor_shown {
            return;
        }
        if let Some(mut copy) = self.current()
            && !copy.is_empty()
        {
            copy.pop();
            self.text.put_text(ui, id, Some(&copy));
        }
        self.cursor_shown = false;
    }

    /// The parent hears the text (property 0x45).
    fn tell_text(ui: &mut Ui, id: NodeId, text: Option<Vec<u16>>, enter: bool) {
        let Some(parent) = ui.parent(id) else { return };
        if ui.win_control(parent).is_none() {
            return;
        }
        ui.property(
            parent,
            Property::EditText {
                from: id,
                text,
                enter,
            },
        );
    }

    /// `EditControl::Update`: while editable the cursor blinks, then
    /// `TextControl::Update`; not editable, nothing at all.
    pub fn update(&mut self, ui: &mut Ui, id: NodeId) {
        if !self.editable {
            return;
        }
        let sum = ui.frame_time + f64::from(self.cursor_timer);
        self.cursor_timer = sum as f32;
        if sum >= f64::from(EDIT_BLINK_SECONDS) {
            loop {
                self.cursor_timer =
                    (f64::from(self.cursor_timer) - f64::from(EDIT_BLINK_SECONDS)) as f32;
                if self.cursor_shown {
                    self.remove_cursor(ui, id);
                } else {
                    let mut copy = self.current().unwrap_or_default();
                    copy.push(u16::from(b'|'));
                    self.text.put_text(ui, id, Some(&copy));
                    self.cursor_shown = true;
                }
                if !(self.cursor_timer >= EDIT_BLINK_SECONDS) {
                    break;
                }
            }
        }
        self.text.update(ui, id);
    }

    /// `EditControl::KeyEvent`: Enter (with modifier bit 0, the release),
    /// Backspace and characters (numpad digits folded onto `0` to `9`, the
    /// key's low byte as a one-byte UTF-8 string) while active and
    /// editable; the presses (bit 1) are taken and do nothing.
    pub fn key_event(&mut self, ui: &mut Ui, id: NodeId, key: u32, mods: u8) -> bool {
        let active = ui.node(id).is_some_and(|n| n.flags & ACTIVE != 0);
        if !active || !self.editable {
            return false;
        }
        if key == 0x0D && mods & 1 != 0 {
            if ui.parent(id).is_none() {
                return true;
            }
            self.remove_cursor(ui, id);
            self.cursor_timer = 0.0;
            let text = self.current();
            Self::tell_text(ui, id, text, true);
            return true;
        }
        if key == 0x08 {
            if mods & 2 != 0 {
                return true;
            }
            self.remove_cursor(ui, id);
            self.cursor_timer = 0.0;
            let mut len = 0usize;
            if let Some(mut copy) = self.current() {
                len = copy.len();
                if len > 0 {
                    copy.pop();
                    self.text.put_text(ui, id, Some(&copy));
                }
            }
            let text = if len > 1 { self.current() } else { None };
            Self::tell_text(ui, id, text, false);
            return true;
        }
        let mut key = key;
        if key != 0x0D && (0x60..=0x69).contains(&key) {
            key -= 0x30;
        }
        if self.digits_only && !(0x30..=0x39).contains(&key) {
            return false;
        }
        if mods & 2 != 0 {
            return true;
        }
        if self.max_length > 0 {
            let len = self.current().map_or(0, |t| t.len());
            if i32::try_from(len).unwrap_or(i32::MAX).saturating_add(1) > self.max_length {
                return true;
            }
        }
        let byte = (key & 0xFF) as u8;
        let Some(ch) = utf8_single_byte(byte) else {
            return true;
        };
        self.remove_cursor(ui, id);
        self.cursor_timer = 0.0;
        let mut copy = self.current().unwrap_or_default();
        copy.push(ch);
        self.text.put_text(ui, id, Some(&copy));
        let text = self.current();
        Self::tell_text(ui, id, text, false);
        true
    }
}

/// `MultiByteToWideChar(CP_UTF8, 0, {byte}, strlen)`: a NUL gives nothing
/// (an empty string converts to no characters), an ASCII byte itself, any
/// other lone byte is invalid UTF-8 and becomes U+FFFD.
fn utf8_single_byte(byte: u8) -> Option<u16> {
    match byte {
        0 => None,
        0x01..=0x7F => Some(u16::from(byte)),
        _ => Some(0xFFFD),
    }
}

impl Widget for EditControl {
    text_control_slots!(text);
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        EditControl::update(self, ui, id);
    }
    fn key_event(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        key: u32,
        mods: &mut crate::ui::message::KeyMods,
    ) -> bool {
        EditControl::key_event(self, ui, id, key, mods.0)
    }
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        match property {
            Property::SetText(_) => {
                self.cursor_shown = false;
            }
            Property::Other(0x43, a, b) => {
                if a == 0 {
                    self.editable = false;
                    self.remove_cursor(ui, id);
                    return 1;
                }
                self.editable = true;
                if b != 0 {
                    self.max_length = b as i32;
                }
                return 1;
            }
            Property::Other(0x44, a, _) => {
                self.digits_only = a & 1 != 0;
                return 1;
            }
            _ => {}
        }
        match self.text.property(ui, id, property) {
            Ok(r) => r,
            Err(p) => base::property(ui, id, self, p),
        }
    }
}
