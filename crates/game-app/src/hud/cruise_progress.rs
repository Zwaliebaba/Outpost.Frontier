//! `HUD_CruiseProgress` (`hud_cruise_progress.cpp`, the `[CruiseProgress]`
//! section of `hud.ini`): the cruise engine's charge caption under the
//! crosshair, one text control, "CruiseProgressText". The constructor
//! leaves the window visible but inactive; `Update` shows it while the
//! cursor is drawn and there is a player ship.
//!
//! The port so far: the constructor (whose reader is not read), `Create`
//! with the two captions, `Update` and `UpdateInactive`. The caption's
//! words and draw from the engine's charge (`Draw`) come with the HUD's
//! draw (divergence D1117).

use neuron_client::ui::children::{TextInit, create_text_control};
use neuron_client::ui::control::{WinControl, base};
use neuron_client::ui::controls::text_control::TextControlWidget;
use neuron_client::ui::message::MouseButton;
use neuron_client::ui::tree::{ACTIVE, DrawHost, NodeId, Payload, Property, Ui, Widget};
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Vector};

use super::HudItem;
use super::waypoint::cursor_and_player;

/// The factory's section.
pub const SECTION: &[u8] = b"CruiseProgress";
/// `HUDItem::Name`, the window's name and its class name.
pub const NAME: &[u8] = b"HUD_CruiseProgress";
/// The ids of the caption while the engine charges ("%s - %d%%").
pub const CHARGING_IDS: u32 = 0x585;
/// The ids of the caption before the charge starts ("%s - %d").
pub const COUNTDOWN_IDS: u32 = 0x64f;
/// The captions' buffers, in UTF-16 units with the terminator.
const CAPTION_ROOM: usize = 0x30;

/// The window (`HUD_CruiseProgress`'s `WinControl` face).
#[derive(Debug, Default)]
pub struct HudCruiseProgress {
    pub control: WinControl,
    /// `+0x364`: "CruiseProgressText".
    pub text: Option<NodeId>,
    /// `+0x368`: ids 0x585.
    pub charging_caption: Vec<u16>,
    /// `+0x3c8`: ids 0x64f.
    pub countdown_caption: Vec<u16>,
    /// `+0x428`: the "charging" caption is up.
    pub charging: bool,
}

/// `GetIdsStringWWithRawFallback(ids, buffer, 0x30)`: at most 0x2f units.
fn caption(ui: &Ui, ids: u32) -> Vec<u16> {
    let Some(c) = ui.catalogs.as_deref() else {
        return Vec::new();
    };
    let mut buffer = [0u16; CAPTION_ROOM];
    let n = c.string_with_card_fallback_into(ids, &mut buffer);
    buffer[..n].to_vec()
}

/// The label: "SubHeader", 0.485 x 0.04 at (-0.243, -0.263, 0), no ids,
/// centred, anchored at its corner (`Arg6` 0.9 is not read).
fn create_label(ui: &mut Ui, id: NodeId) -> Option<NodeId> {
    let text = create_text_control(
        ui,
        id,
        b"CruiseProgressText",
        &TextInit {
            style: Some(b"SubHeader"),
            position: Vector::new(-0.243, -0.263, 0.0),
            width: 0.485,
            height: 0.04,
            ids: 0,
            justification: 1,
            tooltip: 0,
            corner_anchor: true,
            ..TextInit::default()
        },
    );
    // The label's string goes multi-line (its layout dropped) unless it
    // already is.
    if let Some(text) = text
        && let Some(t) = ui.widget_mut::<TextControlWidget>(text)
    {
        t.text.text.set_multi_line(true);
    }
    text
}

