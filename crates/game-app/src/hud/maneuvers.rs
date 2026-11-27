//! `HUD_Maneuvers` (`hud_maneuvers.cpp`, the `[Maneuvers]` section of
//! `hud.ini`): the maneuver bar at the top of the screen, with a button per
//! `maneuver` line (FreeFlight, Goto, Dock, Formation), the frame behind
//! them, the tab under the selected one and the cruise button.
//!
//! The port so far: the construction (`HUD_Maneuvers::HUD_Maneuvers` with
//! its `maneuver` lines, and `Create`), and the show and hide of `Update`
//! and `UpdateInactive`. The shown bar's layout (`UpdateButtons`,
//! `UpdateTabAndCruise`, `SetFrameButtonCount`), the throttle ramp, the
//! maneuver requests (`OnEvent`, `RequestManeuver`, `ActivateManeuver`,
//! `OnResult`) and the auto-hiding `Draw` come with the HUD's update, which
//! reads the player's ship and its behaviour manager (P5.7). Until then
//! the services' `player_ship` answers false and the bar hides (D1131).

use neuron_client::ui::children::{
    ButtonInit, StaticInit, create_button_control, create_static_control,
};
use neuron_client::ui::control::{WinControl, base};
use neuron_client::ui::message::MouseButton;
use neuron_client::ui::services::MontageTile;
use neuron_client::ui::tree::{DrawHost, NodeId, Payload, Property, Ui, Widget};
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};

use super::HudItem;

/// The factory's section.
pub const SECTION: &[u8] = b"Maneuvers";
/// The window's name and class name.
pub const NAME: &[u8] = b"HUD_Maneuvers";
/// The frame's mesh `Create` loads (nine buttons' worth).
pub const FRAME_MESH: &[u8] = b"Interface\\HUD\\HUD_maneuverbox9.cmp";
/// The tab's mesh (`kTabMesh`).
pub const TAB_MESH: &[u8] = b"Interface\\Baseside\\CITY_maneuvertab1.cmp";
/// The cruise button's mesh.
pub const CRUISE_MESH: &[u8] = b"interface\\HUD\\hud_mnvrwarp.3db";

/// `kGameOptInterfaceGroup`.
const OPT_INTERFACE_GROUP: u32 = 0x06;
/// `kGameOptHud`.
const OPT_HUD: u32 = 0x0a;

/// `kManeuverCommands`: the behaviour name -> button command table.
const COMMANDS: [(&[u8], i32); 4] = [
    (b"Dock", 0x79),
    (b"Goto", 0x7a),
    (b"Formation", 0x7f),
    (b"FreeFlight", 0xc6),
];

/// `kButtonHomeOffsets`: the four buttons' rest offsets.
const BUTTON_HOME_OFFSETS: [[f32; 3]; 4] = [
    [0.0, 0.231, 0.3],
    [-0.12, 0.231, 0.015],
    [-0.07, 0.231, 0.015],
    [-0.03, 0.231, 0.015],
];

/// The key commands `Create` registers, in its order.
pub const EVENTS: [u32; 0x18] = [
    0x74, 0x76, 0x79, 0x7a, 0x7b, 0x77, 0x78, 0x7e, 0x7f, 0x80, 0xc6, 0x45, 0x81, 0x82, 0x83, 0x84,
    0x86, 0x85, 0x87, 0x88, 0x43, 0x44, 0x04, 0x0f,
];

/// `HudLookupNamedValue`: the first entry whose name equals `name`
/// ignoring case.
fn lookup_command(name: &[u8]) -> Option<i32> {
    COMMANDS
        .iter()
        .find(|(n, _)| n.eq_ignore_ascii_case(name))
        .map(|&(_, v)| v)
}

