//! `WireStaticControl`, `WireButtonControl` and
//! `RichTextWireButtonControl` (`wire_controls.cpp`): the NeuroNet "wire
//! box" controls, a rectangle of thin lines around a hit box that the
//! dialogs use as buttons.
//!
//! The box's edges (a mask of top, right, bottom, left) go to the
//! immediate-mode layer's line batcher in the state's colour: grey while
//! inactive, yellow while lit or pressed, the hover pulse while focused or
//! hovered, else the idle cyan (white when "always white", a property's
//! colour over all). A wire button takes presses like a mesh button and
//! posts its command or tells the parent 0x26; the rich-text one carries
//! a centred caption window on its box. A column of wire controls shares
//! the edges between its rows ([`join_column`]).

use std::any::Any;

use neuron_core::math::{Matrix, Vector, mul_matrix};

use crate::text::rdl::{Justify, RdlNode, RenderDisplayList};
use crate::ui::control::{WinControl, base, cursor_ray_inline, cursor_ray_near_plane, tell_parent};
use crate::ui::controls::button::run_blink;
use crate::ui::controls::rich_text_win::RichTextWin;
use crate::ui::controls::text_control::{OPT_INTERFACE, OPT_NEURONET};
use crate::ui::message::MouseButton;
use crate::ui::pulse::palette;
use crate::ui::services::{HoverKey, ToolTipHover};
use crate::ui::tree::{ACTIVE, DrawHost, NodeId, Payload, Property, Ui, VISIBLE, Widget};

/// The line width of every edge.
const WIRE_LINE_WIDTH: f32 = 0.003;

/// The wire button's parts.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WireButton {
    /// `+0x388`: 0x01 holds the press, 0x02 right button, 0x04 enabled.
    pub state: u8,
    /// `+0x38c`.
    pub command_id: i32,
}

/// `WireStaticControl` (and, with [`WireButton`] parts,
/// `WireButtonControl`).
#[derive(Debug)]
pub struct WireControl {
    pub control: WinControl,
    /// `+0x32c`.
    pub wire_orientation: Matrix,
    /// `+0x350`: the position plus the offset.
    pub wire_center: Vector,
    /// `+0x35c`: 0x01 hovered, 0x02 focused, 0x04 pressed, 0x08 always
    /// white, 0x10 lit, 0x20 lit while pressed, 0x40 pulse while hovered,
    /// 0x80 draw while inactive.
    pub wire_state: u8,
    /// `+0x35d` bit 0: flush both line batchers at the end of the draw.
    pub wire_state2: u8,
    pub blink: bool,
    pub blink_phase: bool,
    pub blink_timer: f32,
    /// `+0x364`: the tooltip record's command (nothing writes it).
    pub tooltip_aux: i32,
    pub sound_name: Vec<u8>,
    pub colour: [u8; 4],
    pub use_colour: bool,
    pub sound_slot: i32,
    /// `+0x384`: bit 0 top, 1 right, 2 bottom, 3 left.
    pub edge_mask: u32,
    pub button: Option<WireButton>,
    /// The rich-text button's caption window.
    pub caption: Option<NodeId>,
}

impl Default for WireControl {
    fn default() -> Self {
        WireControl::new()
    }
}

fn position(ui: &Ui, id: NodeId) -> Vector {
    ui.node(id).map_or(Vector::ZERO, |n| n.position)
}

fn add3(a: Vector, b: Vector) -> Vector {
    Vector::new(a.x + b.x, a.y + b.y, a.z + b.z)
}

impl WireControl {
    /// The `WireStaticControl` constructor.
    #[must_use]
    pub fn new() -> WireControl {
        WireControl {
            control: WinControl::new(),
            wire_orientation: Matrix::IDENTITY,
            wire_center: Vector::ZERO,
            wire_state: 0x60,
            wire_state2: 0,
            blink: false,
            blink_phase: false,
            blink_timer: 0.0,
            tooltip_aux: 0,
            sound_name: Vec::new(),
            colour: [0; 4],
            use_colour: false,
            sound_slot: 0,
            edge_mask: 0,
            button: None,
            caption: None,
        }
    }

