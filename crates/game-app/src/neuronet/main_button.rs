//! The NeuroNet bar (`nn_main_button.cpp`, the factory class
//! "NN_MainButton"): the row of NeuroNet icons at the top right of the
//! screen (nav map, inventory, mission log, player info, help and, in
//! multiplayer, chat and group) that slide out and open their panel.
//!
//! The port so far: the construction (`NN_MainButton::NN_MainButton` and
//! `Create`: the background, the icon pairs, the nine events, the slot
//! tables) and `NnSystemInit`'s window ([`system_init`]). `Update` (the
//! story star's tick, the voice queue, the dealer's inventory re-open),
//! `Draw`'s conditions (the HUD's pick block, the letterbox, the open
//! panel), `SetProperty`'s 0x4f while animating, `OnEvent` and the
//! panels' slide and scale (`ButtonMover`, `ButtonScaler`) come with the
//! NeuroNet panels (D1135); until then the bar draws its visible children
//! as `Win3D::Draw` does.

use neuron_client::ui::children::{
    ButtonInit, StaticInit, create_button_control, create_static_control,
};
use neuron_client::ui::control::{WinControl, base};
use neuron_client::ui::controls::button::ButtonWidget;
use neuron_client::ui::factory::Factory;
use neuron_client::ui::message::MouseButton;
use neuron_client::ui::tree::{ACTIVE, DrawHost, NodeId, Payload, Property, Ui, VISIBLE, Widget};
use neuron_core::math::{Matrix, Vector};

/// The factory class and the window's name.
pub const CLASS: &[u8] = b"NN_MainButton";
/// The class name as the factory registers it.
const CLASS_NAME: &str = "NN_MainButton";

/// The HUD events `Create` registers on the `Event::Handler` face, in its
/// order: the panel ids, then 0x6e and 0x6b.
pub const EVENTS: [u32; 9] = [0x63, 0x66, 0x67, 0x65, 0x6a, 0x68, 0x69, 0x6e, 0x6b];

/// The panel slots (`Buttons`, `Roots` and the per-panel tables).
pub const SLOTS: usize = 7;

/// The sound of every icon button.
const SELECT_SOUND: &[u8] = b"ui_select_item";

/// The bits of a reference `float`.
const fn f(bits: u32) -> f32 {
    f32::from_bits(bits)
}

const fn v(x: u32, y: u32, z: u32) -> Vector {
    Vector::new(f(x), f(y), f(z))
}

