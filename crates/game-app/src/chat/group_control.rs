//! The group-request window (`nn_group_control.cpp`, the factory class
//! "NN_GroupControl"): the "attention required" box of multiplayer group
//! invitations and trade requests, made hidden by `ChatInit`.
//!
//! The port so far: the construction (`NnGroupControl::Create`: the mesh,
//! the 25 texts, the request display, the Accept, Decline and Leave wire
//! buttons, the close button and the title, the half extents from the
//! mesh) and `RefreshRequestDisplay` with no request outstanding. The
//! request list, its expiry in `UpdateInactive`, the channel table and the
//! messages (`Slot23`, `SetProperty`, `HandleInviteMessage`) come with
//! multiplayer chat.

use std::rc::Rc;

use neuron_client::text::rdl::{Justify, RdlNode, RenderDisplayList, load_ids_text};
use neuron_client::ui::children::{
    ButtonInit, WireButtonInit, create_button_control, create_rich_text_win,
    create_rich_text_wire_button_control,
};
use neuron_client::ui::control::{WinControl, base};
use neuron_client::ui::controls::rich_text_win::RichTextWin;
use neuron_client::ui::controls::wire::WireWidget;
use neuron_client::ui::factory::Factory;
use neuron_client::ui::message::MouseButton;
use neuron_client::ui::tree::{ACTIVE, DrawHost, NodeId, Payload, Property, Ui, VISIBLE, Widget};
use neuron_core::math::{Matrix, Vector};

/// The factory class and the window's name.
pub const CLASS: &[u8] = b"NN_GroupControl";

/// `G_HudChromeColorRgba` (`nn_ui_pulse.cpp`).
pub const HUD_CHROME_COLOR: [u8; 4] = [0x87, 0xC3, 0xE0, 0xC0];

/// `G_GroupBtnLeaveX`: never written.
const LEAVE_X: f32 = 0.0;

/// The texts, in `kNnGroupText` order: style (0 for none), IDS, the debug
/// text of a missing entry. `Create` loads them in another order (the
/// order of [`LOAD_ORDER`]).
const TEXTS: [(u16, u32, &str); 25] = [
    (0x828b, 0x60020, "DEBUG - Leave Group"),
    (
        0,
        0x60064,
        "DEBUG - You have received a group invitation from:",
    ),
    (
        0,
        0x6006f,
        "DEBUG - You have received a trade request from:",
    ),
    (0x828b, 0x60013, "DEBUG - Accept"),
    (0x828b, 0x6001a, "DEBUG - Decline"),
    (0x828b, 0x60014, "DEBUG - Accept Trade"),
    (0x828b, 0x6001b, "DEBUG - Decline Trade"),
    (
        0x9ca4,
        0x6006d,
        "DEBUG - There are no outstanding group invitations.",
    ),
    (0x3ea8, 0x60065, "DEBUG - Group invitation sent to:"),
    (
        0x3ea8,
        0x60067,
        "DEBUG - You have joined into a group with:",
    ),
    (0x3ea8, 0x60066, "DEBUG - New group member:"),
    (0x3ea8, 0x60069, "DEBUG - You have left your group."),
    (0x3ea8, 0x60068, "DEBUG - A group member has left:"),
    (
        0x3ea8,
        0x60060,
        "DEBUG - You must abort or complete your mission.",
    ),
    (
        0x3ea8,
        0x60061,
        "DEBUG - A player failed to join your group due to already being involved in a mission: ",
    ),
    (
        0x3ea8,
        0x6005b,
        "DEBUG - You cannot invite yourself to join a group.",
    ),
    (0x3ea8, 0x60059, "DEBUG - Your group is full."),
    (
        0x3ea8,
        0x6005a,
        "DEBUG - You cannot invite a player already in your group.",
    ),
    (0x3ea8, 0x6005e, "DEBUG - You cannot join your own group."),
    (
        0x3ea8,
        0x60062,
        "DEBUG - You have not received an invitation to this group:",
    ),
    (
        0x3ea8,
        0x6005d,
        "DEBUG - You are already a member of this group:",
    ),
    (
        0x3ea8,
        0x6005c,
        "DEBUG - Sorry, but this group is now full:",
    ),
    (
        0x3ea8,
        0x6005f,
        "DEBUG - You cannot leave your group because you are not in a group.",
    ),
    (0x3ea8, 0x60051, "DEBUG - A player has died:"),
    (0, 0x60012, "DEBUG - ATTENTION REQUIRED"),
];

/// `Create`'s loading order of the texts (indices into [`TEXTS`]).
const LOAD_ORDER: [usize; 25] = [
    1, 2, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
];

const TEXT_LEAVE: usize = 0;
const TEXT_ACCEPT: usize = 3;
const TEXT_DECLINE: usize = 4;
const TEXT_NO_INVITATIONS: usize = 7;
const TEXT_ATTENTION: usize = 24;

