use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;

use game_sim::data::config::GameData;
use game_sim::object::World;
use game_sim::object::world::WorldConfig;
use game_sim::protocol::to_client::{CharacterInfo, ConnectResponse, LoginResponse};
use game_sim::protocol::{ClientMessage, ToClient, ToServer};
use neuron_client::ui::widescreen::Surface;
use neuron_core::net::transport::{
    LOOPBACK_CONNECTION, LoopbackTransport, Transport, TransportEvent, loopback,
};
use neuron_core::time::ManualClock;

use super::*;
use crate::frame::{FrameState, tick_client_frame};
use crate::oracle::script::OracleScript;

type Calls = Rc<RefCell<Vec<String>>>;
type Cx<'a> = ClientContext<'a, Ui, ManualClock>;

/// A recording single-player server: it answers the connection, the login
/// (status 3) and the character list (empty) in its pump, and records its
/// calls.
struct Server {
    world: World,
    end: LoopbackTransport<ToClient, ToServer>,
    calls: Calls,
}

impl Server {
    fn reply(&mut self, message: impl Into<game_sim::protocol::ServerMessage>) {
        let _ = self
            .end
            .send(LOOPBACK_CONNECTION, ToClient::Server(message.into()));
    }
}

impl Session for Server {
    fn world(&mut self) -> &mut World {
        &mut self.world
    }
    fn update(&mut self) {
        while let Some(event) = self.end.poll() {
            match event {
                TransportEvent::Connected(_) => {
                    self.calls.borrow_mut().push("connect".into());
                    self.reply(ConnectResponse { value: 1 });
                }
                TransportEvent::Message {
                    message: ToServer::Client(m),
                    ..
                } => match m {
                    ClientMessage::Login(_) => {
                        self.calls.borrow_mut().push("login".into());
                        self.reply(LoginResponse { value: 3 });
                    }
                    ClientMessage::RequestCharInfo(_) => {
                        self.calls.borrow_mut().push("character info".into());
                        self.reply(CharacterInfo {
                            records: Vec::new(),
                            save_failure_count: 0,
                        });
                    }
                    m => self.calls.borrow_mut().push(format!("{m:?}")),
                },
                _ => {}
            }
        }
    }
    fn elapse_time(&mut self, _dt: f32) {
        self.calls.borrow_mut().push("elapse".into());
    }
    fn single_player(&self) -> bool {
        true
    }
}

fn request() -> LocalServerRequest {
    LocalServerRequest {
        install_dir: ".".into(),
        user_data: ".".into(),
        seed: crate::oracle::script::RAND_SEED,
        oracle: true,
    }
}

fn oracle(script: &str) -> OracleRun {
    OracleRun {
        script: OracleScript::start(script, "s", "o"),
        out: "o".into(),
        keymap: Vec::new(),
        layout: Vec::new(),
    }
}

/// The context over a plain interface, a recording server and `script`,
/// with state 6 entered as `WinMain` ends its start-up.
fn run_with(
    ui: Ui,
    script: &str,
    calls: &Calls,
    starter: StartLocalServer<'_>,
    test: impl for<'a> FnOnce(&mut ClientStates<Cx<'a>>, &mut Cx<'a>),
) {
    let _ = calls;
    let mut t = 0.0;
    let mut cx = ClientContext::new(
        ui,
        ClientTiming::new(ManualClock::new(Some(1000))),
        Some(oracle(script)),
        starter,
        request(),
        Box::new(move || {
            t += 0.001;
            t
        }),
    );
    let mut states = client_states();
    states.enter(&mut cx, 6);
    test(&mut states, &mut cx);
}

fn starter(
    calls: &Calls,
) -> impl FnMut(&LocalServerRequest) -> Option<(Box<dyn Session>, LocalClientEnd)> {
    let calls = Rc::clone(calls);
    move |_| {
        calls.borrow_mut().push("start".into());
        let (client, end) = loopback();
        let server = Server {
            world: World::new(Arc::new(GameData::default()), WorldConfig::default()),
            end,
            calls: Rc::clone(&calls),
        };
        Some((Box::new(server) as Box<dyn Session>, client))
    }
}

fn plain_ui() -> Ui {
    Ui::new(Surface::new(1024, 768))
}

fn frames<'a>(states: &mut ClientStates<Cx<'a>>, cx: &mut Cx<'a>, n: u32) {
    let mut frame = FrameState::default();
    for _ in 0..n {
        tick_client_frame(&mut frame, states, cx, crate::oracle::script::FRAME_SECONDS);
    }
}

fn screens(cx: &ClientContext<'_, Ui, ManualClock>) -> Vec<String> {
    cx.core
        .oracle
        .as_ref()
        .unwrap()
        .script
        .session()
        .iter()
        .filter(|l| l.contains("\"rec\":\"screen\""))
        .cloned()
        .collect()
}

