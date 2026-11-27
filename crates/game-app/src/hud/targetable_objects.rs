//! `HudTargetableObjects` (`hud_targetable_objects.cpp`, the
//! `[Targetable_Objects]` section of `hud.ini`): the brackets, arrows and
//! reticle drawn over the objects in space, the range read-out under the
//! reticle and the four alert icons (missile, trail, radiation, lag).
//!
//! The port so far: the construction and the window tree (the
//! constructor, with `Create`'s body inlined, and its range read-out and
//! alerts). The constructor's 19 mesh holders and their boxes, the
//! `ButtonMontage` tiles, the fonts and the waypoint captions, which only
//! the draw reads, come with the contacts' draw and the targeting
//! (`Render`, `RebuildContacts`, `PickUnderCursor`, `OnEvent` 6, the
//! alerts' updates), with the HUD's draw (P5.7).

use neuron_client::ui::children::{
    StaticInit, TextInit, create_static_control, create_text_control,
};
use neuron_client::ui::control::{WinControl, base};
use neuron_client::ui::message::MouseButton;
use neuron_client::ui::pulse::palette;
use neuron_client::ui::tree::{DrawHost, NodeId, Payload, Property, Ui, Widget};
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};

use super::HudItem;

/// The factory's section, the element's name and the window's name and
/// class name.
pub const SECTION: &[u8] = b"Targetable_Objects";
/// The HUD event the window registers for (a pick).
pub const EVENT: u32 = 6;
/// The range read-out's caption ("RANGE: ").
pub const RANGE_IDS: u32 = 0x453;

/// An alert: name, mesh, offset, tooltip (0: none).
type Alert = (&'static [u8], &'static [u8], [f32; 3], i32);

const ALERTS: [Alert; 4] = [
    (
        b"MissileAlert",
        b"Interface\\hud\\HUD_missilealert.3db",
        [0.126, 0.213, -0.6],
        0x4d0,
    ),
    (
        b"TrailAlert",
        b"Interface\\hud\\HUD_trailalert.3db",
        [-0.126, 0.213, -0.6],
        0x616,
    ),
    (
        b"RadiationAlert",
        b"Interface\\hud\\HUD_radiationalert.3db",
        [0.126, 0.177, -0.6],
        0x617,
    ),
    (
        b"LagAlert",
        b"Interface\\multiplayer\\MULTI_lag.3db",
        [-0.126, 0.177, -0.6],
        0,
    ),
];

/// The window (`HudTargetableObjects`'s `WinControl` face).
#[derive(Default)]
pub struct HudTargetableObjects {
    pub control: WinControl,
    pub reticle_range_text: Option<NodeId>,
    pub missile_alert: Option<NodeId>,
    pub trail_alert: Option<NodeId>,
    pub radiation_alert: Option<NodeId>,
    pub lag_alert: Option<NodeId>,
    /// The alerts' blink timers, periods and states after construction.
    pub missile_blink_timer: f32,
    pub missile_blink_period: f32,
    pub missile_blink_on: bool,
    pub trail_blink_timer: f32,
    pub trail_blink_period: f32,
    pub trail_blink_on: bool,
    pub radiation_blink_timer: f32,
    pub radiation_blink_on: bool,
    pub lag_timer: f32,
    pub lag_blink_on: bool,
    /// `PickSubObjId` (`SUBOBJ_ID_NONE`) and `PickId`.
    pub pick_sub_obj_id: u16,
    pub pick_id: u32,
}

/// `SUBOBJ_ID_NONE`.
const SUBOBJ_ID_NONE: u16 = 0xffff;

/// `CreateAlert`: a static control with a 0.018 square box, red, its
/// tooltip unless 0, hidden.
fn create_alert(ui: &mut Ui, id: NodeId, (name, mesh, [x, y, z], ids): Alert) -> Option<NodeId> {
    let alert = create_static_control(
        ui,
        id,
        name,
        Some(mesh),
        &StaticInit {
            offset: Vector::new(x, y, z),
            half_extents: Vector::new(0.018, 0.018, 0.0),
            no_wire_render: true,
            no_clear_render: true,
            ..StaticInit::default()
        },
    )?;
    let red = palette::RED.to_le_bytes();
    ui.property(alert, Property::Colours(red, red));
    if ids != 0 {
        ui.property(alert, Property::ToolTip(ids));
    }
    ui.property(alert, Property::Hidden(true));
    Some(alert)
}