impl Widget for HudCruiseProgress {
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
    /// `HUD_CruiseProgress::Create`: the label, multi-line; the offset
    /// (0, 0, -1); the two captions; then `Win3D::Create`.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        // The label is made with the window back in its node, so that it
        // finds it as its parent control.
        let mut text = None;
        ui.lend(id, self, |ui| text = create_label(ui, id));
        self.text = text;
        base::set_offset(ui, id, self, Vector::new(0.0, 0.0, -1.0));
        self.charging_caption = caption(ui, CHARGING_IDS);
        self.countdown_caption = caption(ui, COUNTDOWN_IDS);
        ui.base_create(id, parent)
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        base::destroy(ui, id, &mut self.control);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        base::on_delete(ui, id, &mut self.control);
    }
    /// `HUD_CruiseProgress::Update`: shown and active with the cursor drawn
    /// and a player ship, hidden otherwise; then the child walk.
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        if cursor_and_player(ui) {
            base::set_hidden_with_children(ui, id, false);
            if let Some(n) = ui.node_mut(id) {
                n.flags |= ACTIVE;
            }
        } else {
            base::set_hidden_with_children(ui, id, true);
        }
        ui.base_update(id);
    }
    /// `HUD_CruiseProgress::UpdateInactive` (`HUD_Waypoint`'s body): the
    /// show half, then the inactive child walk.
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

/// The element (`HUD_CruiseProgress`'s `HUDItem` face).
#[derive(Debug)]
pub struct HudCruiseProgressItem {
    pub window: NodeId,
}

impl HudItem for HudCruiseProgressItem {
    fn name(&self) -> &[u8] {
        NAME
    }
    fn type_name(&self) -> &'static str {
        "class HUD_CruiseProgress"
    }
    fn window(&self) -> Option<NodeId> {
        Some(self.window)
    }
}

/// `HUDFACTORY<HUD_CruiseProgress>::Create`: the constructor, which calls
/// `Create(0, 0)`, names the window, clears its active bit
/// (`Flags &= 0xfe`) and sets the class name.
pub fn create(_reader: &mut IniReader, ui: &mut Ui) -> Option<Box<dyn HudItem>> {
    let id = ui.alloc(Some(Box::new(HudCruiseProgress::default())));
    ui.with_widget(id, false, |w, ui| {
        w.create(ui, id, None, &mut Payload::None)
    });
    if let Some(n) = ui.node_mut(id) {
        n.set_name(Some(NAME));
        n.flags &= !ACTIVE;
        n.class_name = Some("HUD_CruiseProgress");
    }
    if let Some(w) = ui.widget_mut::<HudCruiseProgress>(id) {
        w.charging = false;
    }
    Some(Box::new(HudCruiseProgressItem { window: id }))
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
        assert_eq!(item.name(), NAME);
        (ui, item.window().unwrap())
    }

    #[test]
    fn the_constructor_makes_the_label_and_leaves_the_window_inactive() {
        let (ui, id) = make(false);
        let node = ui.node(id).unwrap();
        assert_eq!((node.name(), node.flags), (NAME, 0x0a));
        assert_eq!(node.class_name, Some("HUD_CruiseProgress"));
        let w = ui.widget::<HudCruiseProgress>(id).unwrap();
        let text = w.text.expect("the label");
        assert_eq!(ui.children(id), [text]);
        let label = ui.node(text).unwrap();
        assert_eq!(label.name(), b"CruiseProgressText");
        assert_eq!(label.flags, 0x0b);
        assert_eq!(label.position, Vector::new(0.0, 0.0, -1.0));
        let c = ui.win_control(text).unwrap();
        assert_eq!(c.offset, Vector::new(-0.243, -0.263, 0.0));
        assert_eq!(w.control.offset, Vector::new(0.0, 0.0, -1.0));
    }

    #[test]
    fn update_shows_and_activates_the_window_with_a_player_ship() {
        let (mut ui, id) = make(true);
        ui.update_all(0.0);
        assert_eq!(ui.node(id).unwrap().flags, 0x0b);
    }

    #[test]
    fn update_inactive_leaves_the_window_without_a_player_ship() {
        let (mut ui, id) = make(false);
        ui.update_all(0.0);
        assert_eq!(ui.node(id).unwrap().flags, 0x0a);
    }
}
