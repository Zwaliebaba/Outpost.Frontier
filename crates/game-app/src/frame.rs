//! The client's frame (`TickClientFrame`, `client_frame_tick.cpp:188-293`;
//! work package P5.1, record 0009).
//!
//! The frame runs, in the C++ order: the oracle's tick, the global time, the
//! multiplayer disconnect check, the interface update of every registered
//! window root, the local server ([`crate::session::tick_local_server`]),
//! and the state machine. Each state draws and presents inside its own
//! update. What the frame does outside the state machine goes through
//! [`FrameHost`].

use crate::client_state::{ClientStateId, ClientStates, EnvironmentKind, StateHost};

/// The text id of the "connection lost" box (`NnErrorRunModal(0x737, ...)`).
pub const CONNECTION_LOST_IDS: u32 = 0x737;

/// `MenuNavSetIntroModeRequest(3)`: back to the server list.
pub const INTRO_MODE_SERVER_SELECT: i32 = 3;

/// What `TickClientFrame` does outside the state machine.
pub trait FrameHost: StateHost {
    /// `OracleFrameTick` (oracle instrumentation, P0.5): the port's oracle
    /// mode's dumps and script (decision O53 (2)).
    fn oracle_frame_tick(&mut self);
    /// `ShouldSkipLocalServerTick`: the single-player server tick is paused.
    fn skip_local_server_tick(&self) -> bool;
    /// `Timing::UpdateGlobalTime(dt)`.
    fn update_global_time(&mut self, dt: f64);
    /// `SinglePlayer()`.
    fn single_player(&self) -> bool;
    /// `G_Server`'s identity: `None` without a server; a new value whenever
    /// the server changes.
    fn server(&self) -> Option<u64>;
    /// `G_bServerDisconnected`: the multiplayer server dropped the client.
    fn server_disconnected(&self) -> bool;
    /// `G_bServerConnected`: the connect response had arrived.
    fn server_connected(&self) -> bool;
    /// Both flags cleared.
    fn clear_server_flags(&mut self);
    /// `NnErrorRunModal(ids, update_server, nullptr)`: the error box and its
    /// modal frames until it closes.
    fn run_error_modal(&mut self, ids: u32, update_server: bool);
    /// `MenuNavIsServerSelectActive`.
    fn server_select_active(&self) -> bool;
    /// `MenuNavSetIntroModeRequest(mode)`.
    fn set_intro_mode_request(&mut self, mode: i32);
    /// `Win3DUpdateAll(dt)`: the update of every registered window root.
    fn update_interface(&mut self, dt: f64);
    /// `TickLocalServer((float)dt)`, ordinarily
    /// [`crate::session::tick_local_server`] over the client's session.
    fn tick_local_server(&mut self, dt: f32);
}

/// The frame's own memory: the server of the previous frame
/// (`G_FrameLastServer`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FrameState {
    last_server: Option<u64>,
}

