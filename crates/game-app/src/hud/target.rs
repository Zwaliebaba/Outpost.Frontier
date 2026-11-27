//! `HUD_Target` (`hud_target_panel.cpp`, the `[Target]` section of
//! `hud.ini`, the reference's `HudTarget` over `HUD_Target_Base` of
//! `hud_target.cpp`): the target panel at the bottom left, collapsed to a
//! row of buttons or open on the contact list (mode 0), the target view
//! (mode 1) or the formation list (mode 2).
//!
//! The port so far: the construction (`HudTarget::HudTarget` and
//! `Create`), the window's update (`HudTarget::Update` with
//! `HUD_Target_Base::Update`) with the open and close animations (the
//! `ButtonMover` and `ButtonScaler` faces, `Slot12`, `Slot13`,
//! `OnMoveFinished`, `OnScaleFinished`, `UpdateButtons`,
//! `UpdateTradeButton`), the toggle and close clicks, and `GetMode`. The
//! HUD has no game objects yet (divergence D1125): the player ship, its
//! target and the view object are absent, so the update takes the paths of
//! a player without a target. The target view and labels (`SetViewObject`,
//! `Slot14` to `Slot16`), the events (`OnEvent`), the other clicks and the
//! draw (`Draw`, the rank line's text) come with them and with the HUD's
//! draw.

use neuron_client::ui::children::{
    ButtonInit, StaticInit, TextInit, create_button_control, create_static_control,
    create_text_control,
};
use neuron_client::ui::control::{WinControl, base};
use neuron_client::ui::controls::text_control::TextControlWidget;
use neuron_client::ui::message::MouseButton;
use neuron_client::ui::tree::{ACTIVE, DrawHost, NodeId, Payload, Property, Ui, VISIBLE, Widget};
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};

use super::HudItem;
use super::button_mover::{ButtonMover, ButtonScaler};

/// The factory's section and the element's name.
pub const SECTION: &[u8] = b"Target";
/// The window's name and class name.
pub const NAME: &[u8] = b"HUD_Target";
/// The events the constructor registers, in its order.
pub const EVENTS: [u32; 16] = [
    0x5f, 0x21, 0x22, 0xcc, 0x54, 0x58, 0x59, 0x8b, 0x24, 0x25, 0x26, 0xbb, 0xc9, 0xca, 0x23, 0x8a,
];
/// `SUBOBJ_ID_NONE`.
pub const SUBOBJ_ID_NONE: u16 = 0xffff;

/// `kAnimSeconds`.
const ANIM_SECONDS: f32 = 0.3;
/// `kScaleSmall`.
const SCALE_SMALL: f32 = 0.1;
/// The toggle's open spot (`kToggleOpen`).
const TOGGLE_OPEN: [f32; 3] = [-0.375, -0.27, 0.0];
/// The bottom row's y (`kRowY`) and the y it drops to (`kRowLoweredY`).
const ROW_Y: f32 = -0.37;
const ROW_LOWERED_Y: f32 = -0.4;
/// The bottom row's x and z, in `Slot12`'s order.
const ROW_X: [f32; 8] = [
    -0.479, -0.411, -0.411, -0.445, -0.445, -0.343, -0.377, -0.4105,
];
const ROW_Z: [f32; 8] = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -1.0];
/// The y of the buttons on the open panel (`OnScaleFinished`).
const TOP_Y: f32 = -0.171;
/// `HomePosition`, the toggle's rest spot.
const HOME: [f32; 3] = [-0.296, -0.367, 0.0];

fn v([x, y, z]: [f32; 3]) -> Vector {
    Vector::new(x, y, z)
}

fn show(ui: &mut Ui, id: Option<NodeId>) {
    if let Some(n) = id.and_then(|id| ui.node_mut(id)) {
        n.flags |= ACTIVE | VISIBLE;
    }
}

fn clear(ui: &mut Ui, id: Option<NodeId>) {
    if let Some(n) = id.and_then(|id| ui.node_mut(id)) {
        n.flags &= !(ACTIVE | VISIBLE);
    }
}

/// `SetProperty(1, hide, 0)`.
fn hide(ui: &mut Ui, id: Option<NodeId>, hide: bool) {
    if let Some(id) = id {
        ui.property(id, Property::Hidden(hide));
    }
}

