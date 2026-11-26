//! Work package P3.11 (the server's closure of P3.2 and P3.3) over the
//! original data (`outdir/`), on a multiplayer server unless said:
//!
//! - local chat reaches the owners of the objects the sender's client has
//!   been told of, then the sender (`Server_SubmitChat`,
//!   `StarSystem_CollectCreatedOwners`, `Server/server_chat.cpp:766-786`);
//! - deselecting (deleting the selected character), re-selecting and the
//!   character list take a player in space out of its system and a docked
//!   one out of its base (`PlayerDB_LeaveSystem`, `PlayerDB_LeaveBase`,
//!   `PlayerData_DeselectCharacter`, `Server/server_classes.cpp:2138-2210`,
//!   `2637-2688`, `2724-2756`, `2870-2913`, `3001-3004`);
//! - a save in space takes the ship's position from its remote physics
//!   controller (`Server/server_pub.cpp:1566-1620`);
//! - the server's creators start no remote correction, their objects having
//!   no owner player;
//! - single player's `SAVEGAME` reaches `IServerImpl::SaveGame` with the
//!   character id its single-player form carries;
//! - every damage entry sends `ObjectDamaged` and the behaviour manager's
//!   damage record (`EqObj::apply_*_entry`, `Server/server_gameobject.cpp:1528-1720`);
//! - an owner's open trade freezes the cargo's decay.
//!
//! Expected values follow the C++ logic.

use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::Arc;

use game_logic::chat::{self, LOCAL};
use game_logic::events::ControllerMessage;
use game_logic::login::{self, CreateCharacterInfo, LoginInfo, ServerFiles};
use game_logic::player::CharacterId;
use game_logic::saves::{FixedClock, SystemTime};
use game_logic::selection::select_character;
use game_logic::server::{ChatSink, ClientSink, CreateParams, Cx, Logic};
use game_logic::{dock, munitions, objects, session, world};
use game_sim::equip::{Fate, SUBOBJ_ID_ROOT};
use game_sim::object::World;
use game_sim::object::damage::DamageList;
use game_sim::object::world::{Assets, ProjectileSpawn, WorldConfig};
use game_sim::protocol::rdl::{RdlRecord, write_records};
use game_sim::protocol::to_server::SaveGame;
use game_sim::protocol::{ClientMessage, ToClient, ToServer, WireReader, WireWriter};
use neuron_core::id::create_id;
use neuron_core::math::{Matrix, Quaternion, Vector};
use neuron_core::physics::StepMode;
use neuron_core::vfs::Host;
use neuron_server::account::{AccountsRoot, hashed_name};
use neuron_server::controller::{self, Priority};

struct Sink;

impl ClientSink for Sink {
    fn send(&mut self, _client: u32, _message: ToClient) {}
}

type Sent = (u32, u32, Vec<u8>);

#[derive(Clone, Default)]
struct Chat(Rc<RefCell<Vec<Sent>>>);

impl ChatSink for Chat {
    fn send(&mut self, to: u32, from: u32, data: Vec<u8>) {
        self.0.borrow_mut().push((to, from, data));
    }
}

thread_local! {
    static HEARD: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
}

fn heard() -> Vec<String> {
    HEARD.with(|h| std::mem::take(&mut *h.borrow_mut()))
}

/// 2026-11-26 10:00:00.000, a Thursday.
const NOW: SystemTime = SystemTime {
    year: 2026,
    month: 11,
    day_of_week: 4,
    day: 26,
    hour: 10,
    minute: 0,
    second: 0,
    milliseconds: 0,
};

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn setup(name: &str, multiplayer: bool) -> Option<(World, Logic, ServerFiles)> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no {name}");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    let ini = std::fs::read(outdir().join("OutpostFrontier.ini")).ok()?;
    let user_data: PathBuf = Path::new(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = std::fs::remove_dir_all(&user_data);
    std::fs::create_dir_all(&user_data).ok()?;
    let files = ServerFiles {
        exe_dir: outdir(),
        data_dir: outdir().join("data"),
        initial_world: ServerFiles::read_initial_world(&ini),
        accounts: AccountsRoot::new(&user_data, multiplayer),
    };
    let config = if multiplayer {
        WorldConfig::default()
    } else {
        WorldConfig {
            single_player: true,
            step_mode: StepMode::SinglePlayer,
            server_physics: false,
            ..WorldConfig::default()
        }
    };
    let world = World::new(Arc::new(data), config).with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(Sink), 3, multiplayer);
    logic.deliver_hook = Some(|_, _, _, message| {
        let line = match message {
            ControllerMessage::SystemExit {
                system,
                destination,
                player,
            } => format!("SystemExit {system:x} {destination} {player}"),
            ControllerMessage::PlayerLogout { player } => format!("PlayerLogout {player}"),
            ControllerMessage::PlayerLogin { player } => format!("PlayerLogin {player}"),
            ControllerMessage::BaseExit { .. } => "BaseExit".to_owned(),
            ControllerMessage::ObjectDamaged(d) => format!(
                "ObjectDamaged {:x} type {:x} group {:x} cause {} damage {} rel {} health {}",
                d.id,
                d.equip_type,
                d.collision_group_type,
                d.cause,
                d.damage,
                d.relative_damage,
                d.relative_health
            ),
            _ => return,
        };
        HEARD.with(|h| h.borrow_mut().push(line));
    });
    logic.saves.accounts = Some(files.accounts.clone());
    logic.saves.clock = Box::new(FixedClock(NOW));
    Some((world, logic, files))
}

