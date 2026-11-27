//! The client's one owning value (decision O53, record 0009 point 1;
//! work package P5.1): [`ClientContext`], which the frame
//! ([`crate::frame::tick_client_frame`]) and the state machine
//! ([`crate::client_state::ClientStates`]) run over, and the four state
//! objects of `client_state.cpp` over it.
//!
//! The context holds the interface (the engine in `run`, a plain [`Ui`] in
//! tests, behind [`Interface`]) apart from the rest, [`ClientCore`], so a
//! state can hand the interface and the core to the menu side by side. The
//! core holds the session and the client's end of its transport side by
//! side for the same reason, with the client clock, the oracle run, the key
//! map, the loading screen, the intro mode's request and latch, the quit
//! flags and the state the oracle queued.
//!
//! The states:
//!
//! - [`IntroFocus`] (client state 6): `Init`, `Destroy` and `Update` of
//!   `client_state.cpp`'s `IntroFocus`. Not ported: the intro base
//!   (`EnterClientLocationBase` of `Intro1_Base` when the intro draws its
//!   scripts, with its audio mode, fade and script events; D1107), the logo
//!   image (`Nav.LogoImage`) and the fade (D1108); the mission-offer reset
//!   and the login manager's server lists have no state yet.
//! - [`SpaceEnvironment`] (client state 2, `SpaceApp`):
//!   [`crate::space_state::enter_space`], then stream B's
//!   [`crate::space_state::SpaceApp`] with the "player" camera, updated by
//!   [`crate::space_state::update`]; its flight frame's HUD, letterbox and
//!   panel flags stay false for now (D1112).
//! - [`Gameflow`] (client state 3, a base) and [`MissionBriefing`]: stubs
//!   that do not start (P5.9 and P5.10 bring them).
//!
//! The client's first two frames run at the x87's 24 bits (D1400's client
//! side): the context counts the frames ([`ClientCore::frame`]) and answers
//! [`ClientCore::single_precision`] for frames 1 and 2, which the main
//! menu's column reads when it is built and when it is updated.

use neuron_client::engine::Engine;
use neuron_client::render::ff::device::Backend;
use neuron_client::ui::page::PageDesc;
use neuron_client::ui::tree::{NodeId, Ui};
use neuron_client::ui::widescreen::Surface;
use neuron_core::time::Clock;

use game_sim::protocol::to_server::SkipAutosave;
use game_sim::protocol::{ClientMessage, ToServer};

use crate::audio::{Silent, SoundCalls, SoundLayer};
use crate::base::location_controller::LocationController;
use crate::client_state::{ClientStates, Environment, EnvironmentKind, StateHost};
use crate::frame::FrameHost;
use crate::intro::char_select_single::{
    self, PAGE_FILE, Roster, RosterRow, RowDetails, enter_load_game_page,
};
use crate::intro::menu::{MenuHost, MenuNav};
use crate::intro::options;
use crate::intro::type_select::TypeSelect;
use crate::keymap::KeyMap;
use crate::loading_screen::LoadingScreen;
use crate::login::{self, LoginSettings};
use crate::nn_preferences::PreferencesContext;
use crate::oracle::iclient::IClientLog;
use crate::oracle::{ClientHost, OracleRun};
use crate::receiver::{ClientLink, ClientReceiver};
use crate::session::{self, LocalClientEnd, LocalServerRequest, Session, StartLocalServer};
use crate::space_state::{self, SpaceEntry};
use crate::timing::ClientTiming;

/// `IServerImpl::Startup`'s `g_pClientInterface->Startup(1, 0x36d9)`: the
/// players and the port `StartServer` gives.
const STARTUP: (u32, u32) = (1, 0x36d9);

/// The client's first frames that run at the x87's 24 bits (D1400).
const SINGLE_PRECISION_FRAMES: u32 = 2;

/// `Win3DFindByName`'s name of the preferences window.
const PREFERENCES_NAME: &[u8] = b"NN_Preferences";

