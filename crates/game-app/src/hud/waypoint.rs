//! `HUD_Waypoint` (`hud_waypoint.cpp`, the `[Waypoint]` section of
//! `hud.ini`): the waypoint the player flies to, a client-side solar the
//! element spawns, targets, spins and scales. Its window has no children
//! and draws nothing; `Update` shows it only while the cursor is drawn,
//! the HUD options are on and there is a player ship.
//!
//! The port so far: the constructor (whose reader is not read), `Create`,
//! `Update` and `UpdateInactive`. The waypoint itself (`Render`,
//! `SetWaypoint` and the solar) comes with the HUD's draw (divergence
//! D1117).

use neuron_client::engine::options::INTERFACE;
use neuron_client::ui::control::{WinControl, base};
use neuron_client::ui::message::MouseButton;
use neuron_client::ui::tree::{DrawHost, NodeId, Payload, Property, Ui, Widget};
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};

use super::HudItem;

/// The factory's section and `HUDItem::Name`.
pub const SECTION: &[u8] = b"Waypoint";
/// The window's name and class name.
pub const NAME: &[u8] = b"HUD_Waypoint";
/// `kGameOptHud`.
pub const OPT_HUD: u32 = 0x0a;

/// The window (`HUD_Waypoint`'s `WinControl` face) and the waypoint's
/// state.
#[derive(Debug, Default)]
pub struct HudWaypoint {
    pub control: WinControl,
    /// `+0x36c`: -1; "waypoint_target%d" uses it, then counts up.
    pub spawn_counter: i32,
    /// `+0x370`: degrees.
    pub spin_angle: f32,
    /// `+0x374`: a player waypoint (else a best-path point).
    pub user: bool,
    /// `+0x378`: the entry's index (not written by the constructor; 0).
    pub index: i32,
    /// `+0x37c`: 1.0.
    pub scale: f32,
    /// `+0x380`: the "objective_reached" latch.
    pub reached: bool,
    /// `+0x384`: -1, the player waypoint's `AutoId`.
    pub id: i32,
    /// `+0x388`.
    pub field_388: i32,
    /// `+0x38c`: the waypoint is targeted.
    pub active: bool,
}

/// `GetClientMouseCursorDrawFlag() != 0 && GetPlayerObjRW() != nullptr`
/// (the ship through [`UiServices::player_ship`], divergence D1116).
///
/// [`UiServices::player_ship`]: neuron_client::ui::services::UiServices::player_ship
pub(crate) fn cursor_and_player(ui: &mut Ui) -> bool {
    ui.cursor_drawn && ui.services.player_ship()
}

impl Widget for HudWaypoint {
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
    /// `HUD_Waypoint::Create` (shared with `HUD_DamageIndicator`): the
    /// offset (0, 0, -1), the position again, then `Win3D::Create`.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
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
    /// `HUD_Waypoint::Update`: shown, and the child walk, only with the
    /// cursor drawn, the interface and HUD options on and a player ship;
    /// hidden otherwise.
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        if ui.cursor_drawn
            && ui.services.game_option(INTERFACE) != 0
            && ui.services.game_option(OPT_HUD) != 0
            && ui.services.player_ship()
        {
            base::set_hidden_with_children(ui, id, false);
            ui.base_update(id);
            return;
        }
        base::set_hidden_with_children(ui, id, true);
    }
    /// `HUD_Waypoint::UpdateInactive` (shared with `HUD_CruiseProgress`):
    /// shown with the cursor drawn and a player ship, then the inactive
    /// child walk.
    fn update_inactive(&mut self, ui: &mut Ui, id: NodeId) {
        if cursor_and_player(ui) {
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
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        base::property(ui, id, self, property)
    }
}

/// The element (`HUD_Waypoint`'s `HUDItem` face).
#[derive(Debug)]
pub struct HudWaypointItem {
    pub window: NodeId,
}

impl HudItem for HudWaypointItem {
    fn name(&self) -> &[u8] {
        SECTION
    }
    fn type_name(&self) -> &'static str {
        "class HUD_Waypoint"
    }
    fn window(&self) -> Option<NodeId> {
        Some(self.window)
    }
}

/// `HUDFACTORY<HUD_Waypoint>::Create`: the constructor, which runs
/// `Create`'s body (`Create(0, 0)`), names the window "HUD_Waypoint" and
/// sets the counters.
pub fn create(_reader: &mut IniReader, ui: &mut Ui) -> Option<Box<dyn HudItem>> {
    let id = ui.alloc(Some(Box::new(HudWaypoint::default())));
    ui.with_widget(id, false, |w, ui| {
        w.create(ui, id, None, &mut Payload::None)
    });
    if let Some(n) = ui.node_mut(id) {
        n.set_name(Some(NAME));
        n.class_name = Some("HUD_Waypoint");
    }
    if let Some(w) = ui.widget_mut::<HudWaypoint>(id) {
        w.spawn_counter = -1;
        w.spin_angle = 0.0;
        w.user = false;
        w.scale = 1.0;
        w.reached = false;
        w.id = -1;
        w.field_388 = 0;
        w.active = false;
    }
    Some(Box::new(HudWaypointItem { window: id }))
}

#[cfg(test)]
mod tests {
    use neuron_client::ui::services::UiServices;
    use neuron_client::ui::widescreen::Surface;

    use super::*;

    struct Player(bool);
    impl UiServices for Player {
        fn player_ship(&mut self) -> bool {
            self.0
        }
        fn game_option(&mut self, _index: u32) -> i32 {
            1
        }
    }

    fn make(player: bool) -> (Ui, NodeId) {
        let mut ui = Ui::new(Surface::new(1024, 768));
        ui.services = Box::new(Player(player));
        let mut reader = IniReader::new();
        let item = create(&mut reader, &mut ui).expect("an element");
        assert_eq!(item.name(), b"Waypoint");
        (ui, item.window().unwrap())
    }

    #[test]
    fn the_constructor_makes_the_window_active_and_visible() {
        let (ui, id) = make(false);
        let node = ui.node(id).unwrap();
        assert_eq!(node.name(), NAME);
        assert_eq!(node.class_name, Some("HUD_Waypoint"));
        assert_eq!(node.flags, 0x0b);
        let w = ui.widget::<HudWaypoint>(id).unwrap();
        assert_eq!(w.control.offset, Vector::new(0.0, 0.0, -1.0));
        assert_eq!((w.spawn_counter, w.id, w.scale), (-1, -1, 1.0));
    }

    #[test]
    fn update_hides_the_window_without_a_player_ship() {
        let (mut ui, id) = make(false);
        ui.update_all(0.0);
        assert_eq!(ui.node(id).unwrap().flags, 0x08);
        // Inactive now: `UpdateInactive` shows it again only with a ship.
        ui.update_all(0.0);
        assert_eq!(ui.node(id).unwrap().flags, 0x08);
    }

    #[test]
    fn update_keeps_the_window_with_a_player_ship() {
        let (mut ui, id) = make(true);
        ui.update_all(0.0);
        assert_eq!(ui.node(id).unwrap().flags, 0x0b);
    }
}