/// One `maneuver` line (`FHudManeuver`):
/// `maneuver = <behaviour>, <tooltip ids>, <ids>, <mesh>, <gray mesh>`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Maneuver {
    /// Value 0 (`TString<32>`).
    pub name: Vec<u8>,
    /// Value 3.
    pub mesh: Vec<u8>,
    /// Value 4.
    pub gray_mesh: Vec<u8>,
    /// Value 1 (`ftol`): the button's tooltip.
    pub tooltip_ids: i32,
    /// Value 2 (`ftol`).
    pub ids2: i32,
    /// The button's key command ([`COMMANDS`], else 0).
    pub command: i32,
    /// `BehaviorTypes::get_behavior_id(name)`.
    pub behavior_id: i32,
    /// Written by `UpdateButtons`.
    pub selected: bool,
    /// The `ButtonMontage` tiles of the two meshes.
    pub montage_tile: Option<MontageTile>,
    pub gray_montage_tile: Option<MontageTile>,
}

/// `get_value_float(index)` (a `float`) through `ftol`.
fn value_int(reader: &mut IniReader, index: u32) -> i32 {
    let value = reader.value_float(index) as f32;
    neuron_core::num::trunc_to_i32(f64::from(value))
}

impl Maneuver {
    /// The line the reader is on.
    fn read(reader: &mut IniReader, ui: &mut Ui) -> Self {
        // `TString<32>` keeps 31 bytes.
        let mut name = reader.value_string_at(0);
        name.truncate(31);
        let tooltip_ids = value_int(reader, 1);
        let ids2 = value_int(reader, 2);
        let mut mesh = reader.value_string_at(3);
        mesh.truncate(127);
        let command = lookup_command(&name).unwrap_or(0);
        let behavior_id = game_sim::ai::state_graph::behavior_id(&name);
        let mut gray_mesh = reader.value_string_at(4);
        gray_mesh.truncate(127);
        let montage_tile = ui.montage_tile(&mesh);
        let gray_montage_tile = ui.montage_tile(&gray_mesh);
        Self {
            name,
            mesh,
            gray_mesh,
            tooltip_ids,
            ids2,
            command,
            behavior_id,
            selected: false,
            montage_tile,
            gray_montage_tile,
        }
    }
}

/// The window (`HUD_Maneuvers`'s `WinControl` face).
#[derive(Default)]
pub struct HudManeuvers {
    pub control: WinControl,
    /// `Buttons` (+0x36c), "ManeuverButton0" to "ManeuverButton3".
    pub buttons: [Option<NodeId>; 4],
    /// The `maneuver` lines (+0x37c).
    pub maneuvers: Vec<Maneuver>,
    /// +0x38c: 0 after construction.
    pub formation_state: i32,
    /// +0x398: (0.039, 0.039, 0) after construction.
    pub field398: Vector,
    /// +0x3a4, +0x3a8: -1 after construction.
    pub field3a4: i32,
    pub field3a8: i32,
    /// +0x3ac: 100 after construction.
    pub trail_distance: f32,
    /// +0x3b0.
    pub trail_field14: f32,
    /// +0x3b4: `SetFrameButtonCount`'s last count (`Create` leaves 9, the
    /// constructor 0).
    pub frame_button_count: i32,
    /// +0x3b8 "ManeuverFrame".
    pub frame: Option<NodeId>,
    /// +0x3bc "ManeuverTab".
    pub tab: Option<NodeId>,
    /// +0x3c0: 1 once the tab carries [`TAB_MESH`].
    pub tab_mesh_reset: i32,
    /// +0x3c4 "ManeuverCruiseButton".
    pub cruise_button: Option<NodeId>,
    /// +0x3c8: 0x13 after construction.
    pub field3c8: i32,
    pub byte3cc: u8,
    pub byte3cd: u8,
    /// +0x3d0, +0x3dc: the frame's and the tab's half extents from their
    /// meshes.
    pub frame_half_extents: Vector,
    pub tab_half_extents: Vector,
    /// +0x3e8: the throttle ramp of the commands 0x43 and 0x44.
    pub throttle_rate: f32,
}

/// The children `Create` makes, in its order.
#[derive(Default)]
struct Children {
    frame: Option<NodeId>,
    frame_half_extents: Option<Vector>,
    buttons: [Option<NodeId>; 4],
    tab: Option<NodeId>,
    tab_half_extents: Option<Vector>,
    cruise_button: Option<NodeId>,
}

/// The static controls' arguments: no wire render and no clear render.
fn statics(offset: Vector) -> StaticInit<'static> {
    StaticInit {
        offset,
        no_wire_render: true,
        no_clear_render: true,
        ..StaticInit::default()
    }
}