    /// The `WireButtonControl` constructor.
    #[must_use]
    pub fn new_button() -> WireControl {
        WireControl {
            button: Some(WireButton {
                state: 0x04,
                command_id: 0,
            }),
            ..WireControl::new()
        }
    }

    /// `SetPosition`.
    pub fn set_position(&mut self, ui: &mut Ui, id: NodeId, new_position: Vector) {
        self.control.field_2e0 = new_position;
        self.control.attached = true;
        let placed = if self.control.position_includes_offset {
            add3(new_position, self.control.attach_position(ui, id))
        } else {
            new_position
        };
        base::set_position(ui, id, &mut self.control, placed);
        let p = position(ui, id);
        self.wire_center = if self.control.position_includes_offset {
            add3(self.control.offset, p)
        } else {
            p
        };
    }

    /// `SetOrientation`: times the attach orientation, also kept at
    /// `+0x32c`; no mesh, no children.
    pub fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        self.control.attached = true;
        let attach = self.control.attach_orientation(ui, id);
        let result = mul_matrix(&orientation, &attach);
        if let Some(n) = ui.node_mut(id) {
            n.orientation = result;
        }
        self.wire_orientation = result;
    }

    /// `WireStaticControl::Create`: the `Win3D` part, then the parent placed
    /// at its own position.
    pub fn create(&mut self, ui: &mut Ui, id: NodeId, parent: Option<NodeId>) -> bool {
        ui.base_create(id, parent);
        if let Some(p) = ui.parent(id) {
            let at = position(ui, p);
            ui.set_position(p, at);
            if let Some(pp) = crate::ui::control::parent_slot5_position(ui, id) {
                self.set_position(ui, id, pp);
            }
        }
        true
    }

    /// `WireStaticControl::Init`.
    #[allow(clippy::too_many_arguments)]
    pub fn init_static(
        &mut self,
        ui: &Ui,
        id: NodeId,
        offset: Vector,
        half_extents: Vector,
        always_white: u8,
        no_z_enable: bool,
        tooltip: i32,
        edge_mask: u32,
    ) {
        self.edge_mask = if edge_mask != 0 { edge_mask } else { 0xF };
        self.control.offset = offset;
        self.wire_center = add3(offset, position(ui, id));
        self.control.half_extents = half_extents;
        self.wire_state = (self.wire_state & 0xF7) | ((always_white << 3) & 0x08);
        self.control.no_z_enable = no_z_enable;
        self.control.tooltip_ids = tooltip;
    }

    /// `WireButtonControl::Init`: the static part, the command, the sound.
    #[allow(clippy::too_many_arguments)]
    pub fn init_button(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        sound: Option<&[u8]>,
        offset: Vector,
        half_extents: Vector,
        always_white: u8,
        no_z_enable: bool,
        tooltip: i32,
        command: i32,
        edge_mask: u32,
    ) {
        self.init_static(
            ui,
            id,
            offset,
            half_extents,
            always_white,
            no_z_enable,
            tooltip,
            edge_mask,
        );
        if let Some(b) = self.button.as_mut() {
            b.command_id = command;
        }
        if let Some(sound) = sound.filter(|s| !s.is_empty()) {
            self.sound_name = sound.to_vec();
            self.sound_slot = ui.services.find_ui_sound_slot(sound);
        }
    }

    /// `RichTextWireButtonControl::Init`: the button's, then a centred
    /// `RichTextWin` child named after the control plus `Text` on the box,
    /// with the caption id in the 0x828B style.
    #[allow(clippy::too_many_arguments)]
    pub fn init_rich_text(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        sound: Option<&[u8]>,
        offset: Vector,
        half_extents: Vector,
        always_white: u8,
        no_z_enable: bool,
        tooltip: i32,
        caption_ids: u32,
        command: i32,
        edge_mask: u32,
    ) {
        self.init_button(
            ui,
            id,
            sound,
            offset,
            half_extents,
            always_white,
            no_z_enable,
            tooltip,
            command,
            edge_mask,
        );
        let mut name = ui.node(id).map(|n| n.name().to_vec()).unwrap_or_default();
        name.extend_from_slice(b"Text");
        self.caption =
            ui.create_by_class(Some(&name), b"RichTextWin", Some(id), &mut Payload::None);
        let Some(text) = self.caption else { return };
        let h = self.control.half_extents;
        let o = self.control.offset;
        let attach = Vector::new(o.x - h.x, o.y + h.y, o.z);
        ui.property(text, Property::Offset(attach));
        let Some(win) = ui.widget_mut::<RichTextWin>(text) else {
            return;
        };
        win.base.set_norm_size(h.x + h.x, h.y + h.y);
        win.base.set_auto_scrollbar(false);
        win.base.never_hit = true;
        win.base.set_align_mode(2);
        if caption_ids == 0 {
            return;
        }
        let mut list = RenderDisplayList::new();
        list.push(RdlNode::Style(0x828B));
        list.push(RdlNode::Justify(Justify::from_value(1)));
        if let Some(c) = ui.catalogs.clone() {
            crate::text::rdl::load_ids(&mut list, &c, caption_ids);
        }
        if let Some(win) = ui.widget_mut::<RichTextWin>(text) {
            win.append_content(&list);
        }
    }

    /// `Update` (and the button's, which follows a held press).
    pub fn update(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update(id);
        if self.blink {
            run_blink(ui, &mut self.blink_timer, &mut self.blink_phase);
        }
        let Some(b) = self.button.as_ref() else {
            return;
        };
        if b.state & 0x01 == 0 {
            return;
        }
        let ray = cursor_ray_inline(ui);
        if self.hit_test(ui, id, ray) {
            if self.wire_state & 0x04 != 0 {
                return;
            }
            self.wire_state |= 0x04;
            tell_parent(ui, id, 0x2B, 0);
            if ui.parent(id).is_none() {
                return;
            }
            ui.pulse.reset();
            tell_parent(ui, id, 0x2C, 0);
            return;
        }
        if self.wire_state & 0x04 == 0 {
            return;
        }
        self.wire_state &= 0xFB;
        tell_parent(ui, id, 0x2A, 0);
        tell_parent(ui, id, 0x2D, 0);
    }

    /// The four edges of [`Self::draw`]: `(from, to)` per set bit of the
    /// mask, in the reference's order (bottom, right, top, left).
    #[must_use]
    pub fn edges(&self, center: Vector) -> Vec<(Vector, Vector)> {
        let h = self.control.half_extents;
        let c = center;
        let mut corner_a = [-h.x, -h.y, -h.z];
        let mut corner_b = [-corner_a[0], corner_a[1], corner_a[2]];
        let at = |v: [f32; 3]| Vector::new(c.x + v[0], c.y + v[1], c.z + v[2]);
        let mut out = Vec::new();
        if self.edge_mask & 0x4 != 0 {
            out.push((at(corner_a), at(corner_b)));
        }
        corner_a[0] = -corner_a[0];
        corner_a[1] = -corner_a[1];
        if self.edge_mask & 0x2 != 0 {
            out.push((at(corner_b), at(corner_a)));
        }
        corner_b[0] = -corner_b[0];
        corner_b[1] = -corner_b[1];
        if self.edge_mask & 0x1 != 0 {
            out.push((at(corner_a), at(corner_b)));
        }
        let corner_a = [-h.x, -h.y, -h.z];
        if self.edge_mask & 0x8 != 0 {
            out.push((at(corner_b), at(corner_a)));
        }
        out
    }

    /// `WireStaticControl::Draw`.
    pub fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        if host.game_option(OPT_INTERFACE) == 0 || host.game_option(OPT_NEURONET) == 0 {
            return;
        }
        let flags = ui.node(id).map_or(0, |n| n.flags);
        if flags & VISIBLE == 0 {
            return;
        }
        if self.wire_state & 0x80 == 0 && flags & ACTIVE == 0 {
            return;
        }
        if self.blink && self.blink_phase {
            return;
        }
        ui.draw_children(id, host);
        let saved_lit = (self.wire_state >> 4) & 1;
        let mut rgba;
        if flags & ACTIVE == 0 {
            self.wire_state &= 0xEB;
            rgba = [0x80, 0x80, 0x80, 0xFF];
        } else if saved_lit != 0 || (self.wire_state & 0x20 != 0 && self.wire_state & 0x04 != 0) {
            rgba = [0xFF, 0xFF, 0x00, 0xFF];
        } else if self.wire_state & 0x02 != 0
            || (self.wire_state & 0x01 != 0 && self.wire_state & 0x40 != 0)
        {
            rgba = ui.pulse.blend().to_le_bytes();
        } else {
            rgba = palette::IDLE.to_le_bytes();
        }
        if self.wire_state & 0x08 != 0 {
            rgba = [0xFF; 4];
        }
        if self.use_colour {
            rgba = self.colour;
        }
        let center = add3(position(ui, id), self.control.offset);
        self.wire_center = center;
        let argb = (u32::from(rgba[3]) << 24)
            | (u32::from(rgba[0]) << 16)
            | (u32::from(rgba[1]) << 8)
            | u32::from(rgba[2]);
        let rgb = argb & 0x00FF_FFFF;
        let edges = self.edges(center);
        if let Some(imm) = host.immediate() {
            for (from, to) in edges {
                imm.batch_line(from, to, argb, rgb, WIRE_LINE_WIDTH);
            }
        }
        self.wire_state = (self.wire_state & 0xEF) | (saved_lit << 4);
        if self.wire_state2 & 0x01 != 0
            && let Some(imm) = host.immediate()
        {
            imm.flush_batches();
        }
    }

    /// Slot 4: never hit (`Win3D`'s) for the static control,
    /// `WinControl::HitTest` for the buttons.
    pub fn hit_test(&self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        if self.button.is_none() {
            return false;
        }
        base::hit_test_with(ui, id, &self.control, ray, |ui| self.show_tooltip(ui, id))
    }

    /// `ShowToolTip`: the record at the wire centre with `+0x364` (the
    /// buttons: the command) as its binding.
    pub fn show_tooltip(&self, ui: &mut Ui, _id: NodeId) {
        let aux = self
            .button
            .as_ref()
            .map_or(self.tooltip_aux, |b| b.command_id);
        let hover = ToolTipHover {
            key: HoverKey::Ids(self.control.tooltip_ids),
            ids_alt: self.control.tooltip_ids,
            aux,
            world_pos: self.wire_center,
            half_extents: self.control.half_extents,
            live: true,
            ..Default::default()
        };
        ui.feed_tooltip_hover(&hover);
    }

    /// `WireButtonControl::MouseEnter`.
    pub fn mouse_enter(&mut self, ui: &mut Ui, id: NodeId) {
        let Some(b) = self.button.as_ref() else {
            return;
        };
        let enabled = b.state & 0x04 != 0;
        if ui.parent(id).is_some() {
            ui.pulse.reset();
            tell_parent(ui, id, 0x2C, 0);
        }
        self.wire_state |= 0x01;
        if enabled {
            ui.services.play_ui_rollover_sound();
        }
    }

    /// `WireButtonControl::MouseLeave`.
    pub fn mouse_leave(&mut self, ui: &mut Ui, id: NodeId) {
        if self.button.is_none() {
            return;
        }
        if self.control.tooltip_ids != 0 {
            ui.hide_tooltip();
        }
        tell_parent(ui, id, 0x2D, 0);
        self.wire_state &= 0xFE;
    }

    /// `WireButtonControl::MouseEvent` (false for the static control,
    /// whose slot 19 is `WinControl`'s).
    pub fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        let Some(state) = self.button.as_ref().map(|b| b.state) else {
            return false;
        };
        if state & 0x04 == 0 {
            return false;
        }
        if let Some(parent) = ui.parent(id)
            && ui.any_captured()
            && ui.win_control(parent).is_some_and(|c| !c.captured)
        {
            return false;
        }
        if state & 0x01 != 0 {
            if event.field0() != -1 || event.field1() != 1 {
                return false;
            }
            self.wire_state &= 0xFB;
            if let Some(b) = self.button.as_mut() {
                b.state &= 0xFE;
            }
            ui.remove_capture(id);
            let ray = cursor_ray_near_plane(ui);
            if !self.hit_test(ui, id, ray) {
                tell_parent(ui, id, 0x29, 0);
                self.wire_state &= 0xFE;
                return true;
            }
            if self.sound_slot != -1 {
                ui.services
                    .play_or_rewind_ui_sound_slot(self.sound_slot.cast_unsigned());
            } else {
                let name = self.sound_name.clone();
                ui.services.play_sound_arch(&name);
            }
            if state & 0x02 != 0 {
                let Some(parent) = ui.parent(id) else {
                    return true;
                };
                if ui.win_control(parent).is_none() {
                    return true;
                }
                if tell_parent(ui, id, 0x27, 0) != 0 {
                    return true;
                }
                tell_parent(ui, id, 0x26, 0);
                return true;
            }
            self.activate(ui, id);
            return true;
        }
        if event.field0() != -1 {
            return false;
        }
        if event.field1() != 2 && event.field1() != 3 {
            return false;
        }
        if let Some(b) = self.button.as_mut() {
            if event.field1() == 3 {
                b.state |= 0x02;
            } else {
                b.state &= 0xFD;
            }
        }
        tell_parent(ui, id, 0x28, 0);
        self.wire_state |= 0x04;
        if let Some(b) = self.button.as_mut() {
            b.state |= 0x01;
        }
        if ui.parent(id).is_some() {
            ui.pulse.reset();
            tell_parent(ui, id, 0x2C, 0);
        }
        ui.push_capture(id);
        true
    }

    /// `WireButtonControl::Activate`.
    pub fn activate(&mut self, ui: &mut Ui, id: NodeId) {
        let command = self.button.as_ref().map_or(0, |b| b.command_id);
        if command != 0 {
            ui.services.post_command(command.cast_unsigned());
            return;
        }
        tell_parent(ui, id, 0x26, 0);
    }

    /// `SetProperty`'s own messages.
    ///
    /// # Errors
    ///
    /// The property when it is `WinControl`'s.
    pub fn property<'a>(&mut self, property: Property<'a>) -> Result<isize, Property<'a>> {
        Ok(match property {
            Property::GetCommand(out) if self.button.is_some() => {
                *out = self.button.as_ref().map_or(0, |b| b.command_id);
                0
            }
            Property::Other(0x0C, a, _) => {
                self.wire_state = (self.wire_state & 0xEF) | if a != 0 { 0x10 } else { 0 };
                0
            }
            Property::Other(0x2C, ..) => {
                self.wire_state |= 0x02;
                0
            }
            Property::Other(0x2D, ..) => {
                self.wire_state &= 0xFD;
                0
            }
            Property::Colour(rgba, on) => {
                self.colour = rgba;
                self.use_colour = on;
                0
            }
            Property::Other(0x34, a, _) => {
                self.wire_state &= 0x7F;
                if a != 0 {
                    self.wire_state |= 0x80;
                }
                0
            }
            Property::Other(0x51, a, _) => {
                self.blink = a != 0;
                0
            }
            other => return Err(other),
        })
    }
}