#[test]
fn start_up_enters_the_intro_and_logs_in() {
    let calls = Calls::default();
    let mut start = starter(&calls);
    run_with(
        plain_ui(),
        "wait 1000\n",
        &calls,
        &mut start,
        |states, cx| {
            assert_eq!(states.current(), Some(EnvironmentKind::IntroFocus));
            assert_eq!(
                screens(cx),
                [r#"{"frame":0,"rec":"screen","kind":"client","id":6}"#]
            );
            let login = &cx.core.link.as_ref().expect("a server").receiver.login;
            assert_eq!(login.login_status, 3);
            assert!(login.characters.is_some());
            assert_eq!(
                cx.core.iclient_lines(),
                [
                    r#"{"frame":0,"seq":1,"slot":10,"m":"Startup","a":{"MaxPlayers":1,"Port":14041}}"#,
                    r#"{"frame":0,"seq":1,"slot":10,"m":"Startup","r":true}"#,
                    r#"{"frame":0,"seq":2,"slot":11,"m":"OnConnect","a":{"ClientId":1}}"#,
                    r#"{"frame":0,"seq":3,"slot":12,"m":"SendServerLoginResponse","a":{"ClientId":1,"Status":3}}"#,
                    r#"{"frame":0,"seq":4,"slot":13,"m":"SendServerCharacterInfo","a":{"ClientId":1,"Characters":"present"}}"#,
                ]
            );
        },
    );
    let calls = calls.borrow();
    assert_eq!(calls[..3], ["start", "connect", "login"]);
    // The difficulty follows the login, then the list is asked for.
    assert!(calls[3].starts_with("ReqDifficultyScale"));
    assert_eq!(calls[4], "character info");
}

#[test]
fn a_server_that_does_not_start_leaves_the_menu_in_state_13() {
    let calls = Calls::default();
    let mut start = |_: &LocalServerRequest| None;
    run_with(
        plain_ui(),
        "wait 1000\n",
        &calls,
        &mut start,
        |states, cx| {
            assert!(cx.core.session.is_none());
            frames(states, cx, 1);
            assert!(
                screens(cx)
                    .iter()
                    .any(|l| l.contains(r#""kind":"menu","id":13"#))
            );
        },
    );
}

#[test]
fn the_menu_reaches_state_one_builds_the_column_and_the_oracle_sees_it() {
    let calls = Calls::default();
    let mut start = starter(&calls);
    let script = "waitscreen menu 1 900\ntree main\n";
    run_with(plain_ui(), script, &calls, &mut start, |states, cx| {
        assert!(!cx.core.single_precision());
        frames(states, cx, 1);
        assert!(cx.core.single_precision(), "frame 1 is at 24 bits");
        assert_eq!(
            screens(cx)[1],
            r#"{"frame":1,"rec":"screen","kind":"menu","id":1}"#
        );
        let column = cx.core.menu_column.expect("the column");
        assert_eq!(cx.interface.children(column).len(), 11);
        assert!(cx.core.loading_screen.progress_bar.is_some());
        // Frame 2: the wait matches and the tree is dumped through
        // `oracle::ClientHost`, with the column in it.
        frames(states, cx, 1);
        assert!(cx.core.single_precision());
        let tree = cx.core.oracle.as_ref().unwrap().script.tree().to_vec();
        assert!(tree[0].starts_with(
            r#"{"frame":2,"rec":"dump","dump":1,"label":"main","client":6,"menu":1,"#
        ));
        assert!(tree.iter().any(|l| l.contains(r#""name":"Version""#)));
        frames(states, cx, 1);
        assert!(!cx.core.single_precision(), "frame 3 is at 53 bits");
        // The server ran in each frame after the client's delivery.
        assert_eq!(calls.borrow().iter().filter(|c| *c == "elapse").count(), 3);
    });
}

#[test]
fn load_game_leads_to_menu_screens_three_two_and_eighteen() {
    let calls = Calls::default();
    let mut start = starter(&calls);
    run_with(
        plain_ui(),
        "wait 1000\n",
        &calls,
        &mut start,
        |states, cx| {
            frames(states, cx, 1);
            let column = cx.core.menu_column.unwrap();
            // What the column's slide-out hands the menu after a click on Load
            // Game (tests/client_context_data.rs clicks it on the data).
            cx.interface
                .widget_mut::<TypeSelect>(column)
                .unwrap()
                .next_state = 3;
            frames(states, cx, 2);
            let menus: Vec<String> = screens(cx)
                .into_iter()
                .filter(|l| l.contains("menu"))
                .collect();
            // The page file does not load without data: back to the column.
            assert_eq!(
                menus,
                [
                    r#"{"frame":1,"rec":"screen","kind":"menu","id":1}"#,
                    r#"{"frame":3,"rec":"screen","kind":"menu","id":3}"#,
                    r#"{"frame":3,"rec":"screen","kind":"menu","id":2}"#,
                    r#"{"frame":3,"rec":"screen","kind":"menu","id":18}"#,
                    r#"{"frame":3,"rec":"screen","kind":"menu","id":1}"#,
                ]
            );
        },
    );
}

#[test]
fn the_scripts_end_quits_through_state_one() {
    let calls = Calls::default();
    let mut start = starter(&calls);
    run_with(plain_ui(), "quit\n", &calls, &mut start, |states, cx| {
        frames(states, cx, 1);
        assert!(cx.core.quit_posted);
        assert_eq!(states.current(), None);
        assert_eq!(
            screens(cx).last().map(String::as_str),
            Some(r#"{"frame":1,"rec":"screen","kind":"client","id":1}"#)
        );
        cx.core.shutdown_server_connection();
        assert!(cx.core.session.is_none());
        assert_eq!(
            cx.core.iclient_lines().last().map(String::as_str),
            Some(r#"{"frame":1,"seq":5,"slot":15,"m":"Shutdown","a":{}}"#)
        );
    });
}
