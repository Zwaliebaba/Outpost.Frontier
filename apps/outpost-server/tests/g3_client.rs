//! Gate G3's scripted test client (design/migration-plan.md, owner
//! 2026-11-25): the real `OutpostServer` on the original data (`outdir/`)
//! over QUIC on 127.0.0.1, and a `QuicClient` (`neuron-client`, a
//! dev-only dependency of this package) speaking game-sim's multiplayer
//! messages. Two clients each go through the handshake and the client id,
//! the message of the day, the login with a new account, a new character
//! from `newcharacter.ini`'s first faction, its selection, the base it
//! starts in, leaving it, the ship's reservation and the launch from the
//! base's docking ring. Then the first client is told of the second's ship
//! (`CREATESHIP`) and receives its object updates (`UPDATEOBJECT`) as the
//! second client flies it: until Content's population (P3.6) puts NPC ships
//! in space, another player's ship is the only object the server replicates
//! to a fresh player. The console's `status`, `kick` and `quit` end the run.
//!
//! The data is the committed `outdir/`, as the other data tests of the
//! workspace read it; the test returns early with a notice when it is
//! missing.

use std::io::{BufRead, BufReader, Write};
use std::net::SocketAddr;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::mpsc::{self, Receiver};
use std::time::{Duration, Instant};

use game_sim::protocol::common::{CommonMessage, UpdateObject};
use game_sim::protocol::to_client::ServerMessage;
use game_sim::protocol::to_server::{
    CreateNewChar, EnterBase, ExitBase, Login, RequestCharInfo, RequestCreateShip, RequestLaunch,
    SelectCharacter,
};
use game_sim::protocol::{
    ClientMessage, GAME_BUILD, MultiplayerToClient, MultiplayerToServer, ToClient, ToServer,
    quantise_orientation,
};
use neuron_client::net::{ConnectConfig, QuicClient, SERVER_CONNECTION};
use neuron_core::id::create_id;
use neuron_core::net::pin::ServerTrust;
use neuron_core::net::transport::{Transport, TransportEvent};

type Client = QuicClient<MultiplayerToServer, MultiplayerToClient>;

const ACCOUNTS: [&str; 2] = [
    "0f1e2d3c-4b5a6978-8796a5b4-c3d2e1f0",
    "1f2e3d4c-5b6a7988-97a6b5c4-d3e2f1a0",
];
const START_WAIT: Duration = Duration::from_secs(300);
const STEP_WAIT: Duration = Duration::from_secs(60);

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// The server process; killed when the test ends early.
struct Server {
    child: Child,
    lines: Receiver<String>,
    log: Vec<String>,
}

impl Drop for Server {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

impl Server {
    /// The next line of the server's output that starts with `prefix`.
    fn wait_for(&mut self, prefix: &str, wait: Duration) -> String {
        let deadline = Instant::now() + wait;
        loop {
            let left = deadline.saturating_duration_since(Instant::now());
            match self.lines.recv_timeout(left) {
                Ok(line) => {
                    self.log.push(line.clone());
                    if let Some(rest) = line.strip_prefix(prefix) {
                        return rest.to_owned();
                    }
                }
                Err(_) => panic!(
                    "no line starting {prefix:?}; the server wrote:\n{}",
                    self.log.join("\n")
                ),
            }
        }
    }

