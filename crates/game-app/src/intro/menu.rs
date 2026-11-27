//! The intro menu's state machine (`menu_enter_state.cpp`): `FMenuNavState`,
//! `EnterMenuState` and `MenuStateDriverUpdate`'s transitions and polls.
//!
//! Ported so far: the main menu (state 1, the column of
//! [`super::type_select`]), states 2 and 3 of the single-player Load Game
//! path (3 → 2 → 18), state 18's entry as a hook of the host (the Load Game
//! page of [`super::char_select_single`]), state 10's entry as a hook of the
//! host (the Options page of [`super::options`]), state 1's poll of the
//! column, state 10's and state 18's polls of their pages, the leave of
//! state 10, and the first settled frame's loading-screen step. The other
//! states (the multiplayer server, account and character pages, the new
//! game's 16 and 12, the return from the game) come with their screens;
//! until then their entry leaves the state as it is (`default:` of the
//! switch).
//!
//! The frame's draw (`BeginClientRenderFrame` up to the present) is the
//! host's: the intro scene, the logo and the interface.

use neuron_client::ui::tree::{NodeId, Ui};

use super::char_select_single::PageCharacterSelectSingle;
use super::options::PageOptions;
use super::type_select::{TypeSelect, build_column, make_column};

/// What the menu needs from the client around it.
pub trait MenuHost {
    /// `OracleNoteScreen("menu", state)` (oracle instrumentation, P0.5).
    fn note_menu_screen(&mut self, state: i32);
    /// `RefreshLoadingProgress`.
    fn refresh_loading_progress(&mut self);
    /// `SinglePlayer()`.
    fn single_player(&self) -> bool;
    /// `SetClientMouseCursorDrawFlag(1)`.
    fn show_cursor(&mut self);
    /// The frame's x87 precision is 24 bits (the client's first frames,
    /// D1400).
    fn single_precision(&self) -> bool;
    /// State 18: the Load Game page (`PAGE_CharSelectSingle`) made and
    /// shown; its node, `None` when the page did not load. A host keeps one
    /// page node ([`super::char_select_single::make_page`]) and calls
    /// [`super::char_select_single::enter_load_game_page`] with the roster.
    fn enter_load_game(&mut self, ui: &mut Ui) -> Option<NodeId>;
    /// State 10: the Options page (`PageOptions`) made and shown; its node.
    /// A host keeps one page node ([`super::options::make_page`]) and calls
    /// [`super::options::enter_options_page`] with the preferences window's
    /// context.
    fn enter_options(&mut self, ui: &mut Ui) -> NodeId;
    /// The Load Game page asked to load: the intro base left when the
    /// intro draws its scripts (`GetIntroConfigState()->bDrawScripts`,
    /// `LeaveIntroBase`).
    fn leave_intro_base(&mut self);
    /// The load's loading screen (`LoadingScreenSetClear(1)`,
    /// `LoadingScreenStart`).
    fn start_loading_screen(&mut self);
    /// The first settled frame: the loading screen's progress bar made, its
    /// startup image dropped, its clearing stopped
    /// (`LoadingScreenCreateProgressBar`, `LoadingScreenReleaseImage`,
    /// `LoadingScreenSetClear(0)`).
    fn first_settled_frame(&mut self, ui: &mut Ui);
}

/// `FMenuNavState`: the parts the ported states use.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MenuNav {
    /// `+0x04`.
    pub current: i32,
    /// `+0x08`.
    pub prev: i32,
    /// `+0x228`: the page or column shown (slot 10 on it before each
    /// change).
    pub active_page: Option<NodeId>,
    /// `+0x22c`: the first settled frame has run.
    pub flag_22c: bool,
    /// `+0x22d`: set by the multiplayer path; state 1 then restarts single
    /// player.
    pub flag_22d: bool,
    /// The column (`G_TypeSelect`), made on first use.
    pub column: Option<NodeId>,
    /// `+0xc`: the character the game starts with (the Load Game page's
    /// row data word).
    pub character: i32,
    /// `+0x20c`.
    pub flag_20c: bool,
    /// The Options page (`G_PageOptions`), once state 10 was entered.
    pub options_page: Option<NodeId>,
}

impl MenuNav {
    /// `ReleaseActivePageInternal`: slot 10 (`Destroy`) on the shown page.
    pub fn release_active_page(&mut self, ui: &mut Ui) {
        if let Some(page) = self.active_page.take() {
            ui.destroy(page);
        }
    }

