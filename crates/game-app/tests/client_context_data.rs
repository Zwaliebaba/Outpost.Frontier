//! The client's context (`game_app::client`) driven by `menu_walk.txt`
//! through `TickClientFrame` on the original data in `outdir/`, over a
//! recording single-player server, and, with `OUTPOST_CLIENT_ORACLE_DIR`
//! set to a C++ client-oracle run of that script, its `session.jsonl`
//! screen and dump records against the run's (decision O53 (6): the screen
//! changes, the dumps and their frames; the screenshots' names left out, as
//! the port takes none yet, and the intro base's `room` screen, which is
//! not entered yet, D1107).

use std::cell::RefCell;
use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::Arc;

use game_app::client::{ClientContext, client_states};
use game_app::client_state::ClientStateId;
use game_app::frame::{FrameState, tick_client_frame};
use game_app::keymap::KEY_MAP_PATH;
use game_app::oracle::OracleRun;
use game_app::oracle::script::{FRAME_SECONDS, OracleScript, RAND_SEED};
use game_app::session::{LocalClientEnd, LocalServerRequest, Session};
use game_app::timing::ClientTiming;
use game_sim::data::config::GameData;
use game_sim::object::World;
use game_sim::object::world::WorldConfig;
use game_sim::protocol::to_client::{CharacterInfo, ConnectResponse, LoginResponse};
use game_sim::protocol::{ClientMessage, ServerMessage, ToClient, ToServer};
use neuron_client::ui::services::UiServices;
use neuron_client::ui::tree::Ui;
use neuron_client::ui::widescreen::Surface;
use neuron_core::catalog::{CLIENT_MODULES, TextCatalog, TextCatalogs};
use neuron_core::net::transport::{
    LOOPBACK_CONNECTION, LoopbackTransport, Transport, TransportEvent, loopback,
};
use neuron_core::time::ManualClock;
use neuron_core::vfs::Host;

/// The byte-order mark the catalog build puts before a card.
const CARD_BOM: [u8; 2] = [0xFF, 0xFE];

/// More frames than the script takes (the C++ run quits at frame 398).
const MAX_FRAMES: u32 = 600;

fn data_dir() -> PathBuf {
    std::env::var_os("OUTPOST_DATA_DIR").map_or_else(
        || PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data"),
        PathBuf::from,
    )
}

struct DataFiles(PathBuf);

impl UiServices for DataFiles {
    fn data_file(&mut self, path: &[u8]) -> Option<Vec<u8>> {
        let rel: Vec<u8> = path
            .iter()
            .map(|&b| if b == b'\\' { b'/' } else { b })
            .collect();
        let found = Host::new(self.0.clone()).locate(&rel).ok().flatten()?;
        fs::read(found.path).ok()
    }
}

/// A module's catalog built from its TOML source as `cargo xtask catalog`
/// builds it.
fn catalog(text: &str) -> TextCatalog {
    let module: toml::Table = text.parse().expect("the module's TOML");
    let mut catalog = TextCatalog::new();
    let table = |name: &str| {
        module
            .get(name)
            .and_then(toml::Value::as_table)
            .cloned()
            .unwrap_or_default()
    };
    for (id, value) in table("strings") {
        let units = value.as_str().unwrap_or_default().encode_utf16().collect();
        catalog.add_string(id.parse().unwrap(), units).unwrap();
    }
    for (id, value) in table("cards") {
        let mut bytes = CARD_BOM.to_vec();
        bytes.extend(
            value
                .as_str()
                .unwrap_or_default()
                .encode_utf16()
                .flat_map(u16::to_le_bytes),
        );
        catalog.add_card(id.parse().unwrap(), bytes).unwrap();
    }
    catalog
}

fn ui() -> Option<Ui> {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data/text");
    if !dir.is_dir() || !data_dir().join("INTERFACE").is_dir() {
        println!("notice: outdir/ has no data; nothing was run");
        return None;
    }
    let mut catalogs = TextCatalogs::new();
    for module in CLIENT_MODULES {
        let text = fs::read_to_string(dir.join(format!("{module}.toml"))).ok();
        catalogs.register(text.as_deref().map(catalog));
    }
    let mut ui = Ui::new(Surface::new(1024, 768));
    ui.catalogs = Some(Rc::new(catalogs));
    ui.services = Box::new(DataFiles(data_dir()));
    Some(ui)
}

/// A single-player server that answers the connection, the login and the
/// character list (empty) in its pump.
struct Server {
    world: World,
    end: LoopbackTransport<ToClient, ToServer>,
}