use game_sim::data::config::{DataConfig, GameData};

/// Content's GameSupport, which owns the NPCs and hears the bus. Returns
/// its controller id.
fn content(cx: &mut Cx<'_>) -> u32 {
    controller::create(
        cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        Priority::Highest,
    )
    .0
}

fn he01() -> u32 {
    create_id(b"he01")
}

fn w(text: &str) -> Vec<u16> {
    text.encode_utf16().collect()
}

/// Logs `account` in on `session` (making it) and makes the character
/// `pilot` at he01_01_Base. Returns its id.
fn character(
    cx: &mut Cx<'_>,
    files: &ServerFiles,
    session: u32,
    account: &str,
    pilot: &str,
) -> CharacterId {
    if cx
        .logic
        .players
        .get(session)
        .is_none_or(|s| s.client_id == 0)
    {
        let info = LoginInfo {
            account_name: w(account),
            create: true,
        };
        assert_eq!(login::login(cx, files, &info, session), 3);
    }
    let create = CreateCharacterInfo {
        name: w(pilot),
        faction: create_id(b"new_player"),
        base: create_id(b"he01_01_base"),
        package: create_id(b"ge_fighter"),
        pilot: create_id(b"rellan"),
    };
    assert!(login::create_character(cx, files, session, &create));
    CharacterId::new(format!("{}.fl", hashed_name(&create.name)).as_bytes())
}

/// The selected character of `session` leaves its base
/// (`IServerImpl::BaseExit`) and launches its ship. Returns the ship's id.
fn launch(cx: &mut Cx<'_>, session: u32) -> u32 {
    let base = cx.logic.players.get(session).unwrap().current_base;
    session::base_exit(cx, base, session);
    dock::request_create_ship(cx, session);
    let ship_id = cx.logic.players.get(session).unwrap().created_ship_id;
    dock::player_launch(cx, ship_id, session);
    assert_eq!(
        cx.logic.players.get(session).unwrap().ship_object_id,
        ship_id
    );
    ship_id
}

fn in_system(cx: &Cx<'_>, session: u32) -> bool {
    cx.logic
        .server_world
        .systems
        .get(&he01())
        .is_some_and(|s| s.has_player(session))
}

#[test]
fn local_chat_reaches_the_players_the_sender_has_created() {
    let Some((mut world, mut logic, files)) = setup("closure-local-chat", true) else {
        return;
    };
    let chat = Chat::default();
    logic.chat = Box::new(chat.clone());
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let _ = content(&mut cx);
    let mut ships = Vec::new();
    for (session, (account, pilot)) in [(1, ("Near", "Kite")), (2, ("Nigh", "Wren"))] {
        let id = character(&mut cx, &files, session, account, pilot);
        assert!(select_character(&mut cx, &files, session, &id));
        ships.push(launch(&mut cx, session));
    }
    // Player 3 is docked: it hears no local chat.
    let id = character(&mut cx, &files, 3, "Away", "Lark");
    assert!(select_character(&mut cx, &files, 3, &id));
    let message = write_records(&[RdlRecord::Text(w("hello"))], 0x400).0;

    // Before a frame the observers have created nothing on their clients:
    // only the sender gets it back.
    chat.0.borrow_mut().clear();
    chat::submit_chat(&mut cx, 1, &message, LOCAL, -1);
    let to: Vec<u32> = chat.0.borrow().iter().map(|s| s.0).collect();
    assert_eq!(to, [1]);
    let owners = chat::created_owners(&cx, he01(), 1);
    assert!(owners.is_empty(), "{owners:?}");

    // A frame creates each launched ship on the other's client (the two
    // ships are 0 m apart, inside the 5000 m of another player's).
    world::step_system(&mut cx, he01(), 0.01);
    let owners = chat::created_owners(&cx, he01(), 1);
    assert!(owners.contains(&2), "{owners:?}");
    assert!(!owners.contains(&3));
    chat.0.borrow_mut().clear();
    chat::submit_chat(&mut cx, 1, &message, LOCAL, -1);
    let sent = chat.0.borrow().clone();
    let to: Vec<u32> = sent.iter().map(|s| s.0).collect();
    let mut expected: Vec<u32> = owners.iter().map(|o| o & 0xFFFF).collect();
    expected.push(1);
    assert_eq!(to, expected);
    assert!(sent.iter().all(|s| s.1 == LOCAL && s.2 == sent[0].2));
    // The console's local chat is refused.
    chat.0.borrow_mut().clear();
    chat::submit_chat(&mut cx, 0, &message, LOCAL, he01() as i32);
    assert!(chat.0.borrow().is_empty());
}

