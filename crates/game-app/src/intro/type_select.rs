//! The main menu's column (`type_select.cpp`): `TypeSelect`, the window of
//! the five "bImg" button meshes, the five caption buttons
//! (`TextMsgButton`) and the "Version" text, and `EnterMenuState` case 1's
//! build of it (`menu_enter_state.cpp`, `EnterCase1`).
//!
//! The column slides in from the left on the frame time (1.5 per second,
//! the time capped at 0.5 s) and out again when a caption is clicked; the
//! second hidden frame of the slide-out hands the click's command to the
//! menu as its next state.
//!
//! Not ported yet: the multiplayer notice and the LAN or Internet choice of
//! command 4 and the exit dialog of command 0xb and of `USER_CANCEL`
//! (`NnPromptRunYesNo`, `NnPromptRunChoice`, `RequestClientQuitDialog`:
//! modal NeuroNet dialogs), divergence D1102.

use neuron_client::text::font::FontId;
use neuron_client::ui::children::{StaticInit, TextInit, create_text_control, init_static_control};
use neuron_client::ui::controls::list_box::{FontMetrics, ListMetrics};
use neuron_client::ui::controls::text_win::TextWin;
use neuron_client::ui::message::MouseButton;
use neuron_client::ui::tree::{ACTIVE, DrawHost, NodeId, Payload, Property, Ui, VISIBLE, Widget};
use neuron_core::math::Vector;

/// `ui_motion_swish`'s UI sound slot.
const SWISH_SLOT: u32 = 0x1b;
/// The click message of the column's buttons (slot 23).
pub const MSG_CLICK: u32 = 0x3004;

/// The five captions' string ids and commands (`kMainColumnSlots`): New
/// Game 0x10, Load Game 3, Multiplayer 4, Options 0xa, Exit 0xb.
pub const COLUMN_SLOTS: [(u32, i32); 5] = [
    (0x4f4, 0x10),
    (0x4f5, 0x03),
    (0x4f6, 0x04),
    (0x4f8, 0x0a),
    (0x4f9, 0x0b),
];
/// The version text's format ("VER. %d.%d").
pub const VERSION_FORMAT_IDS: u32 = 0x4f7;
/// `kFrontButtonMesh`.
pub const BUTTON_MESH: &[u8] = b"Interface\\NeuroNet\\Inventory\\FRONT_button.cmp";

const SLIDE_X: f32 = -0.455;
const COLUMN_TOP_Y: f32 = 0.172_5;
const COLUMN_STEP_Y: f32 = 0.082_2;
const COLUMN_Z: f32 = -1.0;
const STATIC_X: f32 = -0.33;
const STATIC_Y0: f32 = 0.155;
const STATIC_STEP_Y: f32 = 0.081_5;
const BOX_WIDTH: f32 = 0.25;
const FONT_LINE_SCALE: f32 = 0.002;

/// `TextMsgButton`: a caption that sends its command to its parent's slot
/// 23 on a left press.
#[derive(Default)]
pub struct TextMsgButton {
    pub text_win: TextWin,
    /// `+0xec`.
    pub command: i32,
    /// `+0xe4`, `+0xe5`: set and never read.
    pub field_e4: u8,
    pub field_e5: u8,
}

impl TextMsgButton {
    /// The constructor: `+0xe6` (the palette) on.
    #[must_use]
    pub fn new() -> Self {
        let mut b = Self::default();
        b.text_win.use_palette = true;
        b
    }

    /// `SetFlagsE4E5`.
    pub fn set_flags_e4_e5(&mut self, a: u8, b: u8) {
        self.text_win.flag_e3 = 1;
        self.field_e4 = a;
        self.field_e5 = b;
    }
}

impl Widget for TextMsgButton {
    fn as_any(&self) -> &dyn std::any::Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn std::any::Any {
        self
    }
    fn text_win(&self) -> Option<&TextWin> {
        Some(&self.text_win)
    }
    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        let position = ui.node(id).map_or(Vector::ZERO, |n| n.position);
        self.text_win.hit_test(position, ray)
    }
    /// `TextWin::Create`: the `Win3D` part, the text defaults, a 0.1 box.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        ui.base_create(id, parent);
        self.text_win.init_text_defaults();
        self.text_win.width = 0.1;
        self.text_win.height = 0.1;
        true
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        self.text_win.destroy(ui, id);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        self.text_win.draw(ui, id, host);
    }
    fn mouse_enter(&mut self, ui: &mut Ui, _id: NodeId, _ray: Vector) {
        if self.text_win.use_palette {
            ui.services.play_ui_rollover_sound();
        }
    }
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        if event.field1() != 2 || !self.text_win.use_palette || event.field0() != -1 {
            return false;
        }
        let Some(parent) = ui.parent(id) else {
            return false;
        };
        ui.send_broadcast(parent, MSG_CLICK, self.command.cast_unsigned(), 0)
    }
}

