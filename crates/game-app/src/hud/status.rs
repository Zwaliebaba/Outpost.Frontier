//! `HUD_Status` (`hud_status_core.cpp`, the `[Status]` section of
//! `hud.ini`): the player-status panel at the bottom right, with the ship's
//! weapon list, the repair and battery buttons and their counts, the loot
//! button, and the special-mode captions (turret view, rear view, mouse
//! flight, tradelane) across the screen.
//!
//! The port so far: the construction (`HUD_Status::HUD_Status`, `Create`,
//! with `HUD_Target_Base`'s part), the update (`Update`, with the panel's
//! open and close through [`ButtonMover`] and [`ButtonScaler`], the
//! weapon rows, the counts, the captions) and the clicks that only open or
//! close the panel or scroll the list. What the element reads of the
//! player and of the client is a [`StatusInput`] its owner sets
//! ([`set_input`]). Left for the HUD's input and draw: the sub-target
//! tool tip, the launcher toggling (`ToggleLauncher`), the command switch
//! (`OnEvent`), the weapon groups' first selection and `Draw` with
//! `RefreshWeaponRows`' colours (divergence D1121).
//!
//! The tenth row's number text reads ids `0x515 + 9` = 1310 before the
//! update fills it: the ids `Create` gives the number column count up
//! from 1301, the string table's "1", and its digits stop at 1309, "9";
//! 1310 is a comm line. The update replaces the text of the rows it shows
//! (`SetNumberText`), so the line is only seen in a hidden row.

pub mod button_mover;
pub mod update;

use neuron_client::ui::children::{
    ButtonInit, StaticInit, TextInit, create_button_control, create_static_control,
    create_text_control,
};
use neuron_client::ui::control::{WinControl, base};
use neuron_client::ui::controls::scroll::UiScroll;
use neuron_client::ui::controls::text_control::TextControlWidget;
use neuron_client::ui::message::MouseButton;
use neuron_client::ui::page::EntryPayload;
use neuron_client::ui::tree::{DrawHost, NodeId, Payload, Property, Ui, Widget};
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};

pub use self::button_mover::{ButtonMover, ButtonScaler};
pub use self::update::{find_hardpoint_row, rebuild_weapon_list};
use super::HudItem;

/// The factory's section.
pub const SECTION: &[u8] = b"Status";
/// `HUDItem::Name`.
pub const ITEM_NAME: &[u8] = b"Test_Status";
/// The window's name and class name.
pub const NAME: &[u8] = b"HUD_Status";
/// `G_NanobotArchId` and `G_ShieldBatteryArchId` (`nn_inventory.cpp`): the
/// equipment whose counts [`StatusInput::repair_count`] and
/// [`StatusInput::battery_count`] carry.
pub const NANOBOT_ARCH: &[u8] = b"ge_s_repair_01";
pub const SHIELD_BATTERY_ARCH: &[u8] = b"ge_s_battery_01";

/// The command ids the constructor registers, in its order.
pub const EVENTS: [u32; 0x3b] = [
    0x5e, 0xbc, 0xc0, 0xc1, 0xbd, 0xbe, 0x3a, 0x3c, 0x3b, 0x3e, 0x3d, 0x56, 0x57, 0xa1, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
    0xa0, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0x91, 0x92, 0x93,
    0x94, 0x95, 0x96, 0x8f, 0x90, 0x0f, 0xb7, 0xb8, 0xb9, 0xba, 0x6f,
];

/// The hp types of the ship view's caption table (`G_ShipViewHpTypeCaptions`,
/// `ship_view_control.cpp`), whose row numbers order the weapon list.
pub const HP_TYPE_ROWS: [i32; 0x3c] = [
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 13, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 10, 36, 35,
    11, 12, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 6, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 7,
    57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 8, 9,
];

/// A launcher of the player's ship (`CELauncher`, traverser mask 0xe0), in
/// the equipment manager's order.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StatusLauncher {
    /// `CEquip::Id`.
    pub sub_id: u16,
    /// The hardpoint `GetEquipDesc` gives.
    pub hardpoint: Vec<u8>,
    /// The archetype's `ids_name`.
    pub ids_name: i32,
    /// `GetAmmoCount`: -1 when the projectile needs no ammo.
    pub ammo_count: i32,
    /// `IsActive`.
    pub active: bool,
}