/// The interface the client draws (the `Win3D` registry): the engine's UI
/// manager, or a plain [`Ui`] in tests.
pub trait Interface {
    /// The UI manager, when the interface has one.
    fn ui(&mut self) -> Option<&mut Ui>;
    /// `f` on the client's sound calls: without an engine, the calls of a
    /// client without sound ([`Silent`]).
    fn with_sound(&mut self, f: &mut dyn FnMut(&mut dyn SoundCalls)) {
        f(&mut Silent::default());
    }
    /// `GetGameOption(index)`: the reference's default without an engine.
    fn game_option(&self, index: u32) -> i32 {
        neuron_client::engine::options::GameOptions::default().get(index)
    }
}

impl Interface for Ui {
    fn ui(&mut self) -> Option<&mut Ui> {
        Some(self)
    }
}

impl<B: Backend + 'static> Interface for Engine<B> {
    fn ui(&mut self) -> Option<&mut Ui> {
        self.ui_mut()
    }
    fn with_sound(&mut self, f: &mut dyn FnMut(&mut dyn SoundCalls)) {
        self.with_client_sound(|sound, env| f(&mut SoundLayer { sound, env }));
    }
    fn game_option(&self, index: u32) -> i32 {
        self.options.get(index)
    }
}

/// `kGameOptInverseMouse`.
const OPT_INVERSE_MOUSE: u32 = 0x17;

/// Everything of the client but its interface.
pub struct ClientCore<'a, C: Clock> {
    /// The client clock (`client_timing.cpp`), with the global time.
    pub clock: ClientTiming<C>,
    /// `G_Server`: the session, while the client has a server.
    pub session: Option<Box<dyn Session>>,
    /// The client's end of the session's transport and its receiver
    /// (`G_LoginMgr`'s and the event interface's state), beside the session.
    pub link: Option<ClientLink<LocalClientEnd>>,
    /// The oracle mode's run (decision O53 (2)).
    pub oracle: Option<OracleRun>,
    /// `G_KeyMap`.
    pub keymap: KeyMap,
    /// The location controller's maps and state (`menu_universe_base.cpp`,
    /// P5.9).
    pub location: LocationController,
    /// The loading screen.
    pub loading_screen: LoadingScreen,
    /// What the preferences window reads of the client (its key map is the
    /// client's, copied when the window opens).
    pub preferences: PreferencesContext,
    /// `GIntroModeRequest`.
    pub intro_mode_request: i32,
    /// `GIntroModeLatch`.
    pub intro_mode_latch: i32,
    /// `ClientQuitRequested()`.
    pub quit_requested: bool,
    /// `PostQuitMessage(0)` was called: the frame loop ends.
    pub quit_posted: bool,
    /// The state the oracle's script queued (`G_PendingClientStateId`).
    pub pending_state: Option<u32>,
    /// `TickClientFrame` calls so far (the oracle's frame number).
    pub frame: u32,
    /// The frame's time (`TickClientFrame`'s `DeltaSeconds`, which is
    /// `GetClientFrameTimeCapped()`, the oracle's fixed step included).
    pub frame_dt: f64,
    /// `GetClientMouseButtons` (bit n for button n held), as the frame's
    /// mouse update left it.
    pub mouse_buttons: u32,
    /// `!SinglePlayer()`.
    pub multiplayer: bool,
    /// `G_bNnScreenUp`.
    pub nn_screen_up: bool,
    /// `SetClientMouseCursorDrawFlag(1)` was called.
    pub cursor_drawn: bool,
    /// `Win3DFindByName("NN_Preferences") != nullptr` as the last interface
    /// update left it (the state machine asks without the interface).
    preferences_window: bool,
    /// The intro's column, which the frame's interface update reads the
    /// precision into.
    menu_column: Option<NodeId>,
    /// The Load Game page (`G_PageCharacterSelectSingle`), once made.
    load_game_page: Option<NodeId>,
    /// The Options page (`G_PageOptions`), once made.
    options_page: Option<NodeId>,
    /// The menu asked for the loading screen; the interface sees it after
    /// the menu's driver.
    loading_requested: bool,
    /// The server's identity: a new number for every server started.
    server_count: u64,
    /// The IClient log between two servers.
    iclient_log: Option<IClientLog>,
    start_server: StartLocalServer<'a>,
    request: LocalServerRequest,
    now: Box<dyn FnMut() -> f64 + 'a>,
}

