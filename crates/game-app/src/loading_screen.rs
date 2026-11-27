//! The loading screen's interface part (`loading_screen.cpp`): the
//! progress bar window (`LoadingProgressBar`), which `LoadingScreen::
//! CreateProgressBar` makes once, when the intro menu's first frame has
//! settled (`MenuStateDriverUpdate`'s `Flag22c`).
//!
//! The bar shows (and is active) only while the loading screen runs
//! (`G_LoadingScreenStarted`), which [`LoadingScreen::set_started`] hands
//! the window. The screen's own draw, its startup image and its progress
//! steps come with the boot sequence's drawing.

use neuron_client::ui::children::{
    StaticInit, TextInit, create_static_control, create_text_control,
};
use neuron_client::ui::control::{WinControl, base};
use neuron_client::ui::message::MouseButton;
use neuron_client::ui::tree::{ACTIVE, DrawHost, NodeId, Payload, Property, Ui, Widget};
use neuron_core::math::{Matrix, Vector};

/// The window's name and class name.
pub const NAME: &[u8] = b"LoadingProgressBar";
/// `FRONT_loadingbar.cmp`.
pub const BAR_MESH: &[u8] = b"Interface\\Intro\\objects\\FRONT_loadingbar.cmp";
/// The caption's string id ("LOADING").
pub const CAPTION_IDS: u32 = 0x7c;

/// `LoadingProgressBar`.
#[derive(Default)]
pub struct LoadingProgressBar {
    pub control: WinControl,
    /// `+0x330`: advanced by the frame time modulo the period.
    pub phase: f32,
    /// `+0x334`.
    pub fraction: f32,
    /// `+0x338`: 3 s.
    pub period: f32,
    /// `+0x33c`: "LoadingProgressText".
    pub text: Option<NodeId>,
    /// `+0x340`: "LoadingProgressBar", the bar's mesh.
    pub bar: Option<NodeId>,
    /// `G_LoadingScreenStarted`, as the screen last set it.
    pub started: bool,
}

impl LoadingProgressBar {
    /// The constructor: a period of 3 s, inactive.
    #[must_use]
    pub fn new() -> Self {
        Self {
            period: 3.0,
            ..Self::default()
        }
    }

    /// `Update` and `UpdateInactive`'s test: shown and active while the
    /// loading screen runs; `Update` hides it otherwise.
    fn show_while_started(&mut self, ui: &mut Ui, id: NodeId, hide_otherwise: bool) {
        if self.started {
            base::set_hidden_with_children(ui, id, false);
            if let Some(n) = ui.node_mut(id) {
                n.flags |= ACTIVE;
            }
        } else if hide_otherwise {
            base::set_hidden_with_children(ui, id, true);
            if let Some(n) = ui.node_mut(id) {
                n.flags &= !ACTIVE;
            }
        }
    }
}

/// The children `Create` makes, with the window back in its node.
fn create_children(ui: &mut Ui, id: NodeId) -> (Option<NodeId>, Option<NodeId>) {
    let text = create_text_control(
        ui,
        id,
        b"LoadingProgressText",
        &TextInit {
            style: Some(b"PageHeader"),
            position: Vector::new(-0.095, 0.0192, 0.0),
            width: 0.2,
            height: 0.04,
            ids: 0,
            justification: 1,
            tooltip: 0,
            corner_anchor: true,
            ..TextInit::default()
        },
    );
    let bar = create_static_control(
        ui,
        id,
        NAME,
        Some(BAR_MESH),
        &StaticInit {
            offset: Vector::new(0.0, 0.0, -1.0),
            no_wire_render: true,
            ..StaticInit::default()
        },
    );
    (text, bar)
}

impl Widget for LoadingProgressBar {
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
    /// `LoadingProgressBar::Create`: the caption ("PageHeader", centred) and
    /// the bar's mesh, the offset (0, 0, -1), the caption's text (IDS 0x7c),
    /// then `Win3D::Create`.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        if let Some(n) = ui.node_mut(id) {
            n.flags &= !ACTIVE;
        }
        let mut made = (None, None);
        ui.lend(id, self, |ui| made = create_children(ui, id));
        (self.text, self.bar) = made;
        base::set_offset(ui, id, self, Vector::new(0.0, 0.0, -1.0));
        let caption = ui.ids_string(CAPTION_IDS);
        // `GetIdsStringWWithRawFallback(0x7c, buf, 0x20)`: at most 31 units.
        let caption = &caption[..caption.len().min(0x1f)];
        if let Some(text) = self.text {
            ui.property(text, Property::SetText(Some(caption)));
        }
        ui.base_create(id, parent)
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        base::destroy(ui, id, &mut self.control);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        base::on_delete(ui, id, &mut self.control);
    }
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        self.show_while_started(ui, id, true);
        ui.base_update(id);
    }
    fn update_inactive(&mut self, ui: &mut Ui, id: NodeId) {
        self.show_while_started(ui, id, false);
        ui.base_update_inactive(id);
    }
    /// Draws nothing.
    fn draw(&mut self, _ui: &mut Ui, _id: NodeId, _host: &mut dyn DrawHost) {}
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

/// The loading screen (`LoadingScreen`): the parts the interface sees.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LoadingScreen {
    /// `+0x24`: the bar, once made.
    pub progress_bar: Option<NodeId>,
    /// `G_LoadingScreenStarted`.
    pub started: bool,
}

impl LoadingScreen {
    /// `LoadingScreen::CreateProgressBar`: once, the bar made outside the
    /// factory (`new LoadingProgressBar`, `Create(0, 0)`), then its name and
    /// class name set.
    pub fn create_progress_bar(&mut self, ui: &mut Ui) {
        if self.progress_bar.is_some() {
            return;
        }
        let mut bar = LoadingProgressBar::new();
        bar.started = self.started;
        let id = ui.alloc(Some(Box::new(bar)));
        ui.with_widget(id, false, |w, ui| {
            w.create(ui, id, None, &mut Payload::None)
        });
        if let Some(n) = ui.node_mut(id) {
            n.set_name(Some(NAME));
            n.class_name = Some("LoadingProgressBar");
        }
        self.progress_bar = Some(id);
    }

    /// `G_LoadingScreenStarted` set (by the screen's start and stop), and
    /// handed to the bar, which reads it each frame.
    pub fn set_started(&mut self, ui: &mut Ui, started: bool) {
        self.started = started;
        if let Some(bar) = self
            .progress_bar
            .and_then(|id| ui.widget_mut::<LoadingProgressBar>(id))
        {
            bar.started = started;
        }
    }
}