impl Widget for HudTargetableObjects {
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
    /// `HudTargetableObjects::Create`: the position again, `Win3D::Create`,
    /// and on success event 6.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        let position = ui.node(id).map_or(Vector::ZERO, |n| n.position);
        base::set_position(ui, id, &mut self.control, position);
        if !ui.base_create(id, parent) {
            return false;
        }
        ui.services.register_event(id, EVENT);
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

/// The element (`HudTargetableObjects`'s `HUDItem` face).
#[derive(Debug)]
pub struct HudTargetableObjectsItem {
    pub window: NodeId,
}

impl HudItem for HudTargetableObjectsItem {
    fn name(&self) -> &[u8] {
        SECTION
    }
    fn type_name(&self) -> &'static str {
        "class HudTargetableObjects"
    }
    fn window(&self) -> Option<NodeId> {
        Some(self.window)
    }
}

/// `HUDFACTORY<HudTargetableObjects>::Create`: the constructor (the
/// section's keys are not read), which runs `Create`'s body (`Create(0,
/// 0)`), names the window "Targetable_Objects", sets the blink timers, then
/// makes the range read-out, hidden and white, and the four alerts.
pub fn create(_reader: &mut IniReader, ui: &mut Ui) -> Option<Box<dyn HudItem>> {
    let id = ui.alloc(Some(Box::new(HudTargetableObjects::default())));
    ui.with_widget(id, false, |w, ui| {
        w.create(ui, id, None, &mut Payload::None)
    });
    if let Some(n) = ui.node_mut(id) {
        n.set_name(Some(SECTION));
        n.class_name = Some("Targetable_Objects");
    }
    let range = create_text_control(
        ui,
        id,
        b"ReticleRangeText",
        &TextInit {
            style: Some(b"HudSmall"),
            position: Vector::new(0.003, 0.0, 0.0),
            width: 0.171,
            height: 0.03,
            ids: RANGE_IDS,
            justification: 1,
            tooltip: 0,
            corner_anchor: false,
            ..TextInit::default()
        },
    );
    if let Some(range) = range {
        ui.property(range, Property::Hidden(true));
        ui.property(range, Property::Colour([0xff; 4], true));
    }
    let [missile, trail, radiation, lag] = ALERTS.map(|alert| create_alert(ui, id, alert));
    if let Some(t) = ui.widget_mut::<HudTargetableObjects>(id) {
        t.missile_blink_timer = 0.1;
        t.missile_blink_period = 0.1;
        t.missile_blink_on = true;
        t.trail_blink_timer = 0.5;
        t.trail_blink_period = 0.5;
        t.trail_blink_on = true;
        t.radiation_blink_timer = 0.7;
        t.radiation_blink_on = true;
        t.lag_timer = 1.0;
        t.lag_blink_on = false;
        t.pick_sub_obj_id = SUBOBJ_ID_NONE;
        t.pick_id = 0;
        t.reticle_range_text = range;
        t.missile_alert = missile;
        t.trail_alert = trail;
        t.radiation_alert = radiation;
        t.lag_alert = lag;
    }
    Some(Box::new(HudTargetableObjectsItem { window: id }))
}

#[cfg(test)]
mod tests {
    use neuron_client::ui::tree::VISIBLE;
    use neuron_client::ui::widescreen::Surface;

    use super::*;

    #[test]
    fn the_constructor_makes_the_range_read_out_and_the_alerts_hidden() {
        let mut ui = Ui::new(Surface::new(1024, 768));
        let mut reader = IniReader::new();
        assert!(reader.open(b"hud.ini", b"[Targetable_Objects]\n".to_vec()));
        assert!(reader.read_header());
        let item = create(&mut reader, &mut ui).expect("an element");
        assert_eq!(item.name(), b"Targetable_Objects");
        assert_eq!(item.type_name(), "class HudTargetableObjects");
        let id = item.window().unwrap();
        let node = ui.node(id).unwrap();
        assert_eq!(node.name(), b"Targetable_Objects");
        assert_eq!(node.class_name, Some("Targetable_Objects"));
        assert!(node.is(VISIBLE));
        let children = ui.children(id);
        let names: Vec<_> = children
            .iter()
            .map(|&c| String::from_utf8_lossy(ui.node(c).unwrap().name()).into_owned())
            .collect();
        assert_eq!(
            names,
            [
                "ReticleRangeText",
                "MissileAlert",
                "TrailAlert",
                "RadiationAlert",
                "LagAlert"
            ]
        );
        assert!(children.iter().all(|&c| !ui.node(c).unwrap().is(VISIBLE)));
        let t = ui.widget::<HudTargetableObjects>(id).unwrap();
        assert_eq!(t.lag_alert, Some(children[4]));
        assert_eq!(ui.win_control(children[1]).unwrap().tooltip_ids, 0x4d0);
        assert_eq!(ui.win_control(children[4]).unwrap().tooltip_ids, 0);
        assert_eq!(
            ui.win_control(children[3]).unwrap().half_extents,
            Vector::new(0.018, 0.018, 0.0)
        );
    }
}