/// `LegalHpTypes`: one `hp_type` line of the ship's archetype.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StatusLegalHps {
    pub hp_type: i32,
    pub hardpoints: Vec<Vec<u8>>,
}

/// The player ship's behaviour manager, as `Update` reads it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StatusBehavior {
    /// `Opaque168[0]`.
    pub opaque_168: u8,
    /// `DockingPortStatus`.
    pub docking_port_status: i32,
    /// `CurrentBehaviorIndex`.
    pub current_behavior: i32,
    /// `HudBuildObjectCaption` of the ship's target, when the target's
    /// `TypeBits` has bit 7 and the caption was built.
    pub target_caption: Option<Vec<u16>>,
}

/// The player's ship (`GetPlayerObjRW`, a ship).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StatusShip {
    pub launchers: Vec<StatusLauncher>,
    /// The selected ship archetype's `hp_type` lines, `None` without one.
    pub legal_hps: Option<Vec<StatusLegalHps>>,
    pub behavior: Option<StatusBehavior>,
    /// `is_using_tradelane`.
    pub tradelane: bool,
    /// `any_thrusters_on`.
    pub thrusters_on: bool,
    /// `is_cruise_active`.
    pub cruise_active: bool,
    /// A thruster is mounted (`FindFirst(0x400)`).
    pub has_thruster: bool,
}

/// The last mouse-flight states `Update` saw: process statics in the
/// reference, so the owner keeps them from one HUD to the next.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct StatusLatches {
    pub mouse_flight_unlocked: bool,
    pub mouse_flight_hold_reached: bool,
}

/// What the element reads of the player and the client.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StatusInput {
    pub ship: Option<StatusShip>,
    /// The view object (`SetTarget`'s client object of the player) is live.
    pub view_object: bool,
    /// `HUDItem::Target` is live.
    pub target: bool,
    /// The game session flags (`TestGameSessionFlags`; bit 1 opens the
    /// panel).
    pub session_flags: u32,
    pub mouse_flight_unlocked: bool,
    pub mouse_flight_hold_reached: bool,
    /// `HudIsMousePickBlocked`.
    pub mouse_pick_blocked: bool,
    /// `IsLootInReach`.
    pub loot_in_reach: bool,
    /// The non-mission count of [`NANOBOT_ARCH`], `None` without an entry.
    pub repair_count: Option<i32>,
    /// The same of [`SHIELD_BATTERY_ARCH`].
    pub battery_count: Option<i32>,
    /// The key names of the bound commands (`KeyMapFindActiveBinding`,
    /// `KeyMapFormatBindingName`).
    pub bindings: Vec<(u32, Vec<u16>)>,
    pub latches: StatusLatches,
}

/// A row of the weapon list (`FHudStatusWeapon`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StatusWeapon {
    pub sub_id: u16,
    pub ammo_count: i32,
    pub ids_name: i32,
    pub active: bool,
    pub hardpoint: Vec<u8>,
    pub sort_key: i32,
}

/// The panel's state (`HUD_Target_Base`'s and `HUD_Status`'s fields).
#[derive(Clone, Debug, Default)]
pub struct Status {
    // HUD_Target_Base.
    pub hit_center: Vector,
    pub hit_half_width: f32,
    pub hit_half_height: f32,
    pub toggle_button: Option<NodeId>,
    pub view_panel: Option<NodeId>,
    pub expanded: bool,
    pub home_position: Vector,
    pub mover: ButtonMover,
    pub scaler: ButtonScaler,
    // HUD_Status.
    /// `+0x500`: the weapon groups' first selection is due.
    pub select_group: bool,
    pub active_weapon_group: i32,
    pub weapon_text: [Option<NodeId>; 10],
    pub weapon_number_text: [Option<NodeId>; 10],
    pub ammo_text: [Option<NodeId>; 10],
    pub close_button: Option<NodeId>,
    /// `+0x584..+0x586`.
    pub thrusters_on: bool,
    pub has_thruster: bool,
    pub cruise_active: bool,
    /// `+0x588`: the once-a-second timer.
    pub timer: f32,
    pub weapons: Vec<StatusWeapon>,
    pub minimized_frame: Option<NodeId>,
    /// `+0x5a0`, `+0x5a4`: the rows are shown, and the last value seen.
    pub rows_on: i32,
    pub rows_on_seen: i32,
    pub repair_button: Option<NodeId>,
    pub repair_count_text: Option<NodeId>,
    pub use_battery_button: Option<NodeId>,
    pub battery_count_text: Option<NodeId>,
    /// `+0x5b8`.
    pub in_tradelane: bool,
    /// `+0x5b9`, `+0x5ba`: the space state's command switch sets them.
    pub turret_view: bool,
    pub rear_view: bool,
    pub special_mode_text: Option<NodeId>,
    pub special_mode_text_cancel: Option<NodeId>,
    pub weapon_list_scroll: Option<NodeId>,
    pub weapon_top_row: i32,
    /// `+0x5cc`: the panel waits to follow session flag 2.
    pub sync_pending: bool,
    /// `+0x5cd`: a special mode shows.
    pub special_mode: bool,
    /// `+0x5d0`.
    pub caption: Option<Vec<u16>>,
    pub loot_button: Option<NodeId>,
    pub hp_row_cache: Vec<(Vec<u8>, i32)>,
    /// The last `SetGameSessionFlags(2, ..)` the panel made (open or
    /// closed), for its owner to apply to the client's flags.
    pub session_flag_2: Option<bool>,
}