/// The client's state (record 0009 point 1): the interface and the rest.
pub struct ClientContext<'a, I, C: Clock> {
    /// The interface.
    pub interface: I,
    /// The rest.
    pub core: ClientCore<'a, C>,
}

impl<'a, I: Interface, C: Clock> ClientContext<'a, I, C> {
    /// The context before `EnterClientState`: no server yet; `start_server`
    /// starts the in-process server with `request` when the intro connects,
    /// and `now` is the clock the login's waits read, in seconds.
    pub fn new(
        interface: I,
        clock: ClientTiming<C>,
        oracle: Option<OracleRun>,
        start_server: StartLocalServer<'a>,
        request: LocalServerRequest,
        now: Box<dyn FnMut() -> f64 + 'a>,
    ) -> Self {
        Self {
            interface,
            core: ClientCore {
                clock,
                session: None,
                link: None,
                oracle,
                keymap: KeyMap::new(),
                location: LocationController::new(),
                loading_screen: LoadingScreen::default(),
                preferences: PreferencesContext::default(),
                intro_mode_request: 0,
                intro_mode_latch: 0,
                quit_requested: false,
                quit_posted: false,
                pending_state: None,
                frame: 0,
                frame_dt: 0.0,
                mouse_buttons: 0,
                multiplayer: false,
                nn_screen_up: false,
                cursor_drawn: false,
                preferences_window: false,
                menu_column: None,
                load_game_page: None,
                options_page: None,
                loading_requested: false,
                server_count: 0,
                iclient_log: None,
                start_server,
                request,
                now,
            },
        }
    }
}