/// The buttons' half extents.
const BUTTON_HALF: Vector = Vector::new(0.014, 0.014, 0.0);

/// A button: the "ui_select_item" sound, an action button, hidden (the
/// reference sets a maneuver button's home offset before the hide, which
/// reads none of it).
fn button(
    ui: &mut Ui,
    id: NodeId,
    name: &[u8],
    mesh: &[u8],
    position: Vector,
    tooltip: i32,
    command: i32,
) -> Option<NodeId> {
    let button = create_button_control(
        ui,
        id,
        name,
        Some(mesh),
        &ButtonInit {
            sound: Some(b"ui_select_item"),
            position,
            half_extents: BUTTON_HALF,
            action_button: 1,
            tooltip,
            command,
            ..ButtonInit::default()
        },
    );
    if let Some(button) = button {
        ui.property(button, Property::Hidden(true));
    }
    button
}

/// `WinControlHalfExtentsFromMesh`: `{box[0], box[2], 0}` of the control's
/// mesh, `None` without a box (D1101).
fn half_extents_from_mesh(ui: &Ui, id: Option<NodeId>) -> Option<Vector> {
    let b = ui.win_control(id?)?.mesh.as_ref().and_then(|m| m.aabb())?;
    Some(Vector::new(b[0], b[2], 0.0))
}

impl Children {
    fn create(ui: &mut Ui, id: NodeId, maneuvers: &[Maneuver]) -> Self {
        let frame = create_static_control(
            ui,
            id,
            b"ManeuverFrame",
            Some(FRAME_MESH),
            &statics(Vector::new(0.0, 0.176, -0.5)),
        );
        let frame_half_extents = half_extents_from_mesh(ui, frame);
        let empty = Maneuver::default();
        let mut buttons = [None; 4];
        for (i, slot) in buttons.iter_mut().enumerate() {
            let [x, y, z] = BUTTON_HOME_OFFSETS[i];
            let position = Vector::new(x, y, z);
            // The reference reads the first four elements whatever the
            // vector holds; a shorter list gives empty buttons (D1130).
            let maneuver = maneuvers.get(i).unwrap_or(&empty);
            let name = format!("ManeuverButton{i}");
            let made = button(
                ui,
                id,
                name.as_bytes(),
                &maneuver.mesh,
                position,
                maneuver.tooltip_ids,
                maneuver.command,
            );
            if let Some(c) = made.and_then(|b| ui.win_control_mut(b)) {
                c.home_offset = position;
            }
            *slot = made;
        }
        let tab = create_static_control(
            ui,
            id,
            b"ManeuverTab",
            Some(TAB_MESH),
            &statics(Vector::new(0.0, 0.292, -1.0)),
        );
        if let Some(tab) = tab {
            ui.property(tab, Property::Hidden(true));
        }
        let tab_half_extents = half_extents_from_mesh(ui, tab);
        let cruise_button = button(
            ui,
            id,
            b"ManeuverCruiseButton",
            CRUISE_MESH,
            Vector::new(0.0, 0.297, 0.0),
            0xbf9,
            0x45,
        );
        Self {
            frame,
            frame_half_extents,
            buttons,
            tab,
            tab_half_extents,
            cruise_button,
        }
    }
}

impl HudManeuvers {
    /// `Update`'s and `UpdateInactive`'s test: the cursor drawn and the
    /// player there without the letterbox (`Update` also asks for the
    /// interface and HUD options).
    fn shown(ui: &mut Ui) -> bool {
        ui.cursor_drawn && ui.services.player_ship() && !ui.services.letterbox_visible()
    }
}

