//! The main menu's Options page (`page_options.cpp`, menu state 10):
//! `PageOptions`, a `UIPage::MasterWin` without page data, which makes the
//! preferences window ([`crate::nn_preferences`]) as a root of its own.
//!
//! The menu's poll of state 10 ([`super::menu::MenuNav`]) deletes the
//! window once it is closed (Main Menu's slide out) and goes back to state
//! 1; leaving state 10 always goes to state 1.
//!
//! The page has no `USER_CANCEL` handler (its `Event::Handler` face answers
//! false and the window registers none): the key map's cancel does nothing
//! here ([`PageOptions::on_event`]).

use neuron_client::ui::page::MasterWin;
use neuron_client::ui::tree::{DrawHost, NodeId, Payload, Property, Ui, Widget};

use crate::nn_preferences::{CLASS, NnPreferences, PreferencesContext};

/// The click message of the page's buttons.
pub const MSG_CLICK: u32 = 0x3004;
/// Main Menu's command.
pub const CMD_MAIN_MENU: u32 = 0x300c;

/// `PageOptions`.
#[derive(Default)]
pub struct PageOptions {
    /// The `UIPage::MasterWin` part, without page data.
    pub master: MasterWin,
    /// `+0xb4`: the menu state next, 0 for none.
    pub result: i32,
    /// `+0xb8`: the preferences window.
    pub preferences: Option<NodeId>,
    /// The context the next `Create` gives the window.
    pub context: PreferencesContext,
}

impl PageOptions {
    /// `Slot23`: Main Menu's click (0x3004 with 0x300c) is state 1; 0xf002
    /// is taken.
    pub fn slot23(&mut self, ui: &mut Ui, msg: u32, a: u32) -> bool {
        if msg == MSG_CLICK {
            ui.services.play_or_rewind_ui_sound_slot(0);
            if a != CMD_MAIN_MENU {
                return false;
            }
            self.result = 1;
            return true;
        }
        msg == 0xf002
    }

    /// `OnEvent` (the `Event::Handler` face, slot 0): false for every
    /// event, `USER_CANCEL` (15) too.
    pub fn on_event(&mut self, _msg: u32) -> bool {
        false
    }
}

impl Widget for PageOptions {
    fn as_any(&self) -> &dyn std::any::Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn std::any::Any {
        self
    }

    /// `Create`: `MasterWin::Create` (no page data: its result is not
    /// tested), the result cleared, then the preferences window made as a
    /// root (`WinFactoryCreate("NN_Preferences", "NN_Preferences", 0, 1)`).
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        _payload: &mut Payload<'_>,
    ) -> bool {
        self.master.create(ui, id, parent, &mut Payload::None);
        self.result = 0;
        let window = NnPreferences::from_menu(self.context.clone());
        let made = ui.create(
            Box::new(window),
            Some("NN_Preferences"),
            None,
            &mut Payload::None,
        );
        if let Some(n) = made.and_then(|w| ui.node_mut(w)) {
            n.set_name(Some(CLASS));
        }
        self.preferences = made;
        true
    }

    /// Slot 10: `MasterWin::Destroy`.
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        self.master.destroy(ui, id);
    }

    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        self.master.on_delete(ui, id);
    }

    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        self.master.draw(ui, id, host);
    }

    fn broadcast(&mut self, ui: &mut Ui, _id: NodeId, msg: u32, a: u32, _b: u32) -> bool {
        self.slot23(ui, msg, a)
    }

    fn property(&mut self, _ui: &mut Ui, _id: NodeId, _property: Property<'_>) -> isize {
        0
    }
}

/// The page's node: made once, without a class name, as the reference's
/// static `G_PageOptions`.
pub fn make_page(ui: &mut Ui) -> NodeId {
    ui.alloc(Some(Box::new(PageOptions::default())))
}

/// `EnterMenuState` case 10 after the active page's release: the page's
/// `Create` with the client's context for the window.
pub fn enter_options_page(ui: &mut Ui, page: NodeId, context: PreferencesContext) {
    if let Some(p) = ui.widget_mut::<PageOptions>(page) {
        p.context = context;
    }
    ui.with_widget(page, false, |w, ui| {
        w.create(ui, page, None, &mut Payload::None)
    });
}