impl<C: Clock> ClientCore<'_, C> {
    /// The frame's x87 precision is 24 bits: the client's frames 1 and 2
    /// (D1400).
    #[must_use]
    pub fn single_precision(&self) -> bool {
        (1..=SINGLE_PRECISION_FRAMES).contains(&self.frame)
    }

    /// The IClient log's lines so far (`iclient.jsonl`).
    #[must_use]
    pub fn iclient_lines(&self) -> Vec<String> {
        self.link
            .as_ref()
            .and_then(|l| l.receiver.log.as_ref())
            .or(self.iclient_log.as_ref())
            .map(|log| log.lines().to_vec())
            .unwrap_or_default()
    }

    /// `ShutdownServerConnection`: unless an intro mode is requested, the
    /// server shut down (its `Shutdown` calls the client's) and dropped.
    /// The login manager's `Logout` asks the server to drop the connection,
    /// which the dropped session does.
    pub fn shutdown_server_connection(&mut self) {
        if self.intro_mode_request != 0 {
            return;
        }
        if let Some(mut link) = self.link.take() {
            link.receiver.shutdown();
            self.iclient_log = link.receiver.log.take();
        }
        self.session = None;
    }

    /// `ConnectToServer` in single player: `StartServer` (the connection
    /// shut down, the in-process server started, whose start-up calls the
    /// client's `Startup`), then `CLoginMgr::Connect(true)`; the connection
    /// shut down again when either fails. The error boxes of a failed
    /// connect are not shown (D1109).
    fn connect_to_server(&mut self) -> bool {
        self.shutdown_server_connection();
        self.multiplayer = false;
        let Some((mut session, client)) = (self.start_server)(&self.request) else {
            self.shutdown_server_connection();
            return false;
        };
        self.server_count += 1;
        let mut receiver = ClientReceiver::new(false);
        // `InitGameflowData`'s `BuildClientLocationMaps` (gameflow.cpp:504
        // `GF::PreInit`, gameflow.cpp:670), which the reference runs once at
        // start-up over the shared game data. The client's data is the
        // in-process server's, so the port builds the maps here, into the new
        // receiver's mirrors (D1702).
        let data = session.world().data_arc();
        self.location
            .build_maps(&data.bases, &data.goods, &mut receiver.location);
        receiver.log = self.iclient_log.take().or_else(|| {
            self.oracle.as_ref().map(|_| {
                let mut log = IClientLog::new();
                log.set_frame(self.frame);
                log
            })
        });
        receiver.startup(STARTUP.0, STARTUP.1);
        self.session = Some(session);
        let link = self.link.insert(ClientLink::new(client, receiver));
        if login::connect(link, self.session.as_deref_mut(), &mut *self.now) == 1 {
            return true;
        }
        self.shutdown_server_connection();
        false
    }

    /// `LoginToServer` in single player; the connection shut down when it
    /// fails (its error boxes are not shown, D1109).
    fn login_to_server(&mut self) -> bool {
        let Some(link) = self.link.as_mut() else {
            return false;
        };
        let settings = LoginSettings::single_player(1.0);
        if login::login_to_server(link, self.session.as_deref_mut(), &settings, &mut *self.now) {
            return true;
        }
        self.shutdown_server_connection();
        false
    }

    /// `G_LoginMgr.RequestCharacterInfo()`: 0 without a server.
    fn character_info(&mut self) -> i32 {
        match self.link.as_mut() {
            Some(link) => {
                login::request_character_info(link, self.session.as_deref_mut(), &mut *self.now)
            }
            None => 0,
        }
    }

    /// `CLoginMgr::Reset`'s flags.
    fn reset_login(&mut self) {
        if let Some(link) = self.link.as_mut() {
            link.receiver.login.login_response = false;
            link.receiver.login.character_info_arrived = false;
        }
    }

    /// `MenuNavStartSinglePlayer`: single player, the connection and the
    /// login, the login manager reset, then the character list. The chat
    /// window and the new game's factions (`ReloadFactions`) have no state
    /// yet.
    fn start_single_player(&mut self, nav: &mut MenuNav) -> bool {
        self.multiplayer = false;
        nav.flag_22d = false;
        if !self.connect_to_server() || !self.login_to_server() {
            return false;
        }
        self.reset_login();
        self.character_info() != 0
    }

    /// The roster the Load Game page shows: the character list with the
    /// status `RequestCharacterInfo` answered. A record's text is its
    /// description or its `DescripStrId` (`NnLoadSaveEntryText`); its time
    /// and its ship and system names are not given yet (D1110).
    fn roster(&self, ui: &Ui, status: i32) -> Roster {
        let records = self
            .link
            .as_ref()
            .and_then(|l| l.receiver.login.characters.as_ref())
            .map(|c| c.records.as_slice())
            .unwrap_or_default();
        let rows = records
            .iter()
            .zip(1..)
            .map(|(r, data)| RosterRow {
                name: if r.descrip_str_id != 0 {
                    ui.catalogs
                        .as_ref()
                        .map(|c| c.string(r.descrip_str_id).to_vec())
                        .unwrap_or_default()
                } else {
                    r.description.clone()
                },
                time: Vec::new(),
                data,
                is_restart: r.is_restart != 0,
                details: RowDetails {
                    money: r.money.cast_signed(),
                    rank: r.rank.cast_signed(),
                    ship_name: None,
                    location_name: None,
                },
            })
            .collect();
        Roster { status, rows }
    }

    /// The loading screen's start, which the menu asked for, handed to the
    /// interface.
    fn sync_loading_screen(&mut self, ui: &mut Ui) {
        if std::mem::take(&mut self.loading_requested) {
            let started = self.loading_screen.started;
            self.loading_screen.set_started(ui, started);
        }
    }
}