/// The window (`HUD_Status`'s `WinControl` face).
#[derive(Default)]
pub struct HudStatus {
    pub control: WinControl,
    pub state: Status,
    pub input: StatusInput,
}

fn r(v: f64) -> f32 {
    v as f32
}

fn v(x: f32, y: f32, z: f32) -> Vector {
    Vector::new(x, y, z)
}

fn hide(ui: &mut Ui, node: Option<NodeId>, hidden: bool) {
    if let Some(n) = node {
        ui.property(n, Property::Hidden(hidden));
    }
}

/// `Flags |= 3` (shown) or `Flags &= 0xfc`.
fn show_flags(ui: &mut Ui, node: Option<NodeId>, shown: bool) {
    if let Some(n) = node.and_then(|n| ui.node_mut(n)) {
        if shown {
            n.flags |= 3;
        } else {
            n.flags &= 0xfc;
        }
    }
}

fn offset(ui: &mut Ui, node: Option<NodeId>, at: Vector) {
    if let Some(n) = node {
        ui.property(n, Property::Offset(at));
    }
}

fn set_text(ui: &mut Ui, node: Option<NodeId>, text: Option<&[u16]>) {
    if let Some(n) = node {
        ui.property(n, Property::SetText(text));
    }
}

fn set_ids(ui: &mut Ui, node: Option<NodeId>, ids: usize) {
    if let Some(n) = node {
        ui.property(n, Property::Other(0x1E, ids, 0));
    }
}

/// `NnTextControl::SetNumberText`.
fn set_number_text(ui: &mut Ui, node: Option<NodeId>, text: &[u8]) {
    let Some(id) = node else { return };
    ui.with_widget(id, (), |w, ui| {
        if let Some(t) = w.as_any_mut().downcast_mut::<TextControlWidget>() {
            t.text.set_number_text(ui, id, text);
        }
    });
}

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().collect()
}

/// The window's half extents and hit offset.
fn set_hit_box(ui: &mut Ui, id: NodeId, half: Vector, hit_offset: Vector) {
    if let Some(c) = ui.win_control_mut(id) {
        c.half_extents = half;
        c.hit_offset = hit_offset;
    }
}

/// The minimized hit box (`Create`, the end of a close).
fn minimized_hit_box(ui: &mut Ui, id: NodeId) {
    set_hit_box(ui, id, v(0.105, 0.02, 0.0), v(0.4, -0.371, 0.0));
}

/// `swprintf(Line, 0x1000, Format, Key)`: `%s` takes the key name, `%%` a
/// percent sign.
fn format_key(format: &[u16], key: &[u16]) -> Vec<u16> {
    let mut out = Vec::new();
    let mut i = 0;
    let percent = u16::from(b'%');
    while i < format.len() && format[i] != 0 {
        if format[i] == percent && i + 1 < format.len() {
            if format[i + 1] == u16::from(b's') {
                out.extend_from_slice(key);
                i += 2;
                continue;
            }
            if format[i + 1] == percent {
                out.push(percent);
                i += 2;
                continue;
            }
        }
        out.push(format[i]);
        i += 1;
    }
    out.truncate(0xFFF);
    out
}