/// `TypeSelect`.
#[derive(Default)]
pub struct TypeSelect {
    /// `+0x80`: the state the menu enters next, 0 for none.
    pub next_state: i32,
    /// `+0x84`: the five caption buttons (members of the column).
    pub buttons: [Option<NodeId>; 5],
    /// `+0x534`: the five "bImg" static controls.
    pub statics: [Option<NodeId>; 5],
    /// `+0x548`: the "Version" text control.
    pub version: Option<NodeId>,
    /// Whether multiplayer can be offered (`ProbeMultiplayerAvailable`: the
    /// login manager's network client; the port always has one).
    pub multiplayer_available: bool,
    /// `+0x554`, `+0x558`: the probe's period and last time, in ms.
    pub poll_interval_ms: u32,
    pub last_poll_ms: u32,
    /// `+0x560`.
    pub anim_time: f64,
    /// `+0x568`.
    pub pending_command: i32,
    /// `+0x56c`.
    pub animating: bool,
    /// `+0x570`: frames spent hidden after the slide-out.
    pub phase: i32,
    /// Whether this frame's x87 arithmetic runs at 24 bits: the Direct3D 8
    /// device leaves that precision for the client's first frames before
    /// the frame restores 53 bits (as the in-process server's, D1400). The
    /// slide-in's sum is then rounded to single precision.
    pub single_precision: bool,
}

fn now_ms(ui: &mut Ui) -> u32 {
    // `timeGetTime`: the client's elapsed time in whole milliseconds.
    neuron_core::num::trunc_to_i32_via_i64(ui.services.elapsed_time() * 1000.0).cast_unsigned()
}

impl TypeSelect {
    /// `ProbeMultiplayerAvailable` onto the Multiplayer caption's palette.
    fn probe(&self, ui: &mut Ui) {
        let available = self.multiplayer_available;
        if let Some(b) = self.buttons[2].and_then(|id| ui.widget_mut::<TextMsgButton>(id)) {
            b.text_win.use_palette = available;
        }
    }

    /// `PlaceColumnButton`: button `index` and its mesh at the slide's
    /// current time.
    fn place(&self, ui: &mut Ui, index: usize) {
        let count = 6 - i32::try_from(index).unwrap_or(0);
        let offset = f64::from(count) * 0.1;
        let slide_x = offset + self.anim_time - 0.455_000_013_113_021_85;
        let step = f64::from(COLUMN_STEP_Y);
        let y =
            (f64::from(COLUMN_TOP_Y) - f64::from(u8::try_from(index).unwrap_or(0)) * step) as f32;
        let (x, static_x) = if slide_x > -0.455_000_013_113_021_85 {
            (SLIDE_X, 0.0)
        } else {
            (slide_x as f32, (offset + self.anim_time) as f32)
        };
        if let Some(b) = self.buttons[index] {
            ui.set_position(b, Vector::new(x, y, COLUMN_Z));
        }
        if let Some(s) = self.statics[index] {
            ui.set_position(s, Vector::new(static_x, 0.0, 0.0));
        }
    }

    /// The slide-out a click starts (cases 3, 0xa and 0x10): false while
    /// one runs.
    fn start_slide_out(&mut self, command: i32) -> bool {
        if self.animating {
            return false;
        }
        self.pending_command = command;
        self.phase = 0;
        self.animating = true;
        self.anim_time = 0.35;
        true
    }
}