impl<C: Clock> MenuHost for ClientCore<'_, C> {
    fn note_menu_screen(&mut self, state: i32) {
        if let Some(run) = self.oracle.as_mut() {
            run.script.note_screen("menu", state.cast_unsigned());
        }
    }
    /// The loading screen's progress has nothing to show between loads.
    fn refresh_loading_progress(&mut self) {}
    fn single_player(&self) -> bool {
        !self.multiplayer
    }
    fn show_cursor(&mut self) {
        self.cursor_drawn = true;
    }
    fn single_precision(&self) -> bool {
        ClientCore::single_precision(self)
    }
    /// State 18: the page's `Create` asks for the character list before it
    /// fills its lists, so the list is asked for when the page file loads.
    fn enter_load_game(&mut self, ui: &mut Ui) -> Option<NodeId> {
        let page = *self
            .load_game_page
            .get_or_insert_with(|| char_select_single::make_page(ui));
        let loads = ui
            .services
            .data_file(PAGE_FILE)
            .is_some_and(|bytes| PageDesc::load(&bytes).ok);
        let status = if loads { self.character_info() } else { 0 };
        let roster = self.roster(ui, status);
        enter_load_game_page(ui, page, roster).then_some(page)
    }
    fn enter_options(&mut self, ui: &mut Ui) -> NodeId {
        let page = *self
            .options_page
            .get_or_insert_with(|| options::make_page(ui));
        let mut context = self.preferences.clone();
        context.keymap = self.keymap.clone();
        context.single_player = !self.multiplayer;
        options::enter_options_page(ui, page, context);
        page
    }
    /// The intro base is not entered (D1107), so there is none to leave.
    fn leave_intro_base(&mut self) {}
    fn start_loading_screen(&mut self) {
        self.loading_screen.started = true;
        self.loading_requested = true;
    }
    /// `LoadingScreenCreateProgressBar`; the startup image and the clear
    /// flag have no state yet.
    fn first_settled_frame(&mut self, ui: &mut Ui) {
        self.loading_screen.create_progress_bar(ui);
    }
}

impl<I: Interface, C: Clock> StateHost for ClientContext<'_, I, C> {
    fn note_client_screen(&mut self, id: i32) {
        if let Some(run) = self.core.oracle.as_mut() {
            run.script.note_screen("client", id.cast_unsigned());
        }
    }
    /// The loading screen's progress has nothing to show between loads.
    fn refresh_loading_progress(&mut self) {}
    fn start_loading_screen(&mut self) {
        self.core.loading_screen.started = true;
        if let Some(ui) = self.interface.ui() {
            self.core.loading_screen.set_started(ui, true);
        }
    }
    fn post_quit(&mut self) {
        self.core.quit_posted = true;
    }
    fn preferences_open(&self) -> bool {
        self.core.preferences_window
    }
    fn clear_nn_screen_up(&mut self) {
        self.core.nn_screen_up = false;
    }
    fn shutdown_server_connection(&mut self) {
        self.core.shutdown_server_connection();
    }
    fn connect_and_login(&mut self) -> bool {
        self.core.connect_to_server() && self.core.login_to_server()
    }
    fn request_character_info(&mut self) -> bool {
        self.core.character_info() != 0
    }
    fn select_character(&mut self, id: &[u8]) -> bool {
        let core = &mut self.core;
        match core.link.as_mut() {
            Some(link) => {
                login::select_character_by_id(link, core.session.as_deref_mut(), id, &mut *core.now)
            }
            None => false,
        }
    }
    fn player_base_id(&self) -> u32 {
        self.core
            .link
            .as_ref()
            .and_then(|l| l.receiver.login.selected.as_ref())
            .map_or(0, |r| r.base_id)
    }
    fn skip_autosave(&mut self) {
        if let Some(link) = self.core.link.as_mut() {
            link.send(ToServer::Client(ClientMessage::SkipAutosave(
                SkipAutosave {},
            )));
        }
    }
    fn quit_requested(&self) -> bool {
        self.core.quit_requested
    }
    fn set_quit_requested(&mut self, quit: bool) {
        self.core.quit_requested = quit;
    }
}