/// An icon pair: the button's name, the root's name, the mesh, x, the
/// root's tool tip and its command (the panel id).
type Pair = (&'static [u8], &'static [u8], &'static [u8], u32, i32, i32);

/// The five pairs of both modes, in `Create`'s order.
const COMMON_PAIRS: [Pair; 5] = [
    (
        b"NNNavmapButton",
        b"NNNavmapButtonRoot",
        b"interface\\NeuroNet\\icons\\NN_Map.3db",
        0x3ee9_78d6,
        0x3fc,
        0x63,
    ),
    (
        b"NNInventoryButton",
        b"NNInventoryButtonRoot",
        b"interface\\NeuroNet\\icons\\NN_Inventory.3db",
        0x3f01_0625,
        0x3ed,
        0x66,
    ),
    (
        b"NNMissionLogButton",
        b"NNMissionLogButtonRoot",
        b"interface\\NeuroNet\\icons\\NN_storystar.3db",
        0x3ed0_e561,
        0x421,
        0x67,
    ),
    (
        b"NNPlayerInfoButton",
        b"NNPlayerInfoButtonRoot",
        b"interface\\NeuroNet\\icons\\NN_PlayerStatus.3db",
        0x3f0d_4fe0,
        0x55f,
        0x65,
    ),
    (
        b"NNHelpButton",
        b"NNHelpButtonRoot",
        b"interface\\NeuroNet\\icons\\NN_info.3db",
        0x3f19_999a,
        0x570,
        0x6a,
    ),
];

/// The multiplayer pairs: chat and group.
const MULTIPLAYER_PAIRS: [Pair; 2] = [
    (
        b"NNChatButton",
        b"NNChatButtonRoot",
        b"interface\\NeuroNet\\icons\\NN_Chat.3db",
        0x3eb8_51ec,
        0x435,
        0x68,
    ),
    (
        b"NNGroupButton",
        b"NNGroupButtonRoot",
        b"interface\\NeuroNet\\icons\\NN_request.3db",
        0x3e9f_be78,
        0x560,
        0x69,
    ),
];

/// Where each button slides to when its panel opens (`OpenTarget`).
pub const OPEN_TARGET: [Vector; SLOTS] = [
    v(0, 0x3d75_c28f, 0xbfa0_0000),
    v(0, 0x3d8f_5c29, 0xbfa0_0000),
    v(0, 0x3d8f_5c29, 0xbfa0_0000),
    v(0, 0x3da3_d70a, 0xbfa0_0000),
    v(0, 0x3da3_d70a, 0xbfa0_0000),
    v(0, 0x3d61_47ae, 0xbf80_0000),
    v(0, 0x3d4c_cccd, 0xbf80_0000),
];

/// Where the open button sits while its panel shows (`PanelOrigin`, which
/// `SetPanelOrigin` overrides).
pub const PANEL_ORIGIN: [Vector; SLOTS] = [
    v(0xbf1e_b852, 0x3e99_999a, 0xbfa0_0000),
    v(0, 0x3d9b_a5e3, 0xbfa4_dd2f),
    v(0, 0x3d9b_a5e3, 0xbfa4_dd2f),
    v(0, 0x3d9b_a5e3, 0xbfa4_dd2f),
    v(0, 0x3d9b_a5e3, 0xbfa4_dd2f),
    v(0, 0, 0xbf80_0000),
    v(0, 0, 0xbf80_0000),
];

/// The "ScaledBackground" positions (`BackgroundPosition`).
pub const BACKGROUND_POSITION: [Vector; SLOTS] = [
    v(0, 0x3d8f_5c29, 0xbfbc_28f6),
    v(0, 0x3d9b_a5e3, 0xbfa4_dd2f),
    v(0, 0x3d9b_a5e3, 0xbfa4_dd2f),
    v(0, 0x3d9b_a5e3, 0xbfa4_dd2f),
    v(0, 0x3d9b_a5e3, 0xbfa4_dd2f),
    v(0, 0x3d61_47ae, 0xbf80_0000),
    v(0, 0x3d4c_cccd, 0xbf80_0000),
];

/// The background's collapsed scale per slot (`PanelScale`).
pub const PANEL_SCALE: [f32; SLOTS] = [
    f(0x3da3_d70a),
    f(0x3d4c_cccd),
    f(0x3d4c_cccd),
    f(0x3d4c_cccd),
    f(0x3d4c_cccd),
    f(0x3da3_d70a),
    f(0x3da3_d70a),
];

/// The window (`NN_MainButton`'s `WinControl` face and its state).
pub struct NnMainButton {
    pub control: WinControl,
    /// `SinglePlayer()` when `Create` runs: five pairs and the
    /// "HUD_nnbox5" background, or seven and "HUD_nnbox7".
    pub single_player: bool,
    /// `+0x3b4` "NNBackground".
    pub background: Option<NodeId>,
    /// `+0x3b8 Buttons`, in slot order: nav map, inventory, mission log,
    /// player info, help, chat, group (`+0x370..+0x388` by name).
    pub buttons: [Option<NodeId>; SLOTS],
    /// `+0x3d4 Roots`, the buttons' "Root" twins in the same order.
    pub roots: [Option<NodeId>; SLOTS],
    pub open_target: [Vector; SLOTS],
    pub panel_origin: [Vector; SLOTS],
    pub panel_scale: [f32; SLOTS],
    pub background_position: [Vector; SLOTS],
    /// `+0x3a8`, `+0x3ac`, `+0x3b0`: the open button, its panel id and
    /// its saved tool tip.
    pub open_button: Option<NodeId>,
    pub open_panel_id: i32,
    pub saved_tooltip: i32,
    /// `+0x508` "ScaledBackground".
    pub scaled_background: Option<NodeId>,
    /// `+0x50c..+0x518`: the closing button, its panel id, its saved tool
    /// tip and its slide start.
    pub closing_button: Option<NodeId>,
    pub closing_panel_id: i32,
    pub closing_tooltip: i32,
    pub closing_start: Vector,
    /// `+0x524`, `+0x528`: the queued button and panel id.
    pub queued_button: Option<NodeId>,
    pub queued_panel_id: i32,
    /// `+0x52c..+0x52f`.
    pub info_card_layout: bool,
    pub inventory_after_dealer: bool,
    pub inventory_with_dealer: bool,
    pub force_mission_log: bool,
}

impl Default for NnMainButton {
    /// `NN_MainButton::NN_MainButton`: everything zero (the reference
    /// leaves the tables to `Create`), single player.
    fn default() -> Self {
        Self {
            control: WinControl::default(),
            single_player: true,
            background: None,
            buttons: [None; SLOTS],
            roots: [None; SLOTS],
            open_target: [Vector::ZERO; SLOTS],
            panel_origin: [Vector::ZERO; SLOTS],
            panel_scale: [0.0; SLOTS],
            background_position: [Vector::ZERO; SLOTS],
            open_button: None,
            open_panel_id: 0,
            saved_tooltip: 0,
            scaled_background: None,
            closing_button: None,
            closing_panel_id: 0,
            closing_tooltip: 0,
            closing_start: Vector::ZERO,
            queued_button: None,
            queued_panel_id: 0,
            info_card_layout: false,
            inventory_after_dealer: false,
            inventory_with_dealer: false,
            force_mission_log: false,
        }
    }
}

/// The reference's panel-id switch: 0x66 -> 1, 0x67 -> 2, 0x65 -> 3,
/// 0x6a -> 4, 0x68 -> 5, 0x69 -> 6, anything else -> 0.
#[must_use]
pub fn panel_slot(panel_id: i32) -> usize {
    match panel_id {
        0x66 => 1,
        0x67 => 2,
        0x65 => 3,
        0x6a => 4,
        0x68 => 5,
        0x69 => 6,
        _ => 0,
    }
}

/// `NnMainButtonCreatePair`: the plain button at (x, 0.459, -1.25) with
/// no tool tip and no command, its rest offset there, hidden; then the
/// root with the tool tip and the command, spinning (property 0x36 = 1).
fn create_pair(ui: &mut Ui, bar: NodeId, pair: &Pair) -> (Option<NodeId>, Option<NodeId>) {
    let &(name, root_name, mesh, x, tooltip, command) = pair;
    let position = v(x, 0x3eeb_020c, 0xbfa0_0000);
    let half_extents = v(0x3c93_74bc, 0x3c93_74bc, 0);
    let init = ButtonInit {
        sound: Some(SELECT_SOUND),
        position,
        half_extents,
        ..ButtonInit::default()
    };
    let button = create_button_control(ui, bar, name, Some(mesh), &init);
    if let Some(button) = button {
        if let Some(b) = ui.widget_mut::<ButtonWidget>(button) {
            b.button.control.home_offset = position;
        }
        if let Some(n) = ui.node_mut(button) {
            n.flags &= !(ACTIVE | VISIBLE);
        }
    }
    let root_init = ButtonInit {
        tooltip,
        command,
        ..init
    };
    let root = create_button_control(ui, bar, root_name, Some(mesh), &root_init);
    if let Some(root) = root {
        ui.property(root, Property::Other(0x36, 1, 0));
    }
    (button, root)
}

/// The children `Create` makes, in its order.
#[derive(Default)]
struct Children {
    background: Option<NodeId>,
    buttons: [Option<NodeId>; SLOTS],
    roots: [Option<NodeId>; SLOTS],
}

impl Children {
    fn create(ui: &mut Ui, id: NodeId, single_player: bool) -> Self {
        // "NNBackground": no wire render, no clear render.
        let (mesh, offset): (&[u8], Vector) = if single_player {
            (
                b"Interface\\HUD\\HUD_nnbox5.cmp",
                v(0x3ece_5604, 0x3ebb_645a, 0xbf80_0000),
            )
        } else {
            (
                b"Interface\\HUD\\HUD_nnbox7.cmp",
                v(0x3eb9_5810, 0x3ebb_645a, 0xbf80_0000),
            )
        };
        let background = create_static_control(
            ui,
            id,
            b"NNBackground",
            Some(mesh),
            &StaticInit {
                offset,
                no_wire_render: true,
                no_clear_render: true,
                ..StaticInit::default()
            },
        );
        let mut made = Self {
            background,
            ..Self::default()
        };
        // Slots 0 to 4 are the common pairs in `Create`'s order; chat and
        // group take slots 5 and 6, null in single player.
        for (slot, pair) in COMMON_PAIRS.iter().enumerate() {
            (made.buttons[slot], made.roots[slot]) = create_pair(ui, id, pair);
        }
        if !single_player {
            for (slot, pair) in MULTIPLAYER_PAIRS.iter().enumerate() {
                (made.buttons[5 + slot], made.roots[5 + slot]) = create_pair(ui, id, pair);
            }
        }
        made
    }
}

impl Widget for NnMainButton {
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
    /// `NN_MainButton::Create`: the background, the icon pairs, the nine
    /// events, the slot tables, then `Win3D::Create`.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        // The children are made with the window back in its node, so that
        // they find it as their parent control.
        let single_player = self.single_player;
        let mut made = Children::default();
        ui.lend(id, self, |ui| {
            made = Children::create(ui, id, single_player)
        });
        self.background = made.background;
        self.buttons = made.buttons;
        self.roots = made.roots;
        for msg in EVENTS {
            ui.services.register_event(id, msg);
        }
        self.open_target = OPEN_TARGET;
        self.panel_origin = PANEL_ORIGIN;
        self.background_position = BACKGROUND_POSITION;
        self.panel_scale = PANEL_SCALE;
        ui.base_create(id, parent)
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

fn single_player_bar() -> Box<dyn Widget> {
    Box::new(NnMainButton::default())
}

fn multiplayer_bar() -> Box<dyn Widget> {
    Box::new(NnMainButton {
        single_player: false,
        ..NnMainButton::default()
    })
}

/// `WINFACTORY<NN_MainButton>`. The factory's constructor takes no
/// argument, so the class makes the single-player bar; [`system_init`]
/// shadows it with the multiplayer one around a multiplayer create.
pub fn register(factory: &mut Factory) {
    factory.register(CLASS_NAME, single_player_bar);
}

/// Whether `window` is a live NeuroNet bar (`G_NnMainButton != 0`).
#[must_use]
pub fn exists(ui: &Ui, window: Option<NodeId>) -> bool {
    window.is_some_and(|id| ui.widget::<NnMainButton>(id).is_some())
}

/// `NnSystemInit`'s window: nothing while the bar exists (`G_NnMainButton`
/// set); otherwise, with `create_main_button`, `WinFactoryCreate(
/// "NN_MainButton", "NN_MainButton", 0, 0)` into `window`.
/// `single_player` is `SinglePlayer()`.
///
/// Left out, as the other subsystems' parts of `NnSystemInit`: the story
/// star's `G_bNnStoryStarKeepPending = 0` (the story star), the
/// KnowledgeMap and InfocardMap singletons (`KnowledgeMapCreate`,
/// `InfocardMapCreate`, the NeuroNet's info-card data, which come with the
/// panels that read them) and `NnVoiceInit` (the NeuroNet voice queue,
/// which comes with the client's audio). None of them makes a window.
pub fn system_init(
    ui: &mut Ui,
    window: &mut Option<NodeId>,
    create_main_button: bool,
    single_player: bool,
) {
    if exists(ui, *window) {
        return;
    }
    *window = None;
    if !create_main_button {
        return;
    }
    if !single_player {
        ui.factory.register(CLASS_NAME, multiplayer_bar);
    }
    *window = ui.create_by_class(Some(CLASS), CLASS, None, &mut Payload::None);
    if !single_player {
        ui.factory.unregister(CLASS_NAME);
    }
}
