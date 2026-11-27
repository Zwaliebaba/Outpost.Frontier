//! `HUD_DamageIndicator` (`hud_damage_indicator.cpp`, the
//! `[DamageIndicator]` section of `hud.ini`): four copies of
//! `HUD_damagebottom.3db`, turned 180, 0, -90 and 90 degrees about Z, that
//! flash on the side of the screen a hit came from and fade over 0.7 s.
//! Its window has no children; `Update` shows it while the cursor is drawn
//! and there is a player ship, and fades the four flashes.
//!
//! The port so far: the constructor (whose reader is not read), the four
//! arrows' placement, `Create`, `Update` and `UpdateInactive`. The arrows'
//! meshes are kept by their path; the hits (`Flash*`) and `Draw` come with
//! the HUD's draw (divergence D1117).

use neuron_client::ui::control::{WinControl, base};
use neuron_client::ui::message::MouseButton;
use neuron_client::ui::tree::{ACTIVE, DrawHost, NodeId, Payload, Property, Ui, Widget};
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Transform, Vector};

use super::HudItem;
use super::waypoint::cursor_and_player;

/// The factory's section and `HUDItem::Name`.
pub const SECTION: &[u8] = b"DamageIndicator";
/// The window's name and class name.
pub const NAME: &[u8] = b"HUD_DamageIndicator";
/// `kDamageBottomMesh`, every arrow's mesh.
pub const MESH: &[u8] = b"interface\\hud\\HUD_damagebottom.3db";

/// `FHudDamageIndicatorPart`: one arrow, its mesh and its flash timer.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Part {
    /// The mesh holder's path (`ClientMeshLoadFromPath(path, 0, 1)`).
    pub mesh: Vec<u8>,
    /// The instance's matrix: the turn about Z at (0, 0, -1).
    pub transform: Transform,
    /// `+0x28`: 0 to 1; +0.7 (or +0.35) per hit, less the frame time each
    /// frame.
    pub flash: f32,
}

impl Part {
    /// `FHudDamageIndicatorPart(Path, Degrees)`: the angle is stored as a
    /// float before `FSIN` reloads it, `FCOS` takes the unrounded product.
    #[must_use]
    pub fn new(path: &[u8], degrees: f32) -> Self {
        let angle = f64::from(degrees) * f64::from(0.017_453_292_f32);
        let angle_f = angle as f32;
        let cos = angle.cos() as f32;
        let sin = f64::from(angle_f).sin() as f32;
        Self {
            mesh: path.to_vec(),
            transform: Transform::new(
                Matrix::from_rows([[cos, -sin, 0.0], [sin, cos, 0.0], [0.0, 0.0, 1.0]]),
                Vector::new(0.0, 0.0, -1.0),
            ),
            flash: 0.0,
        }
    }

    /// `FadeFlash`: above zero the timer loses the frame time, otherwise
    /// (NaN too) it is 0.
    pub fn fade(&mut self, frame_time: f64) {
        self.flash = if self.flash > 0.0 {
            (f64::from(self.flash) - frame_time) as f32
        } else {
            0.0
        };
    }
}

/// The window (`HUD_DamageIndicator`'s `WinControl` face) and its arrows.
#[derive(Debug, Default)]
pub struct HudDamageIndicator {
    pub control: WinControl,
    /// `+0x364`, `+0x390`, `+0x3bc`, `+0x3e8`.
    pub part_180: Part,
    pub part_0: Part,
    pub part_minus_90: Part,
    pub part_90: Part,
}

impl HudDamageIndicator {
    /// The member constructors, in their order.
    fn new() -> Self {
        Self {
            control: WinControl::default(),
            part_180: Part::new(MESH, 180.0),
            part_0: Part::new(MESH, 0.0),
            part_minus_90: Part::new(MESH, -90.0),
            part_90: Part::new(MESH, 90.0),
        }
    }

    /// The four fades of `Update` and `UpdateInactive`, in their order.
    fn fade(&mut self, frame_time: f64) {
        self.part_0.fade(frame_time);
        self.part_180.fade(frame_time);
        self.part_minus_90.fade(frame_time);
        self.part_90.fade(frame_time);
    }
}