impl Server {
    fn reply(&mut self, message: impl Into<ServerMessage>) {
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
                TransportEvent::Connected(_) => self.reply(ConnectResponse { value: 1 }),
                TransportEvent::Message {
                    message: ToServer::Client(ClientMessage::Login(_)),
                    ..
                } => self.reply(LoginResponse { value: 3 }),
                TransportEvent::Message {
                    message: ToServer::Client(ClientMessage::RequestCharInfo(_)),
                    ..
                } => self.reply(CharacterInfo {
                    records: Vec::new(),
                    save_failure_count: 0,
                }),
                _ => {}
            }
        }
    }
    fn elapse_time(&mut self, _dt: f32) {}
    fn single_player(&self) -> bool {
        true
    }
}

/// The record without the screenshot's name.
fn comparable(line: &str) -> String {
    let mut line = line.to_owned();
    if let Some(at) = line.find(",\"shot\":") {
        line.truncate(at);
    }
    line
}

/// The `screen` and `dump` records, but the intro base's room.
fn screens_and_dumps(lines: &[String]) -> Vec<String> {
    lines
        .iter()
        .filter(|l| l.contains("\"rec\":\"screen\"") || l.contains("\"rec\":\"dump\""))
        .filter(|l| !l.contains("\"kind\":\"room\""))
        .map(|l| comparable(l))
        .collect()
}

#[test]
fn menu_walk_runs_through_the_context() {
    let Some(ui) = ui() else { return };
    let script_path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../CppSource/OutpostFrontier/oracle/menu_walk.txt");
    let script = fs::read_to_string(&script_path).expect("menu_walk.txt");
    let starts = Rc::new(RefCell::new(0));
    let counted = Rc::clone(&starts);
    let mut start = move |_: &LocalServerRequest| -> Option<(Box<dyn Session>, LocalClientEnd)> {
        *counted.borrow_mut() += 1;
        let (client, end) = loopback();
        let world = World::new(Arc::new(GameData::default()), WorldConfig::default());
        Some((Box::new(Server { world, end }), client))
    };
    let mut t = 0.0;
    let mut cx = ClientContext::new(
        ui,
        ClientTiming::new(ManualClock::new(Some(1000))),
        Some(OracleRun {
            script: OracleScript::start(&script, "menu_walk.txt", "out"),
            out: "out".into(),
            keymap: Vec::new(),
            layout: Vec::new(),
        }),
        &mut start,
        LocalServerRequest {
            install_dir: ".".into(),
            user_data: ".".into(),
            seed: RAND_SEED,
            oracle: true,
        },
        Box::new(move || {
            t += 0.001;
            t
        }),
    );
    let map = cx.interface.services.data_file(KEY_MAP_PATH.as_bytes());
    cx.core.keymap.load_base(map.as_deref(), KEY_MAP_PATH);

    let mut states = client_states();
    states.enter(&mut cx, ClientStateId::Intro.raw());
    let mut frame = FrameState::default();
    while !cx.core.quit_posted && cx.core.frame < MAX_FRAMES {
        tick_client_frame(&mut frame, &mut states, &mut cx, FRAME_SECONDS);
    }
    let session = cx.core.oracle.as_ref().unwrap().script.session().to_vec();
    assert!(cx.core.quit_posted, "the script ends: {session:#?}");
    assert_eq!(*starts.borrow(), 1, "one server for the whole walk");
    let ours = screens_and_dumps(&session);
    // The walk's screens: the intro, the column, Load Game, the column again
    // and Options, then the quit.
    let ids: Vec<&str> = ours
        .iter()
        .filter(|l| l.contains("\"rec\":\"screen\""))
        .map(|l| &l[l.find("\"kind\"").unwrap()..])
        .collect();
    assert_eq!(
        ids,
        [
            r#""kind":"client","id":6}"#,
            r#""kind":"menu","id":1}"#,
            r#""kind":"menu","id":3}"#,
            r#""kind":"menu","id":2}"#,
            r#""kind":"menu","id":18}"#,
            r#""kind":"menu","id":1}"#,
            r#""kind":"menu","id":10}"#,
            r#""kind":"client","id":1}"#,
        ]
    );

    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let text = fs::read_to_string(dir.join("session.jsonl")).expect("session.jsonl");
    let cpp: Vec<String> = text.lines().map(String::from).collect();
    assert_eq!(ours, screens_and_dumps(&cpp));
    println!("the {} screen and dump records are equal", ours.len());
}