/// `SetProperty(msg, a, b)` of a message the port has no variant for.
fn other(ui: &mut Ui, id: Option<NodeId>, msg: i32, a: usize, b: usize) {
    if let Some(id) = id {
        ui.property(id, Property::Other(msg, a, b));
    }
}

/// `SetPos`: `SetProperty(6, pos, 0)`.
fn set_pos(ui: &mut Ui, id: Option<NodeId>, x: f32, y: f32, z: f32) {
    if let Some(id) = id {
        ui.property(id, Property::Offset(Vector::new(x, y, z)));
    }
}

/// The children `Create` makes.
#[derive(Clone, Copy, Debug, Default)]
pub struct Controls {
    /// `ViewPanel`, "TargetBackground".
    pub view_panel: Option<NodeId>,
    /// "TargetMinimizedFrame".
    pub minimized_frame: Option<NodeId>,
    /// `ToggleButton`, "HudTargetOpen".
    pub toggle_button: Option<NodeId>,
    /// "TargetShipName".
    pub ship_name: Option<NodeId>,
    /// "SubtargetName".
    pub subtarget_name: Option<NodeId>,
    /// "TargetPreviousButton".
    pub previous_button: Option<NodeId>,
    /// "TargetNextButton".
    pub next_button: Option<NodeId>,
    /// "TargetScanButton".
    pub scan_button: Option<NodeId>,
    /// "TargetTractorButton".
    pub tractor_button: Option<NodeId>,
    /// "TargetCommButton".
    pub comm_button: Option<NodeId>,
    /// "TargetRequestGroupButton".
    pub request_group_button: Option<NodeId>,
    /// "TargetTradeButton".
    pub trade_button: Option<NodeId>,
    /// "TargetCloseButton".
    pub close_button: Option<NodeId>,
    /// "SwitchToContactList".
    pub contact_list_button: Option<NodeId>,
    /// "SwitchToTarget".
    pub target_button: Option<NodeId>,
    /// "NextEnemyButton".
    pub next_enemy_button: Option<NodeId>,
    /// "FormationList".
    pub formation_button: Option<NodeId>,
    /// "TargetRankText".
    pub rank_text: Option<NodeId>,
}

/// The window (`HUD_Target`'s `WinControl` face, with the state of its
/// `HUD_Target_Base`, `ButtonMover` and `ButtonScaler` faces).
#[derive(Default)]
pub struct HudTarget {
    pub control: WinControl,
    pub c: Controls,
    /// `ButtonMover` (`+0x364`).
    pub mover: ButtonMover,
    /// `ButtonScaler` (`+0x4e0`).
    pub scaler: ButtonScaler,
    /// `SubTargetId` (`+0x3b4`).
    pub sub_target_id: u16,
    /// `HitCenter` (`+0x4b4`), `HitHalfWidth`, `HitHalfHeight`.
    pub hit_center: Vector,
    pub hit_half_width: f32,
    pub hit_half_height: f32,
    /// `bExpanded` (`+0x4d0`).
    pub expanded: bool,
    /// `HomePosition` (`+0x4d4`).
    pub home_position: Vector,
    /// `LabelRefreshTimer` (`+0x558`).
    pub label_refresh_timer: f32,
    /// `bHadViewObject`.
    pub had_view_object: bool,
    /// `bScannerOpen`.
    pub scanner_open: bool,
    /// `Mode` (`+0x648`): 0 contact list, 1 target, 2 formation.
    pub mode: i32,
    /// `ShownMode`.
    pub shown_mode: i32,
    /// `bFormationAvailable`.
    pub formation_available: bool,
    /// `ShownRank`, `Rank`.
    pub shown_rank: f32,
    pub rank: f32,
    /// `bClearPlayerTarget`.
    pub clear_player_target: bool,
    /// `bFirstUpdate`.
    pub first_update: bool,
    /// The game session flags (`GSessionFlags`, `TestGameSessionFlags`):
    /// the panel's own word, 3 as the reference's starts, until the HUD
    /// reads the client's (D1125).
    pub session_flags: u32,
}

/// The static controls' arguments (`0, 0, 0, 1, 0, 0, 1, 0, 0, 0`): no
/// wire render and no clear render.
fn statics(offset: [f32; 3]) -> StaticInit<'static> {
    StaticInit {
        offset: v(offset),
        no_wire_render: true,
        no_clear_render: true,
        ..StaticInit::default()
    }
}

