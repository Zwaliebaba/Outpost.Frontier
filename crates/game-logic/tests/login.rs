//! The single-player account flow (work package P3.2): `PlayerDB::login` with account
//! creation (the restart file), the character list, a character created
//! from the multiplayer template, and the restart file refused for
//! deletion, against `Server.dll` as `OracleSinglePlayerAccount` runs it
//! (`CppSource/Server/server_savegame.cpp`, `Oracle.exe dump`, kind
//! `accounts`).
//!
//! `OUTPOST_DATA_DIR` names the dump root (`EXE` and `DATA`) and
//! `OUTPOST_ORACLE_DIR` the oracle's output; without the first the test
//! prints a notice and passes. The accounts folder is a fresh folder under
//! the target directory. The test on `outdir/` runs the same flow.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::login::{
    self, CreateCharacterInfo, LoginInfo, ServerFiles, build_character_set, create_new_character,
    destroy_character,
};
use game_logic::player::CharacterId;
use game_logic::server::{Cx, Logic, RecordingSink};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::WorldConfig;
use neuron_core::id::create_id;
use neuron_core::vfs::Host;
use neuron_server::account::{AccountsRoot, hashed_name};

fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut out, b| {
        let _ = write!(out, "{b:02x}");
        out
    })
}

fn wide_hex(units: &[u16]) -> String {
    units.iter().fold(String::new(), |mut out, u| {
        let _ = write!(out, "{u:04x}");
        out
    })
}

fn line(out: &mut Vec<String>, key: &str, value: &str) {
    out.push(format!("{{\"k\":\"{key}\",\"v\":\"{value}\"}}"));
}

fn file_lines(out: &mut Vec<String>, key: &str, path: &Path) {
    let Ok(bytes) = std::fs::read(path) else {
        line(out, key, "missing");
        return;
    };
    for part in bytes.split_inclusive(|&b| b == b'\n') {
        line(out, key, &hex(part));
    }
}

fn character_set(out: &mut Vec<String>, cx: &mut Cx<'_>) {
    let set = build_character_set(cx, 1).expect("slot");
    for c in &set.characters {
        line(
            out,
            "character",
            &format!(
                "{},{},{},{},{},{:08x},{},{:08x},{:08x},{:08x},{},{:08x}",
                hex(c.id.as_bytes()),
                u32::from(c.is_restart),
                u32::from(c.is_autosave),
                wide_hex(&c.name),
                c.descrip_str_id,
                c.ship_archetype,
                c.money,
                c.system_id,
                c.base_id,
                c.location_id,
                c.rank,
                c.voice_id
            ),
        );
    }
    line(
        out,
        "save_failure_count",
        &set.save_failure_count.to_string(),
    );
}

/// The flow over the data and templates of `exe` and `data_dir`, with the
/// accounts under `user_data`: the lines `OracleSinglePlayerAccount`
/// writes.
fn run(exe: &Path, data_dir: &Path, user_data: &Path) -> Option<Vec<String>> {
    let config = DataConfig::read(&Host::new(exe), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(data_dir), &config, None);
    let ini = std::fs::read(exe.join("OutpostFrontier.ini")).ok()?;
    let files = ServerFiles {
        exe_dir: exe.to_path_buf(),
        data_dir: data_dir.to_path_buf(),
        initial_world: ServerFiles::read_initial_world(&ini),
        accounts: AccountsRoot::new(user_data, false),
    };
    let account: Vec<u16> = "OracleLogin".encode_utf16().collect();
    let dir = files.accounts.account_dir(&account);
    let _ = std::fs::remove_dir_all(&dir);

    let mut world = World::new(
        Arc::new(data),
        WorldConfig {
            single_player: true,
            ..WorldConfig::default()
        },
    );
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 2, false);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let mut out = Vec::new();
    let info = LoginInfo {
        account_name: account,
        create: true,
    };
    let result = login::login(&mut cx, &files, &info, 1);
    line(&mut out, "login", &result.to_string());
    file_lines(&mut out, "restart", &dir.join("Restart.fl"));
    character_set(&mut out, &mut cx);

    let create = CreateCharacterInfo {
        name: "Oracle Pilot".encode_utf16().collect(),
        faction: create_id(b"new_player"),
        base: create_id(b"he01_01_base"),
        package: create_id(b"ge_fighter"),
        pilot: create_id(b"rellan"),
    };
    cx.world.set_multiplayer(true);
    let created = create_new_character(&mut cx, &files, 1, &create);
    cx.world.set_multiplayer(false);
    line(&mut out, "created", &u32::from(created).to_string());
    file_lines(
        &mut out,
        "created_file",
        &dir.join(format!("{}.fl", hashed_name(&create.name))),
    );
    character_set(&mut out, &mut cx);
    let destroyed = destroy_character(&mut cx, &files, 1, &CharacterId::new(b"Restart.fl"));
    line(
        &mut out,
        "destroy_restart",
        &u32::from(destroyed).to_string(),
    );
    Some(out)
}