impl Widget for HudManeuvers {
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
    /// `HUD_Maneuvers::Create`: the frame (the frame count 9 and its half
    /// extents), the four buttons at their home offsets, hidden, the tab,
    /// hidden, and its half extents, the cruise button, hidden, the 24 key
    /// commands, the offset (0, 0, -1), the position again, then
    /// `Win3D::Create`.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        // The children are made with the window back in its node, so that
        // they find it as their parent control.
        let maneuvers = std::mem::take(&mut self.maneuvers);
        let mut made = Children::default();
        ui.lend(id, self, |ui| made = Children::create(ui, id, &maneuvers));
        self.maneuvers = maneuvers;
        self.frame = made.frame;
        self.frame_button_count = 9;
        if let Some(half) = made.frame_half_extents {
            self.frame_half_extents = half;
        }
        self.buttons = made.buttons;
        self.tab = made.tab;
        if let Some(half) = made.tab_half_extents {
            self.tab_half_extents = half;
        }
        self.cruise_button = made.cruise_button;
        for event in EVENTS {
            ui.services.register_event(id, event);
        }
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
    /// `HUD_Maneuvers::Update`: shown with the cursor, the interface and
    /// HUD options and the player and without the letterbox, else hidden
    /// with its children; then `Win3D::Update`. The throttle ramp and the
    /// shown bar's layout (`UpdateButtons`) wait for the player's ship;
    /// the options come from the services (D1116, D1131).
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        let shown = ui.services.game_option(OPT_INTERFACE_GROUP) != 0
            && ui.services.game_option(OPT_HUD) != 0
            && Self::shown(ui);
        if !shown {
            base::set_hidden_with_children(ui, id, true);
        }
        ui.base_update(id);
    }
    /// `HUD_Maneuvers::UpdateInactive`: shown again with the cursor and
    /// the player and without the letterbox, then `Win3D::UpdateInactive`.
    fn update_inactive(&mut self, ui: &mut Ui, id: NodeId) {
        if Self::shown(ui) {
            base::set_hidden_with_children(ui, id, false);
        }
        ui.base_update_inactive(id);
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
    /// `HUD_Maneuvers::SetProperty`: only 0x26 (a button activated), which
    /// waits for the maneuver requests; nothing goes to `WinControl`.
    fn property(&mut self, _ui: &mut Ui, _id: NodeId, _property: Property<'_>) -> isize {
        0
    }
}

/// The element (`HUD_Maneuvers`'s `HUDItem` face).
#[derive(Debug)]
pub struct HudManeuversItem {
    pub window: NodeId,
}

impl HudItem for HudManeuversItem {
    fn name(&self) -> &[u8] {
        SECTION
    }
    fn type_name(&self) -> &'static str {
        "class HUD_Maneuvers"
    }
    fn window(&self) -> Option<NodeId> {
        Some(self.window)
    }
}

/// `HUDFACTORY<HUD_Maneuvers>::Create`: the constructor, which reads the
/// section's `maneuver` lines, makes the window (`Create(0, 0)`), names it
/// "HUD_Maneuvers" and sets its fields.
pub fn create(reader: &mut IniReader, ui: &mut Ui) -> Option<Box<dyn HudItem>> {
    let mut maneuvers = Vec::new();
    while reader.read_value() {
        if reader.is_value(b"maneuver") {
            maneuvers.push(Maneuver::read(reader, ui));
        }
    }
    let id = ui.alloc(Some(Box::new(HudManeuvers {
        maneuvers,
        ..HudManeuvers::default()
    })));
    ui.with_widget(id, false, |w, ui| {
        w.create(ui, id, None, &mut Payload::None)
    });
    if let Some(n) = ui.node_mut(id) {
        n.set_name(Some(NAME));
        n.class_name = Some("HUD_Maneuvers");
    }
    if let Some(m) = ui.widget_mut::<HudManeuvers>(id) {
        m.formation_state = 0;
        m.field398 = Vector::new(0.039, 0.039, 0.0);
        m.field3a4 = -1;
        m.field3a8 = -1;
        m.trail_field14 = 0.0;
        m.frame_button_count = 0;
        m.throttle_rate = 0.0;
        m.trail_distance = 100.0;
        m.tab_mesh_reset = 1;
        m.field3c8 = 0x13;
        m.byte3cc = 0;
        m.byte3cd = 0;
    }
    Some(Box::new(HudManeuversItem { window: id }))
}

#[cfg(test)]
mod tests {
    use neuron_client::ui::tree::{ACTIVE, VISIBLE};
    use neuron_client::ui::widescreen::Surface;

    use super::*;