/// A button of the panel: no second mesh, no key command, no hardpoint,
/// `0, 1, 0, 0` (an action button), the tool tip, no second tool tip and
/// the command.
#[allow(clippy::too_many_arguments)]
fn button(
    ui: &mut Ui,
    id: NodeId,
    name: &[u8],
    mesh: &[u8],
    sound: &[u8],
    position: Vector,
    half: Vector,
    tooltip: i32,
    command: i32,
) -> Option<NodeId> {
    create_button_control(
        ui,
        id,
        name,
        Some(mesh),
        &ButtonInit {
            sound: Some(sound),
            position,
            half_extents: half,
            action_button: 1,
            tooltip,
            command,
            ..ButtonInit::default()
        },
    )
}

/// A label of the panel in "HudSmall", anchored at its corner.
fn label(
    ui: &mut Ui,
    id: NodeId,
    name: &[u8],
    position: [f32; 3],
    width: f32,
    tooltip: i32,
) -> Option<NodeId> {
    create_text_control(
        ui,
        id,
        name,
        &TextInit {
            style: Some(b"HudSmall"),
            position: v(position),
            width,
            height: 0.03,
            ids: 0,
            justification: 1,
            tooltip,
            corner_anchor: true,
            ..TextInit::default()
        },
    )
}

/// The label's home offset (`HomeOffset`, the rest spot `Slot15` puts it
/// back to).
fn set_home_offset(ui: &mut Ui, id: Option<NodeId>, offset: [f32; 3]) {
    if let Some(t) = id.and_then(|id| ui.widget_mut::<TextControlWidget>(id)) {
        t.text.control.home_offset = v(offset);
    }
}

