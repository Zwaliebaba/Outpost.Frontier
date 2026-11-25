//! The dedicated server's frame over the loopback transport and the admin
//! commands' effects on the game (work package P3.9), without a network
//! and without the original data: a bare world, accounts in a temporary
//! folder.
//!
//! - The transport pump: a connection hears its client id and the message
//!   of the day; a login answers; a closed connection logs the session
//!   out and frees its slot.
//! - Each console command: accounts, account create and delete, the admin
//!   right, ban and kick (which disconnect the account's client), kick by
//!   client id, the players, the three chats, the news and the status.

use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::Arc;

use game_logic::admin::{self, Command, HostControl};
use game_logic::host::ServerHost;
use game_logic::login::ServerFiles;
use game_logic::server::Logic;
use game_sim::data::config::GameData;
use game_sim::object::World;
use game_sim::object::world::WorldConfig;
use game_sim::protocol::common::CommonMessage;
use game_sim::protocol::rdl::extract_text;
use game_sim::protocol::to_client::ServerMessage;
use game_sim::protocol::to_server::Login;
use game_sim::protocol::{ClientMessage, ToClient, ToServer};
use neuron_core::net::transport::{
    ConnectionId, DisconnectReason, LOOPBACK_CONNECTION, LoopbackTransport, Transport,
    TransportEvent, loopback,
};
use neuron_server::account::{AccountsRoot, encode_password_file};
use neuron_server::admin_files::{MOTD_FILE, read_motd};

type ServerEnd = LoopbackTransport<ToClient, ToServer>;
type ClientEnd = LoopbackTransport<ToServer, ToClient>;