/// The text controls' arguments: corner anchored, no tooltip.
fn text(style: &'static [u8], position: Vector, width: f32, height: f32) -> TextInit<'static> {
    TextInit {
        style: Some(style),
        position,
        width,
        height,
        corner_anchor: true,
        ..TextInit::default()
    }
}

/// The small buttons' arguments: an action button with a tooltip and a
/// command.
fn button(
    sound: &'static [u8],
    position: Vector,
    half: Vector,
    tooltip: i32,
    command: i32,
) -> ButtonInit<'static> {
    ButtonInit {
        sound: Some(sound),
        position,
        half_extents: half,
        action_button: 1,
        tooltip,
        command,
        ..ButtonInit::default()
    }
}

/// `WinControl::CreateUIScroll`.
#[allow(clippy::too_many_arguments)]
fn create_ui_scroll(
    ui: &mut Ui,
    parent: NodeId,
    name: &[u8],
    position: Vector,
    width: f32,
    height: f32,
    min: i32,
    max: i32,
    page: i32,
    horizontal: bool,
) -> Option<NodeId> {
    let payload = EntryPayload::ScrollCycle {
        right: width,
        bottom: height,
        extra: [min as f32, max as f32, page as f32, 1.0],
        flag: horizontal,
    };
    let id = ui.create_by_class(
        Some(name),
        b"UIScroll",
        Some(parent),
        &mut Payload::Page(&payload),
    )?;
    ui.with_widget(id, (), |w, ui| {
        if let Some(s) = w.as_any_mut().downcast_mut::<UiScroll>() {
            s.set_scroll_offset(ui, id, position);
            s.set_value(0.0);
            s.horizontal = horizontal;
        }
    });
    Some(id)
}

/// `-0.199f - Step` and `-0.199 - Step` of row `index`, `Step = float(index *
/// 0.018f)`.
fn row_y(index: u8) -> (f32, f32) {
    let step = r(f64::from(index) * f64::from(0.018_f32));
    let number = r(f64::from(-0.199_f32) - f64::from(step));
    let row = r(-0.199 - f64::from(step));
    (number, row)
}

/// The name columns' height, `0.016999999f` (`0x3c8b4395`).
const ROW_HEIGHT: f32 = f32::from_bits(0x3c8b_4395);