    const INI: &str = "[Maneuvers]\n\
        maneuver = FreeFlight, 1254, 1254, interface\\HUD\\MNVR_freeflight.3db, interface\\HUD\\MNVR_freeflight_gray.3db\n\
        other = 1\n\
        maneuver = goto, 3051.9, 3101, a.3db, b.3db\n\
        maneuver = Dock, 993, 3104, c.3db, d.3db\n\
        maneuver = Formation, 995, 3117, e.3db, f.3db\n";

    /// A client with a player ship.
    struct Player;
    impl neuron_client::ui::services::UiServices for Player {
        fn player_ship(&mut self) -> bool {
            true
        }
    }

    fn load() -> (Ui, NodeId) {
        let mut ui = Ui::new(Surface::new(1024, 768));
        let mut reader = IniReader::new();
        assert!(reader.open(b"hud.ini", INI.as_bytes().to_vec()));
        assert!(reader.read_header());
        let item = create(&mut reader, &mut ui).expect("an element");
        assert_eq!(item.name(), b"Maneuvers");
        (ui, item.window().expect("a window"))
    }

    #[test]
    fn the_constructor_reads_the_maneuver_lines() {
        let (ui, id) = load();
        let m = ui.widget::<HudManeuvers>(id).unwrap();
        let rows: Vec<_> = m
            .maneuvers
            .iter()
            .map(|m| {
                (
                    m.name.as_slice(),
                    m.tooltip_ids,
                    m.ids2,
                    m.command,
                    m.behavior_id,
                )
            })
            .collect();
        assert_eq!(
            rows,
            [
                (&b"FreeFlight"[..], 1254, 1254, 0xc6, 0x13),
                (&b"goto"[..], 3051, 3101, 0x7a, 2),
                (&b"Dock"[..], 993, 3104, 0x79, 7),
                (&b"Formation"[..], 995, 3117, 0x7f, 10),
            ]
        );
        assert_eq!(m.maneuvers[1].gray_mesh, b"b.3db");
        assert_eq!(m.frame_button_count, 0);
        assert_eq!(m.trail_distance, 100.0);
        assert_eq!(m.field3c8, 0x13);
    }

    #[test]
    fn create_makes_the_frame_buttons_tab_and_cruise_button() {
        let (ui, id) = load();
        let node = ui.node(id).unwrap();
        assert_eq!(node.name(), NAME);
        assert_eq!(node.class_name, Some("HUD_Maneuvers"));
        let names: Vec<_> = ui
            .children(id)
            .into_iter()
            .map(|c| String::from_utf8_lossy(ui.node(c).unwrap().name()).into_owned())
            .collect();
        assert_eq!(
            names,
            [
                "ManeuverFrame",
                "ManeuverButton0",
                "ManeuverButton1",
                "ManeuverButton2",
                "ManeuverButton3",
                "ManeuverTab",
                "ManeuverCruiseButton",
            ]
        );
        let m = ui.widget::<HudManeuvers>(id).unwrap();
        assert_eq!(m.control.offset, Vector::new(0.0, 0.0, -1.0));
        let b1 = ui.win_control(m.buttons[1].unwrap()).unwrap();
        assert_eq!(b1.home_offset, Vector::new(-0.12, 0.231, 0.015));
        assert_eq!(b1.mesh_path, b"a.3db");
        let frame = ui.node(m.frame.unwrap()).unwrap();
        assert!(frame.is(VISIBLE));
        assert!(!ui.node(m.tab.unwrap()).unwrap().is(VISIBLE));
    }

    #[test]
    fn update_hides_the_bar_without_a_player_and_shows_it_with_one() {
        let (mut ui, id) = load();
        ui.update_node(id);
        let frame = ui.widget::<HudManeuvers>(id).unwrap().frame.unwrap();
        assert!(!ui.node(id).unwrap().is(ACTIVE | VISIBLE));
        assert!(!ui.node(frame).unwrap().is(VISIBLE));
        // Inactive now: `UpdateInactive` shows it once there is a player.
        ui.update_node(id);
        assert!(!ui.node(id).unwrap().is(VISIBLE));
        ui.services = Box::new(Player);
        ui.update_node(id);
        assert!(ui.node(id).unwrap().is(VISIBLE));
        assert!(ui.node(frame).unwrap().is(VISIBLE));
    }
}