/// `NnGroupControl`.
#[derive(Default)]
pub struct NnGroupControl {
    pub control: WinControl,
    /// `+0x360`: the texts (shared by the reference nodes that show them).
    pub texts: Vec<Rc<RenderDisplayList>>,
    pub btn_close: Option<NodeId>,
    pub btn_leave: Option<NodeId>,
    pub btn_accept: Option<NodeId>,
    pub btn_decline: Option<NodeId>,
    pub request_display: Option<NodeId>,
    pub attention_title: Option<NodeId>,
    /// `+0x340`: the group's members by chat id (filled by the chat's
    /// messages, which come with multiplayer chat).
    pub group_members: std::collections::BTreeSet<u32>,
}

/// Registers the "NN_GroupControl" class.
pub fn register(factory: &mut Factory) {
    factory.register("NN_GroupControl", || Box::new(NnGroupControl::default()));
}

/// `LoadGroupText`: the style node, if any, then the IDS text (or the
/// debug text).
fn load_group_text(ui: &Ui, style: u16, ids: u32, debug: &str) -> RenderDisplayList {
    let mut list = RenderDisplayList::default();
    if style != 0 {
        list.push(RdlNode::Style(style));
    }
    let debug: Vec<u16> = debug.encode_utf16().collect();
    match ui.catalogs.as_deref() {
        Some(c) => {
            load_ids_text(&mut list, c, ids, Some(&debug));
        }
        None => {
            list.push(RdlNode::Justify(Justify::from_value(1)));
            list.push(RdlNode::Text(debug));
        }
    }
    list
}

fn with_rich_text(ui: &mut Ui, id: Option<NodeId>, f: impl FnOnce(&mut RichTextWin)) {
    if let Some(r) = id.and_then(|id| ui.widget_mut::<RichTextWin>(id)) {
        f(r);
    }
}

/// The caption window of a rich-text wire button (`TextWinRef`).
fn caption_of(ui: &Ui, button: Option<NodeId>) -> Option<NodeId> {
    ui.widget::<WireWidget>(button?)?.wire.caption
}

fn set_flags(ui: &mut Ui, id: Option<NodeId>, f: impl FnOnce(u8) -> u8) {
    if let Some(n) = id.and_then(|id| ui.node_mut(id)) {
        n.flags = f(n.flags);
    }
}

impl NnGroupControl {
    fn text_ref(&self, index: usize) -> Option<RdlNode> {
        self.texts.get(index).map(|t| RdlNode::Ref(Rc::clone(t)))
    }

    /// `RefreshRequestDisplay` with no request outstanding: Accept and
    /// Decline inactive, the request display "no outstanding invitations",
    /// centred; Leave active while the group has members. (The main
    /// button's alert comes with the HUD.)
    pub fn refresh_request_display(&mut self, ui: &mut Ui) {
        set_flags(ui, self.btn_accept, |f| f & !ACTIVE);
        set_flags(ui, self.btn_decline, |f| f & !ACTIVE);
        let no_invitations = self.text_ref(TEXT_NO_INVITATIONS);
        with_rich_text(ui, self.request_display, |r| {
            r.clear_content();
            r.append_node(&RdlNode::Justify(Justify::from_value(1)));
            r.append_node(&RdlNode::Style(0x9ca4));
            if let Some(n) = &no_invitations {
                r.append_node(n);
            }
        });
        let in_group = !self.group_members.is_empty();
        set_flags(ui, self.btn_leave, |f| {
            if in_group { f | ACTIVE } else { f & !ACTIVE }
        });
    }
}

/// The children `Create` makes.
#[derive(Default)]
struct Children {
    request_display: Option<NodeId>,
    btn_accept: Option<NodeId>,
    btn_decline: Option<NodeId>,
    btn_leave: Option<NodeId>,
    btn_close: Option<NodeId>,
    attention_title: Option<NodeId>,
}

impl Children {
    fn store(self, g: &mut NnGroupControl) {
        g.request_display = self.request_display;
        g.btn_accept = self.btn_accept;
        g.btn_decline = self.btn_decline;
        g.btn_leave = self.btn_leave;
        g.btn_close = self.btn_close;
        g.attention_title = self.attention_title;
    }