impl Status {
    /// `HUD_Status::Create`'s children, in its order (the window's hit box
    /// between them is set by the caller).
    fn create_children(&mut self, ui: &mut Ui, id: NodeId) {
        let statics = |position: Vector| StaticInit {
            offset: position,
            no_wire_render: true,
            no_clear_render: true,
            ..StaticInit::default()
        };
        self.view_panel = create_static_control(
            ui,
            id,
            b"StatusBackground",
            Some(b"Interface\\hud\\HUD_shipinfo.cmp"),
            &statics(v(0.3387, -0.243, -0.91)),
        );
        self.minimized_frame = create_static_control(
            ui,
            id,
            b"StatusMinimizedFrame",
            Some(b"Interface\\hud\\HUD_shipinfo_min.cmp"),
            &statics(v(0.4101, -0.37, -1.0)),
        );
        minimized_hit_box(ui, id);
        self.toggle_button = create_button_control(
            ui,
            id,
            b"HudStatusOpen",
            Some(b"interface\\hud\\hud_ShipInfo_button.3db"),
            &button(
                b"ui_window_open",
                self.home_position,
                v(0.02, 0.02, 0.0),
                0x3ff,
                0,
            ),
        );
        if let Some(t) = self.toggle_button {
            ui.property(t, Property::Other(0x36, 1, 0));
        }
        let clickable = |ui: &mut Ui, n: Option<NodeId>| {
            if let Some(n) = n {
                ui.property(n, Property::Other(0x24, 1, 0));
            }
        };
        for index in 0..10u8 {
            let i = usize::from(index);
            let (number_y, row) = row_y(index);
            let name = format!("WeaponNumberText{index}");
            self.weapon_number_text[i] = create_text_control(
                ui,
                id,
                name.as_bytes(),
                &TextInit {
                    ids: 0x515 + u32::from(index),
                    justification: 1,
                    ..text(b"HudSmall", v(0.27, number_y, 0.0), 0.02, ROW_HEIGHT)
                },
            );
            clickable(ui, self.weapon_number_text[i]);
            let name = format!("WeaponText{index}");
            self.weapon_text[i] = create_text_control(
                ui,
                id,
                name.as_bytes(),
                &text(b"HudSmall", v(0.288, row, 0.0), 0.165, ROW_HEIGHT),
            );
            clickable(ui, self.weapon_text[i]);
            let name = format!("AmmoText{index}");
            self.ammo_text[i] = create_text_control(
                ui,
                id,
                name.as_bytes(),
                &TextInit {
                    ids: 0x514,
                    ..text(b"HudSmall", v(0.458, row, 0.0), 0.04, 0.017)
                },
            );
            clickable(ui, self.ammo_text[i]);
            hide(ui, self.weapon_number_text[i], true);
            hide(ui, self.weapon_text[i], true);
            hide(ui, self.ammo_text[i], true);
        }
        let small = v(0.014, 0.014, 0.0);
        self.close_button = create_button_control(
            ui,
            id,
            b"StatusCloseButton",
            Some(b"interface\\NeuroNet\\Inventory\\x.3db"),
            &button(b"ui_select_item", v(0.383, -0.307, -0.5), small, 0x409, 0),
        );
        let placeholder = wide("123");
        let count = |ui: &mut Ui, name: &[u8], position: Vector, tooltip: i32| {
            let n = create_text_control(
                ui,
                id,
                name,
                &TextInit {
                    justification: 1,
                    tooltip,
                    ..text(b"HudSmall", position, 0.034, 0.03)
                },
            );
            set_text(ui, n, Some(&placeholder));
            n
        };
        self.repair_button = create_button_control(
            ui,
            id,
            b"HudRepairButton",
            Some(b"interface\\hud\\HUD_repairactivate.3db"),
            &button(b"ui_item_select", v(0.341, -0.371, 0.0), small, 0x43e, 0x8f),
        );
        self.repair_count_text = count(ui, b"RepairCountText", v(0.347, -0.359, 0.0), 0x4b2);
        self.use_battery_button = create_button_control(
            ui,
            id,
            b"UseBatteryButton",
            Some(b"interface\\hud\\HUD_shieldgaugeicon.3db"),
            &button(b"ui_item_select", v(0.397, -0.371, 0.0), small, 0x4b5, 0x90),
        );
        self.battery_count_text = count(ui, b"BatteryCountText", v(0.404, -0.359, 0.0), 0x4b3);
        self.weapon_list_scroll = create_ui_scroll(
            ui,
            id,
            b"WeaponListScroll",
            v(0.49, -0.234, -1.0),
            0.016,
            0.118,
            0,
            5,
            10,
            false,
        );
        self.slot13(ui, true);
        self.special_mode_text = create_text_control(
            ui,
            id,
            b"SpecialModeText",
            &TextInit {
                justification: 1,
                ..text(b"PageHeader", v(-0.513, -0.29, 0.0), 1.022, 0.03)
            },
        );
        self.special_mode_text_cancel = create_text_control(
            ui,
            id,
            b"SpecialModeTextCancel",
            &TextInit {
                justification: 1,
                ..text(b"Normal", v(-0.513, -0.321, 0.0), 1.028, 0.03)
            },
        );
        show_flags(ui, self.special_mode_text, false);
        show_flags(ui, self.special_mode_text_cancel, false);
        // `RebuildWeaponList` reads the player, which the element does not
        // have before its owner sets the input (divergence D1120).
        self.loot_button = create_button_control(
            ui,
            id,
            b"LootButton",
            Some(b"interface\\HUD\\HUD_loot_bw.3db"),
            &button(b"ui_select_item", v(0.21, -0.37, 0.0), small, 0x3a2, 0x6f),
        );
        show_flags(ui, self.loot_button, false);
    }