/// `WireControlsJoinColumn`: each control's edge mask from its state, its
/// neighbours' and whether the next one is shown, so a highlighted row and
/// its lit neighbours share edges.
pub fn join_column(ui: &mut Ui, controls: &[NodeId]) {
    let wire = |ui: &Ui, id: NodeId| -> Option<(u8, u32, bool, u8)> {
        let w = ui
            .node(id)
            .and_then(|n| n.widget())
            .and_then(|w| w.as_any().downcast_ref::<WireWidget>())?;
        let flags = ui.node(id).map_or(0, |n| n.flags);
        Some((
            w.wire.wire_state,
            w.wire.edge_mask,
            w.wire.use_colour,
            flags,
        ))
    };
    let set_mask = |ui: &mut Ui, id: NodeId, mask: u32| {
        if let Some(w) = ui.widget_mut::<WireWidget>(id) {
            w.wire.edge_mask = mask;
        }
    };
    let mut prev: Option<NodeId> = None;
    for (index, &cur) in controls.iter().enumerate() {
        let next = controls.get(index + 1).copied();
        let Some((state, _, cur_colour, _)) = wire(ui, cur) else {
            continue;
        };
        let next_w = next.and_then(|n| wire(ui, n));
        let prev_w = prev.and_then(|p| wire(ui, p));
        let focused = (state >> 1) & 1 != 0;
        let mut alone = false;
        if focused || state & 1 != 0 {
            alone = true;
            if next_w.is_some_and(|w| w.0 & 0x10 != 0) || prev_w.is_some_and(|w| w.0 & 0x10 != 0) {
                alone = false;
            }
        }
        let mask = match next_w {
            None => 0xF,
            Some((_, _, _, nf)) if nf & 2 == 0 || nf & 1 == 0 => 0xF,
            _ if state & 0x10 != 0 || alone => 0xF,
            Some((n_state, ..)) => {
                if !focused && state & 1 == 0 {
                    if prev_w.is_some_and(|w| w.0 & 0x13 != 0) {
                        0xA
                    } else {
                        0xB
                    }
                } else {
                    let mut m = 0xA;
                    if prev_w.is_none_or(|w| w.0 & 0x10 == 0) {
                        m = 0xB;
                    }
                    if n_state & 0x10 == 0 {
                        m |= 4;
                    }
                    m
                }
            }
        };
        set_mask(ui, cur, mask);
        if let (Some(p), Some((_, p_mask, p_colour, _))) = (prev, prev.and_then(|p| wire(ui, p)))
            && p_mask & 4 == 0
            && mask & 1 != 0
            && !p_colour
            && cur_colour
        {
            set_mask(ui, p, p_mask | 4);
            set_mask(ui, cur, mask & !1);
        }
        prev = Some(cur);
    }
}