/// One client frame (`TickClientFrame(dt)`).
pub fn tick_client_frame<C: FrameHost>(
    frame: &mut FrameState,
    states: &mut ClientStates<C>,
    cx: &mut C,
    dt: f64,
) {
    cx.oracle_frame_tick();
    if !cx.skip_local_server_tick() {
        cx.update_global_time(dt);
    }

    let server = cx.server();
    let server_changed = frame.last_server != server;
    frame.last_server = server;

    if !cx.quit_requested() && !cx.single_player() && cx.server_disconnected() {
        if cx.server_connected() {
            cx.run_error_modal(CONNECTION_LOST_IDS, false);
        }
        cx.clear_server_flags();
        cx.shutdown_server_connection();
        if !cx.server_select_active() {
            cx.set_intro_mode_request(INTRO_MODE_SERVER_SELECT);
            cx.set_quit_requested(true);
        }
    }

    cx.update_interface(dt);
    // `(float)dt`: the C++ narrows the frame time for the server and the
    // states.
    #[allow(clippy::cast_possible_truncation)]
    let dt32 = dt as f32;
    cx.tick_local_server(dt32);

    let mut next = 0;
    let mut force_intro = false;
    if server_changed
        && cx.server().is_none()
        && states.current() != Some(EnvironmentKind::IntroFocus)
    {
        if !cx.single_player() {
            cx.set_intro_mode_request(INTRO_MODE_SERVER_SELECT);
        }
        force_intro = true;
    } else {
        if states.current().is_some() {
            if cx.quit_requested() {
                force_intro = true;
            } else {
                next = states.update_current(cx, dt32);
            }
        }
        if !force_intro && cx.quit_requested() {
            force_intro = true;
        }
    }
    if force_intro {
        next = ClientStateId::Intro.raw();
        cx.set_quit_requested(false);
    }
    let next = states.consume_pending(next);
    if next != 0 {
        states.enter(cx, next);
    }
    if states.current().is_none() {
        cx.post_quit();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::client_state::Environment;

    #[derive(Default)]
    struct Host {
        log: Vec<String>,
        single_player: bool,
        server: Option<u64>,
        disconnected: bool,
        connected: bool,
        quit: bool,
        server_select: bool,
        skip: bool,
        answer: i32,
    }

    impl StateHost for Host {
        fn note_client_screen(&mut self, id: i32) {
            self.log.push(format!("enter {id}"));
        }
        fn refresh_loading_progress(&mut self) {}
        fn start_loading_screen(&mut self) {}
        fn post_quit(&mut self) {
            self.log.push("post quit".into());
        }
        fn preferences_open(&self) -> bool {
            false
        }
        fn clear_nn_screen_up(&mut self) {}
        fn shutdown_server_connection(&mut self) {
            self.log.push("shutdown server".into());
        }
        fn connect_and_login(&mut self) -> bool {
            false
        }
        fn request_character_info(&mut self) -> bool {
            false
        }
        fn select_character(&mut self, _: &[u8]) -> bool {
            false
        }
        fn player_base_id(&self) -> u32 {
            0
        }
        fn skip_autosave(&mut self) {}
        fn quit_requested(&self) -> bool {
            self.quit
        }
        fn set_quit_requested(&mut self, quit: bool) {
            self.quit = quit;
            self.log.push(format!("quit {quit}"));
        }
    }

    impl FrameHost for Host {
        fn oracle_frame_tick(&mut self) {
            self.log.push("oracle".into());
        }
        fn skip_local_server_tick(&self) -> bool {
            self.skip
        }
        fn update_global_time(&mut self, dt: f64) {
            self.log.push(format!("time {dt}"));
        }
        fn single_player(&self) -> bool {
            self.single_player
        }
        fn server(&self) -> Option<u64> {
            self.server
        }
        fn server_disconnected(&self) -> bool {
            self.disconnected
        }
        fn server_connected(&self) -> bool {
            self.connected
        }
        fn clear_server_flags(&mut self) {
            self.disconnected = false;
            self.connected = false;
            self.log.push("clear flags".into());
        }
        fn run_error_modal(&mut self, ids: u32, update_server: bool) {
            self.log.push(format!("error {ids:#x} {update_server}"));
        }
        fn server_select_active(&self) -> bool {
            self.server_select
        }
        fn set_intro_mode_request(&mut self, mode: i32) {
            self.log.push(format!("intro mode {mode}"));
        }
        fn update_interface(&mut self, dt: f64) {
            self.log.push(format!("ui {dt}"));
        }
        fn tick_local_server(&mut self, dt: f32) {
            self.log.push(format!("server {dt}"));
        }
    }

    struct State(EnvironmentKind);

    impl Environment<Host> for State {
        fn kind(&self) -> EnvironmentKind {
            self.0
        }
        fn init(&mut self, cx: &mut Host, _: i32) -> bool {
            cx.log.push(format!("{:?} init", self.0));
            true
        }
        fn destroy(&mut self, cx: &mut Host) {
            cx.log.push(format!("{:?} destroy", self.0));
        }
        fn resume(&mut self, _: &mut Host) {}
        fn update(&mut self, cx: &mut Host, dt: f32) -> i32 {
            cx.log.push(format!("{:?} update {dt}", self.0));
            cx.answer
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

    fn frame(f: &mut FrameState, s: &mut ClientStates<Host>, h: &mut Host) -> Vec<String> {
        h.log.clear();
        tick_client_frame(f, s, h, 0.25);
        std::mem::take(&mut h.log)
    }

    #[test]
    fn a_frame_runs_time_ui_server_then_the_state() {
        let (mut f, mut s) = (FrameState::default(), states());
        let mut h = Host {
            single_player: true,
            server: Some(1),
            ..Host::default()
        };
        s.enter(&mut h, 6);
        assert_eq!(
            frame(&mut f, &mut s, &mut h),
            [
                "oracle",
                "time 0.25",
                "ui 0.25",
                "server 0.25",
                "IntroFocus update 0.25"
            ]
        );
        h.skip = true;
        h.answer = 2;
        assert_eq!(
            frame(&mut f, &mut s, &mut h),
            [
                "oracle",
                "ui 0.25",
                "server 0.25",
                "IntroFocus update 0.25",
                "enter 2",
                "IntroFocus destroy",
                "Space init"
            ]
        );
    }

    #[test]
    fn a_pending_state_wins_over_the_answer() {
        let (mut f, mut s) = (FrameState::default(), states());
        let mut h = Host {
            single_player: true,
            answer: 2,
            ..Host::default()
        };
        s.enter(&mut h, 6);
        s.set_pending(8);
        let log = frame(&mut f, &mut s, &mut h);
        assert_eq!(
            log[log.len() - 3..],
            ["enter 8", "IntroFocus destroy", "MissionBriefing init"]
        );
    }

    #[test]
    fn a_quit_request_returns_to_the_intro_without_the_update() {
        let (mut f, mut s) = (FrameState::default(), states());
        let mut h = Host {
            single_player: true,
            ..Host::default()
        };
        s.enter(&mut h, 2);
        h.quit = true;
        let log = frame(&mut f, &mut s, &mut h);
        assert_eq!(
            log,
            [
                "oracle",
                "time 0.25",
                "ui 0.25",
                "server 0.25",
                "quit false",
                "enter 6",
                "Space destroy",
                "IntroFocus init"
            ]
        );
    }

    #[test]
    fn a_lost_server_drops_a_multiplayer_game_to_the_server_list() {
        let (mut f, mut s) = (FrameState::default(), states());
        let mut h = Host {
            server: Some(7),
            ..Host::default()
        };
        s.enter(&mut h, 2);
        frame(&mut f, &mut s, &mut h);
        h.disconnected = true;
        h.connected = true;
        let log = frame(&mut f, &mut s, &mut h);
        assert_eq!(
            log,
            [
                "oracle",
                "time 0.25",
                "error 0x737 false",
                "clear flags",
                "shutdown server",
                "intro mode 3",
                "quit true",
                "ui 0.25",
                "server 0.25",
                "quit false",
                "enter 6",
                "Space destroy",
                "IntroFocus init"
            ]
        );
    }

    #[test]
    fn a_server_gone_this_frame_forces_the_intro_and_no_state_posts_the_quit() {
        let (mut f, mut s) = (FrameState::default(), states());
        let mut h = Host {
            single_player: true,
            server: Some(1),
            ..Host::default()
        };
        s.enter(&mut h, 2);
        frame(&mut f, &mut s, &mut h);
        h.server = None;
        let log = frame(&mut f, &mut s, &mut h);
        assert_eq!(
            log[4..],
            ["quit false", "enter 6", "Space destroy", "IntroFocus init"]
        );
        // A frame with no current state posts the quit.
        let mut empty = states();
        let log = frame(&mut f, &mut empty, &mut h);
        assert_eq!(log.last().map(String::as_str), Some("post quit"));
    }
}