impl<I: Interface, C: Clock> FrameHost for ClientContext<'_, I, C> {
    /// The frame counted (D1400's frames), then the oracle's dumps and
    /// script, which see the interface; the IClient log takes the frame.
    fn oracle_frame_tick(&mut self) {
        let core = &mut self.core;
        core.frame += 1;
        let Some(run) = core.oracle.as_mut() else {
            return;
        };
        run.script.frame_tick(&mut ClientHost {
            ui: self.interface.ui(),
            pending_state: &mut core.pending_state,
        });
        let frame = run.script.frame();
        if let Some(log) = core.link.as_mut().and_then(|l| l.receiver.log.as_mut()) {
            log.set_frame(frame);
        }
    }
    fn take_pending_state(&mut self) -> Option<i32> {
        self.core.pending_state.take().map(u32::cast_signed)
    }
    fn skip_local_server_tick(&self) -> bool {
        self.core
            .clock
            .skip_local_server_tick(!self.core.multiplayer)
    }
    fn update_global_time(&mut self, dt: f64) {
        self.core.frame_dt = dt;
        self.core.clock.update_global_time(dt);
    }
    fn single_player(&self) -> bool {
        !self.core.multiplayer
    }
    fn server(&self) -> Option<u64> {
        self.core.session.as_ref().map(|_| self.core.server_count)
    }
    fn server_disconnected(&self) -> bool {
        self.core
            .link
            .as_ref()
            .is_some_and(|l| l.receiver.login.server_disconnected)
    }
    fn server_connected(&self) -> bool {
        self.core
            .link
            .as_ref()
            .is_some_and(|l| l.receiver.login.server_connected)
    }
    fn clear_server_flags(&mut self) {
        if let Some(link) = self.core.link.as_mut() {
            link.receiver.login.server_disconnected = false;
            link.receiver.login.server_connected = false;
        }
    }
    /// Multiplayer only (P6.1): the box is not shown.
    fn run_error_modal(&mut self, _ids: u32, _update_server: bool) {}
    /// The server list (state 5) comes with multiplayer.
    fn server_select_active(&self) -> bool {
        false
    }
    fn set_intro_mode_request(&mut self, mode: i32) {
        self.core.intro_mode_request = mode;
    }
    /// `Win3DUpdateAll(dt)`, with the frame's precision handed to the
    /// intro's column first.
    fn update_interface(&mut self, dt: f64) {
        let single = self.core.single_precision();
        let Some(ui) = self.interface.ui() else {
            return;
        };
        if let Some(column) = self
            .core
            .menu_column
            .and_then(|c| ui.widget_mut::<TypeSelect>(c))
        {
            column.single_precision = single;
        }
        ui.update_all(dt);
        self.core.preferences_window = ui.find_by_name(PREFERENCES_NAME).is_some();
    }
    fn tick_local_server(&mut self, dt: f32) {
        let skip = self.skip_local_server_tick();
        let core = &mut self.core;
        let Some(link) = core.link.as_mut() else {
            return;
        };
        session::tick_local_server(core.session.as_deref_mut(), skip, dt, |s| {
            link.deliver_all(Some(s.world()));
        });
    }
}

/// `IntroFocus` (client state 6): the intro menu and its state machine.
#[derive(Debug, Default)]
pub struct IntroFocus {
    /// `FMenuNavState`.
    pub nav: MenuNav,
}

