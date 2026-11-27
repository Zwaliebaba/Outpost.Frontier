//! The intro menu's state machine (`menu_enter_state.cpp`): `FMenuNavState`,
//! `EnterMenuState` and `MenuStateDriverUpdate`'s transitions and polls.
//!
//! Ported so far: the main menu (state 1, the column of
//! [`super::type_select`]), states 2 and 3 of the single-player Load Game
//! path (3 → 2 → 18), state 18's entry as a hook of the host (the Load Game
//! page comes with P5.2), state 1's poll of the column, and the first
//! settled frame's loading-screen step. The other states (the multiplayer
//! server, account and character pages, options, the new game's 16 and 12,
//! the return from the game) come with their screens; until then their
//! entry leaves the state as it is (`default:` of the switch).
//!
//! The frame's draw (`BeginClientRenderFrame` up to the present) is the
//! host's: the intro scene, the logo and the interface.

use neuron_client::ui::tree::{NodeId, Ui};

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
    /// shown; its node, `None` when the page did not load.
    fn enter_load_game(&mut self, ui: &mut Ui) -> Option<NodeId>;
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
            // Leaving 5 and 10 come with their screens.
            host.refresh_loading_progress();
            self.prev = self.current;
            self.enter_state(ui, host, self.current);
            host.refresh_loading_progress();
        }
        if self.current == 1
            && let Some(next) = self
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
        if !self.flag_22c {
            host.first_settled_frame(ui);
            self.flag_22c = true;
        }
        0
    }
}

#[cfg(test)]
mod tests;
