//! The page descriptor's rich-text widgets: `UIRichTextWin`
//! (`ui_rich_text_win.cpp`), a `RichTextWin` sized by its payload, and
//! `UITextMsgButton` (`ui_text_msg_button.cpp`), a centred caption that
//! pulses while hovered, carries an optional style mesh behind it and
//! tells its parent 0x3004 with its message argument when clicked.

use std::any::Any;
use std::rc::Rc;

use neuron_core::math::{Matrix, Vector, transform_matrix};

use crate::text::rdl::{Justify, PackedAttributes, RdlNode, RenderDisplayList, TextAttributes};
use crate::ui::control::{WinControl, base};
use crate::ui::controls::rich_text_win::RichTextWin;
use crate::ui::message::MouseButton;
use crate::ui::page::{EntryPayload, Style};
use crate::ui::pulse::palette;
use crate::ui::tree::{DrawHost, NodeId, Payload, Property, Ui, Widget};

/// `UIRichTextWin`.
#[derive(Debug, Default)]
pub struct UiRichTextWin {
    pub win: RichTextWin,
}

/// The payload's right and bottom, when it is one with a size.
fn payload_size(payload: &Payload<'_>) -> Option<(f32, f32)> {
    let Payload::Page(p) = payload else {
        return None;
    };
    Some(match p {
        EntryPayload::Text { right, bottom, .. }
        | EntryPayload::TextMsgButton { right, bottom, .. }
        | EntryPayload::Bounds { right, bottom }
        | EntryPayload::WinObject { right, bottom, .. }
        | EntryPayload::Table { right, bottom, .. }
        | EntryPayload::ScrollCycle { right, bottom, .. } => (*right, *bottom),
    })
}

/// The rich-text window's slots every page widget over it shares.
macro_rules! rich_text_slots {
    () => {
        fn as_any(&self) -> &dyn Any {
            self
        }
        fn as_any_mut(&mut self) -> &mut dyn Any {
            self
        }
        fn win_control(&self) -> Option<&WinControl> {
            Some(&self.win.base.control)
        }
        fn win_control_mut(&mut self) -> Option<&mut WinControl> {
            Some(&mut self.win.base.control)
        }
        fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
            self.win.hit_test(ui, id, ray)
        }
        fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
            self.win.destroy(ui, id);
        }
        fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
            self.win.on_delete(ui, id);
        }
        fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
            self.win.property(ui, id, property)
        }
    };
}

impl Widget for UiRichTextWin {
    rich_text_slots!();
    /// `UIRichTextWin::Create`: `RichTextWin::Create` without a payload,
    /// then the payload's size.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> bool {
        let created = self.win.create(ui, id, parent, &mut Payload::None);
        if created && let Some((w, h)) = payload_size(payload) {
            self.win.base.norm_width = w;
            self.win.base.norm_height = h;
        }
        created
    }
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        self.win.set_position(ui, id, position);
    }
    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        self.win.set_orientation(ui, id, orientation);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        self.win.draw(ui, id, host);
    }
}

/// The idle caption attributes (bytes 08 87 C3 E0).
pub const IDLE_ATTRS: PackedAttributes = 0xE0C3_8708;
/// The caption of a non-interactive button (bytes 08 80 80 80).
pub const INACTIVE_ATTRS: PackedAttributes = 0x8080_8008;
/// The `<default>` style mesh.
pub const DEFAULT_STYLE_MESH: &[u8] = b"Interface\\NeuroNet\\Inventory\\FRONT_button.cmp";

/// `UITextMsgButton`.
#[derive(Debug)]
pub struct UiTextMsgButton {
    pub win: RichTextWin,
    /// `+0x498`.
    pub interactive: bool,
    /// `+0x49c`, `+0x4a0`: the payload's size.
    pub local_width: f32,
    pub local_height: f32,
    /// `+0x4a4`: the click message's argument.
    pub message_arg: i32,
    /// `+0x4a8`.
    pub caption_body: Rc<RenderDisplayList>,
    /// `+0x4b4`.
    pub text_body: Rc<RenderDisplayList>,
    /// `+0x4c0`.
    pub caption_attrs: PackedAttributes,
}