    fn rows(&self) -> impl Iterator<Item = [Option<NodeId>; 3]> + '_ {
        (0..10).map(|i| {
            [
                self.weapon_text[i],
                self.weapon_number_text[i],
                self.ammo_text[i],
            ]
        })
    }

    /// Every row's three texts and the scroll bar hidden or shown.
    fn hide_rows(&self, ui: &mut Ui, hidden: bool) {
        for i in 0..10 {
            hide(ui, self.weapon_number_text[i], hidden);
            hide(ui, self.weapon_text[i], hidden);
            hide(ui, self.ammo_text[i], hidden);
        }
        hide(ui, self.weapon_list_scroll, hidden);
    }

    /// HUDItem slot 13: the rows and the scroll bar (while the rows are
    /// on), then the background and the close button.
    fn slot13(&self, ui: &mut Ui, hidden: bool) {
        if self.rows_on != 0 {
            let rows: Vec<_> = self.rows().collect();
            for [text, number, ammo] in rows {
                hide(ui, text, hidden);
                hide(ui, number, hidden);
                hide(ui, ammo, hidden);
            }
            hide(ui, self.weapon_list_scroll, hidden);
        }
        hide(ui, self.view_panel, hidden);
        hide(ui, self.close_button, hidden);
    }

    /// HUDItem slot 12: open (the mover, then the scaler) or close (the
    /// scaler, then the mover); nothing while either runs.
    pub fn slot12(&mut self, ui: &mut Ui) {
        if self.mover.active || self.scaler.active {
            return;
        }
        if !self.expanded {
            self.mover.start(0.3);
            let m = &mut self.mover;
            m.add_button(self.toggle_button, self.home_position, v(0.371, -0.27, 0.0));
            m.add_button(
                self.minimized_frame,
                v(0.4101, -0.37, -1.0),
                v(0.4101, -0.4, -1.0),
            );
            m.add_button(
                self.repair_button,
                v(0.341, -0.371, 0.0),
                v(0.341, -0.4, 0.0),
            );
            m.add_button(
                self.repair_count_text,
                v(0.347, -0.359, 0.0),
                v(0.347, -0.4, 0.0),
            );
            m.add_button(
                self.use_battery_button,
                v(0.397, -0.371, 0.0),
                v(0.397, -0.4, 0.0),
            );
            m.add_button(
                self.battery_count_text,
                v(0.404, -0.359, 0.0),
                v(0.404, -0.4, 0.0),
            );
            self.timer = 1.0;
            return;
        }
        self.slot13(ui, true);
        self.scaler.start(0.3);
        self.scaler.add_button(self.view_panel, 1.0, 0.1);
        self.hide_rows(ui, true);
    }

    /// `OnMoveFinished`: the second stage of an open, the end of a close.
    fn on_move_finished(&mut self, ui: &mut Ui, id: NodeId) {
        self.timer = 1.0;
        if !self.expanded {
            self.slot13(ui, true);
            show_flags(ui, self.toggle_button, false);
            show_flags(ui, self.minimized_frame, false);
            self.scaler.start(0.3);
            self.scaler.add_button(self.view_panel, 0.1, 1.0);
            if self.scaler.update(ui.frame_time) {
                self.on_scale_finished(ui, id);
            }
            return;
        }
        self.expanded = false;
        self.session_flag_2 = Some(false);
        minimized_hit_box(ui, id);
    }

    /// `OnScaleFinished`: the end of an open, the second stage of a close.
    fn on_scale_finished(&mut self, ui: &mut Ui, id: NodeId) {
        if !self.expanded {
            self.expanded = true;
            self.session_flag_2 = Some(true);
            self.slot13(ui, false);
            offset(ui, self.repair_button, v(0.3, -0.171, 0.0));
            offset(ui, self.repair_count_text, v(0.306, -0.16, 0.0));
            offset(ui, self.use_battery_button, v(0.357, -0.171, 0.0));
            offset(ui, self.battery_count_text, v(0.365, -0.16, 0.0));
            if self.rows_on != 0 {
                self.hide_rows(ui, false);
            }
            hide(ui, self.minimized_frame, true);
            set_hit_box(ui, id, v(0.131, 0.117, 0.0), v(0.376, -0.267, 0.0));
            return;
        }
        hide(ui, self.minimized_frame, false);
        self.mover.start(0.3);
        self.mover
            .add_button(self.toggle_button, v(0.371, -0.27, 0.0), self.home_position);
        let low = [
            (
                self.repair_button,
                v(0.341, -0.4, 0.0),
                v(0.341, -0.371, 0.0),
            ),
            (
                self.repair_count_text,
                v(0.347, -0.4, 0.0),
                v(0.347, -0.359, 0.0),
            ),
            (
                self.use_battery_button,
                v(0.397, -0.4, 0.0),
                v(0.397, -0.371, 0.0),
            ),
            (
                self.battery_count_text,
                v(0.404, -0.4, 0.0),
                v(0.404, -0.359, 0.0),
            ),
        ];
        for (node, at, _) in low {
            offset(ui, node, at);
        }
        for (node, at, home) in low {
            self.mover.add_button(node, at, home);
        }
        self.mover.add_button(
            self.minimized_frame,
            v(0.4101, -0.4, -1.0),
            v(0.4101, -0.37, -1.0),
        );
        self.timer = 1.0;
    }
}