impl Controls {
    /// `HudTarget::Create`'s children, in its order. The hit box of the
    /// collapsed panel is set between the two frames and the toggle.
    fn create(ui: &mut Ui, id: NodeId, home: Vector) -> Self {
        let mut c = Controls {
            view_panel: create_static_control(
                ui,
                id,
                b"TargetBackground",
                Some(b"Interface\\hud\\HUD_target.cmp"),
                &statics([-0.3405, -0.243, -0.91]),
            ),
            minimized_frame: create_static_control(
                ui,
                id,
                b"TargetMinimizedFrame",
                Some(b"Interface\\hud\\HUD_target_min.cmp"),
                &statics([-0.4105, -0.37, -1.0]),
            ),
            ..Controls::default()
        };
        let toggle_half = Vector::new(0.02, 0.02, 0.0);
        c.toggle_button = button(
            ui,
            id,
            b"HudTargetOpen",
            b"interface\\hud\\hud_ShipTarget.3db",
            b"ui_window_open",
            home,
            toggle_half,
            0x3fe,
            0,
        );
        other(ui, c.toggle_button, 0x36, 1, 0);

        let ship_name_pos = [-0.488, -0.2, 0.0];
        c.ship_name = label(ui, id, b"TargetShipName", ship_name_pos, 0.205, 0);
        other(ui, c.ship_name, 0x50, 1, 0x14);
        set_home_offset(ui, c.ship_name, ship_name_pos);
        let sub_name_pos = [-0.488, -0.218, 0.0];
        c.subtarget_name = label(ui, id, b"SubtargetName", sub_name_pos, 0.205, 0);
        other(ui, c.subtarget_name, 0x50, 1, 0x14);
        set_home_offset(ui, c.subtarget_name, sub_name_pos);

        let small = Vector::new(0.014, 0.014, 0.0);
        let item = b"ui_item_select".as_slice();
        let select = b"ui_select_item".as_slice();
        let pos = |x: f32, y: f32, z: f32| Vector::new(x, y, z);
        c.previous_button = button(
            ui,
            id,
            b"TargetPreviousButton",
            b"interface\\hud\\HUD_previoussub.3db",
            item,
            pos(-0.262, -0.237, 0.0),
            small,
            0x420,
            0,
        );
        c.next_button = button(
            ui,
            id,
            b"TargetNextButton",
            b"interface\\hud\\HUD_nextsub.3db",
            item,
            pos(-0.262, -0.268, 0.0),
            small,
            0x41f,
            0,
        );
        let scan = pos(-0.411, -0.37, 0.0);
        c.scan_button = button(
            ui,
            id,
            b"TargetScanButton",
            b"interface\\hud\\HUD_scanshipinactive.3db",
            item,
            scan,
            small,
            0x440,
            0x8a,
        );
        c.tractor_button = button(
            ui,
            id,
            b"TargetTractorButton",
            b"interface\\hud\\HUD_tractorbeam2.3db",
            item,
            scan,
            small,
            0xbc2,
            0x8b,
        );
        let comm = pos(-0.445, -0.37, 0.0);
        c.comm_button = button(
            ui,
            id,
            b"TargetCommButton",
            b"interface\\hud\\HUD_communication.3db",
            item,
            comm,
            small,
            0x3fd,
            0,
        );
        c.request_group_button = button(
            ui,
            id,
            b"TargetRequestGroupButton",
            b"interface\\hud\\HUD_grouprequest.3db",
            item,
            comm,
            small,
            0x621,
            0,
        );
        c.trade_button = button(
            ui,
            id,
            b"TargetTradeButton",
            b"interface\\hud\\HUD_trade.3db",
            item,
            pos(-0.202, -0.37, 0.0),
            small,
            0xbc3,
            0,
        );
        c.close_button = button(
            ui,
            id,
            b"TargetCloseButton",
            b"interface\\NeuroNet\\Inventory\\x.3db",
            select,
            pos(-0.386, -0.308, -0.5),
            small,
            0x409,
            0,
        );
        c.contact_list_button = button(
            ui,
            id,
            b"SwitchToContactList",
            b"interface\\hud\\HUD_contactlist.3db",
            select,
            pos(-0.343, -0.37, 0.0),
            small,
            0x448,
            0x54,
        );
        c.target_button = button(
            ui,
            id,
            b"SwitchToTarget",
            b"interface\\HUD\\HUD_targetedobject.3db",
            select,
            pos(-0.377, -0.37, 0.0),
            small,
            0x5ff,
            0x58,
        );
        c.next_enemy_button = button(
            ui,
            id,
            b"NextEnemyButton",
            b"interface\\HUD\\HUD_nextenemy.3db",
            select,
            pos(-0.479, -0.37, 0.0),
            small,
            0x3e2,
            0x26,
        );
        c.formation_button = button(
            ui,
            id,
            b"FormationList",
            b"interface\\HUD\\HUD_formation.3db",
            select,
            pos(-0.278, -0.37, 0.0),
            small,
            0x60c,
            0x59,
        );
        hide(ui, c.formation_button, true);
        c.rank_text = create_text_control(
            ui,
            id,
            b"TargetRankText",
            &TextInit {
                style: Some(b"HudSmall"),
                position: Vector::new(-0.49, -0.361, 0.0),
                width: 0.228,
                height: 0.03,
                ids: 0,
                justification: 1,
                tooltip: 0x574,
                corner_anchor: true,
                ..TextInit::default()
            },
        );
        c
    }
}

impl HudTarget {
    /// `GetMode`: raw, the mode; otherwise the mode while open and neither
    /// animation runs, else 3.
    #[must_use]
    pub fn get_mode(&self, raw: bool) -> i32 {
        if raw || (self.expanded && !self.mover.is_active() && !self.scaler.is_active()) {
            self.mode
        } else {
            3
        }
    }

    /// `SetMode`.
    pub fn set_mode(&mut self, mode: i32) {
        self.mode = mode;
    }

    /// `SetGameSessionFlags`: the bits of `mask` set or cleared (the
    /// server's copy, `SetInterfaceState`, waits for D1125's removal).
    pub fn set_game_session_flags(&mut self, mask: u32, set: bool) {
        if set {
            self.session_flags |= mask;
        } else {
            self.session_flags &= !mask;
        }
    }

    /// `ButtonMover::IsActive() || ButtonScaler::IsActive()`.
    fn animating(&self) -> bool {
        self.mover.is_active() || self.scaler.is_active()
    }

    /// The mode switches' checked states (`SetProperty(0xc, ...)`).
    fn check_mode_buttons(&self, ui: &mut Ui, formation: bool) {
        other(
            ui,
            self.c.contact_list_button,
            0xc,
            usize::from(self.mode == 0),
            0,
        );
        other(
            ui,
            self.c.target_button,
            0xc,
            usize::from(self.mode == 1),
            0,
        );
        if formation {
            other(
                ui,
                self.c.formation_button,
                0xc,
                usize::from(self.mode == 2),
                0,
            );
        }
    }