impl Widget for TypeSelect {
    fn as_any(&self) -> &dyn std::any::Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn std::any::Any {
        self
    }
    /// `TypeSelect::Create`: the time at 1, the `Win3D` part, the
    /// multiplayer probe and its one-second poll.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        self.anim_time = 1.0;
        ui.base_create(id, parent);
        self.probe(ui);
        self.poll_interval_ms = 1000;
        self.last_poll_ms = now_ms(ui);
        true
    }
    /// `TypeSelect::Destroy`: hidden, every child destroyed (the static
    /// controls deleted), the version text deleted, unregistered.
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        if let Some(n) = ui.node_mut(id) {
            n.flags &= !(ACTIVE | VISIBLE);
        }
        for child in ui.children(id) {
            let is_static = ui
                .node(child)
                .and_then(|n| n.class_name)
                .is_some_and(|c| c == "StaticControl");
            ui.destroy(child);
            if is_static {
                ui.delete(child);
            }
        }
        if let Some(v) = self.version.take() {
            ui.delete(v);
        }
        self.statics = [None; 5];
        ui.unregister(id);
    }
    /// `TypeSelect::Update`: the children, the multiplayer poll, then the
    /// slide in or out.
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update(id);
        let now = now_ms(ui);
        if now.wrapping_sub(self.last_poll_ms) > self.poll_interval_ms {
            self.probe(ui);
            self.last_poll_ms = now;
        }
        let dt = ui.frame_time;
        if self.anim_time <= 1.0 && !self.animating {
            let sum = dt.min(0.5) * 1.5 + self.anim_time;
            self.anim_time = if self.single_precision {
                f64::from(sum as f32)
            } else {
                sum
            };
            for i in 0..5 {
                self.place(ui, i);
            }
            if self.anim_time > -0.8 && self.anim_time < -0.1 {
                ui.services.play_ui_sound_slot(SWISH_SLOT);
            }
            return;
        }
        if !self.animating {
            return;
        }
        self.anim_time -= dt * 1.5;
        for i in 0..5 {
            self.place(ui, i);
        }
        if self.anim_time < 0.0 && self.anim_time > -0.4 {
            ui.services.play_ui_sound_slot(SWISH_SLOT);
        }
        // At or below -0.9 (an unordered time continues as above it).
        #[allow(clippy::neg_cmp_op_on_partial_ord)]
        let above = !(self.anim_time <= -0.9);
        if above {
            return;
        }
        self.phase += 1;
        for slot in self.buttons.iter().chain(&self.statics) {
            if let Some(n) = slot.and_then(|id| ui.node_mut(id)) {
                n.flags &= !(ACTIVE | VISIBLE);
            }
        }
        if self.phase >= 2 {
            self.next_state = self.pending_command;
            self.animating = false;
        }
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        ui.draw_children(id, host);
    }
    /// `TypeSelect::Slot23`: a caption's click.
    fn broadcast(&mut self, ui: &mut Ui, _id: NodeId, msg: u32, a: u32, _b: u32) -> bool {
        if msg != MSG_CLICK {
            return false;
        }
        ui.services.play_or_rewind_ui_sound_slot(0);
        match a {
            3 | 0xa | 0x10 => self.start_slide_out(a.cast_signed()),
            // The multiplayer notice and the exit dialog are modal NeuroNet
            // dialogs, not ported yet: answered as declined (D1102).
            4 | 0xb => true,
            0x11 => {
                self.next_state = 0x11;
                true
            }
            _ => false,
        }
    }
    fn property(&mut self, _ui: &mut Ui, _id: NodeId, _property: Property<'_>) -> isize {
        0
    }
}

/// The client's version as the C++ packs it (`g_client.packed_version`);
/// the port's executable carries no version resource, so it is the
/// reference's fallback, 1.0 build 11.
pub const PACKED_VERSION: u32 = ((1 << 8) << 16) | 0xb;

/// `swprintf(format, major, minor)` for the two `%d`s of the version format.
fn format_version(format: &[u16], major: u32, minor: u32) -> Vec<u16> {
    let text = String::from_utf16_lossy(format);
    let mut values = [major, minor].into_iter();
    let mut out = String::new();
    let mut rest = text.as_str();
    while let Some(at) = rest.find("%d") {
        out.push_str(&rest[..at]);
        out.push_str(&values.next().map_or_else(String::new, |v| v.to_string()));
        rest = &rest[at + 2..];
    }
    out.push_str(rest);
    out.encode_utf16().collect()
}

/// The font's line height (`IFont` slot 6), 0 without the font system.
fn line_height(ui: &mut Ui, name: &[u8]) -> u32 {
    let Some(sys) = ui.fonts.as_mut() else {
        return 0;
    };
    let font: Option<FontId> = sys.find_font(name);
    FontMetrics { sys, font }.line_height().cast_unsigned()
}