#[test]
fn leaving_the_world_takes_a_player_out_of_its_system_and_base() {
    let Some((mut world, mut logic, files)) = setup("closure-leave", true) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let _ = content(&mut cx);
    let first = character(&mut cx, &files, 1, "Rover", "Alpha");
    let second = character(&mut cx, &files, 1, "Rover", "Bravo");
    let other = character(&mut cx, &files, 2, "Other", "Other");
    assert!(select_character(&mut cx, &files, 2, &other));

    // Re-selecting from space: the old character leaves he01 (its ship is
    // destroyed, `SystemExit` with no destination), then `PlayerLogout`
    // and `PlayerLogin` for another character.
    assert!(select_character(&mut cx, &files, 1, &first));
    let ship_id = launch(&mut cx, 1);
    let ship = cx.world.find(ship_id).unwrap();
    assert!(in_system(&cx, 1));
    let _ = heard();
    assert!(select_character(&mut cx, &files, 1, &second));
    let lines = heard();
    let exit = format!("SystemExit {:x} 0 1", he01());
    assert_eq!(
        lines.iter().take(3).cloned().collect::<Vec<_>>(),
        [
            exit.clone(),
            "PlayerLogout 1".to_owned(),
            "PlayerLogin 1".to_owned()
        ],
        "{lines:?}"
    );
    assert!(!in_system(&cx, 1));
    assert!(
        cx.world.get(ship).is_none_or(|o| o.dead)
            || cx.logic.objects.get(ship).is_some_and(|o| o.destroyed)
    );
    let slot = cx.logic.players.get(1).unwrap();
    assert_eq!(slot.selected_character, second);
    // The new character is docked at its base.
    let base = create_id(b"he01_01_Base");
    assert_eq!(slot.current_base, base);

    // The character list from a base: the base is left, and the system the
    // docked slot names (the base's) too, with its `SystemExit`.
    let _ = heard();
    let set = login::build_character_set(&mut cx, 1).expect("the list");
    assert!(!set.characters.is_empty());
    let slot = cx.logic.players.get(1).unwrap();
    assert_eq!(
        (slot.current_base, slot.current_location, slot.system_id),
        (0, 0, 0)
    );
    assert_eq!(heard().iter().filter(|l| **l == exit).count(), 1);

    // The character list from space leaves the system.
    assert!(select_character(&mut cx, &files, 1, &second));
    launch(&mut cx, 1);
    assert!(in_system(&cx, 1));
    let _ = heard();
    let _ = login::build_character_set(&mut cx, 1).expect("the list");
    assert!(!in_system(&cx, 1));
    assert_eq!(heard().first(), Some(&exit));
    assert_eq!(cx.logic.players.get(1).unwrap().system_id, 0);

    // Deleting the selected character deselects it first: it leaves
    // he01, `PlayerLogout`, `PlayerLogin`, no selection.
    assert!(select_character(&mut cx, &files, 1, &second));
    launch(&mut cx, 1);
    let _ = heard();
    assert!(login::destroy_character(&mut cx, &files, 1, &second));
    assert_eq!(
        heard(),
        [
            exit,
            "PlayerLogout 1".to_owned(),
            "PlayerLogin 1".to_owned()
        ]
    );
    assert!(!in_system(&cx, 1));
    let slot = cx.logic.players.get(1).unwrap();
    assert!(!slot.selected_character.is_valid());
    assert!(!slot.saved_characters.contains_key(&second));
    // Deselecting with nothing selected does nothing.
    login::deselect_character(&mut cx, 1);
    assert!(heard().is_empty());
}