    /// The panel's hit box (`HalfExtents`, `HitOffset`).
    fn set_hit_box(&mut self, half: [f32; 2], offset: [f32; 2]) {
        self.control.half_extents = Vector::new(half[0], half[1], 0.0);
        self.control.hit_offset = Vector::new(offset[0], offset[1], 0.0);
    }

    /// `Slot12`: the open (the toggle slides to the panel's corner, the
    /// bottom row drops) or the close (the panel controls hidden, the view
    /// panel shrinks).
    pub fn slot12(&mut self, ui: &mut Ui) {
        if self.animating() {
            return;
        }
        if !self.expanded {
            self.mover.start(ANIM_SECONDS);
            self.mover
                .add_button(self.c.toggle_button, self.home_position, v(TOGGLE_OPEN));
            let row = self.bottom_row([0, 1, 2, 3, 4]);
            for (i, control) in row.into_iter().enumerate() {
                let from = Vector::new(ROW_X[i], ROW_Y, ROW_Z[i]);
                let to = Vector::new(ROW_X[i], ROW_LOWERED_Y, ROW_Z[i]);
                self.mover.add_button(control, from, to);
            }
            self.label_refresh_timer = 1.0;
            other(ui, self.c.ship_name, 0x50, 1, 0x14);
            other(ui, self.c.subtarget_name, 0x50, 1, 0x14);
            return;
        }
        self.slot13(ui, true);
        self.scaler.start(ANIM_SECONDS);
        self.scaler.add_button(self.c.view_panel, 1.0, SCALE_SMALL);
        clear(ui, self.c.formation_button);
    }

    /// The bottom row in the mover's order: the next enemy, scan, tractor,
    /// then the comm and request group buttons in the order `order`'s 3
    /// and 4 pick (`Slot12` has comm first, `OnScaleFinished` request
    /// group first), the two switches and the minimized frame.
    fn bottom_row(&self, order: [usize; 5]) -> [Option<NodeId>; 8] {
        let first = [
            self.c.next_enemy_button,
            self.c.scan_button,
            self.c.tractor_button,
            self.c.comm_button,
            self.c.request_group_button,
        ];
        [
            first[order[0]],
            first[order[1]],
            first[order[2]],
            first[order[3]],
            first[order[4]],
            self.c.contact_list_button,
            self.c.target_button,
            self.c.minimized_frame,
        ]
    }

    /// `Slot13`: the open panel's controls hidden or shown.
    pub fn slot13(&self, ui: &mut Ui, hidden: bool) {
        for control in [
            self.c.view_panel,
            self.c.ship_name,
            self.c.subtarget_name,
            self.c.close_button,
            self.c.rank_text,
            self.c.previous_button,
            self.c.next_button,
            self.c.trade_button,
        ] {
            hide(ui, control, hidden);
        }
    }

    /// `OnMoveFinished` (`HudTarget`'s, which replaces the base's): the
    /// open's slide ended (the panel controls shown, the toggle and the
    /// minimized frame hidden, the view panel grows), or the close's (the
    /// panel is collapsed).
    fn on_move_finished(&mut self, ui: &mut Ui, id: NodeId) {
        if !self.expanded {
            self.slot13(ui, false);
            clear(ui, self.c.toggle_button);
            clear(ui, self.c.minimized_frame);
            self.scaler.start(ANIM_SECONDS);
            self.scaler.add_button(self.c.view_panel, SCALE_SMALL, 1.0);
            if self.scaler.update(ui, ui.frame_time) {
                self.on_scale_finished(ui, id);
            }
            return;
        }
        self.expanded = false;
        self.set_game_session_flags(1, false);
        self.check_mode_buttons(ui, true);
        self.set_hit_box([0.109, 0.02], [-0.402, -0.371]);
        self.label_refresh_timer = 1.0;
    }