/// `EnterCase1`'s column: the column created again (its buttons are its
/// members: made once, created again each time), the five meshes, the five
/// captions, the slide parked at -2, and the version text.
///
/// `single_precision`: the frame's x87 precision (24 bits in the client's
/// first frames, D1400); the column's float arithmetic then rounds every
/// step to single precision instead of once at the store.
pub fn build_column(ui: &mut Ui, column: NodeId, single_precision: bool) {
    if let Some(c) = ui.widget_mut::<TypeSelect>(column) {
        c.next_state = 0;
        c.animating = false;
        c.pending_command = 0;
        c.multiplayer_available = true;
    }
    ui.with_widget(column, false, |w, ui| {
        w.create(ui, column, None, &mut Payload::None)
    });
    let height = line_height(ui, b"Normal");
    #[allow(clippy::cast_precision_loss)]
    let box_height = if single_precision {
        height as f32 * FONT_LINE_SCALE
    } else {
        (f64::from(height) * f64::from(FONT_LINE_SCALE)) as f32
    };

    let mut statics = [None; 5];
    for (i, slot) in statics.iter_mut().enumerate() {
        let made = ui.create_by_class(
            Some(b"bImg"),
            b"StaticControl",
            Some(column),
            &mut Payload::None,
        );
        *slot = made;
        let Some(id) = made else { continue };
        let index = u8::try_from(i).unwrap_or(0);
        let y = if single_precision {
            STATIC_Y0 - f32::from(index) * STATIC_STEP_Y
        } else {
            (f64::from(STATIC_Y0) - f64::from(index) * f64::from(STATIC_STEP_Y)) as f32
        };
        init_static_control(
            ui,
            id,
            Some(BUTTON_MESH),
            &StaticInit {
                offset: Vector::new(STATIC_X, y, COLUMN_Z),
                no_wire_render: true,
                no_coloring: true,
                no_clear_render: true,
                ..StaticInit::default()
            },
        );
    }

    let mut buttons = ui
        .widget::<TypeSelect>(column)
        .map_or([None; 5], |c| c.buttons);
    for (i, &(ids, command)) in COLUMN_SLOTS.iter().enumerate() {
        let mut caption = ui.ids_string(ids);
        caption.truncate(0x3f);
        let id = match buttons[i] {
            Some(id) if ui.contains(id) => id,
            _ => ui.alloc(Some(Box::new(TextMsgButton::new()))),
        };
        buttons[i] = Some(id);
        // `TextMsgButton::Init`.
        if let Some(b) = ui.widget_mut::<TextMsgButton>(id) {
            b.command = command;
            b.text_win.use_palette = true;
        }
        ui.with_widget(id, false, |w, ui| {
            w.create(ui, id, Some(column), &mut Payload::None)
        });
        ui.with_widget(id, (), |w, ui| {
            if let Some(b) = w.as_any_mut().downcast_mut::<TextMsgButton>() {
                b.text_win.width = BOX_WIDTH;
                b.text_win.height = box_height;
                b.text_win.set_text(ui, &caption);
                b.text_win.set_font(ui, b"ButtonText", 1.0);
            }
        });
    }
    for id in buttons.iter().flatten() {
        if let Some(b) = ui.widget_mut::<TextMsgButton>(*id) {
            b.text_win.align = 1;
            b.set_flags_e4_e5(1, 1);
        }
    }

    let version = create_text_control(
        ui,
        column,
        b"Version",
        &TextInit {
            style: Some(b"Normal"),
            position: Vector::new(0.32, -0.35, -1.0),
            width: BOX_WIDTH,
            height: box_height,
            justification: 1,
            ..TextInit::default()
        },
    );
    if let Some(v) = version {
        let format = ui.ids_string(VERSION_FORMAT_IDS);
        let word = (PACKED_VERSION >> 16) & 0xffff;
        let text = format_version(&format, (word >> 8) & 0xff, word & 0xff);
        ui.property(v, Property::SetText(Some(&text)));
    }

    if let Some(c) = ui.widget_mut::<TypeSelect>(column) {
        c.statics = statics;
        c.buttons = buttons;
        c.version = version;
        c.anim_time = -2.0;
    }
}

/// The column's node: made once, without a class name, as the reference's
/// static `G_TypeSelect`.
pub fn make_column(ui: &mut Ui) -> NodeId {
    ui.alloc(Some(Box::new(TypeSelect::default())))
}
