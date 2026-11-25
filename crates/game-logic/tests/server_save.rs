//! The server's saves over the original data (`outdir/`, the rebranded
//! he01 and he02): the autosave when a player's ship lands on a
//! multiplayer server (`SystemPlayerView_OnObjectLanded`), the autosave at
//! the end of a player's gate jump (`IServerImpl::SystemSwitchOutComplete`),
//! and single player's `pub::Save` (the autosave of type 1 with its skip
//! latch and `autosave_forbidden`, the mission save with its `Save%04x`
//! name and the copy over `AutoSave.fl`), with `IServerImpl::SaveGame`,
//! `MissionSaveB` and `CharacterSkipAutosave` through the receive path.
//!
//! Each written file is read back with the server's reader
//! (`PlayerData_LoadSaveRecord`) and checked against what the C++'s
//! `PlayerData_SerializeToFile` writes for the same slot
//! (`Server/server_pub.cpp:1179`, `server_savegame.cpp:688-806`): the
//! description and `tstamp` from the local clock, the system, the base or
//! the ship's position, the ship, the cargo, the money and the reputation.
//! The text itself is the P3.2 writer's, which the `saves_written` data
//! dump compares with the C++'s byte for byte (`save_writer.rs`).

use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::Arc;

use game_logic::api::{misc, player as pp};
use game_logic::character_file::{SaveData, SavedCharacter, load_save_record};
use game_logic::login::{self, CreateCharacterInfo, LoginInfo, ServerFiles};
use game_logic::player::CharacterId;
use game_logic::saves::{self, FixedClock, SystemTime};
use game_logic::selection::select_character;
use game_logic::server::{ClientSink, CreateParams, Cx, Logic};
use game_logic::{dock, receive, transfer, world};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::data::equip_desc::EquipDesc;
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use game_sim::protocol::to_server::{MissionSaveB, SkipAutosave};
use game_sim::protocol::{ClientMessage, ToClient, ToServer};
use neuron_core::id::create_id;
use neuron_core::msvcrt::{format_g, text_mode_read};
use neuron_core::physics::StepMode;
use neuron_core::util::wide_hex::wide_to_hex;
use neuron_core::vfs::Host;
use neuron_server::account::{AccountsRoot, hashed_name};
use neuron_server::controller::{self, Priority};
use neuron_server::save::read_save;

#[derive(Clone, Default)]
struct Shared(Rc<RefCell<Vec<(u32, ToClient)>>>);

impl ClientSink for Shared {
    fn send(&mut self, client: u32, message: ToClient) {
        self.0.borrow_mut().push((client, message));
    }
}

/// 2026-11-22 13:05:09.250, a Sunday.
const NOW: SystemTime = SystemTime {
    year: 2026,
    month: 11,
    day_of_week: 0,
    day: 22,
    hour: 13,
    minute: 5,
    second: 9,
    milliseconds: 250,
};

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// The data, the server's files under a fresh user-data folder, the world
/// and a logic whose saves go to that folder with the clock at [`NOW`].
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
    let mut logic = Logic::new(Box::new(Shared::default()), 2, multiplayer);
    logic.deliver_hook = Some(|_, _, _, _| {});
    logic.saves.accounts = Some(files.accounts.clone());
    logic.saves.clock = Box::new(FixedClock(NOW));
    Some((world, logic, files))
}

fn content(cx: &mut Cx<'_>) {
    controller::create(
        cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        Priority::Highest,
    );
}

/// A multiplayer account `account` with a new character `pilot` at
/// he01_01_Base, selected in session 1. Returns the character's id, which
/// is its file's name.
fn mp_character(cx: &mut Cx<'_>, files: &ServerFiles, account: &str, pilot: &str) -> CharacterId {
    let info = LoginInfo {
        account_name: account.encode_utf16().collect(),
        create: true,
    };
    assert_eq!(login::login(cx, files, &info, 1), 3);
    let create = CreateCharacterInfo {
        name: pilot.encode_utf16().collect(),
        faction: create_id(b"new_player"),
        base: create_id(b"he01_01_base"),
        package: create_id(b"ge_fighter"),
        pilot: create_id(b"rellan"),
    };
    assert!(login::create_character(cx, files, 1, &create));
    let id = CharacterId::new(format!("{}.fl", hashed_name(&create.name)).as_bytes());
    assert!(select_character(cx, files, 1, &id));
    id
}

