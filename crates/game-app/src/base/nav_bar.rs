//! The navigation bar (`nav_bar.cpp`, `NavBar`): the base-side bar of the
//! room buttons ("ButtonControl1" to "ButtonControl5" of `navbar.ini`) and
//! the service tabs above them ("RoomControl1" to "RoomControl7"), with the
//! maneuver frame and tab meshes behind them.
//!
//! This increment builds the bar's window tree: `NavBar::Create` (the frame
//! and the hidden tab, then `navbar.ini`'s `[BaseFrame]` and its twelve
//! buttons through the factory) and the `[NavBar]` values, the hotspot
//! names' meshes. `NavBarEnsure` makes the bar on entering a room; the
//! layout, the animations and the clicks follow with the rooms.

use std::collections::BTreeMap;

use neuron_client::ui::control::{WinControl, base};
use neuron_client::ui::controls::static_control::StaticControlWidget;
use neuron_client::ui::factory::Factory;
use neuron_client::ui::message::MouseButton;
use neuron_client::ui::tree::{DrawHost, NodeId, Payload, Property, Ui, Widget};
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};

/// The bar's layout file (`GetFileSysPath`'s argument).
pub const NAVBAR_INI: &[u8] = b"interface\\baseside\\navbar.ini";

/// The name and the factory class of the bar (`NavBarEnsure`).
pub const NAME: &[u8] = b"NavBar";

/// The mesh an unconfigured button shows (`DefaultMesh`).
pub const DEFAULT_MESH: &[u8] = b"interface\\baseside\\unknown.3db";

/// `NavBar`.
#[derive(Debug, Default)]
pub struct NavBar {
    pub control: WinControl,
    /// `IniValues`: the `[NavBar]` keys and values (`SetIniValue`), in the
    /// `std::map<std::string>` order.
    pub ini_values: BTreeMap<Vec<u8>, Vec<u8>>,
    /// "ManeuverFrame", the frame mesh behind the room buttons.
    pub maneuver_frame: Option<NodeId>,
    /// "ManeuverTab", the tab mesh behind the service tabs, hidden.
    pub maneuver_tab: Option<NodeId>,
    /// `DefaultMesh`, set at the end of `Create`.
    pub default_mesh: Option<&'static [u8]>,
}

/// `WinControl::CreateStaticControl` with `NavBar::Create`'s arguments: a
/// "StaticControl" child named `name` under `parent`, its mesh at `offset`
/// with no hardpoint, no hit box, no wire and no clear render
/// (`StaticControl::Init`).
fn create_frame_mesh(
    ui: &mut Ui,
    parent: NodeId,
    name: &[u8],
    mesh: &[u8],
    offset: Vector,
) -> Option<NodeId> {
    let id = ui.create_by_class(
        Some(name),
        b"StaticControl",
        Some(parent),
        &mut Payload::None,
    )?;
    ui.with_widget(id, (), |w, ui| {
        if let Some(s) = w.as_any_mut().downcast_mut::<StaticControlWidget>() {
            s.control.init(
                ui,
                id,
                Some(mesh),
                offset,
                None,
                Vector::ZERO,
                false,
                false,
                false,
                true,
                false,
                false,
                true,
                0,
                0,
                false,
                [0.0; 3],
            );
        }
    });
    Some(id)
}

impl NavBar {
    /// `SetIniValue`: the value stored under its key.
    pub fn set_ini_value(&mut self, name: &[u8], value: &[u8]) {
        self.ini_values.insert(name.to_vec(), value.to_vec());
    }

    /// The `[NavBar]` values of the layout file, read after its load.
    fn read_ini_values(&mut self, text: &[u8]) {
        let mut ini = IniReader::new();
        if ini.open(NAVBAR_INI, text.to_vec()) && ini.find_header(b"NavBar") {
            while ini.read_value() {
                self.set_ini_value(&ini.name(), &ini.value_string());
            }
        }
    }
}

/// Registers the "NavBar" factory (`WINFACTORY<NavBar>`).
pub fn register(factory: &mut Factory) {
    factory.register("NavBar", || Box::new(NavBar::default()));
}

/// `NavBarEnsure`'s first half: the "NavBar" window found, or made by the
/// factory under no parent (`WinFactoryCreate("NavBar", "NavBar", 0, 0)`).
/// `OnLocationEntered` follows with the rooms.
pub fn ensure(ui: &mut Ui) -> Option<NodeId> {
    if let Some(id) = ui.find_by_name(NAME) {
        return Some(id);
    }
    ui.create_by_class(Some(NAME), NAME, None, &mut Payload::None)
}

impl Widget for NavBar {
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
    /// `NavBar::Create`: "ManeuverFrame" at (0, 0.176, -0.5),
    /// "ManeuverTab" at (0, 0.283, -1) and hidden, `Win3D::Create`, then
    /// `navbar.ini` through `LoadIni` (with the bar lent back to its node,
    /// so that the buttons find it), its `[NavBar]` values and the default
    /// mesh.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        let frame_at = Vector::new(
            0.0,
            f32::from_bits(0x3e34_3958),
            f32::from_bits(0xbf00_0000),
        );
        self.maneuver_frame = create_frame_mesh(
            ui,
            id,
            b"ManeuverFrame",
            b"Interface\\HUD\\HUD_maneuverbox6.cmp",
            frame_at,
        );
        let tab_at = Vector::new(
            0.0,
            f32::from_bits(0x3e90_624e),
            f32::from_bits(0xbf80_0000),
        );
        self.maneuver_tab = create_frame_mesh(
            ui,
            id,
            b"ManeuverTab",
            b"Interface\\Baseside\\CITY_maneuvertab3.cmp",
            tab_at,
        );
        if let Some(tab) = self.maneuver_tab {
            ui.property(tab, Property::Hidden(true));
        }
        ui.base_create(id, parent);
        // `LoadIni(path)` (slot 28) and the `[NavBar]` values of the same
        // file; a file that cannot be opened gives neither.
        if let Some(text) = ui.services.data_file(NAVBAR_INI) {
            let mut ini = IniReader::new();
            if ini.open(NAVBAR_INI, text.clone()) {
                ui.lend(id, self, |ui| {
                    neuron_client::ui::control::load_ini(ui, id, &mut ini);
                });
            }
            self.read_ini_values(&text);
        }
        self.default_mesh = Some(DEFAULT_MESH);
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