/// The wire controls' factory product ("WireStaticControl",
/// "WireButtonControl", "RichTextWireButtonControl").
#[derive(Debug, Default)]
pub struct WireWidget {
    pub wire: WireControl,
}

impl WireWidget {
    #[must_use]
    pub fn button() -> WireWidget {
        WireWidget {
            wire: WireControl::new_button(),
        }
    }
}

impl Widget for WireWidget {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    fn win_control(&self) -> Option<&WinControl> {
        Some(&self.wire.control)
    }
    fn win_control_mut(&mut self) -> Option<&mut WinControl> {
        Some(&mut self.wire.control)
    }
    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        self.wire.hit_test(ui, id, ray)
    }
    fn show_tooltip(&self, ui: &mut Ui, id: NodeId) {
        self.wire.show_tooltip(ui, id);
    }
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        self.wire.set_position(ui, id, position);
    }
    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        self.wire.set_orientation(ui, id, orientation);
    }
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        self.wire.create(ui, id, parent)
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        base::destroy(ui, id, &mut self.wire.control);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        if self
            .wire
            .button
            .as_ref()
            .is_some_and(|b| b.state & 0x01 != 0)
        {
            ui.remove_capture(id);
        }
        base::on_delete(ui, id, &mut self.wire.control);
    }
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        self.wire.update(ui, id);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        self.wire.draw(ui, id, host);
    }
    fn mouse_enter(&mut self, ui: &mut Ui, id: NodeId, _ray: Vector) {
        self.wire.mouse_enter(ui, id);
    }
    fn mouse_leave(&mut self, ui: &mut Ui, id: NodeId, _ray: Vector) {
        self.wire.mouse_leave(ui, id);
    }
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        if self.wire.button.is_none() {
            return base::mouse_event(ui, id, self, event);
        }
        self.wire.mouse_event(ui, id, event)
    }
    fn broadcast(&mut self, ui: &mut Ui, id: NodeId, msg: u32, _a: u32, _b: u32) -> bool {
        base::broadcast(ui, id, self, msg)
    }
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        match self.wire.property(property) {
            Ok(r) => r,
            Err(p) => base::property(ui, id, self, p),
        }
    }
}