    /// `EnterMenuState(nav, state)`.
    pub fn enter_state(&mut self, ui: &mut Ui, host: &mut dyn MenuHost, state: i32) {
        host.note_menu_screen(state);
        host.refresh_loading_progress();
        match state {
            1 => self.enter_main_menu(ui, host),
            // Single player goes straight to the Load Game page; the
            // multiplayer account path comes with its screens.
            2 => {
                if host.single_player() {
                    self.current = 18;
                }
            }
            3 => self.current = 2,
            10 => {
                self.release_active_page(ui);
                let page = host.enter_options(ui);
                self.options_page = Some(page);
                self.active_page = Some(page);
            }
            18 => {
                self.release_active_page(ui);
                match host.enter_load_game(ui) {
                    Some(page) => self.active_page = Some(page),
                    None => self.current = 1,
                }
            }
            _ => {}
        }
        host.refresh_loading_progress();
    }

    /// `EnterCase1`: the column built again and shown.
    fn enter_main_menu(&mut self, ui: &mut Ui, host: &mut dyn MenuHost) {
        // `Flag22d` (back from multiplayer) restarts single player: comes
        // with the multiplayer screens.
        host.show_cursor();
        self.release_active_page(ui);
        let column = *self.column.get_or_insert_with(|| make_column(ui));
        build_column(ui, column, host.single_precision());
        self.active_page = Some(column);
    }

    /// `MenuStateDriverUpdate`'s state part: leave and enter states until
    /// the state settles, then the state's poll; then (after the host's
    /// draw) the first settled frame's step. Answers the transition code
    /// (0 so far).
    pub fn driver_update(&mut self, ui: &mut Ui, host: &mut dyn MenuHost) -> i32 {
        while self.current != self.prev {
            host.refresh_loading_progress();
            // Leaving 5 comes with its screen. Leaving 10 goes to state 1
            // when `GOptionsChild_67c20c` is missing or inactive; nothing
            // ever sets it, so always.
            if self.prev == 10 {
                self.current = 1;
            }
            host.refresh_loading_progress();
            self.prev = self.current;
            self.enter_state(ui, host, self.current);
            host.refresh_loading_progress();
        }
        // One poll, the settled state's (the C++'s `switch` over its byte
        // table): a state the poll moves to is entered by the next call.
        match self.current {
            1 => {
                if let Some(next) = self
                    .column
                    .and_then(|c| ui.widget::<TypeSelect>(c))
                    .map(|c| c.next_state)
                    && next != 0
                {
                    self.current = next;
                    if next == 0xc {
                        host.refresh_loading_progress();
                    }
                }
            }
            10 => self.poll_options(ui),
            18 => self.poll_load_game(ui, host),
            _ => {}
        }
        if !self.flag_22c {
            host.first_settled_frame(ui);
            self.flag_22c = true;
        }
        0
    }

    /// Poll index 4 (state 10): without a preferences window, state 1; a
    /// closed one is deleted and state 1 follows; else the page's result
    /// becomes the state, cleared once taken.
    fn poll_options(&mut self, ui: &mut Ui) {
        let Some(page) = self.options_page else {
            self.current = 1;
            return;
        };
        let window = ui
            .widget::<PageOptions>(page)
            .and_then(|p| p.preferences)
            .filter(|&w| ui.contains(w));
        let Some(window) = window else {
            self.current = 1;
            if let Some(p) = ui.widget_mut::<PageOptions>(page) {
                p.preferences = None;
                p.result = 0;
            }
            return;
        };
        let closed = ui
            .widget::<crate::nn_preferences::NnPreferences>(window)
            .is_some_and(|w| w.state.closed);
        let Some(p) = ui.widget_mut::<PageOptions>(page) else {
            return;
        };
        if closed {
            p.preferences = None;
            p.result = 0;
            ui.delete_tree(window);
            self.current = 1;
            return;
        }
        if p.result != 0 {
            self.current = p.result;
            p.result = 0;
        }
    }

    /// Poll index 8 (state 18): the page's load request leaves the intro
    /// base and tells the page; its result becomes the state, with its
    /// character (the result is not cleared: the page's next `Create`
    /// clears it).
    fn poll_load_game(&mut self, ui: &mut Ui, host: &mut dyn MenuHost) {
        let Some(page) = self.active_page else { return };
        let Some(p) = ui.widget_mut::<PageCharacterSelectSingle>(page) else {
            return;
        };
        if std::mem::take(&mut p.loading_screen_requested) {
            host.start_loading_screen();
        }
        if p.load_requested {
            host.leave_intro_base();
            p.intro_base_left = true;
        }
        if p.result != 0 {
            self.current = p.result;
            self.character = p.selected_character;
            self.flag_20c = true;
        }
    }
}

#[cfg(test)]
mod tests;