impl Widget for HudDamageIndicator {
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
    /// `HUD_DamageIndicator::Create` (`HUD_Waypoint`'s body): the offset
    /// (0, 0, -1), the position again, then `Win3D::Create`.
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
    /// `HUD_DamageIndicator::Update`: shown and active with the cursor drawn
    /// and a player ship, hidden otherwise; the fades; the child walk.
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        if cursor_and_player(ui) {
            base::set_hidden_with_children(ui, id, false);
            if let Some(n) = ui.node_mut(id) {
                n.flags |= ACTIVE;
            }
        } else {
            base::set_hidden_with_children(ui, id, true);
        }
        self.fade(ui.frame_time);
        ui.base_update(id);
    }
    /// `HUD_DamageIndicator::UpdateInactive`: the show half, the fades, the
    /// inactive child walk.
    fn update_inactive(&mut self, ui: &mut Ui, id: NodeId) {
        if cursor_and_player(ui) {
            base::set_hidden_with_children(ui, id, false);
        }
        self.fade(ui.frame_time);
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

/// The element (`HUD_DamageIndicator`'s `HUDItem` face).
#[derive(Debug)]
pub struct HudDamageIndicatorItem {
    pub window: NodeId,
}

impl HudItem for HudDamageIndicatorItem {
    fn name(&self) -> &[u8] {
        SECTION
    }
    fn type_name(&self) -> &'static str {
        "class HUD_DamageIndicator"
    }
    fn window(&self) -> Option<NodeId> {
        Some(self.window)
    }
}

/// `HUDFACTORY<HUD_DamageIndicator>::Create`: the constructor, the four
/// arrows, `Create`'s body (`Create(0, 0)`), the window's name and class
/// name. The flags are not touched.
pub fn create(_reader: &mut IniReader, ui: &mut Ui) -> Option<Box<dyn HudItem>> {
    let id = ui.alloc(Some(Box::new(HudDamageIndicator::new())));
    ui.with_widget(id, false, |w, ui| {
        w.create(ui, id, None, &mut Payload::None)
    });
    if let Some(n) = ui.node_mut(id) {
        n.set_name(Some(NAME));
        n.class_name = Some("HUD_DamageIndicator");
    }
    Some(Box::new(HudDamageIndicatorItem { window: id }))
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
    }

    fn make(player: bool) -> (Ui, NodeId) {
        let mut ui = Ui::new(Surface::new(1024, 768));
        ui.services = Box::new(Player(player));
        let mut reader = IniReader::new();
        let item = create(&mut reader, &mut ui).expect("an element");
        assert_eq!(item.name(), b"DamageIndicator");
        (ui, item.window().unwrap())
    }

    #[test]
    fn the_arrows_turn_about_z_one_metre_ahead() {
        let bits = |m: &Matrix| m.m.map(|r| r.map(f32::to_bits));
        let p = Part::new(MESH, 0.0);
        assert_eq!(p.transform.rotation, Matrix::IDENTITY);
        assert_eq!(p.transform.translation, Vector::new(0.0, 0.0, -1.0));
        let p = Part::new(MESH, 90.0);
        // cos(pi/2 as rounded) and sin of the float angle.
        let angle = f64::from(90.0_f32) * f64::from(0.017_453_292_f32);
        assert_eq!(p.transform.rotation.m[0][0], angle.cos() as f32);
        assert_eq!(p.transform.rotation.m[1][0], 1.0);
        assert_eq!(p.transform.rotation.m[0][1], -1.0);
        let p = Part::new(MESH, -90.0);
        assert_eq!(p.transform.rotation.m[1][0], -1.0);
        let p = Part::new(MESH, 180.0);
        assert_eq!(p.transform.rotation.m[0][0], -1.0);
        assert_eq!(bits(&p.transform.rotation)[2], [0, 0, 1.0_f32.to_bits()]);
        assert_eq!(p.mesh, MESH);
    }

    #[test]
    fn a_flash_fades_by_the_frame_time_and_stops_at_zero() {
        let mut p = Part::new(MESH, 0.0);
        p.flash = 0.7;
        p.fade(0.25);
        assert_eq!(p.flash, (f64::from(0.7_f32) - 0.25) as f32);
        p.flash = -0.1;
        p.fade(0.25);
        assert_eq!(p.flash, 0.0);
    }

    #[test]
    fn update_hides_the_window_without_a_player_ship() {
        let (mut ui, id) = make(false);
        let node = ui.node(id).unwrap();
        assert_eq!((node.name(), node.flags), (NAME, 0x0b));
        ui.update_all(0.0);
        assert_eq!(ui.node(id).unwrap().flags, 0x08);
    }

    #[test]
    fn update_shows_the_window_with_a_player_ship() {
        let (mut ui, id) = make(true);
        ui.widget_mut::<HudDamageIndicator>(id)
            .unwrap()
            .part_90
            .flash = 0.5;
        ui.update_all(0.125);
        assert_eq!(ui.node(id).unwrap().flags, 0x0b);
        let w = ui.widget::<HudDamageIndicator>(id).unwrap();
        assert_eq!(w.part_90.flash, 0.375);
    }
}
