//! Character selection over the original data (`outdir/`): the restart
//! file selected in single player, and two players selecting characters
//! on a multiplayer server, with what reaches the client sink and the
//! controllers.

use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::Arc;

use game_logic::login::{self, CreateCharacterInfo, LoginInfo, ServerFiles};
use game_logic::player::CharacterId;
use game_logic::selection::select_character;
use game_logic::server::{ClientSink, CreateParams, Cx, Logic};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::WorldConfig;
use game_sim::protocol::ToClient;
use game_sim::protocol::common::CommonMessage;
use game_sim::protocol::to_client::ServerMessage;
use neuron_core::id::create_id;
use neuron_core::vfs::Host;
use neuron_server::account::{AccountsRoot, hashed_name};
use neuron_server::controller::{self, Priority};

#[derive(Clone, Default)]
struct Shared(Rc<RefCell<Vec<(u32, ToClient)>>>);

impl ClientSink for Shared {
    fn send(&mut self, client: u32, message: ToClient) {
        self.0.borrow_mut().push((client, message));
    }
}

thread_local! {
    static HEARD: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
}

fn setup(name: &str, multiplayer: bool) -> Option<(Arc<GameData>, ServerFiles)> {
    let outdir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let config = DataConfig::read(&Host::new(&outdir), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(outdir.join("data")), &config, None);
    let ini = std::fs::read(outdir.join("OutpostFrontier.ini")).ok()?;
    let user_data: PathBuf = Path::new(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = std::fs::remove_dir_all(&user_data);
    std::fs::create_dir_all(&user_data).ok()?;
    let files = ServerFiles {
        exe_dir: outdir.clone(),
        data_dir: outdir.join("data"),
        initial_world: ServerFiles::read_initial_world(&ini),
        accounts: AccountsRoot::new(&user_data, multiplayer),
    };
    Some((Arc::new(data), files))
}

fn kind(message: &ToClient) -> String {
    match message {
        ToClient::Common(m) => format!("{m:?}")
            .split(['(', ' ', '{'])
            .next()
            .unwrap()
            .to_owned(),
        ToClient::Server(m) => format!("{m:?}")
            .split(['(', ' ', '{'])
            .next()
            .unwrap()
            .to_owned(),
    }
}

#[test]
fn the_restart_file_is_selected_in_single_player() {
    let Some((data, files)) = setup("selection-sp", false) else {
        println!("outdir/ is missing: no selection");
        return;
    };
    let mut world = World::new(
        data,
        WorldConfig {
            single_player: true,
            ..WorldConfig::default()
        },
    );
    let sink = Shared::default();
    let mut logic = Logic::new(Box::new(sink.clone()), 2, false);
    logic.deliver_hook = Some(|_, _, _, message| {
        HEARD.with(|h| h.borrow_mut().push(message.name().to_owned()));
    });
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    controller::create(
        &mut cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        Priority::Highest,
    );
    let info = LoginInfo {
        account_name: "Solo".encode_utf16().collect(),
        create: true,
    };
    assert_eq!(login::login(&mut cx, &files, &info, 1), 3);
    let id = CharacterId::new(b"Restart.fl");
    assert!(select_character(&mut cx, &files, 1, &id));
    let slot = cx.logic.players.get(1).unwrap();
    assert_eq!(slot.cash, 500);
    assert_ne!(slot.rep_vibe, -1);
    assert_ne!(slot.system_id, 0);
    assert_eq!(cx.logic.clock.base, 0.0, "the restart file has no [Time]");
    // Single player sends no player list: the four ch1 states only.
    let kinds: Vec<String> = sink.0.borrow().iter().map(|(_, m)| kind(m)).collect();
    assert_eq!(
        kinds,
        [
            "SetVisitedState",
            "SetWeaponGroup",
            "SetMissionLog",
            "SetInterfaceState"
        ]
    );
    if let (_, ToClient::Common(CommonMessage::SetVisitedState(state))) = &sink.0.borrow()[0] {
        let count = u32::from_le_bytes(state.data[..4].try_into().unwrap()) as usize;
        assert_eq!(state.data.len(), 4 + 5 * count);
        // newplayer.fl has no visit lines.
        assert_eq!(count, 0);
    } else {
        panic!("visited state first");
    }
    let heard = HEARD.with(|h| h.borrow().clone());
    assert!(heard.contains(&"LoadGame".to_owned()), "{heard:?}");
    assert_eq!(heard.last().map(String::as_str), Some("PlayerLoad"));
    // The feelings came from the file's [Group] sections.
    assert!(
        cx.world
            .reputation()
            .groups()
            .any(|(_, g)| !g.feelings.is_empty())
    );
    assert!(!select_character(
        &mut cx,
        &files,
        1,
        &CharacterId::new(b"none.fl")
    ));
}

#[test]
fn players_learn_of_each_other_on_a_multiplayer_server() {
    let Some((data, files)) = setup("selection-mp", true) else {
        println!("outdir/ is missing: no selection");
        return;
    };
    let mut world = World::new(
        data,
        WorldConfig {
            single_player: false,
            ..WorldConfig::default()
        },
    );
    let sink = Shared::default();
    let mut logic = Logic::new(Box::new(sink.clone()), 2, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let mut ids = Vec::new();
    for (session, (account, pilot)) in [("One", "Alpha"), ("Two", "Bravo")].into_iter().enumerate()
    {
        let session = session as u32 + 1;
        let info = LoginInfo {
            account_name: account.encode_utf16().collect(),
            create: true,
        };
        assert_eq!(login::login(&mut cx, &files, &info, session), 3);
        let create = CreateCharacterInfo {
            name: pilot.encode_utf16().collect(),
            faction: create_id(b"new_player"),
            base: create_id(b"he01_01_base"),
            package: create_id(b"ge_fighter"),
            pilot: create_id(b"rellan"),
        };
        assert!(login::create_character(&mut cx, &files, session, &create));
        ids.push(CharacterId::new(
            format!("{}.fl", hashed_name(&create.name)).as_bytes(),
        ));
    }
    assert!(select_character(&mut cx, &files, 1, &ids[0]));
    sink.0.borrow_mut().clear();
    assert!(select_character(&mut cx, &files, 2, &ids[1]));
    let sent = sink.0.borrow();
    let to = |client: u32| -> Vec<String> {
        sent.iter()
            .filter(|(c, _)| *c == client)
            .map(|(_, m)| kind(m))
            .collect()
    };
    // Player 2: the list cleared, player 1 added with its group, rank and
    // system, itself added, then its states.
    assert_eq!(
        to(2),
        [
            "PlayerList",
            "PlayerList",
            "MiscObjUpdate",
            "MiscObjUpdate",
            "MiscObjUpdate",
            "PlayerList",
            "MiscObjUpdate",
            "MiscObjUpdate",
            "MiscObjUpdate",
            "SetVisitedState",
            "SetWeaponGroup",
            "SetMissionLog",
            "SetInterfaceState"
        ]
    );
    // Player 1 hears of player 2 only.
    assert_eq!(
        to(1),
        [
            "PlayerList",
            "MiscObjUpdate",
            "MiscObjUpdate",
            "MiscObjUpdate"
        ]
    );
    let names: Vec<Vec<u16>> = sent
        .iter()
        .filter_map(|(_, m)| match m {
            ToClient::Server(ServerMessage::PlayerList(p)) if p.kind == 1 => Some(p.name.clone()),
            _ => None,
        })
        .collect();
    let bravo: Vec<u16> = "Bravo\0".encode_utf16().collect();
    let alpha: Vec<u16> = "Alpha\0".encode_utf16().collect();
    assert_eq!(names, [alpha, bravo.clone(), bravo]);
}