    /// The children of `Create`, made while the window is back in its node.
    fn create(ui: &mut Ui, id: NodeId, texts: &[Rc<RenderDisplayList>]) -> Children {
        let mut c = Children::default();
        let text_ref = |i: usize| texts.get(i).map(|t| RdlNode::Ref(Rc::clone(t)));
        c.request_display = ui.create_by_class(
            Some(b"RequestDisplay"),
            b"RichTextWin",
            Some(id),
            &mut Payload::None,
        );
        if let Some(rd) = c.request_display {
            ui.with_widget(rd, (), |w, ui| {
                let Some(r) = w.as_any_mut().downcast_mut::<RichTextWin>() else {
                    return;
                };
                let r = &mut r.base;
                r.set_attach_offset(ui, rd, Vector::new(-0.23, 0.2, 0.0));
                r.set_norm_size_in(ui, rd, 0.46, 0.15);
                r.set_auto_scrollbar(true);
                r.frame_color = HUD_CHROME_COLOR;
                r.set_align_mode(2);
                r.hover.ids_alt = 0x1b83;
                r.tooltip_enabled = true;
            });
        }

        let half = Vector::new(0.089, 0.021, 0.0);
        let wire = |position: Vector, tooltip: i32| WireButtonInit {
            sound: Some(b"ui_select_item"),
            position,
            half_extents: half,
            tooltip,
            ..WireButtonInit::default()
        };
        c.btn_accept = create_rich_text_wire_button_control(
            ui,
            id,
            b"BtnAccept",
            &wire(Vector::new(-0.11, -0.03, 0.0), 0x1b80),
        );
        append_caption(ui, c.btn_accept, text_ref(TEXT_ACCEPT));
        c.btn_decline = create_rich_text_wire_button_control(
            ui,
            id,
            b"BtnDecline",
            &wire(Vector::new(0.12, -0.03, 0.0), 0x1b81),
        );
        append_caption(ui, c.btn_decline, text_ref(TEXT_DECLINE));
        c.btn_leave = create_rich_text_wire_button_control(
            ui,
            id,
            b"BtnLeave",
            &wire(Vector::new(LEAVE_X, -0.1, 0.0), 0x1b82),
        );
        append_caption(ui, c.btn_leave, text_ref(TEXT_LEAVE));

        c.btn_close = create_button_control(
            ui,
            id,
            b"BtnClose",
            Some(b"Interface\\NeuroNet\\Inventory\\X.3db"),
            &ButtonInit {
                sound: Some(b"ui_select_item"),
                position: Vector::new(0.23, 0.23, 0.0),
                half_extents: Vector::new(0.02, 0.02, 0.0),
                tooltip: 0x409,
                ..ButtonInit::default()
            },
        );

        // 0.035 + 0.2 in single precision, as the reference's constant.
        let title_y = 0.035_f32 + 0.2_f32;
        c.attention_title = create_rich_text_win(
            ui,
            id,
            b"AttentionTitle",
            0.46,
            0.035,
            Vector::new(-0.23, title_y, 0.0),
            false,
        );
        let attention = text_ref(TEXT_ATTENTION);
        with_rich_text(ui, c.attention_title, |r| {
            r.append_node(&RdlNode::Style(0x9ca4));
            r.append_node(&RdlNode::Style(0x1df5));
            if let Some(n) = &attention {
                r.append_node(n);
            }
        });

        set_flags(ui, c.btn_leave, |f| f | ACTIVE | VISIBLE);
        set_flags(ui, c.btn_accept, |f| f | ACTIVE | VISIBLE);
        set_flags(ui, c.btn_decline, |f| f | ACTIVE | VISIBLE);
        c
    }
}

/// The text node appended to a wire button's caption window.
fn append_caption(ui: &mut Ui, button: Option<NodeId>, node: Option<RdlNode>) {
    let caption = caption_of(ui, button);
    with_rich_text(ui, caption, |r| {
        if let Some(n) = &node {
            r.append_node(n);
        }
    });
}

impl Widget for NnGroupControl {
    fn as_any(&self) -> &dyn std::any::Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn std::any::Any {
        self
    }
    fn win_control(&self) -> Option<&WinControl> {
        Some(&self.control)
    }
    fn win_control_mut(&mut self) -> Option<&mut WinControl> {
        Some(&mut self.control)
    }
    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        base::hit_test(ui, id, &self.control, ray)
    }
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        base::set_position(ui, id, &mut self.control, position);
    }
    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        base::set_orientation(ui, id, &mut self.control, orientation);
    }
    /// `NnGroupControl::Create`: `Win3D::Create`, the offset (0, 0.052, 0),
    /// the mesh, `+0x2d2`, the 25 texts, the children, Accept, Decline and
    /// Leave shown, the empty request display, then the half extents from
    /// the mesh.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        if !ui.base_create(id, parent) {
            return false;
        }
        self.control.offset = Vector::new(0.0, 0.052, 0.0);
        base::set_mesh(
            ui,
            id,
            &mut self.control,
            b"Interface\\intro\\objects\\MULTI_attentionreq'd.cmp",
            b"",
        );
        self.control.field_2d2 = true;
        let mut texts: Vec<Option<Rc<RenderDisplayList>>> = vec![None; TEXTS.len()];
        for &i in &LOAD_ORDER {
            let (style, ids, debug) = TEXTS[i];
            texts[i] = Some(Rc::new(load_group_text(ui, style, ids, debug)));
        }
        self.texts = texts.into_iter().flatten().collect();
        // The children are made with the window back in its node, so that
        // they find it as their parent control; their ids come out after.
        let mut made = Children::default();
        let texts = self.texts.clone();
        ui.lend(id, self, |ui| made = Children::create(ui, id, &texts));
        made.store(self);
        self.refresh_request_display(ui);
        let _ = self.control.half_extents_from_mesh();
        true
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        base::destroy(ui, id, &mut self.control);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        base::on_delete(ui, id, &mut self.control);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        ui.draw_children(id, host);
    }
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        base::mouse_event(ui, id, self, event)
    }
    fn broadcast(&mut self, ui: &mut Ui, id: NodeId, msg: u32, _a: u32, _b: u32) -> bool {
        base::broadcast(ui, id, self, msg)
    }
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        base::property(ui, id, self, property)
    }
}