fn temp_dir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("outpost-host-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

fn w(text: &str) -> Vec<u16> {
    text.encode_utf16().collect()
}

/// A multiplayer server over a bare world with its accounts in `dir`.
fn host(dir: &Path) -> (ServerHost<ServerEnd>, ClientEnd) {
    let accounts = AccountsRoot::new(dir, true);
    let files = ServerFiles {
        exe_dir: dir.to_path_buf(),
        data_dir: dir.to_path_buf(),
        initial_world: Vec::new(),
        accounts: accounts.clone(),
    };
    let world = World::new(
        Arc::new(GameData::new()),
        WorldConfig {
            single_player: false,
            ..WorldConfig::default()
        },
    );
    let mut logic = Logic::new(
        Box::new(game_logic::server::RecordingSink::default()),
        4,
        true,
    );
    logic.saves.accounts = Some(accounts);
    let (client, server) = loopback::<ToServer, ToClient>();
    let host = ServerHost::new(world, logic, files, Rc::new(RefCell::new(server)));
    (host, client)
}

fn drain(client: &mut ClientEnd) -> Vec<ToClient> {
    let mut out = Vec::new();
    while let Some(event) = client.poll() {
        if let TransportEvent::Message { message, .. } = event {
            out.push(message);
        }
    }
    out
}

fn login(client: &mut ClientEnd, account: &str) {
    let message = ToServer::Client(ClientMessage::Login(Login {
        create_flag: 1,
        account_name: w(account),
        second: Vec::new(),
    }));
    client.send(LOOPBACK_CONNECTION, message).unwrap();
}

/// The host side of the commands over the loopback's server end.
struct Control(Rc<RefCell<ServerEnd>>);

impl HostControl for Control {
    fn close(&mut self, client: u32) -> bool {
        self.0
            .borrow_mut()
            .disconnect(ConnectionId::new(client), DisconnectReason::Kicked)
            .is_ok()
    }

    fn connection_count(&self) -> usize {
        usize::from(self.0.borrow().link_stats(LOOPBACK_CONNECTION).is_some())
    }

    fn peer(&self, _client: u32) -> Option<String> {
        Some("loopback".into())
    }

    fn status_lines(&self) -> Vec<String> {
        vec!["Server: test".into()]
    }
}

fn run(host: &mut ServerHost<ServerEnd>, command: Command) -> Vec<String> {
    let mut control = Control(Rc::clone(host.transport()));
    let mut cx = host.cx();
    admin::execute(&mut cx, &mut control, &command)
}

#[test]
fn a_connection_hears_its_id_and_the_news_and_logs_in() {
    let dir = temp_dir("connect");
    let (mut host, mut client) = host(&dir);
    {
        let mut cx = host.cx();
        game_logic::session::set_motd(&mut cx, &w("Welcome"));
    }
    let pumped = host.frame(0.0, 0.0);
    assert_eq!(pumped.connected, 1);
    let got = drain(&mut client);
    assert!(matches!(
        &got[0],
        ToClient::Server(ServerMessage::ConnectResponse(r)) if r.value == 1
    ));
    assert!(matches!(
        &got[1],
        ToClient::Server(ServerMessage::MiscObjUpdate(m)) if m.flags == 0x10 && m.text == w("Welcome")
    ));

    login(&mut client, "pilot");
    host.frame(0.0, 0.0);
    let got = drain(&mut client);
    assert!(
        got.iter().any(|m| matches!(
            m,
            ToClient::Server(ServerMessage::LoginResponse(r)) if r.value == 3
        )),
        "{got:?}"
    );
    assert_eq!(host.logic.stats.logins, 1);
    assert!(host.logic.players.is_valid(1));

    // A second login of the same session is dropped.
    login(&mut client, "pilot");
    host.frame(0.0, 0.0);
    assert!(drain(&mut client).is_empty());
    assert_eq!(host.logic.stats.logins, 1);

    // Every message reaches its entry point: the single-player difficulty
    // is refused on a dedicated server, and nothing is left unrouted.
    let scale = ToServer::Client(ClientMessage::ReqDifficultyScale(
        game_sim::protocol::to_server::ReqDifficultyScale { value: 0.5 },
    ));
    client.send(LOOPBACK_CONNECTION, scale).unwrap();
    host.frame(0.0, 0.0);
    assert!(host.unrouted.is_empty(), "{:?}", host.unrouted);
    assert_eq!(host.logic.players.get(1).unwrap().difficulty_scale, 1.0);

    // The client goes: the session logs out and its slot is free.
    drop(client);
    let pumped = host.frame(0.0, 0.0);
    assert_eq!(pumped.disconnected, 1);
    assert!(!host.logic.players.is_valid(1));
    assert_eq!(host.logic.stats.logouts, 1);
    let account = host.logic.accounts.find_by_name(&w("pilot")).unwrap();
    assert_eq!(host.logic.accounts.get(account).unwrap().client_id, 0);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn account_commands_change_the_registry_and_the_files() {
    let dir = temp_dir("accounts");
    let (mut host, _client) = host(&dir);
    let root = AccountsRoot::new(&dir, true);
    assert_eq!(run(&mut host, Command::Accounts), ["No accounts."]);
    let out = run(
        &mut host,
        Command::CreateAccount {
            name: "Pilot".into(),
            password: "secret".into(),
        },
    );
    assert_eq!(out, ["Account pilot created."], "the name is lower-cased");
    let folder = root.account_dir(&w("pilot"));
    assert!(folder.join("name").is_file());
    assert_eq!(
        std::fs::read(folder.join("password")).unwrap(),
        encode_password_file(&w("secret"))
    );
    assert!(
        run(
            &mut host,
            Command::CreateAccount {
                name: "pilot".into(),
                password: String::new(),
            }
        )[0]
        .contains("could not be created")
    );

    run(
        &mut host,
        Command::SetAdmin {
            name: "pilot".into(),
            admin: true,
        },
    );
    assert!(folder.join("admin").is_file());
    run(
        &mut host,
        Command::Ban {
            name: "pilot".into(),
            ban: true,
        },
    );
    assert!(folder.join("banned").is_file());
    assert_eq!(
        run(&mut host, Command::Accounts),
        ["pilot [banned] [admin]"]
    );
    run(
        &mut host,
        Command::Ban {
            name: "pilot".into(),
            ban: false,
        },
    );
    assert!(!folder.join("banned").exists());

    // A character of the account, then its deletion.
    let character = w("Ace");
    let id = host.logic.accounts.find_by_name(&w("pilot")).unwrap();
    host.logic
        .accounts
        .get_mut(id)
        .unwrap()
        .characters
        .push(character.clone());
    let file_id = neuron_server::registry::character_id(&character);
    host.logic.accounts.insert_character_id(&file_id, id);
    assert_eq!(
        run(&mut host, Command::Accounts),
        ["pilot [admin]", "    Ace"]
    );
    run(&mut host, Command::DeleteCharacter { name: "Ace".into() });
    assert!(host.logic.accounts.get(id).unwrap().characters.is_empty());
    assert!(
        !host.logic.accounts.get(id).unwrap().locked,
        "unlocked again"
    );

    run(
        &mut host,
        Command::DeleteAccount {
            name: "PILOT".into(),
        },
    );
    assert!(!folder.exists());
    assert_eq!(run(&mut host, Command::Accounts), ["No accounts."]);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn ban_and_kick_disconnect_the_accounts_client() {
    let dir = temp_dir("kick");
    let (mut host, mut client) = host(&dir);
    host.frame(0.0, 0.0);
    login(&mut client, "pilot");
    host.frame(0.0, 0.0);
    drain(&mut client);
    assert!(host.logic.players.is_valid(1));

    // A ban that changes the account's state disconnects its client at
    // once, as LockAccountAccess does.
    let out = run(
        &mut host,
        Command::Ban {
            name: "pilot".into(),
            ban: true,
        },
    );
    assert_eq!(out, ["Account pilot is banned."]);
    assert!(!host.logic.players.is_valid(1));
    assert_eq!(host.logic.stats.logouts, 1);
    let mut events = Vec::new();
    while let Some(event) = client.poll() {
        events.push(event);
    }
    assert!(events.contains(&TransportEvent::Disconnected {
        connection: LOOPBACK_CONNECTION,
        reason: DisconnectReason::Kicked,
    }));
    // The transport's own disconnect then finds the slot free.
    host.frame(0.0, 0.0);
    assert_eq!(host.logic.stats.logouts, 1);
    assert_eq!(
        run(
            &mut host,
            Command::Kick {
                name: "pilot".into()
            }
        ),
        ["Account pilot is not logged in."]
    );
    assert_eq!(
        run(&mut host, Command::KickClient { client: 1 }),
        ["No client 1."]
    );
    assert_eq!(
        run(
            &mut host,
            Command::Kick {
                name: "nobody".into()
            }
        ),
        ["No account nobody."]
    );
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn kick_by_client_closes_the_connection() {
    let dir = temp_dir("kick-client");
    let (mut host, mut client) = host(&dir);
    host.frame(0.0, 0.0);
    login(&mut client, "pilot");
    host.frame(0.0, 0.0);
    assert_eq!(
        run(&mut host, Command::KickClient { client: 1 }),
        ["Client 1 kicked."]
    );
    assert!(!host.logic.players.is_valid(1));
    drain(&mut client);
    assert!(client.link_stats(LOOPBACK_CONNECTION).is_none());
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn chat_news_players_and_status() {
    let dir = temp_dir("chat");
    let (mut host, mut client) = host(&dir);
    host.frame(0.0, 0.0);
    login(&mut client, "pilot");
    host.frame(0.0, 0.0);
    drain(&mut client);

    // Universe chat from the console reaches every player in use.
    run(
        &mut host,
        Command::ChatUniverse {
            text: "hello".into(),
        },
    );
    let got = drain(&mut client);
    let chat = got
        .iter()
        .find_map(|m| match m {
            ToClient::Common(CommonMessage::ChatMsg(c)) => Some(c.clone()),
            _ => None,
        })
        .expect("a chat message");
    assert_eq!(chat.target_id, 1);
    assert_eq!(chat.field_04, game_logic::chat::UNIVERSE);
    let (text, _) = extract_text(&chat.data, 0x400);
    assert_eq!(String::from_utf16_lossy(&text), "CONSOLE: hello");

    // Private chat to a character nobody has, and to an unknown system.
    assert_eq!(
        run(
            &mut host,
            Command::ChatPlayer {
                character: "Ace".into(),
                text: "hi".into(),
            }
        ),
        ["No character Ace."]
    );
    assert_eq!(
        run(
            &mut host,
            Command::ChatSystem {
                system: "li01".into(),
                text: "hi".into(),
            }
        ),
        ["No system li01."]
    );

    // The news is kept, saved as motd.dat and sent to the players.
    assert_eq!(run(&mut host, Command::News), ["No news."]);
    run(
        &mut host,
        Command::SetNews {
            text: "Docks open".into(),
        },
    );
    assert_eq!(host.logic.motd, w("Docks open"));
    let saved = std::fs::read(AccountsRoot::new(&dir, true).local_user_path(MOTD_FILE)).unwrap();
    assert_eq!(read_motd(&saved), w("Docks open"));
    let got = drain(&mut client);
    assert!(got.iter().any(|m| matches!(
        m,
        ToClient::Server(ServerMessage::MiscObjUpdate(u)) if u.text == w("Docks open")
    )));
    assert_eq!(run(&mut host, Command::News), ["Docks open"]);

    // No character is selected, so the player list is empty.
    assert_eq!(run(&mut host, Command::Players), ["No players."]);
    let status = run(&mut host, Command::Status);
    assert_eq!(status[0], "Server: test");
    assert!(status.contains(&"Player Count: 1".to_owned()), "{status:?}");
    assert!(status.contains(&"Total Logins: 1".to_owned()));
    assert!(status.contains(&"Connections: 1".to_owned()));
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn the_dispatching_handlers_reach_the_bus_and_the_counters() {
    use game_sim::protocol::to_server::{AbortMission, DbgConsole, RequestNewCharInfo};
    let dir = temp_dir("dispatch");
    let (mut host, mut client) = host(&dir);
    host.frame(0.0, 0.0);
    login(&mut client, "pilot");
    host.frame(0.0, 0.0);
    for message in [
        ClientMessage::AbortMission(AbortMission { value: 7 }),
        ClientMessage::RequestNewCharInfo(RequestNewCharInfo {}),
        ClientMessage::DbgConsole(DbgConsole::default()),
    ] {
        client
            .send(LOOPBACK_CONNECTION, ToServer::Client(message))
            .unwrap();
    }
    host.update();
    assert_eq!(host.logic.stats.aborted_missions, 1);
    assert!(host.unrouted.is_empty(), "{:?}", host.unrouted);
    let report = host
        .logic
        .reports
        .0
        .last()
        .expect("the debug packet's report");
    assert_eq!(report.source, "Network\\OutpostServerPacketHandler.cpp");
    assert_eq!(report.line, 0x204);
    assert_eq!(
        report.text,
        "*** ERROR: FLPACKET_CLIENT_DBGCONSOLE encountered in release mode"
    );
    let _ = std::fs::remove_dir_all(&dir);
}