    /// `OnScaleFinished`: the view panel at full size (the panel is open,
    /// its buttons moved to the top row) or gone (the toggle slides home
    /// and the bottom row rises).
    fn on_scale_finished(&mut self, ui: &mut Ui, id: NodeId) {
        let c = self.c;
        if !self.expanded {
            self.expanded = true;
            self.set_game_session_flags(1, true);
            set_pos(ui, c.next_enemy_button, -0.465, TOP_Y, 0.0);
            set_pos(ui, c.scan_button, -0.397, TOP_Y, 0.0);
            set_pos(ui, c.tractor_button, -0.397, TOP_Y, 0.0);
            set_pos(ui, c.comm_button, -0.431, TOP_Y, 0.0);
            set_pos(ui, c.request_group_button, -0.431, TOP_Y, 0.0);
            set_pos(ui, c.contact_list_button, -0.295, TOP_Y, 0.0);
            set_pos(ui, c.target_button, -0.329, TOP_Y, 0.0);
            set_pos(ui, c.formation_button, -0.363, TOP_Y, 0.0);
            if self.mode == 1 {
                // `Slot15`, the view object's labels, has nothing to show
                // without one (D1125).
                clear(ui, c.toggle_button);
                clear(ui, c.minimized_frame);
                show(ui, c.rank_text);
                show(ui, c.previous_button);
                show(ui, c.next_button);
                show(ui, c.trade_button);
            } else {
                base::set_hidden_with_children(ui, id, true);
                show(ui, Some(id));
                show(ui, c.view_panel);
                show(ui, c.close_button);
                for control in [
                    c.ship_name,
                    c.subtarget_name,
                    c.rank_text,
                    c.previous_button,
                    c.next_button,
                    c.trade_button,
                ] {
                    clear(ui, control);
                }
            }
            self.label_refresh_timer = 1.0;
            show(ui, c.contact_list_button);
            show(ui, c.target_button);
            show(ui, c.next_enemy_button);
            // The reference sets the target switch's bits twice.
            show(ui, c.target_button);
            hide(ui, c.formation_button, !self.formation_available);
            self.check_mode_buttons(ui, true);
            self.set_hit_box([0.131, 0.117], [-0.379, -0.266]);
            return;
        }

        self.mover.start(ANIM_SECONDS);
        self.mover
            .add_button(c.toggle_button, v(TOGGLE_OPEN), self.home_position);
        let lowered = ROW_LOWERED_Y;
        set_pos(ui, c.next_enemy_button, -0.479, lowered, 0.0);
        set_pos(ui, c.scan_button, -0.411, lowered, 0.0);
        set_pos(ui, c.tractor_button, -0.411, lowered, 0.0);
        set_pos(ui, c.comm_button, -0.445, lowered, 0.0);
        set_pos(ui, c.request_group_button, -0.445, lowered, 0.0);
        set_pos(ui, c.contact_list_button, -0.343, lowered, 0.0);
        set_pos(ui, c.target_button, -0.377, lowered, 0.0);
        hide(ui, c.minimized_frame, false);
        set_pos(ui, c.minimized_frame, -0.4105, lowered, -1.0);
        let row = self.bottom_row([0, 1, 2, 4, 3]);
        for (i, control) in row.into_iter().enumerate() {
            let from = Vector::new(ROW_X[i], ROW_LOWERED_Y, ROW_Z[i]);
            let to = Vector::new(ROW_X[i], ROW_Y, ROW_Z[i]);
            self.mover.add_button(control, from, to);
        }
    }

    /// `ButtonMover::Update` with the owner's `OnMoveFinished`.
    fn update_mover(&mut self, ui: &mut Ui, id: NodeId) {
        if self.mover.update(ui, ui.frame_time) {
            self.on_move_finished(ui, id);
        }
    }

    /// `ButtonScaler::Update` with the owner's `OnScaleFinished`.
    fn update_scaler(&mut self, ui: &mut Ui, id: NodeId) {
        if self.scaler.update(ui, ui.frame_time) {
            self.on_scale_finished(ui, id);
        }
    }

    /// `HUD_Target_Base::Update`: the children, the slide, then (open) the
    /// sub-target under the mouse, which the view object's parts give
    /// (none without one, D1125).
    fn base_update(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update(id);
        self.update_mover(ui, id);
        if self.expanded {
            self.sub_target_id = SUBOBJ_ID_NONE;
        }
    }