fn scratch(name: &str) -> PathBuf {
    let dir = Path::new(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = std::fs::create_dir_all(&dir);
    dir
}

#[test]
fn data_dump_accounts() {
    let Some(root) = std::env::var_os("OUTPOST_DATA_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_DATA_DIR is not set; the accounts data dump was skipped");
        return;
    };
    let Some(lines) = run(
        &root.join("EXE"),
        &root.join("DATA"),
        &scratch("accounts-dump"),
    ) else {
        println!("notice: no EXE/OutpostFrontier.ini under the dump root");
        return;
    };
    let Some(oracle) = std::env::var_os("OUTPOST_ORACLE_DIR").map(PathBuf::from) else {
        println!("notice: OUTPOST_ORACLE_DIR is not set; nothing compared");
        return;
    };
    let listed = std::fs::read_to_string(oracle.join("accounts/files.txt")).unwrap_or_default();
    assert_eq!(listed.lines().collect::<Vec<_>>(), ["single_player"]);
    let want = std::fs::read_to_string(oracle.join("accounts/single_player.jsonl"))
        .expect("accounts/single_player.jsonl");
    let want: Vec<&str> = want.lines().collect();
    let mut problems = Vec::new();
    if want.len() != lines.len() {
        problems.push(format!(
            "{} lines, the oracle has {}",
            lines.len(),
            want.len()
        ));
    }
    for (i, (w, g)) in want.iter().zip(&lines).enumerate() {
        if w != g {
            problems.push(format!("line {}: oracle {w}, port {g}", i + 1));
        }
    }
    for p in problems.iter().take(25) {
        println!("{p}");
    }
    assert!(
        problems.is_empty(),
        "{} differences in the accounts dump",
        problems.len()
    );
    println!("accounts: the flow equals the oracle");
}

#[test]
fn the_account_flow_runs_on_outdir() {
    let outdir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let Some(lines) = run(&outdir, &outdir.join("data"), &scratch("accounts-outdir")) else {
        println!("outdir/ is missing: no accounts");
        return;
    };
    // A new account logs in with its restart file as its one character.
    assert_eq!(lines[0], "{\"k\":\"login\",\"v\":\"3\"}");
    assert!(lines.iter().any(|l| l.starts_with("{\"k\":\"restart\"")));
    let characters: Vec<&String> = lines
        .iter()
        .filter(|l| l.starts_with("{\"k\":\"character\""))
        .collect();
    assert_eq!(characters.len(), 2, "{lines:#?}");
    assert!(characters[0].contains(&hex(b"Restart.fl")));
    // The created file exists; outside a multiplayer server its key gets a
    // second `.fl`, so it does not load and the list stays as it was.
    assert!(lines.contains(&"{\"k\":\"created\",\"v\":\"1\"}".to_owned()));
    assert!(
        lines
            .iter()
            .any(|l| l.starts_with("{\"k\":\"created_file\"") && !l.contains("missing"))
    );
    assert_eq!(
        lines.last().unwrap(),
        "{\"k\":\"destroy_restart\",\"v\":\"0\"}"
    );
    let _ = login::ServerFiles::read_initial_world(b"");
}