/// The slot leaves its base as `IServerImpl::BaseExit` leaves it (not
/// ported yet: the base it left kept, no base), then launches its ship
/// (`RequestCreateShip`, `PlayerLaunch`). Returns the ship's id.
fn launch(cx: &mut Cx<'_>) -> u32 {
    {
        let slot = cx.logic.players.get_mut(1).unwrap();
        slot.exited_base_id = slot.current_base;
        slot.current_base = 0;
    }
    dock::request_create_ship(cx, 1);
    let ship_id = cx.logic.players.get(1).unwrap().created_ship_id;
    dock::player_launch(cx, ship_id, 1);
    assert_eq!(cx.logic.players.get(1).unwrap().ship_object_id, ship_id);
    ship_id
}

/// The file's text as the server reads it, and the record its reader
/// makes of it.
fn read_back(cx: &mut Cx<'_>, path: &Path) -> (Vec<u8>, SavedCharacter) {
    let bytes = std::fs::read(path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    // Plain text written in text mode: every `\n` follows a `\r`.
    assert!(bytes.starts_with(b"\r\n[Player]\r\n"), "{}", path.display());
    assert!(bytes.windows(2).all(|w| w[1] != b'\n' || w[0] == b'\r'));
    let text = read_save(&bytes)
        .text()
        .expect("a plain-text save")
        .to_vec();
    assert_eq!(text, text_mode_read(&bytes));
    let data = cx.world.data_arc();
    let mut record = SavedCharacter::default();
    let mut reports = game_sim::data::Reports::default();
    let save = SaveData {
        universe: &data.universe,
        archetypes: &data.archetypes,
        reputation: &data.reputation,
        costumes: &data.costumes,
    };
    assert!(load_save_record(
        &text,
        &path.display().to_string(),
        &mut record,
        save,
        cx.world.strings_mut(),
        &mut reports
    ));
    (text, record)
}

fn has_line(text: &[u8], line: &str) -> bool {
    text.split(|&b| b == b'\n').any(|l| l == line.as_bytes())
}

/// `%g` read back as the reader reads it.
fn g(value: f32) -> f32 {
    format_g(f64::from(value), 6).parse().unwrap()
}

fn items(list: &[EquipDesc], with_mission: bool) -> Vec<(u32, i32, u8, u32, bool)> {
    list.iter()
        .filter(|e| with_mission || !e.mission)
        .map(|e| {
            (
                e.arch_id,
                e.count,
                e.equipped,
                e.health.to_bits(),
                e.mission,
            )
        })
        .collect()
}

/// The fields every save carries from the slot: the ship, the cargo and
/// equipment (on a multiplayer server without the mission items), the
/// money, the system and the reputation (the vibe's affiliation and
/// feelings).
fn check_slot_fields(cx: &mut Cx<'_>, record: &SavedCharacter, multiplayer: bool) {
    let slot = cx.logic.players.get(1).unwrap().clone();
    assert_eq!(record.ship_archetype, slot.ship_archetype);
    assert_eq!(record.money, slot.cash);
    assert_eq!(record.system_id, slot.system_id);
    assert_eq!(
        items(&record.equipment, true),
        items(&slot.equipment, !multiplayer)
    );
    let summary = cx.world.vibes().0.summary(slot.rep_vibe).expect("a vibe");
    assert_eq!(record.rep_group, summary.affiliation);
    let relations: Vec<(u32, f32)> = record
        .relations
        .iter()
        .map(|r| (r.group_id, r.feeling))
        .collect();
    let want: Vec<(u32, f32)> = summary
        .relations
        .iter()
        .map(|&(group, feeling)| (group, g(feeling)))
        .collect();
    assert_eq!(relations, want);
    assert!(!relations.is_empty());
}

/// The `tstamp` line: `SystemTimeToFileTime` of the local time, the high
/// half first.
fn tstamp_line() -> String {
    let (high, low) = NOW.file_time();
    format!("tstamp = {high},{low}")
}

fn nickname(cx: &Cx<'_>, system: u32) -> String {
    let s = &cx.world.data().universe.systems[&system];
    String::from_utf8_lossy(cx.world.strings().get(s.nickname)).into_owned()
}

#[test]
fn a_multiplayer_landing_autosaves_the_character() {
    let Some((mut world, mut logic, files)) = setup("server-save-land", true) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    content(&mut cx);
    let he01 = create_id(b"he01");
    let base = create_id(b"he01_01_Base");
    let id = mp_character(&mut cx, &files, "Lander", "Lander");
    let path = files
        .accounts
        .account_dir(&"Lander".encode_utf16().collect::<Vec<_>>())
        .join(String::from_utf8_lossy(id.as_bytes()).as_ref());
    let before = std::fs::read(&path).expect("the new character's file");
    let ship_id = launch(&mut cx);
    let ship = cx.world.find(ship_id).unwrap();

    // Cargo and money in space.
    let gold = create_id(b"commodity_gold");
    pp::add_cargo(&mut cx, 1, gold, 7, 1.0, false).unwrap();
    pp::add_cargo(&mut cx, 1, gold, 2, 1.0, true).unwrap();
    // `AddCargo`'s mission flag keeps the line apart; the line is marked a
    // mission item as a mission's own cargo is.
    for item in &mut cx.logic.players.get_mut(1).unwrap().equipment {
        if item.arch_id == gold && item.count == 2 {
            item.mission = true;
        }
    }
    pp::adjust_cash(&mut cx, 1, 1234).unwrap();
    assert_eq!(
        std::fs::read(&path).unwrap(),
        before,
        "nothing is saved before the landing"
    );

    // The forced landing queues the ship; the system's frame lands it and,
    // before the client hears `LAND`, writes the character's file: in
    // space, so no base and the ship's position.
    dock::force_land(&mut cx, base, 1);
    let at = cx.world.get(ship).unwrap().engine.position;
    world::step_system(&mut cx, he01, 0.01);
    let (text, record) = read_back(&mut cx, &path);
    let description: Vec<u16> = "11/22/26 13:05:09".encode_utf16().collect();
    assert!(
        has_line(
            &text,
            &format!("description = {}", wide_to_hex(&description))
        ),
        "{}",
        String::from_utf8_lossy(&text)
    );
    assert_eq!(record.description, description);
    assert_eq!(record.descrip_str_id, 0);
    assert!(!text.windows(13).any(|w| w == b"descrip_strid"));
    assert!(has_line(&text, &tstamp_line()));
    assert_eq!(
        (record.timestamp_high, record.timestamp_low),
        NOW.file_time()
    );
    assert!(has_line(
        &text,
        &format!("system = {}", nickname(&cx, he01))
    ));
    assert_eq!(record.system_id, he01);
    assert_eq!(record.base_id, 0);
    assert!(!text.windows(7).any(|w| w == b"\nbase ="));
    assert_eq!(record.position, [g(at.x), g(at.y), g(at.z)]);
    assert_eq!(record.last_base_id, base);
    check_slot_fields(&mut cx, &record, true);
    // The mission item stays out of a multiplayer save; the rest is the
    // ship's hold.
    let gold_lines: Vec<i32> = record
        .equipment
        .iter()
        .filter(|e| e.arch_id == gold)
        .map(|e| e.count)
        .collect();
    assert_eq!(gold_lines, [7]);
    // The slot's record is the one the writer refreshed, with the position
    // it wrote before `%g` rounded it.
    let refreshed = login::saved_character(&cx, 1, &id).unwrap();
    assert_eq!(refreshed.position, [at.x, at.y, at.z]);
    assert_eq!(refreshed.system_id, he01);
    assert_eq!(refreshed.money, record.money);
    // The character list's reader loads the file.
    assert!(login::load_saved_character(
        &mut cx,
        1,
        &files
            .accounts
            .account_dir(&"Lander".encode_utf16().collect::<Vec<_>>()),
        id.as_bytes()
    ));
}

#[test]
fn a_gate_jump_ends_with_an_autosave() {
    let Some((mut world, mut logic, files)) = setup("server-save-jump", true) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    content(&mut cx);
    let (he01, he02) = (create_id(b"he01"), create_id(b"he02"));
    let id = mp_character(&mut cx, &files, "Jumper", "Jumper");
    let dir = files
        .accounts
        .account_dir(&"Jumper".encode_utf16().collect::<Vec<_>>());
    let path = dir.join(String::from_utf8_lossy(id.as_bytes()).as_ref());
    let ship_id = launch(&mut cx);
    let ship = cx.world.find(ship_id).unwrap();
    let gate = cx.world.find(create_id(b"he01_to_he02")).unwrap();
    pp::adjust_cash(&mut cx, 1, 777).unwrap();
    let money = cx.logic.players.get(1).unwrap().cash;
    let before = std::fs::read(&path).unwrap();

    // The gate completes the dock (no save), then the client's jump-out
    // report moves the ship and the player to he02 and saves.
    dock::complete_dock(&mut cx, he01, ship, gate);
    assert_eq!(std::fs::read(&path).unwrap(), before);
    transfer::system_switch_out_complete(&mut cx, ship_id, 1);
    let at = cx.world.get(ship).unwrap().engine.position;
    let (text, record) = read_back(&mut cx, &path);
    assert!(has_line(
        &text,
        &format!("system = {}", nickname(&cx, he02))
    ));
    assert_eq!(record.system_id, he02);
    assert_eq!(record.base_id, 0);
    assert_eq!(record.position, [g(at.x), g(at.y), g(at.z)]);
    assert_eq!(record.money, money);
    assert!(has_line(&text, &tstamp_line()));
    check_slot_fields(&mut cx, &record, true);
    assert_eq!(cx.logic.players.get(1).unwrap().system_id, he02);

    // Without a selected character a multiplayer autosave writes nothing;
    // nor does a slot in no system.
    std::fs::remove_file(&path).unwrap();
    cx.logic.players.get_mut(1).unwrap().system_id = 0;
    assert!(!saves::auto_save(&mut cx, 1));
    cx.logic.players.get_mut(1).unwrap().system_id = he02;
    let selected = std::mem::take(&mut cx.logic.players.get_mut(1).unwrap().selected_character);
    assert!(!saves::auto_save(&mut cx, 1));
    assert!(!path.exists());
    cx.logic.players.get_mut(1).unwrap().selected_character = selected;
    assert!(saves::auto_save(&mut cx, 1));
    assert!(path.exists());
    // `pub::Save` is single player's only.
    std::fs::remove_file(&path).unwrap();
    misc::save(&mut cx, 1, 1);
    misc::save(&mut cx, 1, 0x1234);
    assert!(
        std::fs::read_dir(&dir).unwrap().all(|e| !e
            .unwrap()
            .file_name()
            .to_string_lossy()
            .ends_with(".fl")),
        "no save in the account's folder"
    );
}

#[test]
fn single_player_pub_save_writes_the_autosave_and_mission_saves() {
    let Some((mut world, mut logic, files)) = setup("server-save-sp", false) else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    content(&mut cx);
    let info = LoginInfo {
        account_name: "Solo".encode_utf16().collect(),
        create: true,
    };
    assert_eq!(login::login(&mut cx, &files, &info, 1), 3);
    assert!(select_character(
        &mut cx,
        &files,
        1,
        &CharacterId::new(b"Restart.fl")
    ));
    let dir = files.accounts.account_dir(&info.account_name);
    let (he01, base) = (create_id(b"he01"), create_id(b"he01_01_Base"));
    {
        let slot = cx.logic.players.get_mut(1).unwrap();
        slot.current_base = base;
        slot.system_id = he01;
    }
    let gold = create_id(b"commodity_gold");
    // Docked, `AddCargo`'s mission flag only keeps the line apart; the
    // item is marked a mission item as a mission's own cargo is.
    pp::add_cargo(&mut cx, 1, gold, 3, 1.0, true).unwrap();
    for item in &mut cx.logic.players.get_mut(1).unwrap().equipment {
        if item.arch_id == gold {
            item.mission = true;
        }
    }
    pp::adjust_cash(&mut cx, 1, 4500).unwrap();
    cx.logic.clock.base = 1234.5;

    // Type 1: the autosave, `AutoSave.fl` with the text id 0x65c and no
    // description, docked at the base, the game time and the feelings of
    // the single-player state after the player.
    misc::save(&mut cx, 1, 1);
    let autosave = dir.join("AutoSave.fl");
    let (text, record) = read_back(&mut cx, &autosave);
    assert!(has_line(&text, "descrip_strid = 1628"));
    assert!(!text.windows(11).any(|w| w == b"description"));
    assert_eq!(record.descrip_str_id, saves::AUTOSAVE_STRID);
    assert!(has_line(&text, &tstamp_line()));
    assert!(has_line(
        &text,
        &format!("system = {}", nickname(&cx, he01))
    ));
    let base_name = String::from_utf8_lossy(
        cx.world
            .strings()
            .get(cx.world.data().universe.bases[&base].nickname),
    )
    .into_owned();
    assert!(has_line(&text, &format!("base = {base_name}")));
    assert_eq!(record.base_id, base);
    assert_eq!(record.system_id, he01);
    check_slot_fields(&mut cx, &record, false);
    // Single player keeps the mission cargo.
    assert!(
        record
            .equipment
            .iter()
            .any(|e| e.arch_id == gold && e.mission)
    );
    assert!(has_line(&text, "[Time]"));
    assert!(has_line(
        &text,
        &format!("seconds = {}", format_g(1234.5, 6))
    ));
    assert!(has_line(&text, "[Group]"));
    // The save is read back into the slot's characters, and the list shows
    // it as the autosave.
    let key = CharacterId::new(b"AutoSave.fl");
    assert!(login::saved_character(&cx, 1, &key).is_some());
    let set = login::build_character_set(&mut cx, 1).unwrap();
    let entry = set
        .characters
        .iter()
        .find(|c| c.id == key)
        .expect("the autosave in the list");
    assert!(entry.is_autosave);
    assert_eq!(entry.descrip_str_id, 0x65c);

    // The skip latch: the next autosave writes nothing and clears it.
    std::fs::remove_file(&autosave).unwrap();
    let skip = ToServer::Client(ClientMessage::SkipAutosave(SkipAutosave {}));
    assert!(receive::deliver(&mut cx, 1, &skip));
    assert!(cx.logic.players.get(1).unwrap().skip_next_autosave);
    misc::save(&mut cx, 1, 1);
    assert!(!autosave.exists());
    assert!(!cx.logic.players.get(1).unwrap().skip_next_autosave);
    misc::save(&mut cx, 1, 1);
    assert!(autosave.exists());
    // A base that forbids autosaves writes nothing (and clears the latch).
    std::fs::remove_file(&autosave).unwrap();
    cx.logic.players.get_mut(1).unwrap().current_base = create_id(b"he05_01_Base");
    assert!(cx.world.data().universe.bases[&create_id(b"he05_01_Base")].autosave_forbidden);
    assert!(!saves::auto_save(&mut cx, 1));
    assert!(!autosave.exists());
    cx.logic.players.get_mut(1).unwrap().current_base = base;

    // Any other type is a mission save: a fresh `Save%04x.fl` with the type
    // as its text id, copied over `AutoSave.fl`.
    let listed = |dir: &Path| -> Vec<String> {
        let mut names: Vec<String> = std::fs::read_dir(dir)
            .unwrap()
            .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
            .collect();
        names.sort();
        names
    };
    let before = listed(&dir);
    misc::save(&mut cx, 1, 0x4321);
    let after = listed(&dir);
    let new: Vec<&String> = after.iter().filter(|n| !before.contains(n)).collect();
    assert_eq!(new.len(), 2, "{after:?}");
    assert!(new.iter().any(|n| *n == "AutoSave.fl"));
    let mission = new.iter().find(|n| **n != "AutoSave.fl").unwrap();
    let digits = mission
        .strip_prefix("Save")
        .and_then(|n| n.strip_suffix(".fl"))
        .expect("Save%04x.fl");
    assert!(digits.len() >= 4 && u32::from_str_radix(digits, 16).is_ok());
    let mission_bytes = std::fs::read(dir.join(mission)).unwrap();
    assert_eq!(std::fs::read(&autosave).unwrap(), mission_bytes);
    let (text, record) = read_back(&mut cx, &dir.join(mission));
    assert!(has_line(&text, &format!("descrip_strid = {}", 0x4321)));
    assert_eq!(record.base_id, base);
    check_slot_fields(&mut cx, &record, false);
    assert!(login::saved_character(&cx, 1, &CharacterId::new(mission.as_bytes())).is_some());

    // `MissionSaveB` from the client: another mission save.
    let before = listed(&dir);
    let packet = ToServer::Client(ClientMessage::MissionSaveB(MissionSaveB { value: 0x77 }));
    assert!(receive::deliver(&mut cx, 1, &packet));
    let after = listed(&dir);
    let new: Vec<&String> = after.iter().filter(|n| !before.contains(n)).collect();
    assert_eq!(new.len(), 1, "{after:?}");
    let (text, _) = read_back(&mut cx, &dir.join(new[0]));
    assert!(has_line(&text, "descrip_strid = 119"));
    assert_eq!(
        std::fs::read(&autosave).unwrap(),
        std::fs::read(dir.join(new[0])).unwrap()
    );

    // `IServerImpl::SaveGame`: the player's own save, named by the
    // character id up to its first `.`, with the description.
    let description: Vec<u16> = "Before the run".encode_utf16().collect();
    saves::server_save_game(&mut cx, b"MySave.fl", Some(&description), 1);
    let (text, record) = read_back(&mut cx, &dir.join("MySave.fl"));
    assert_eq!(record.description, description);
    assert!(!text.windows(13).any(|w| w == b"descrip_strid"));
    assert!(login::saved_character(&cx, 1, &CharacterId::new(b"MySave.fl")).is_some());
    // Not from a ship.
    cx.logic.players.get_mut(1).unwrap().ship_object_id = 5;
    saves::server_save_game(&mut cx, b"InShip", Some(&description), 1);
    assert!(!dir.join("InShip.fl").exists());
}

#[test]
fn a_server_without_an_accounts_folder_writes_nothing() {
    let Some((mut world, mut logic, files)) = setup("server-save-none", false) else {
        return;
    };
    logic.saves.accounts = None;
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let info = LoginInfo {
        account_name: "Nobody".encode_utf16().collect(),
        create: true,
    };
    assert_eq!(login::login(&mut cx, &files, &info, 1), 3);
    let dir = files.accounts.account_dir(&info.account_name);
    let before = std::fs::read_dir(&dir).unwrap().count();
    misc::save(&mut cx, 1, 1);
    misc::save(&mut cx, 1, 9);
    assert_eq!(std::fs::read_dir(&dir).unwrap().count(), before);
    assert_eq!(cx.logic.players.get(1).unwrap().save_failure_count, 0);
}
