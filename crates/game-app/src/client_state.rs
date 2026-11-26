//! The client state machine (`client_state.cpp`, `client_state.h`; work
//! package P5.1, record 0009 point 2).
//!
//! The C++ keeps four state objects (`IEnvironment`) as singletons and the
//! current one in `g_client.current_state`. Here [`ClientStates`] owns the
//! four and names the current one by its [`EnvironmentKind`]; a state reaches
//! the rest of the client through the context `C` it is given, and the
//! effects `EnterClientState` has outside the states go through
//! [`StateHost`], which `C` implements.

use neuron_core::diag::{self, DumpFlags};

/// The state ids `EnterClientState` takes (`client_state.h`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ClientStateId {
    /// 1: destroy the current state and quit.
    Quit = 1,
    /// 2: space, `Init(0)`.
    Space = 2,
    /// 3: a base (GF), `Init(1)` when coming from the intro, else `Init(0)`.
    Base = 3,
    /// 4: the base's slot 10, the state kept.
    BaseResume = 4,
    /// 5: space's slot 10, the state kept.
    SpaceResume = 5,
    /// 6: the intro menu (IntroFocus), `Init(0)`.
    Intro = 6,
    /// 7: load the queued character, then a base or space.
    LoadGame = 7,
    /// 8: the mission briefing, `Init(0)`.
    MissionBriefing = 8,
}

impl ClientStateId {
    /// The state of an id; `None` for 0 (stay) and the ids the C++'s
    /// `switch` has no case for, which keep the current state.
    #[must_use]
    pub const fn from_raw(id: i32) -> Option<Self> {
        Some(match id {
            1 => Self::Quit,
            2 => Self::Space,
            3 => Self::Base,
            4 => Self::BaseResume,
            5 => Self::SpaceResume,
            6 => Self::Intro,
            7 => Self::LoadGame,
            8 => Self::MissionBriefing,
            _ => return None,
        })
    }

    /// The id.
    #[must_use]
    pub const fn raw(self) -> i32 {
        self as i32
    }
}

/// `IEnvironment::GetEnvironmentType` (slot 7), which also names the four
/// state objects.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EnvironmentKind {
    /// 0: space (`SpaceApp`).
    Space = 0,
    /// 1: a base (`GF`).
    Gameflow = 1,
    /// 2: the intro menu (`IntroFocus`).
    IntroFocus = 2,
    /// 3: the mission briefing.
    MissionBriefing = 3,
}

/// One state object (`IEnvironment`): the slots the state machine calls.
pub trait Environment<C> {
    /// `GetEnvironmentType` (slot 7).
    fn kind(&self) -> EnvironmentKind;
    /// `Init(param)`: false when the state cannot start.
    fn init(&mut self, cx: &mut C, param: i32) -> bool;
    /// `Destroy`.
    fn destroy(&mut self, cx: &mut C);
    /// Slot 10: states 4 and 5 resume the base or space in place.
    fn resume(&mut self, cx: &mut C);
    /// `Update(dt)` (slot 14): the state's frame, drawn and presented inside
    /// it; the answer is the state id to enter, 0 to stay.
    fn update(&mut self, cx: &mut C, dt: f32) -> i32;
}