/// A multiplayer server over `outdir/`: an account made at login, a
/// character created from `MPNewCharacter.fl` and found again by a fresh
/// scan of the folders, the character deleted, and a locked and a banned
/// account refused.
#[test]
fn multiplayer_accounts_on_outdir() {
    let outdir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let Some(config) = DataConfig::read(&Host::new(&outdir), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no accounts");
        return;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir.join("data")), &config, None);
    let ini = std::fs::read(outdir.join("OutpostFrontier.ini")).expect("ini");
    let user_data = scratch("accounts-mp");
    let _ = std::fs::remove_dir_all(user_data.join("Accts"));
    let files = ServerFiles {
        exe_dir: outdir.clone(),
        data_dir: outdir.join("data"),
        initial_world: ServerFiles::read_initial_world(&ini),
        accounts: AccountsRoot::new(&user_data, true),
    };
    let data = Arc::new(data);
    let world_config = WorldConfig {
        single_player: false,
        ..WorldConfig::default()
    };
    let mut world = World::new(Arc::clone(&data), world_config);
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 2, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let account: Vec<u16> = "Wing".encode_utf16().collect();
    let info = LoginInfo {
        account_name: account.clone(),
        create: true,
    };
    // Made at login: the folder is the hashed name, with its `name` file.
    assert_eq!(login::login(&mut cx, &files, &info, 1), 3);
    assert!(files.accounts.account_dir(&account).join("name").is_file());
    assert_eq!(
        login::login(&mut cx, &files, &info, 2),
        1,
        "logged in already"
    );
    let create = CreateCharacterInfo {
        name: "Ace".encode_utf16().collect(),
        faction: create_id(b"new_player"),
        base: create_id(b"he01_01_base"),
        package: create_id(b"ge_fighter"),
        pilot: create_id(b"rellan"),
    };
    assert!(login::create_character(&mut cx, &files, 1, &create));
    assert!(
        !login::create_character(&mut cx, &files, 1, &create),
        "name taken"
    );
    let key = CharacterId::new(format!("{}.fl", hashed_name(&create.name)).as_bytes());
    let record = login::saved_character(&cx, 1, &key).expect("the new character loads");
    assert_eq!(record.name, create.name);
    assert_eq!(record.money, 2000);

    // A fresh scan finds the account and the character's name.
    let scanned = login::scan_registry(&files);
    let id = scanned.find_by_name(&account).expect("account");
    assert_eq!(
        scanned.get(id).unwrap().characters,
        std::slice::from_ref(&create.name)
    );
    assert_eq!(scanned.find_by_character_name(&create.name), Some(id));

    assert!(destroy_character(&mut cx, &files, 1, &key));
    assert_eq!(cx.logic.accounts.find_by_character_name(&create.name), None);
    assert!(login::saved_character(&cx, 1, &key).is_none());

    // Locked, then banned, on a server that has just scanned its folders.
    let mut world = World::new(Arc::clone(&data), world_config);
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 2, true);
    logic.accounts = login::scan_registry(&files);
    assert_eq!(logic.accounts.lock(&account), None);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    assert_eq!(login::login(&mut cx, &files, &info, 1), 7);
    std::fs::write(files.accounts.account_dir(&account).join("banned"), b"").expect("banned");
    cx.logic.accounts = login::scan_registry(&files);
    assert_eq!(login::login(&mut cx, &files, &info, 1), 0);
    // An unknown account without the create flag.
    let stranger = LoginInfo {
        account_name: "Nobody".encode_utf16().collect(),
        create: false,
    };
    assert_eq!(login::login(&mut cx, &files, &stranger, 2), 2);
}