impl HudStatus {
    /// Runs `f` on the state and the input with the window back in its
    /// node, so that calls through the tree find it.
    fn run<R: Default>(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        f: impl FnOnce(&mut Status, &mut Ui, &mut StatusInput) -> R,
    ) -> R {
        let mut state = std::mem::take(&mut self.state);
        let mut input = std::mem::take(&mut self.input);
        let mut out = R::default();
        ui.lend(id, self, |ui| out = f(&mut state, ui, &mut input));
        self.state = state;
        self.input = input;
        out
    }
}

impl Widget for HudStatus {
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
    /// `HUD_Target_Base::HitTest` without its gates (D1121).
    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        self.state.hit_box(ray) || base::hit_test(ui, id, &self.control, ray)
    }
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        base::set_position(ui, id, &mut self.control, position);
    }
    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        base::set_orientation(ui, id, &mut self.control, orientation);
    }
    /// `HUD_Status::Create`: the children with the minimized hit box among
    /// them, the offset (0, 0, -1), the position again, then
    /// `Win3D::Create`.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        self.run(ui, id, |state, ui, _| state.create_children(ui, id));
        base::set_offset(ui, id, self, Vector::new(0.0, 0.0, -1.0));
        let position = ui.node(id).map_or(Vector::ZERO, |n| n.position);
        base::set_position(ui, id, &mut self.control, position);
        ui.base_create(id, parent)
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        base::destroy(ui, id, &mut self.control);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        base::on_delete(ui, id, &mut self.control);
    }
    /// `HUD_Status::Update` over the input its owner set.
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        self.run(ui, id, |state, ui, input| state.update(ui, id, input));
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        ui.draw_children(id, host);
    }
    /// `HUD_Status::MouseEvent`: always consumed (D1121).
    fn mouse_event(&mut self, _ui: &mut Ui, _id: NodeId, _event: MouseButton) -> bool {
        true
    }
    fn broadcast(&mut self, ui: &mut Ui, id: NodeId, msg: u32, _a: u32, _b: u32) -> bool {
        base::broadcast(ui, id, self, msg)
    }
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        self.run(ui, id, |state, ui, input| {
            state.property(ui, &property, input)
        })
    }
}

/// The element (`HUD_Status`'s `HUDItem` face).
#[derive(Debug)]
pub struct HudStatusItem {
    pub window: NodeId,
}

impl HudItem for HudStatusItem {
    fn name(&self) -> &[u8] {
        ITEM_NAME
    }
    fn type_name(&self) -> &'static str {
        "class HUD_Status"
    }
    fn window(&self) -> Option<NodeId> {
        Some(self.window)
    }
}

/// The input the window at `window` reads from its next update on.
pub fn set_input(ui: &mut Ui, window: NodeId, input: StatusInput) {
    if let Some(w) = ui.widget_mut::<HudStatus>(window) {
        w.input = input;
    }
}

/// `HUDFACTORY<HUD_Status>::Create`: the constructor, which sets the home
/// position, makes the window (`Create(0, 0)`), names it "HUD_Status",
/// sets the hit centre and registers the panel's commands.
pub fn create(_reader: &mut IniReader, ui: &mut Ui) -> Option<Box<dyn HudItem>> {
    let mut window = HudStatus::default();
    let s = &mut window.state;
    s.select_group = true;
    s.timer = 1.0;
    s.rows_on = 1;
    s.rows_on_seen = 1;
    s.sync_pending = true;
    s.home_position = v(0.295, -0.367, 0.0);
    let id = ui.alloc(Some(Box::new(window)));
    ui.with_widget(id, false, |w, ui| {
        w.create(ui, id, None, &mut Payload::None)
    });
    if let Some(n) = ui.node_mut(id) {
        n.set_name(Some(NAME));
        n.class_name = Some("HUD_Status");
    }
    if let Some(w) = ui.widget_mut::<HudStatus>(id) {
        w.state.hit_center = v(0.418, -0.29, -1.0);
        w.state.hit_half_width = 0.11;
        w.state.hit_half_height = 0.11;
    }
    for event in EVENTS {
        ui.services.register_event(id, event);
    }
    Some(Box::new(HudStatusItem { window: id }))
}

#[cfg(test)]
mod tests;