    /// `UpdateButtons`: once a second of frame time, the sub-target arrows,
    /// the rank line, and the scan, tractor, comm and request group buttons
    /// for the view object, then the trade button. Without a view object
    /// (D1125) the arrows and the rank line are hidden on a full update and
    /// the four buttons always.
    fn update_buttons(&mut self, ui: &mut Ui, full: bool) {
        self.label_refresh_timer = r(ui.frame_time + f64::from(self.label_refresh_timer));
        // FCOM 1.0 / TEST AH,0x41: on only when strictly above 1.
        if self.label_refresh_timer.partial_cmp(&1.0) != Some(std::cmp::Ordering::Greater) {
            return;
        }
        self.label_refresh_timer = r(f64::from(self.label_refresh_timer) - 1.0);
        let c = self.c;
        if full {
            hide(ui, c.previous_button, true);
            hide(ui, c.next_button, true);
            clear(ui, c.rank_text);
        }
        if c.scan_button.is_some() {
            hide(ui, c.scan_button, true);
            let scanner = ui.find_by_name(b"NN_Scanner");
            self.scanner_open = scanner.is_some();
            other(ui, c.scan_button, 0xc, usize::from(self.scanner_open), 0);
            if let Some(scanner) = scanner {
                ui.send_broadcast(scanner, 0xef02, 0, 0);
            }
        }
        hide(ui, c.tractor_button, true);
        if c.comm_button.is_some() {
            hide(ui, c.comm_button, true);
            hide(ui, c.request_group_button, true);
        }
        self.update_trade_button(ui);
    }

    /// `UpdateTradeButton`: a trade is offered to another player's ship in
    /// range; without a view object (D1125) the button is hidden (and no
    /// trade is open to withdraw).
    fn update_trade_button(&self, ui: &mut Ui) {
        hide(ui, self.c.trade_button, true);
    }
}

/// A double expression stored to a `float` (T11).
#[allow(clippy::cast_possible_truncation)]
fn r(v: f64) -> f32 {
    v as f32
}

impl Widget for HudTarget {
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
    /// `HUD_Target_Base::HitTest`: false without the drawn cursor, without
    /// the player's object or with the letterbox up; open, the box round
    /// `HitCenter` first, then `WinControl::HitTest`. The HUD has no
    /// player object yet (D1125), so it is always false.
    fn hit_test(&mut self, _ui: &mut Ui, _id: NodeId, _ray: Vector) -> bool {
        false
    }
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        base::set_position(ui, id, &mut self.control, position);
    }
    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        base::set_orientation(ui, id, &mut self.control, orientation);
    }
    /// `HudTarget::Create`: the children with the collapsed hit box, the
    /// toggle's spin and the labels' home offsets, the open panel's
    /// controls hidden with the arrows and the trade button, the mode
    /// switches checked, the offset (0, 0, -1), the position again, then
    /// `Win3D::Create`. The tool tip's style list (a `StyleNode` and ids
    /// 0x60070) comes with the draw.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        let home = self.home_position;
        let mut made = Controls::default();
        ui.lend(id, self, |ui| made = Controls::create(ui, id, home));
        self.c = made;
        self.set_hit_box([0.109, 0.02], [-0.402, -0.371]);
        self.slot13(ui, true);
        hide(ui, self.c.previous_button, true);
        hide(ui, self.c.next_button, true);
        hide(ui, self.c.trade_button, true);
        self.check_mode_buttons(ui, false);
        base::set_offset(ui, id, self, Vector::new(0.0, 0.0, -1.0));
        let position = ui.node(id).map_or(Vector::ZERO, |n| n.position);
        base::set_position(ui, id, &mut self.control, position);
        ui.base_create(id, parent)
    }
    /// `HudTarget::Update`.
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        if self.first_update {
            self.first_update = false;
            let session_expanded = self.session_flags & 1 != 0;
            if session_expanded != self.expanded {
                self.slot12(ui);
            }
        }
        // `SyncPlayerTarget` and the far or cloaked target's clear need the
        // player's ship (D1125).
        let c = self.c;
        if self.animating() {
            hide(
                ui,
                c.toggle_button,
                self.expanded && !self.mover.is_active(),
            );
            self.update_scaler(ui, id);
            self.update_trade_button(ui);
            self.base_update(ui, id);
            return;
        }
        if self.expanded {
            // The formation button needs the player's ship in formation or
            // with followers (D1125).
            let formation = false;
            hide(ui, c.formation_button, !formation);
            if !formation && self.mode == 2 {
                self.mode = 0;
            }
            self.formation_available = formation;
        }
        // The player ship's target becomes the view object (D1125: none).
        if self.mode != self.shown_mode {
            self.check_mode_buttons(ui, true);
        }
        if self.expanded && self.mode != 1 {
            if self.shown_mode == 1 {
                base::set_hidden_with_children(ui, id, true);
                show(ui, Some(id));
                for control in [
                    c.view_panel,
                    c.close_button,
                    c.contact_list_button,
                    c.next_enemy_button,
                    c.target_button,
                ] {
                    show(ui, control);
                }
                hide(ui, c.formation_button, !self.formation_available);
                self.label_refresh_timer = 1.0;
            }
            self.shown_mode = self.mode;
            ui.base_update(id);
            self.update_mover(ui, id);
            self.update_scaler(ui, id);
            self.update_buttons(ui, false);
            return;
        }
        if self.expanded && self.shown_mode != 1 {
            self.shown_mode = self.mode;
            for control in [
                c.ship_name,
                c.subtarget_name,
                c.rank_text,
                c.previous_button,
                c.next_button,
                c.trade_button,
            ] {
                show(ui, control);
            }
            clear(ui, c.toggle_button);
        }
        self.base_update(ui, id);
        hide(ui, c.ship_name, true);
        hide(ui, c.subtarget_name, true);
        hide(ui, c.rank_text, true);
        clear(ui, c.previous_button);
        clear(ui, c.next_button);
        clear(ui, c.trade_button);
        self.update_buttons(ui, true);
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
    /// `HudTarget::MouseEvent`: a release (`Field0 == -1`) hides the tool
    /// tip (the sub-target's camera command needs a picked sub-target, none
    /// without the view object); every event is taken.
    fn mouse_event(&mut self, ui: &mut Ui, _id: NodeId, event: MouseButton) -> bool {
        if event.index == 0 {
            ui.hide_tooltip();
        }
        true
    }
    /// `HudTarget::SetProperty` over `HUD_Target_Base::SetProperty`: the
    /// close button's and the toggle's clicks run `Slot12`; the view
    /// object's buttons need one (D1125). Nothing reaches `WinControl`'s
    /// handler.
    fn property(&mut self, ui: &mut Ui, _id: NodeId, property: Property<'_>) -> isize {
        if let Property::FromChild {
            msg: 0x26, from, ..
        } = property
        {
            if Some(from) == self.c.close_button {
                self.slot12(ui);
                return 1;
            }
            if Some(from) == self.c.toggle_button {
                self.slot12(ui);
            }
        }
        0
    }
}