impl Default for UiTextMsgButton {
    fn default() -> Self {
        UiTextMsgButton {
            win: RichTextWin::new(),
            interactive: false,
            local_width: 0.0,
            local_height: 0.0,
            message_arg: 0,
            caption_body: Rc::new(RenderDisplayList::new()),
            text_body: Rc::new(RenderDisplayList::new()),
            caption_attrs: 0,
        }
    }
}

impl UiTextMsgButton {
    /// `ApplyCaptionLayout`: centred, the caption list, the 0x828B style,
    /// the caption's colour, the text list.
    pub fn apply_caption_layout(&mut self) {
        self.win.clear_content();
        self.win
            .append_node(&RdlNode::Justify(Justify::from_value(1)));
        self.win
            .append_node(&RdlNode::Ref(Rc::clone(&self.caption_body)));
        self.win.append_node(&RdlNode::Style(0x828B));
        self.win
            .append_node(&RdlNode::Attributes(TextAttributes::new(
                self.caption_attrs,
                0xFFFF_FF00,
                0,
            )));
        self.win
            .append_node(&RdlNode::Ref(Rc::clone(&self.text_body)));
    }
}

impl Widget for UiTextMsgButton {
    rich_text_slots!();

    /// `UITextMsgButton::Create`.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> bool {
        let created = self.win.create(ui, id, parent, &mut Payload::None);
        self.interactive = true;
        self.caption_attrs = IDLE_ATTRS;
        let mut style = None;
        if let Payload::Page(EntryPayload::TextMsgButton {
            right,
            bottom,
            caption,
            extra_int,
            extra_id,
            style: s,
        }) = payload
        {
            self.local_width = *right;
            self.win.base.norm_width = *right;
            self.local_height = *bottom;
            self.win.base.norm_height = *bottom;
            let mut list = RenderDisplayList::new();
            if *extra_id != 0 {
                if let Some(c) = ui.catalogs.clone() {
                    crate::text::rdl::load_ids(&mut list, &c, extra_id.cast_unsigned());
                }
            } else {
                let end = caption
                    .iter()
                    .position(|&u| u == 0)
                    .unwrap_or(caption.len());
                list.push(RdlNode::Text(caption[..end].to_vec()));
            }
            self.text_body = Rc::new(list);
            self.message_arg = *extra_int;
            style = Some(s.clone());
        }
        self.win.base.set_align_mode(2);
        let mesh: Option<&[u8]> = match &style {
            Some(Style::Default) => Some(DEFAULT_STYLE_MESH),
            Some(Style::Named(name)) => Some(name),
            Some(Style::None) | None => None,
        };
        if let Some(path) = mesh {
            let path = path.to_vec();
            base::set_mesh(ui, id, &mut self.win.base.control, &path, b"");
            self.win.base.control.field_2d2 = true;
        }
        self.apply_caption_layout();
        created
    }

    /// `UITextMsgButton::SetPosition`: the style mesh at the button's
    /// centre on the z = -1 plane, written into the offset; then the
    /// window's.
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        let orientation = ui.node(id).map_or(Matrix::IDENTITY, |n| n.orientation);
        let half = Vector::new(self.local_width * 0.5, self.local_height * -0.5, 0.0);
        let centre = transform_matrix(&orientation, half);
        let sum_x = f64::from(centre.x) + f64::from(position.x);
        let sum_y = f64::from(centre.y) + f64::from(position.y);
        let scale = -1.0 / f64::from(position.z);
        self.win.base.control.offset = Vector::new(
            (sum_x * scale - f64::from(position.x)) as f32,
            (sum_y * scale - f64::from(position.y)) as f32,
            -1.0 - position.z,
        );
        self.win.set_position(ui, id, position);
    }

    /// `UITextMsgButton::SetOrientation`: `WinControl`'s, then a style mesh
    /// kept upright.
    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        base::set_orientation(ui, id, &mut self.win.base.control, orientation);
        if let Some(mesh) = self.win.base.control.mesh.as_mut() {
            mesh.set_orientation(Matrix::IDENTITY);
        }
    }

    /// `UITextMsgButton::Draw`: the caption's colour follows the hover, the
    /// idle and the non-interactive states (a change relays the content),
    /// then the window draws.
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        let hovered = ui.hover() == Some(id);
        let pulse = ui.pulse.blend_from(palette::AMBER);
        let mut bytes = self.caption_attrs.to_le_bytes();
        let mut relayout = false;
        if self.interactive {
            if hovered {
                let rgb = [pulse as u8, (pulse >> 8) as u8, (pulse >> 16) as u8];
                if bytes[1..4] != rgb {
                    bytes[1..4].copy_from_slice(&rgb);
                    self.caption_attrs = u32::from_le_bytes(bytes);
                    relayout = true;
                }
            } else if (self.caption_attrs ^ IDLE_ATTRS) & 0xFFFF_FF00 != 0 {
                self.caption_attrs = IDLE_ATTRS;
                relayout = true;
            }
        } else if (self.caption_attrs ^ INACTIVE_ATTRS) & 0xFFFF_FF00 != 0 {
            self.caption_attrs = INACTIVE_ATTRS;
            relayout = true;
        }
        if relayout {
            self.apply_caption_layout();
        }
        self.win.draw(ui, id, host);
    }

    /// An interactive button plays the rollover sound.
    fn mouse_enter(&mut self, ui: &mut Ui, _id: NodeId, _ray: Vector) {
        if self.interactive {
            ui.services.play_ui_rollover_sound();
        }
    }

    /// A press (field 1 is 2) on an interactive button tells the parent
    /// (slot 23: 0x3004 with the message argument) and is taken.
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        if event.field1() != 2 || event.field0() != -1 || !self.interactive {
            return false;
        }
        if let Some(parent) = ui.parent(id) {
            ui.send_broadcast(parent, 0x3004, self.message_arg.cast_unsigned(), 0);
        }
        true
    }
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;
    use std::rc::Rc;

    use super::*;
    use crate::ui::message::{ButtonAction, MouseButton};
    use crate::ui::widescreen::Surface;

    /// A parent that keeps the slot 23 messages it hears.
    struct Parent(Rc<RefCell<Vec<(u32, u32)>>>);

    impl Widget for Parent {
        fn as_any(&self) -> &dyn Any {
            self
        }
        fn as_any_mut(&mut self) -> &mut dyn Any {
            self
        }
        fn broadcast(&mut self, _: &mut Ui, _: NodeId, msg: u32, a: u32, _: u32) -> bool {
            self.0.borrow_mut().push((msg, a));
            true
        }
    }

    #[test]
    fn a_text_button_lays_out_its_caption_and_tells_its_parent_of_a_press() {
        let mut ui = Ui::new(Surface::new(640, 480));
        let heard = Rc::new(RefCell::new(Vec::new()));
        let parent = ui
            .create(
                Box::new(Parent(heard.clone())),
                None,
                None,
                &mut Payload::None,
            )
            .unwrap();
        let payload = EntryPayload::TextMsgButton {
            right: 0.3,
            bottom: 0.05,
            caption: "Back".encode_utf16().collect(),
            extra_int: 1001,
            extra_id: 0,
            style: Style::None,
        };
        let id = ui
            .create_by_class(
                Some(b"MainMenu"),
                b"UITextMsgButton",
                Some(parent),
                &mut Payload::Page(&payload),
            )
            .unwrap();
        let b = ui.widget::<UiTextMsgButton>(id).unwrap();
        assert!(b.interactive);
        assert_eq!(b.message_arg, 1001);
        assert_eq!(b.win.base.norm_width, 0.3);
        assert_eq!(b.win.base.align_mode, 2);
        assert_eq!(b.win.content.nodes.len(), 5);
        assert_eq!(
            b.text_body.nodes,
            vec![RdlNode::Text("Back".encode_utf16().collect())]
        );
        assert!(b.win.base.control.mesh.is_none());
        let press = MouseButton {
            index: 0,
            action: ButtonAction::Down,
        };
        assert!(ui.with_widget(id, false, |w, ui| w.mouse_event(ui, id, press)));
        assert_eq!(*heard.borrow(), vec![(0x3004, 1001)]);
        let release = MouseButton {
            index: 0,
            action: ButtonAction::Up,
        };
        assert!(!ui.with_widget(id, false, |w, ui| w.mouse_event(ui, id, release)));
        // The style mesh's offset puts it at the button's centre on z = -1.
        ui.set_position(id, Vector::new(-0.5, 0.25, -2.0));
        let offset = ui.win_control(id).unwrap().offset;
        assert_eq!(offset.z, 1.0);
    }
}