impl<I: Interface, C: Clock> Environment<ClientContext<'_, I, C>> for IntroFocus {
    fn kind(&self) -> EnvironmentKind {
        EnvironmentKind::IntroFocus
    }

    /// `IntroFocus::Init`: the connection shut down, the login manager
    /// reset, the intro mode's request latched; latch 2 or 4 goes back to
    /// the game's return (state 0x13), 3 to the server list (5), 0 and 1 to
    /// the column (1), and every latch but 2, 3 and 4 starts single player,
    /// state 0xd when that fails. Then the cursor. The logo (D1108) and the
    /// intro base (D1107) are not ported.
    fn init(&mut self, cx: &mut ClientContext<'_, I, C>, _param: i32) -> bool {
        let core = &mut cx.core;
        core.shutdown_server_connection();
        core.reset_login();
        self.nav.flag_20c = false;
        self.nav.prev = 0;
        core.intro_mode_latch = std::mem::take(&mut core.intro_mode_request);
        match core.intro_mode_latch {
            2 | 4 => self.nav.current = 0x13,
            3 => self.nav.current = 5,
            latch => {
                if latch == 0 || latch == 1 {
                    self.nav.current = 1;
                }
                if !core.start_single_player(&mut self.nav) {
                    self.nav.current = 0xd;
                }
            }
        }
        self.nav.active_page = None;
        core.cursor_drawn = true;
        true
    }

    /// `IntroFocus::Destroy`: the shown page released, then the column and
    /// the pages destroyed.
    fn destroy(&mut self, cx: &mut ClientContext<'_, I, C>) {
        let Some(ui) = cx.interface.ui() else {
            return;
        };
        self.nav.release_active_page(ui);
        let pages = [
            self.nav.column,
            cx.core.load_game_page,
            cx.core.options_page,
        ];
        for page in pages.into_iter().flatten() {
            ui.destroy(page);
        }
    }

    /// Slot 10 is a bare `RET`.
    fn resume(&mut self, _cx: &mut ClientContext<'_, I, C>) {}

    /// `MenuStateDriverUpdate`; the menu's draw is the engine's frame after
    /// it.
    fn update(&mut self, cx: &mut ClientContext<'_, I, C>, _dt: f32) -> i32 {
        let ClientContext { interface, core } = cx;
        let Some(ui) = interface.ui() else {
            return 0;
        };
        let code = self.nav.driver_update(ui, core);
        core.menu_column = self.nav.column;
        core.sync_loading_screen(ui);
        code
    }
}

/// Client state 2 (`SpaceApp`): `InitSpaceState`'s dialogue with the server,
/// then the space controller (`space_state::SpaceApp`, stream B's) and its
/// frame (`space_state::update`, `UpdateSpaceState`).
#[derive(Default)]
pub struct SpaceEnvironment {
    /// The controller, made when the ship has launched.
    pub app: Option<space_state::SpaceApp>,
}

/// The controller once the ship launched: in `system`, for the surface,
/// with the "player" camera installed (`InitSpaceState` switches to it when
/// no camera is active, which a new controller never has).
fn launched_app(surface: Surface, system: u32) -> space_state::SpaceApp {
    let mut app = space_state::SpaceApp::new(surface, system);
    let _ = app.cameras.switch_mode(b"player");
    app
}

/// The frame's values the player's mouse flight reads (`FlightFrame`):
/// the interface's hover, cursor and surface, the mouse buttons, the capped
/// frame time (zero while paused; the caller adds the inverse-mouse
/// option). The HUD's turret and rear views (P5.7), the letterbox (P5.4)
/// and the NeuroNet main button's panel (P5.10) stay false until those
/// packages (D1112).
fn flight_frame<C: Clock>(
    ui: Option<&mut Ui>,
    core: &ClientCore<'_, C>,
) -> crate::player_behavior::FlightFrame {
    let mut frame = crate::player_behavior::FlightFrame {
        mouse_buttons: core.mouse_buttons,
        frame_time: if core.clock.paused() {
            0.0
        } else {
            core.frame_dt
        },
        ..crate::player_behavior::FlightFrame::default()
    };
    if let Some(ui) = ui {
        frame.win3d_hover = ui.hover().is_some();
        frame.cursor = ui.cursor;
        let s = ui.camera.surface;
        frame.surface = (s.width, s.height);
    }
    frame
}