/// What `EnterClientState` does outside the state objects.
pub trait StateHost {
    /// `OracleNoteScreen("client", id)` (oracle instrumentation, P0.5): the
    /// port's oracle mode logs every state change (decision O53 (2)).
    fn note_client_screen(&mut self, id: i32);
    /// `RefreshLoadingProgress`.
    fn refresh_loading_progress(&mut self);
    /// `LoadingScreenSetClear(1)` then `LoadingScreenStart()`.
    fn start_loading_screen(&mut self);
    /// `PostQuitMessage(0)`.
    fn post_quit(&mut self);
    /// `Win3DFindByName("NN_Preferences") != nullptr`.
    fn preferences_open(&self) -> bool;
    /// `G_bNnScreenUp = false`.
    fn clear_nn_screen_up(&mut self);
    /// `ShutdownServerConnection`.
    fn shutdown_server_connection(&mut self);
    /// `ConnectAndLogin(g_display.ini_path)`.
    fn connect_and_login(&mut self) -> bool;
    /// `G_LoginMgr.RequestCharacterInfo() != 0`.
    fn request_character_info(&mut self) -> bool;
    /// `G_LoginMgr.SelectCharacterById(id)`.
    fn select_character(&mut self, id: &[u8]) -> bool;
    /// `GetPlayerBaseId`: the player's base, 0 in space.
    fn player_base_id(&self) -> u32;
    /// `G_Server->CharacterSkipAutosave(GetPlayerConnectionId())`.
    fn skip_autosave(&mut self);
    /// `ClientQuitRequested()`.
    fn quit_requested(&self) -> bool;
    /// `SetClientQuitRequested(quit)`.
    fn set_quit_requested(&mut self, quit: bool);
}

/// The four state objects, the current one, and the state queued for the
/// next frame (`G_PendingClientStateId`) with the character state 7 loads
/// (`G_LoadGameCharacterId`).
pub struct ClientStates<C> {
    space: Box<dyn Environment<C>>,
    gameflow: Box<dyn Environment<C>>,
    intro: Box<dyn Environment<C>>,
    briefing: Box<dyn Environment<C>>,
    current: Option<EnvironmentKind>,
    pending: i32,
    load_game_character: Vec<u8>,
}

impl<C: StateHost> ClientStates<C> {
    /// The four states, none current.
    ///
    /// # Panics
    ///
    /// When a state names another kind than its place.
    #[must_use]
    pub fn new(
        space: Box<dyn Environment<C>>,
        gameflow: Box<dyn Environment<C>>,
        intro: Box<dyn Environment<C>>,
        briefing: Box<dyn Environment<C>>,
    ) -> Self {
        assert_eq!(space.kind(), EnvironmentKind::Space);
        assert_eq!(gameflow.kind(), EnvironmentKind::Gameflow);
        assert_eq!(intro.kind(), EnvironmentKind::IntroFocus);
        assert_eq!(briefing.kind(), EnvironmentKind::MissionBriefing);
        Self {
            space,
            gameflow,
            intro,
            briefing,
            current: None,
            pending: 0,
            load_game_character: Vec::new(),
        }
    }

    /// `g_client.current_state`, by kind.
    #[must_use]
    pub fn current(&self) -> Option<EnvironmentKind> {
        self.current
    }

    /// `IsClientIntroEnvironment`: the current state is the intro menu.
    #[must_use]
    pub fn is_intro(&self) -> bool {
        self.current == Some(EnvironmentKind::IntroFocus)
    }

    fn state(&mut self, kind: EnvironmentKind) -> &mut dyn Environment<C> {
        match kind {
            EnvironmentKind::Space => self.space.as_mut(),
            EnvironmentKind::Gameflow => self.gameflow.as_mut(),
            EnvironmentKind::IntroFocus => self.intro.as_mut(),
            EnvironmentKind::MissionBriefing => self.briefing.as_mut(),
        }
    }

    /// `QueueLoadGameCharacter(id, skip_autosave)`: an empty id does
    /// nothing; otherwise the next frame enters state 7 with this character,
    /// and the server is told to skip the autosave when asked.
    pub fn queue_load_game_character(&mut self, cx: &mut C, id: &[u8], skip_autosave: bool) {
        if id.is_empty() {
            return;
        }
        self.pending = ClientStateId::LoadGame.raw();
        self.load_game_character = id.to_vec();
        if skip_autosave {
            cx.skip_autosave();
        }
    }

    /// `G_PendingClientStateId = id`: the next frame enters `id` instead of
    /// the current state's answer (the oracle script's `state`).
    pub fn set_pending(&mut self, id: i32) {
        self.pending = id;
    }