#[test]
fn a_save_in_space_reads_the_remote_correction() {
    let Some((mut world, mut logic, files)) = setup("closure-save-pos", true) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let _ = content(&mut cx);
    let id = character(&mut cx, &files, 1, "Saver", "Saver");
    assert!(select_character(&mut cx, &files, 1, &id));
    let ship_id = launch(&mut cx, 1);
    let ship = cx.world.find(ship_id).unwrap();
    assert!(
        cx.world.rc_active(ship),
        "a player's ship follows its client"
    );
    // In space without a base (`BaseExit` keeps the slot's base; the
    // writer prints the base of a slot that has one).
    cx.logic.players.get_mut(1).unwrap().current_base = 0;
    // The writer's text (`save_text`, the body `serialize_to_file` writes
    // once the cheat gate passes; the new character's loadout is not sold
    // at its base, which the gate reports).
    let pos_line = |cx: &mut Cx<'_>| -> String {
        let written = game_logic::save_writer::save_text(cx, 1, None, 0, (0, 0)).expect("a slot");
        let text = String::from_utf8_lossy(&written.text).into_owned();
        text.lines()
            .find(|l| l.starts_with("pos = "))
            .unwrap_or_else(|| panic!("{text}"))
            .to_owned()
    };
    // No sample yet: the ship's own position.
    let own = cx.world.get(ship).unwrap().engine.position;
    let fmt = |v: Vector| {
        let g = |x: f32| neuron_core::msvcrt::format_g(f64::from(x), 6);
        format!("pos = {},{},{}", g(v.x), g(v.y), g(v.z))
    };
    assert_eq!(
        cx.world.rc_simulation(ship).map(|s| s.sample_count()),
        Some(0)
    );
    assert_eq!(pos_line(&mut cx), fmt(own));
    // A client's sample: the latest sample, not the body's position.
    let sample = Vector::new(own.x + 123.0, own.y - 45.0, own.z + 6.0);
    cx.world.rc_set(ship, sample, Quaternion::IDENTITY, 0.5);
    assert_ne!(cx.world.rc_simulation(ship).unwrap().sample_count(), 0);
    assert_eq!(pos_line(&mut cx), fmt(sample));
    // Not under remote correction: the zeros the locals hold.
    cx.world.rc_end(ship);
    assert_eq!(pos_line(&mut cx), "pos = 0,0,0");
}

#[test]
fn the_creators_start_no_remote_correction() {
    let Some((mut world, mut logic, _files)) = setup("closure-creators", true) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    world::load(&mut cx, he01());
    let at = Vector::new(0.0, 0.0, 0.0);
    let spawn = |arch: &[u8]| ProjectileSpawn {
        archetype: create_id(arch),
        owner: 77,
        position: at,
        orientation: Matrix::IDENTITY,
        velocity: Vector::new(0.0, 0.0, 10.0),
        target: None,
        angular_velocity: Vector::ZERO,
    };
    let mine =
        munitions::create_mine(&mut cx, he01(), &spawn(b"mine01_mark01_ammo")).expect("mine");
    let counter =
        munitions::create_counter_measure(&mut cx, he01(), &spawn(b"ge_s_cm_01_ammo")).expect("cm");
    for h in [mine, counter] {
        assert_eq!(cx.world.get(h).unwrap().simple.owner_player, 0);
        assert!(!cx.world.rc_active(h));
    }
}

