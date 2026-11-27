//! `HUD_Core` (`hud_status_core.cpp`, the `[Core]` section of `hud.ini`):
//! the gauge window at the bottom of the screen, with the hull, shield and
//! power bars and the velocity and thrust read-outs.
//!
//! The port so far: the construction (`HUD_Core::HUD_Core` and `Create`).
//! The bars' fill and the read-outs (`Update`, `Draw`) and the event
//! (`OnEvent` 0x6d) come with the HUD's update and draw.

use neuron_client::ui::children::{
    StaticInit, TextInit, create_static_control, create_text_control,
};
use neuron_client::ui::control::{WinControl, base};
use neuron_client::ui::message::MouseButton;
use neuron_client::ui::tree::{DrawHost, NodeId, Payload, Property, Ui, Widget};
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};

use super::HudItem;

/// The factory's section.
pub const SECTION: &[u8] = b"Core";
/// The window's name and class name.
pub const NAME: &[u8] = b"HUD_Core";
/// The window's mesh.
pub const MESH: &[u8] = b"Interface\\hud\\HUD_guagewindow.cmp";
/// The HUD event the window registers for.
pub const EVENT: u32 = 0x6d;

/// The window (`HUD_Core`'s `WinControl` face).
#[derive(Default)]
pub struct HudCore {
    pub control: WinControl,
    pub background2: Option<NodeId>,
    pub health_bar: Option<NodeId>,
    pub shield_bar: Option<NodeId>,
    pub power_bar: Option<NodeId>,
    pub velocity_text: Option<NodeId>,
    pub thrust_text: Option<NodeId>,
    /// The read-outs' caches: -999 and -1 after construction.
    pub last_velocity: i32,
    pub last_thrust: i32,
}

/// A bar: name, mesh, the offset, the tool tip (`SetProperty(3, ids, 0)`).
type Bar = (&'static [u8], &'static [u8], [f32; 3], i32);

const BARS: [Bar; 3] = [
    (
        b"HealthBar",
        b"Interface\\hud\\HUD_hullgauge.3db",
        [0.005, -0.376, -1.0],
        0x470,
    ),
    (
        b"ShieldBar",
        b"Interface\\hud\\HUD_shieldgauge.3db",
        [0.005, -0.366, -1.0],
        0x471,
    ),
    (
        b"PowerBar",
        b"Interface\\hud\\HUD_energygauge.3db",
        [0.005, -0.355, -1.0],
        0x3f2,
    ),
];

/// The children `Create` makes, in its order.
#[derive(Default)]
struct Children {
    background2: Option<NodeId>,
    bars: [Option<NodeId>; 3],
    velocity_text: Option<NodeId>,
    thrust_text: Option<NodeId>,
}

fn v([x, y, z]: [f32; 3]) -> Vector {
    Vector::new(x, y, z)
}

/// The static controls' arguments: no wire render and no clear render.
fn statics(offset: [f32; 3], half_extents: Vector) -> StaticInit<'static> {
    StaticInit {
        offset: v(offset),
        half_extents,
        no_wire_render: true,
        no_clear_render: true,
        ..StaticInit::default()
    }
}

/// A read-out: "Normal", 0.056 x 0.03, centred, anchored at its corner.
fn read_out(ui: &mut Ui, id: NodeId, name: &[u8], x: f32) -> Option<NodeId> {
    create_text_control(
        ui,
        id,
        name,
        &TextInit {
            style: Some(b"Normal"),
            position: Vector::new(x, 0.008, 0.0),
            width: 0.056,
            height: 0.03,
            ids: 0,
            justification: 1,
            tooltip: 0,
            corner_anchor: true,
            ..TextInit::default()
        },
    )
}

impl Children {
    fn create(ui: &mut Ui, id: NodeId) -> Self {
        let background2 = create_static_control(
            ui,
            id,
            b"Background2",
            Some(b"Interface\\hud\\HUD_numberboxes.cmp"),
            &statics([0.005, -0.365, -1.0], Vector::ZERO),
        );
        let half = Vector::new(0.085, 0.0055, 0.0);
        let bars = BARS.map(|(name, mesh, offset, tooltip)| {
            let bar = create_static_control(ui, id, name, Some(mesh), &statics(offset, half));
            if let Some(bar) = bar {
                ui.property(bar, Property::ToolTip(tooltip));
            }
            bar
        });
        let velocity_text = read_out(ui, id, b"VelocityText", 0.108);
        let thrust_text = read_out(ui, id, b"ThrustText", -0.16);
        Self {
            background2,
            bars,
            velocity_text,
            thrust_text,
        }
    }
}

impl Widget for HudCore {
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
    /// `HUD_Core::Create`: the mesh, the number boxes, the three bars with
    /// their tool tips, the two read-outs, the half extents (0.1, 0.02, 0)
    /// and hit offset (0.005, 0, 0), event 0x6d, the offset (0.005,
    /// -0.365, -1), the position again, then `Win3D::Create`.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        base::set_mesh(ui, id, &mut self.control, MESH, b"");
        // The children are made with the window back in its node, so that
        // they find it as their parent control.
        let mut made = Children::default();
        ui.lend(id, self, |ui| made = Children::create(ui, id));
        self.background2 = made.background2;
        [self.health_bar, self.shield_bar, self.power_bar] = made.bars;
        self.velocity_text = made.velocity_text;
        self.thrust_text = made.thrust_text;
        self.control.half_extents = Vector::new(0.1, 0.02, 0.0);
        self.control.hit_offset = Vector::new(0.005, 0.0, 0.0);
        ui.services.register_event(id, EVENT);
        base::set_offset(ui, id, self, Vector::new(0.005, -0.365, -1.0));
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

/// The element (`HUD_Core`'s `HUDItem` face).
#[derive(Debug)]
pub struct HudCoreItem {
    pub window: NodeId,
}

impl HudItem for HudCoreItem {
    fn name(&self) -> &[u8] {
        SECTION
    }
    fn type_name(&self) -> &'static str {
        "class HUD_Core"
    }
    fn window(&self) -> Option<NodeId> {
        Some(self.window)
    }
}

/// `HUDFACTORY<HUD_Core>::Create`: the constructor, which makes the window
/// (`Create(0, 0)`), names it "HUD_Core" and sets the read-outs' caches.
pub fn create(_reader: &mut IniReader, ui: &mut Ui) -> Option<Box<dyn HudItem>> {
    let id = ui.alloc(Some(Box::new(HudCore::default())));
    ui.with_widget(id, false, |w, ui| {
        w.create(ui, id, None, &mut Payload::None)
    });
    if let Some(n) = ui.node_mut(id) {
        n.set_name(Some(NAME));
        n.class_name = Some("HUD_Core");
    }
    if let Some(core) = ui.widget_mut::<HudCore>(id) {
        core.last_velocity = -999;
        core.last_thrust = -1;
    }
    Some(Box::new(HudCoreItem { window: id }))
}