    /// `ConsumePendingClientState`: a queued state replaces `id` and is
    /// cleared.
    pub fn consume_pending(&mut self, id: i32) -> i32 {
        if self.pending != 0 {
            let queued = self.pending;
            self.pending = 0;
            return queued;
        }
        id
    }

    /// The current state's `Update(dt)`, 0 without one
    /// (`TickClientFrame`'s slot 14).
    pub fn update_current(&mut self, cx: &mut C, dt: f32) -> i32 {
        match self.current {
            Some(kind) => self.state(kind).update(cx, dt),
            None => 0,
        }
    }

    /// `CallClientStateUpdateSlot(dt)`: the current state's `Update`, the
    /// answer dropped (the NN prompts' modal loops).
    pub fn call_update_slot(&mut self, cx: &mut C, dt: f32) {
        let _ = self.update_current(cx, dt);
    }

    fn destroy_current(&mut self, cx: &mut C) {
        if let Some(kind) = self.current {
            self.state(kind).destroy(cx);
        }
    }

    /// `EnterClientState(id, current)`: the state `id` names, entered from
    /// the current one, which becomes current (`g_client.current_state`); an
    /// id without a case keeps the current state. The C++'s paths are kept
    /// as they are, among them state 7's failures, which destroy the current
    /// state and leave it current.
    pub fn enter(&mut self, cx: &mut C, id: i32) -> Option<EnvironmentKind> {
        cx.note_client_screen(id);
        cx.refresh_loading_progress();
        let previous = self.current;
        let result = match ClientStateId::from_raw(id) {
            Some(ClientStateId::Quit) => {
                self.destroy_current(cx);
                cx.post_quit();
                None
            }
            Some(ClientStateId::Space) => {
                cx.start_loading_screen();
                self.destroy_current(cx);
                self.init_space(cx)
            }
            Some(ClientStateId::Base) => self.enter_base(cx),
            Some(ClientStateId::BaseResume) => {
                self.gameflow.resume(cx);
                previous
            }
            Some(ClientStateId::SpaceResume) => {
                self.space.resume(cx);
                previous
            }
            Some(ClientStateId::Intro) => {
                self.destroy_current(cx);
                // `EnterMenuState`: the intro's `Init` answer is not read.
                let _ = self.intro.init(cx, 0);
                Some(EnvironmentKind::IntroFocus)
            }
            Some(ClientStateId::LoadGame) => self.enter_load_game(cx),
            Some(ClientStateId::MissionBriefing) => {
                cx.start_loading_screen();
                self.destroy_current(cx);
                self.briefing
                    .init(cx, 0)
                    .then_some(EnvironmentKind::MissionBriefing)
            }
            None => previous,
        };
        cx.refresh_loading_progress();
        self.current = result;
        result
    }

    /// `InitSpaceStateTail`.
    fn init_space(&mut self, cx: &mut C) -> Option<EnvironmentKind> {
        self.space.init(cx, 0).then_some(EnvironmentKind::Space)
    }

    /// `EnterBaseState`.
    fn enter_base(&mut self, cx: &mut C) -> Option<EnvironmentKind> {
        cx.start_loading_screen();
        if !cx.preferences_open() {
            cx.clear_nn_screen_up();
        }
        let mut from_intro = 1;
        if let Some(kind) = self.current {
            if kind != EnvironmentKind::IntroFocus {
                from_intro = 0;
            }
            self.state(kind).destroy(cx);
        }
        self.gameflow
            .init(cx, from_intro)
            .then_some(EnvironmentKind::Gameflow)
    }