/// The element (`HUD_Target`'s `HUDItem` face).
#[derive(Debug)]
pub struct HudTargetItem {
    pub window: NodeId,
}

impl HudItem for HudTargetItem {
    fn name(&self) -> &[u8] {
        SECTION
    }
    fn type_name(&self) -> &'static str {
        "class HudTarget"
    }
    fn window(&self) -> Option<NodeId> {
        Some(self.window)
    }
}

/// `HUDFACTORY<HudTarget>::Create`: the constructor, which sets the
/// toggle's home spot, makes the window (`Create(0, 0)`), names it
/// "HUD_Target", sets the open panel's hit box, registers the events and
/// asks for the first update's check of the session flags.
pub fn create(_reader: &mut IniReader, ui: &mut Ui) -> Option<Box<dyn HudItem>> {
    let id = ui.alloc(Some(Box::new(HudTarget {
        home_position: v(HOME),
        sub_target_id: SUBOBJ_ID_NONE,
        session_flags: 3,
        ..HudTarget::default()
    })));
    ui.with_widget(id, false, |w, ui| {
        w.create(ui, id, None, &mut Payload::None)
    });
    if let Some(n) = ui.node_mut(id) {
        n.set_name(Some(NAME));
        n.class_name = Some("HUD_Target");
    }
    if let Some(t) = ui.widget_mut::<HudTarget>(id) {
        t.hit_center = Vector::new(-0.387, -0.29, -1.0);
        t.hit_half_width = 0.11;
        t.hit_half_height = 0.11;
        t.shown_rank = -1.0;
        t.first_update = true;
    }
    for event in EVENTS {
        ui.services.register_event(id, event);
    }
    Some(Box::new(HudTargetItem { window: id }))
}

#[cfg(test)]
mod tests;