    fn command(&mut self, line: &str) {
        let stdin = self.child.stdin.as_mut().expect("stdin");
        writeln!(stdin, "{line}").expect("the console takes the line");
        stdin.flush().expect("flush");
    }
}

fn start_server(dir: &Path) -> Server {
    let config = dir.join("OutpostServer.toml");
    let text = format!(
        "[server]\nname = \"G3\"\n[network]\naddress = \"127.0.0.1\"\nport = 0\n[paths]\ninstall_dir = {}\nuser_data = {}\n",
        toml_string(&outdir()),
        toml_string(&dir.join("user")),
    );
    std::fs::write(&config, text).unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_OutpostServer"))
        .arg("--config")
        .arg(&config)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()
        .expect("the server starts");
    let stdout = child.stdout.take().unwrap();
    let (tx, lines) = mpsc::channel();
    std::thread::spawn(move || {
        for line in BufReader::new(stdout).lines() {
            let Ok(line) = line else { break };
            if tx.send(line).is_err() {
                break;
            }
        }
    });
    Server {
        child,
        lines,
        log: Vec::new(),
    }
}

fn toml_string(path: &Path) -> String {
    format!("{:?}", path.display().to_string())
}

fn send(client: &mut Client, message: impl Into<ClientMessage>) {
    client
        .send(
            SERVER_CONNECTION,
            MultiplayerToServer(ToServer::Client(message.into())),
        )
        .expect("the connection is open");
}

/// Polls until `pick` takes a message; every message seen is kept.
fn expect<T>(
    client: &mut Client,
    seen: &mut Vec<ToClient>,
    what: &str,
    mut pick: impl FnMut(&ToClient) -> Option<T>,
) -> T {
    let deadline = Instant::now() + STEP_WAIT;
    while Instant::now() < deadline {
        while let Some(event) = client.poll() {
            match event {
                TransportEvent::Message { message, .. } => {
                    let message = message.0;
                    let found = pick(&message);
                    seen.push(message);
                    if let Some(found) = found {
                        return found;
                    }
                }
                TransportEvent::Disconnected { reason, .. } => {
                    panic!("disconnected ({reason:?}) waiting for {what}")
                }
                TransportEvent::Connected(_) => {}
            }
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    let names: Vec<&str> = seen.iter().map(name).collect();
    let last = &seen[seen.len().saturating_sub(8)..];
    panic!("no {what}; the client received {names:?}, last {last:#?}");
}

fn name(message: &ToClient) -> &'static str {
    match message {
        ToClient::Common(m) => m.name(),
        ToClient::Server(m) => m.name(),
    }
}

/// A pilot in space: its connection, what it received and its ship.
struct Pilot {
    client: Client,
    seen: Vec<ToClient>,
    ship: u32,
}

/// One client from the handshake to the launch: the client id and the
/// message of the day, the login creating `account`, the empty character
/// list, a new character `name` of `newcharacter.ini`'s first faction, its
/// selection, the base it starts in, leaving it, the ship's reservation
/// and the launch from the base's docking ring.
fn join(address: SocketAddr, fingerprint: &str, account: &str, name: &str, id: u32) -> Pilot {
    let mut client = Client::connect(ConnectConfig::new(
        vec![address],
        GAME_BUILD,
        account,
        "",
        ServerTrust::FirstUse,
    ))
    .expect("the client starts");
    let deadline = Instant::now() + STEP_WAIT;
    loop {
        match client.poll() {
            Some(TransportEvent::Connected(_)) => break,
            Some(TransportEvent::Disconnected { reason, .. }) => panic!("refused: {reason:?}"),
            _ => {}
        }
        assert!(Instant::now() < deadline, "no handshake");
        std::thread::sleep(Duration::from_millis(5));
    }
    assert_eq!(
        client
            .server_fingerprint()
            .map(|f| f.to_string())
            .as_deref(),
        Some(fingerprint)
    );
    let mut seen = Vec::new();
    let connected = expect(&mut client, &mut seen, "CONNECTRESPONSE", |m| match m {
        ToClient::Server(ServerMessage::ConnectResponse(r)) => Some(r.value),
        _ => None,
    });
    assert_eq!(connected, id, "the lowest free client id");
    expect(
        &mut client,
        &mut seen,
        "the message of the day",
        |m| match m {
            ToClient::Server(ServerMessage::MiscObjUpdate(u)) if u.flags == 0x10 => Some(()),
            _ => None,
        },
    );

    send(
        &mut client,
        Login {
            create_flag: 1,
            account_name: account.encode_utf16().collect(),
            second: Vec::new(),
        },
    );
    let login = expect(&mut client, &mut seen, "LOGINRESPONSE", |m| match m {
        ToClient::Server(ServerMessage::LoginResponse(r)) => Some(r.value),
        _ => None,
    });
    assert_eq!(login, 3, "the login succeeds");
    send(&mut client, RequestCharInfo {});
    let empty = expect(&mut client, &mut seen, "CHARACTERINFO", |m| match m {
        ToClient::Server(ServerMessage::CharacterInfo(c)) => Some(c.records.len()),
        _ => None,
    });
    assert_eq!(empty, 0, "a new account has no character");

    let base = create_id(b"he01_01_Base");
    send(
        &mut client,
        CreateNewChar {
            name: name.encode_utf16().collect(),
            faction: create_id(b"new_player"),
            base,
            package: create_id(b"ge_fighter"),
            pilot: create_id(b"rellan"),
        },
    );
    let record = expect(&mut client, &mut seen, "the new character", |m| match m {
        ToClient::Server(ServerMessage::CharacterInfo(c)) => c.records.first().cloned(),
        _ => None,
    });
    assert_eq!(String::from_utf16_lossy(&record.name), name);
    assert_eq!(record.base_id, base);

    send(
        &mut client,
        SelectCharacter {
            character_id: record.character_id.clone(),
        },
    );
    expect(&mut client, &mut seen, "CHARSELECTVERIFIED", |m| match m {
        ToClient::Server(ServerMessage::CharSelectVerified(_)) => Some(()),
        _ => None,
    });
    send(&mut client, EnterBase { value: base });
    send(&mut client, ExitBase { value: base });
    send(&mut client, RequestCreateShip {});
    let ship = expect(
        &mut client,
        &mut seen,
        "REQUESTCREATESHIPRESP",
        |m| match m {
            ToClient::Server(ServerMessage::RequestCreateShipResp(r)) => Some(r.field_04),
            _ => None,
        },
    );
    assert_ne!(ship, 0);
    send(&mut client, RequestLaunch { value: ship });
    let (launched, ring) = expect(&mut client, &mut seen, "LAUNCH", |m| match m {
        ToClient::Server(ServerMessage::Launch(l)) => Some((l.field_00, l.field_04)),
        _ => None,
    });
    assert_eq!(launched, ship);
    assert_ne!(ring, 0, "the launch is from the base's docking ring");
    Pilot { client, seen, ship }
}

/// `FLPACKET_COMMON_UPDATEOBJECT` of the pilot's own ship.
fn update_ship(pilot: &mut Pilot, position: [f32; 3], time: f32) {
    let update = UpdateObject {
        flags: 0,
        object_id: pilot.ship,
        position,
        orientation: quantise_orientation([1.0, 0.0, 0.0, 0.0]),
        extra: 0,
        update_time: time,
    };
    pilot
        .client
        .send(
            SERVER_CONNECTION,
            MultiplayerToServer(ToServer::Common(update.into())),
        )
        .expect("the connection is open");
}

#[test]
fn scripted_clients_log_in_create_characters_launch_and_see_each_other() {
    if !outdir().join("OutpostFrontier.ini").is_file() {
        println!("outdir/ is missing: Gate G3's scripted client was not run");
        return;
    }
    let dir = std::env::temp_dir().join(format!("outpost-g3-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    let mut server = start_server(&dir);
    let address: SocketAddr = server
        .wait_for("Listening on ", START_WAIT)
        .parse()
        .expect("an address");
    let fingerprint = server.wait_for("Certificate fingerprint ", STEP_WAIT);
    server.wait_for("Server running, ready for log in.", STEP_WAIT);

    let mut first = join(address, &fingerprint, ACCOUNTS[0], "Gate Three", 1);
    let mut second = join(address, &fingerprint, ACCOUNTS[1], "Gate Four", 2);

    // Each client is told of the other's ship once it is in range...
    let other = second.ship;
    expect(
        &mut first.client,
        &mut first.seen,
        "the second ship's create",
        |m| match m {
            ToClient::Server(ServerMessage::CreateShip(c)) if c.object.obj_id == other => Some(()),
            _ => None,
        },
    );
    // ...and receives its updates as the other client flies it.
    let position = match second.seen.iter().rev().find_map(|m| match m {
        ToClient::Server(ServerMessage::Launch(l)) => Some(l.position),
        _ => None,
    }) {
        Some(p) => p,
        None => panic!("the second client was launched"),
    };
    let deadline = Instant::now() + STEP_WAIT;
    let mut time = 1.0;
    let mut moved = None;
    while moved.is_none() && Instant::now() < deadline {
        time += 0.1;
        update_ship(
            &mut second,
            [position[0] + time * 10.0, position[1], position[2]],
            time,
        );
        std::thread::sleep(Duration::from_millis(100));
        while let Some(event) = first.client.poll() {
            if let TransportEvent::Message { message, .. } = event {
                if let ToClient::Common(CommonMessage::UpdateObject(u)) = &message.0
                    && u.object_id == other
                {
                    moved = Some(u.position);
                }
                first.seen.push(message.0);
            }
        }
        while second.client.poll().is_some() {}
    }
    let moved = moved.expect("the first client receives the second ship's updates");
    assert!(moved[0] > position[0], "{moved:?} after {position:?}");

    // The console sees both players, then saves them and stops.
    server.command("status");
    let players = server.wait_for("Player Count: ", STEP_WAIT);
    assert_eq!(players, "2");
    server.command("kick #2");
    server.wait_for("Client 2 kicked.", STEP_WAIT);
    server.command("quit");
    server.wait_for("Server stopped.", STEP_WAIT);
    let status = server.child.wait().expect("the server ends");
    assert!(status.success());
    let _ = std::fs::remove_dir_all(&dir);
}
