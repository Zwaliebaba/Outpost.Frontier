//! The client's context (`game_app::client`) driven by `menu_walk.txt`
//! through `TickClientFrame` on the original data in `outdir/`, over a
//! recording single-player server, with the start-up's interface windows
//! and the `IDR_ALWAYS_PRESENT` key map, and, with
//! `OUTPOST_CLIENT_ORACLE_DIR` set to a C++ client-oracle run of that
//! script, its `session.jsonl` and `tree.jsonl` against the run's (decision
//! O53 (6), `game_app::oracle::compare`): the screen changes, the dumps,
//! the waits and the script lines' results, `command USER_CANCEL`'s note
//! included, and every node of the nine dumps. Left out as the comparison
//! of the port's real run leaves them out: the screenshots' names (D1114),
//! the intro base's `room` (D1107) and `rtti`; and what this test's
//! interface cannot give: the text windows' heights (no fonts) and two
//! mesh-dependent fields of the group-request window (no mesh loader).

use std::cell::RefCell;
use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::Arc;

use game_app::client::{ClientContext, client_states};
use game_app::client_state::ClientStateId;
use game_app::frame::{FrameState, tick_client_frame};
use game_app::keymap::{GroupResult, KEY_MAP_PATH, KeyMapFiles};
use game_app::nn_preferences::{DeviceMode, DisplayMode, PreferencesContext};
use game_app::oracle::OracleRun;
use game_app::oracle::compare;
use game_app::oracle::script::{FRAME_SECONDS, OracleScript, RAND_SEED};
use game_app::perf_options::PerfOptions;
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

mod support;

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
    game_app::boot_interface::register(&mut ui.factory);
    game_app::boot_interface::create(&mut ui);
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

/// The key map's two files: the base map from the data, no player's map.
struct KeyMapData<'a>(&'a mut dyn UiServices);

impl KeyMapFiles for KeyMapData<'_> {
    fn base(&mut self) -> (Option<Vec<u8>>, String) {
        (
            self.0.data_file(KEY_MAP_PATH.as_bytes()),
            KEY_MAP_PATH.to_string(),
        )
    }
    fn user(&mut self) -> Option<Vec<u8>> {
        None
    }
}

/// What the C++ run's client gives the preferences window: the display
/// 1024 x 768 x 32, a `PerfOptions.ini` of version 5 with no options, a US
/// keyboard layout and a device with the gamma and environment-map
/// abilities (`tests/options_data.rs`); the context adds the key map.
fn preferences() -> PreferencesContext {
    let mut perf = PerfOptions::default();
    let file = b"[PerfVersion]\nversion=5\nSkipMachineWarnings=TRUE\n".to_vec();
    assert!(perf.load(Some(file), None).is_some(), "a current file");
    let mode = |width, height, bpp| DeviceMode {
        width,
        height,
        bpp,
        unknown_format: false,
    };
    PreferencesContext {
        key_names: support::us_key_names(),
        perf,
        display: DisplayMode {
            width: 1024,
            height: 768,
            color_bpp: 32,
        },
        device_modes: vec![
            mode(800, 600, 16),
            mode(800, 600, 32),
            mode(1024, 768, 16),
            mode(1024, 768, 32),
        ],
        desktop: (1024, 768),
        video_memory: 256 << 20,
        gamma_ability: 1,
        environment_map_ability: 1,
        max_submitted_sounds: 32,
        ..PreferencesContext::default()
    }
}

/// A tree line without what this test's interface cannot give: the text
/// windows' heights (no fonts), and the mesh bounds and hardpoints (no mesh
/// loader): the group-request window's half extents and the position of
/// its close button, which sits on its mesh's hardpoint.
fn without_fonts_and_meshes(line: &str) -> String {
    if line.contains("\"name\":\"NN_GroupControl\"") {
        compare::mask(line, "half")
    } else if line.contains("\"name\":\"BtnClose\"") {
        compare::mask(line, "pos")
    } else {
        line.to_owned()
    }
}

/// The first `n` differing lines, for the failure's message.
fn first_differences(ours: &[String], cpp: &[String], n: usize) -> String {
    ours.iter()
        .zip(cpp)
        .enumerate()
        .filter(|(_, (a, b))| a != b)
        .take(n)
        .map(|(i, (a, b))| format!("line {i}:\n  port: {a}\n  C++:  {b}\n"))
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
            audio: Vec::new(),
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
    cx.core.preferences = preferences();

    let mut states = client_states();
    states.enter(&mut cx, ClientStateId::Intro.raw());
    // `RegisterAcceleratorGroup("IDR_ALWAYS_PRESENT")`, as `run` does after
    // the intro's entry: the key map that `command USER_CANCEL` reads.
    let group = cx.core.keymap.register_group(
        b"IDR_ALWAYS_PRESENT",
        &mut KeyMapData(cx.interface.services.as_mut()),
        &mut |_, _, _| {},
    );
    assert_eq!(group, GroupResult::Done);
    let mut frame = FrameState::default();
    while !cx.core.quit_posted && cx.core.frame < MAX_FRAMES {
        tick_client_frame(&mut frame, &mut states, &mut cx, FRAME_SECONDS);
    }
    let run = cx.core.oracle.as_ref().unwrap();
    let session = run.script.session().to_vec();
    assert!(cx.core.quit_posted, "the script ends: {session:#?}");
    assert_eq!(*starts.borrow(), 1, "one server for the whole walk");
    let ours = compare::session_records(&session).lines;
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
    // The Options page takes no cancel: nothing is registered for it there.
    assert!(
        ours.iter().any(|l| l.contains(
            r#""text":"command USER_CANCEL","ok":true,"note":"id=15 active=1 vk=27 mods=0 down=0 up=-1""#
        )),
        "{ours:#?}"
    );
    let tree: Vec<String> = compare::tree_records(run.script.tree(), false, &[])
        .lines
        .iter()
        .map(|l| without_fonts_and_meshes(l))
        .collect();

    let Some(dir) = std::env::var_os("OUTPOST_CLIENT_ORACLE_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_CLIENT_ORACLE_DIR is not set; nothing was compared");
        return;
    };
    let text = fs::read_to_string(dir.join("session.jsonl")).expect("session.jsonl");
    let cpp: Vec<String> = text.lines().map(String::from).collect();
    let cpp = compare::session_records(&cpp).lines;
    assert_eq!(ours, cpp, "{}", first_differences(&ours, &cpp, 6));
    println!("session.jsonl: the {} records are equal", ours.len());

    let text = fs::read_to_string(dir.join("tree.jsonl")).expect("tree.jsonl");
    let cpp: Vec<String> = text.lines().map(String::from).collect();
    let cpp: Vec<String> = compare::tree_records(&cpp, false, &[])
        .lines
        .iter()
        .map(|l| without_fonts_and_meshes(l))
        .collect();
    assert_eq!(tree.len(), cpp.len(), "tree.jsonl's lines");
    assert!(tree == cpp, "{}", first_differences(&tree, &cpp, 6));
    println!(
        "tree.jsonl: the {} lines of the 9 dumps are equal",
        tree.len()
    );
}