impl<I: Interface, C: Clock> Environment<ClientContext<'_, I, C>> for SpaceEnvironment {
    fn kind(&self) -> EnvironmentKind {
        EnvironmentKind::Space
    }

    /// `InitSpaceState`: the dialogue with the server, true once the ship
    /// launched (no answer in single player asks the client to quit); then
    /// the controller for the player's system with the "player" camera
    /// installed, as `InitSpaceState` installs it when none is.
    fn init(&mut self, cx: &mut ClientContext<'_, I, C>, _param: i32) -> bool {
        let core = &mut cx.core;
        let Some(link) = core.link.as_mut() else {
            return false;
        };
        let entry = space_state::enter_space(link, core.session.as_deref_mut(), &mut *core.now);
        if entry == SpaceEntry::Quit {
            core.quit_requested = true;
        }
        if entry != SpaceEntry::Entered {
            return false;
        }
        let system = space_state::player_system(link);
        let surface = cx
            .interface
            .ui()
            .map_or_else(Surface::default, |ui| ui.camera.surface);
        self.app = Some(launched_app(surface, system));
        true
    }
    fn destroy(&mut self, _cx: &mut ClientContext<'_, I, C>) {
        self.app = None;
    }
    fn resume(&mut self, _cx: &mut ClientContext<'_, I, C>) {}
    /// `UpdateSpaceState`: 0 to stay, 6 to leave space.
    fn update(&mut self, cx: &mut ClientContext<'_, I, C>, dt: f32) -> i32 {
        let skip = cx.core.clock.skip_local_server_tick(!cx.core.multiplayer);
        let inverse_mouse = cx.interface.game_option(OPT_INVERSE_MOUSE) != 0;
        let mut frame = flight_frame(cx.interface.ui(), &cx.core);
        frame.inverse_mouse = inverse_mouse;
        let (interface, core) = (&mut cx.interface, &mut cx.core);
        let (Some(app), Some(link)) = (self.app.as_mut(), core.link.as_mut()) else {
            return 6;
        };
        // StarSysUpdate's music on the sound layer: whether the background
        // music finished (`BindClientBackgroundMusic`, `IsFinished`) before
        // the frame, and the machine's change (`SetClientBackgroundMusic`)
        // after it.
        interface.with_sound(&mut |s| {
            app.background_music_finished = s.background_music_finished();
        });
        let next = space_state::update(app, link, core.session.as_deref_mut(), &frame, skip, dt);
        if let Some(change) = app.music_change.take() {
            interface.with_sound(&mut |s| {
                let _ =
                    s.set_background_music(change.track, u32::from(change.no_fade), change.fade);
            });
        }
        next
    }
}

/// Client state 3 (`GF`, a base): a stub that does not start; the bases are
/// P5.9's.
#[derive(Debug, Default)]
pub struct Gameflow;

impl<I: Interface, C: Clock> Environment<ClientContext<'_, I, C>> for Gameflow {
    fn kind(&self) -> EnvironmentKind {
        EnvironmentKind::Gameflow
    }
    fn init(&mut self, _cx: &mut ClientContext<'_, I, C>, _param: i32) -> bool {
        false
    }
    fn destroy(&mut self, _cx: &mut ClientContext<'_, I, C>) {}
    fn resume(&mut self, _cx: &mut ClientContext<'_, I, C>) {}
    fn update(&mut self, _cx: &mut ClientContext<'_, I, C>, _dt: f32) -> i32 {
        0
    }
}

/// Client state 8 (the mission briefing): a stub that does not start; the
/// briefing comes with the NeuroNet screens (P5.10).
#[derive(Debug, Default)]
pub struct MissionBriefing;

impl<I: Interface, C: Clock> Environment<ClientContext<'_, I, C>> for MissionBriefing {
    fn kind(&self) -> EnvironmentKind {
        EnvironmentKind::MissionBriefing
    }
    fn init(&mut self, _cx: &mut ClientContext<'_, I, C>, _param: i32) -> bool {
        false
    }
    fn destroy(&mut self, _cx: &mut ClientContext<'_, I, C>) {}
    fn resume(&mut self, _cx: &mut ClientContext<'_, I, C>) {}
    fn update(&mut self, _cx: &mut ClientContext<'_, I, C>, _dt: f32) -> i32 {
        0
    }
}

/// The four state objects over the context, none current.
#[must_use]
pub fn client_states<'a, I: Interface + 'a, C: Clock + 'a>() -> ClientStates<ClientContext<'a, I, C>>
{
    ClientStates::new(
        Box::new(SpaceEnvironment::default()),
        Box::new(Gameflow),
        Box::new(IntroFocus::default()),
        Box::new(MissionBriefing),
    )
}

#[cfg(test)]
mod tests;