    /// `EnterLoadGameState`.
    fn enter_load_game(&mut self, cx: &mut C) -> Option<EnvironmentKind> {
        let previous = self.current;
        cx.start_loading_screen();
        self.destroy_current(cx);
        cx.shutdown_server_connection();
        if !cx.connect_and_login() || !cx.request_character_info() {
            return previous;
        }
        if !cx.select_character(&self.load_game_character) {
            diag::dump(
                DumpFlags::WARNING,
                &format!(
                    "Client\\main.cpp(1789) : *** WARNING: Selection of character id {} failed\n",
                    String::from_utf8_lossy(&self.load_game_character)
                ),
            );
            cx.set_quit_requested(true);
            return previous;
        }
        if cx.player_base_id() & 0x3fff_ffff != 0 {
            return self
                .gameflow
                .init(cx, 1)
                .then_some(EnvironmentKind::Gameflow);
        }
        self.init_space(cx)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct Host {
        log: Vec<String>,
        preferences_open: bool,
        connect: bool,
        character_info: bool,
        select: bool,
        base_id: u32,
        quit: bool,
        init_fails: Vec<EnvironmentKind>,
    }

    impl StateHost for Host {
        fn note_client_screen(&mut self, id: i32) {
            self.log.push(format!("screen {id}"));
        }
        fn refresh_loading_progress(&mut self) {
            self.log.push("progress".into());
        }
        fn start_loading_screen(&mut self) {
            self.log.push("loading".into());
        }
        fn post_quit(&mut self) {
            self.log.push("post quit".into());
        }
        fn preferences_open(&self) -> bool {
            self.preferences_open
        }
        fn clear_nn_screen_up(&mut self) {
            self.log.push("nn screen down".into());
        }
        fn shutdown_server_connection(&mut self) {
            self.log.push("shutdown server".into());
        }
        fn connect_and_login(&mut self) -> bool {
            self.log.push("connect".into());
            self.connect
        }
        fn request_character_info(&mut self) -> bool {
            self.log.push("character info".into());
            self.character_info
        }
        fn select_character(&mut self, id: &[u8]) -> bool {
            self.log
                .push(format!("select {}", String::from_utf8_lossy(id)));
            self.select
        }
        fn player_base_id(&self) -> u32 {
            self.base_id
        }
        fn skip_autosave(&mut self) {
            self.log.push("skip autosave".into());
        }
        fn quit_requested(&self) -> bool {
            self.quit
        }
        fn set_quit_requested(&mut self, quit: bool) {
            self.quit = quit;
            self.log.push(format!("quit {quit}"));
        }
    }

    struct State(EnvironmentKind);

    impl Environment<Host> for State {
        fn kind(&self) -> EnvironmentKind {
            self.0
        }
        fn init(&mut self, cx: &mut Host, param: i32) -> bool {
            cx.log.push(format!("{:?} init {param}", self.0));
            !cx.init_fails.contains(&self.0)
        }
        fn destroy(&mut self, cx: &mut Host) {
            cx.log.push(format!("{:?} destroy", self.0));
        }
        fn resume(&mut self, cx: &mut Host) {
            cx.log.push(format!("{:?} resume", self.0));
        }
        fn update(&mut self, cx: &mut Host, dt: f32) -> i32 {
            cx.log.push(format!("{:?} update {dt}", self.0));
            0
        }
    }

    fn states() -> ClientStates<Host> {
        ClientStates::new(
            Box::new(State(EnvironmentKind::Space)),
            Box::new(State(EnvironmentKind::Gameflow)),
            Box::new(State(EnvironmentKind::IntroFocus)),
            Box::new(State(EnvironmentKind::MissionBriefing)),
        )
    }

    fn entered(states: &mut ClientStates<Host>, host: &mut Host, id: i32) -> Vec<String> {
        host.log.clear();
        states.enter(host, id);
        std::mem::take(&mut host.log)
    }

    #[test]
    fn the_intro_destroys_the_current_state_and_ignores_its_init() {
        let mut s = states();
        let mut h = Host {
            init_fails: vec![EnvironmentKind::IntroFocus],
            ..Host::default()
        };
        assert_eq!(
            entered(&mut s, &mut h, 6),
            ["screen 6", "progress", "IntroFocus init 0", "progress"]
        );
        assert!(s.is_intro());
        assert_eq!(
            entered(&mut s, &mut h, 2),
            [
                "screen 2",
                "progress",
                "loading",
                "IntroFocus destroy",
                "Space init 0",
                "progress"
            ]
        );
        assert_eq!(s.current(), Some(EnvironmentKind::Space));
    }

    #[test]
    fn a_base_from_the_intro_starts_with_one_and_from_space_with_zero() {
        let mut s = states();
        let mut h = Host::default();
        s.enter(&mut h, 6);
        let log = entered(&mut s, &mut h, 3);
        assert_eq!(
            log,
            [
                "screen 3",
                "progress",
                "loading",
                "nn screen down",
                "IntroFocus destroy",
                "Gameflow init 1",
                "progress"
            ]
        );
        s.enter(&mut h, 2);
        h.preferences_open = true;
        let log = entered(&mut s, &mut h, 3);
        assert_eq!(
            log,
            [
                "screen 3",
                "progress",
                "loading",
                "Space destroy",
                "Gameflow init 0",
                "progress"
            ]
        );
    }

    #[test]
    fn a_failed_init_leaves_no_state_and_quit_posts_the_quit() {
        let mut s = states();
        let mut h = Host {
            init_fails: vec![EnvironmentKind::Space],
            ..Host::default()
        };
        s.enter(&mut h, 6);
        assert_eq!(s.enter(&mut h, 2), None);
        s.enter(&mut h, 6);
        assert_eq!(
            entered(&mut s, &mut h, 1),
            [
                "screen 1",
                "progress",
                "IntroFocus destroy",
                "post quit",
                "progress"
            ]
        );
        assert_eq!(s.current(), None);
    }

    #[test]
    fn resumes_and_unknown_ids_keep_the_current_state() {
        let mut s = states();
        let mut h = Host::default();
        s.enter(&mut h, 6);
        assert_eq!(
            entered(&mut s, &mut h, 5),
            ["screen 5", "progress", "Space resume", "progress"]
        );
        assert_eq!(
            entered(&mut s, &mut h, 9),
            ["screen 9", "progress", "progress"]
        );
        assert!(s.is_intro());
    }

    #[test]
    fn a_load_game_reconnects_and_enters_the_base_or_space() {
        let mut s = states();
        let mut h = Host {
            connect: true,
            character_info: true,
            select: true,
            base_id: 0x4000_0000,
            ..Host::default()
        };
        s.enter(&mut h, 6);
        s.queue_load_game_character(&mut h, b"", true);
        assert_eq!(s.consume_pending(0), 0);
        s.queue_load_game_character(&mut h, b"save1", true);
        assert_eq!(h.log.last().map(String::as_str), Some("skip autosave"));
        let id = s.consume_pending(0);
        assert_eq!((id, s.consume_pending(0)), (7, 0));
        // The base id's top two bits are flags: in space.
        assert_eq!(
            entered(&mut s, &mut h, id),
            [
                "screen 7",
                "progress",
                "loading",
                "IntroFocus destroy",
                "shutdown server",
                "connect",
                "character info",
                "select save1",
                "Space init 0",
                "progress"
            ]
        );
        h.base_id = 0x4000_0123;
        s.enter(&mut h, 6);
        s.enter(&mut h, 7);
        assert_eq!(s.current(), Some(EnvironmentKind::Gameflow));
    }

    #[test]
    fn a_failed_load_game_leaves_the_destroyed_state_current() {
        let mut s = states();
        let mut h = Host {
            connect: true,
            character_info: true,
            ..Host::default()
        };
        s.enter(&mut h, 6);
        s.queue_load_game_character(&mut h, b"lost", false);
        let id = s.consume_pending(0);
        let log = entered(&mut s, &mut h, id);
        assert_eq!(
            log,
            [
                "screen 7",
                "progress",
                "loading",
                "IntroFocus destroy",
                "shutdown server",
                "connect",
                "character info",
                "select lost",
                "quit true",
                "progress"
            ]
        );
        assert!(s.is_intro());
        h.connect = false;
        s.enter(&mut h, 7);
        assert!(s.is_intro());
    }
}