#[test]
fn single_player_savegame_carries_its_character_id() {
    let Some((mut world, mut logic, files)) = setup("closure-savegame", false) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let info = LoginInfo {
        account_name: Vec::new(),
        create: true,
    };
    let _ = login::login(&mut cx, &files, &info, 1);
    if cx.logic.players.get(1).is_none_or(|s| s.client_id == 0) {
        println!("no single-player login: no SAVEGAME");
        return;
    }
    // The single-player form keeps the id; the wire form leaves it out.
    let packet = SaveGame {
        character_id: b"MySave.fl".to_vec(),
        description: [w("A save"), vec![0]].concat(),
    };
    for (ctx, kept) in [
        (game_sim::protocol::WireContext::SINGLE_PLAYER_CLIENT, true),
        (game_sim::protocol::WireContext::CLIENT, false),
    ] {
        let message = ToServer::Client(ClientMessage::SaveGame(packet.clone()));
        let mut writer = WireWriter::new();
        message.encode(&ctx, &mut writer).expect("encodes");
        let bytes = writer.into_bytes();
        let back = ToServer::decode(&ctx, &mut WireReader::new(&bytes)).expect("decodes");
        let ToServer::Client(ClientMessage::SaveGame(back)) = back else {
            panic!("a SAVEGAME");
        };
        assert_eq!(back.description, packet.description);
        assert_eq!(back.character_id.is_empty(), !kept);
    }
    // In a base, the session saves under the id's name up to its `.`.
    let accounts = cx.logic.saves.accounts.clone().unwrap();
    let account = cx.logic.players.get(1).unwrap().account_name.clone();
    let path = accounts.account_dir(&account).join("MySave.fl");
    let _ = std::fs::remove_file(&path);
    let message = ToServer::Client(ClientMessage::SaveGame(packet));
    assert!(session::route(&mut cx, &files, 1, &message));
    let text = std::fs::read(&path).expect("the save");
    let description = neuron_core::util::wide_hex::wide_to_hex(&w("A save"));
    let text = String::from_utf8_lossy(&text).into_owned();
    assert!(
        text.contains(&format!("description = {description}")),
        "{text}"
    );
}

#[test]
fn damage_entries_send_object_damaged_and_the_behaviour_record() {
    let Some((mut world, mut logic, _files)) = setup("closure-damaged", true) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let controller = content(&mut cx);
    world::load(&mut cx, he01());
    let info = game_logic::api::space_obj::ShipInfo::new(
        controller,
        he01(),
        create_id(b"he_fighter"),
        create_id(b"li_p_he_fighter_loadout01"),
        Vector::new(0.0, 0.0, 0.0),
        Matrix::IDENTITY,
    );
    let id = game_logic::api::space_obj::create(&mut cx, &info).expect("the ship");
    let ship = cx.world.find(id).unwrap();
    let (hp, max) = {
        let o = cx.world.get(ship).unwrap();
        (o.simple.hit_pts, o.simple.max_hit_pts)
    };
    let _ = heard();
    let mut list = DamageList::new(2, 0x1234, 0);
    list.add_damage_entry(SUBOBJ_ID_ROOT, hp - 100.0, Fate::None);
    let mut hooks = world::Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.apply_damage_list_hooked(ship, &list, &mut hooks);
    let damage = 100.0f32;
    assert_eq!(
        heard(),
        [format!(
            "ObjectDamaged {id:x} type 0 group 0 cause 2 damage {damage} rel {} health {}",
            (f64::from(damage) / f64::from(max)) as f32,
            (f64::from(hp - 100.0) / f64::from(max)) as f32
        )]
    );
    // The ship's manager accumulates a cause 2 to 7 when it runs a
    // behaviour; the record reaches it either way through
    // `World::behavior_damage_event`.
    let level = |w: &World| w.behavior_manager(ship).map(|m| m.state.sys.timers.level);
    if let Some(m) = cx.world.behavior_manager(ship)
        && m.state.behavior != -1
    {
        let before = level(cx.world).unwrap();
        cx.world.behavior_damage_event(ship, id, 3, 25.0);
        assert_eq!(level(cx.world), Some(before + 25.0));
        cx.world.behavior_damage_event(ship, id, 9, 25.0);
        assert_eq!(level(cx.world), Some(before + 25.0));
    }
    // An object that is not equipped sends nothing.
    let loot = objects::create_loot(&mut cx, he01(), 0, &[]);
    assert!(loot.is_empty());
}

#[test]
fn the_field_loads_give_their_zones_loot() {
    let Some((mut world, mut logic, _files)) = setup("closure-zone-loot", true) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    world::load(&mut cx, he01());
    let fields = cx.world.load_asteroid_fields(he01());
    assert!(fields > 0);
    let data = cx.world.data_arc();
    let mut set = 0;
    for (&id, zone) in &data.universe.zones {
        let loot = cx.world.zone_lootable(id);
        if loot != zone.lootable {
            set += 1;
        }
        let info = game_logic::api::zone::lootable_info(&mut cx, id).expect("a zone");
        assert_eq!(
            info.archetype,
            loot.map_or(0, |l| l.asteroid_loot_commodity)
        );
    }
    // he01's fields name zones whose `[LootableZone]` the shared data does
    // not hold; the world keeps it (`SetLootableInfo`).
    println!("he01: {fields} fields, {set} zones given loot");
    assert!(set > 0, "no zone of he01 took a field's loot");
}
