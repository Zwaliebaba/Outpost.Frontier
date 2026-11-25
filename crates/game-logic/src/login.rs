//! Accounts, login and the character list (work package P3.2):
//! `PlayerDB::load_user_data`, `create_account`, `create_restart_file`,
//! `PlayerData_LoginAccount`, `PlayerData_LoadSavedCharacter`,
//! `PlayerData_ReloadSavedCharacter`, `PlayerData_BuildCharacterSet`,
//! `PlayerData_DestroyCharacter` and `PlayerData_CreateNewCharacter`
//! (`Server/server_classes.cpp`, `Server/server_savegame.cpp`).
//!
//! Files are written in text mode (`"wt"`), so with CR LF, and read as
//! [`read_save`] reads them. A multiplayer server keeps its accounts in
//! [`neuron_server::registry::AccountRegistry`] (`Logic::accounts`). Of
//! the multiplayer prologue of the character list the autosave is ported
//! ([`crate::saves`]); leaving the system and the base is not yet.

use std::path::{Path, PathBuf};

use game_sim::data::config::GameData;
use neuron_core::hash::{eq_ignore_case, stricmp};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::msvcrt::{text_mode_read, text_mode_write};
use neuron_core::util::wide_hex::wide_to_hex;
use neuron_server::account::{AccountsRoot, decode_name_file, hashed_name, list_save_files};
use neuron_server::save::read_save;

use crate::character_file::{CollisionGroupDesc, SaveData, SavedCharacter, load_save_record};
use crate::player::CharacterId;
use crate::server::Cx;

const PLAYER_DB: &str = "Server\\PlayerDB.cpp";

/// The files the server reads and writes outside the data tree.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ServerFiles {
    /// The executable's folder: `NewPlayer.fl`, `NewCharacter.fl` and
    /// `MPNewCharacter.fl` (`GetModuleFileNameA`).
    pub exe_dir: PathBuf,
    /// The data folder (`NeuronRuntime::Data->GetFileSysPath`).
    pub data_dir: PathBuf,
    /// `[OutpostFrontier] initial_world` of the server INI (`g_InitialWorldPath`),
    /// relative to the data folder.
    pub initial_world: Vec<u8>,
    /// The accounts folder.
    pub accounts: AccountsRoot,
}

impl ServerFiles {
    /// The first `initial_world` of an `[OutpostFrontier]` section of the
    /// server INI (`Server_ReadInitialWorldPath`); empty when there is none.
    #[must_use]
    pub fn read_initial_world(ini: &[u8]) -> Vec<u8> {
        let mut reader = IniReader::new();
        if !reader.open_memory(ini) {
            return Vec::new();
        }
        while reader.read_header() {
            if reader.is_header(b"OutpostFrontier") {
                while reader.read_value() {
                    if reader.is_value(b"initial_world") {
                        let mut value = reader.value_string();
                        value.truncate(259);
                        return value;
                    }
                }
            }
        }
        Vec::new()
    }

    /// `_makepath(path, 0, DataPath, g_InitialWorldPath, 0)`, found as
    /// Windows finds it (case-insensitively).
    #[must_use]
    pub fn initial_world_path(&self) -> PathBuf {
        locate(&self.data_dir, &self.initial_world)
    }

    /// A file next to the executable, found as Windows finds it.
    #[must_use]
    pub fn exe_file(&self, name: &str) -> PathBuf {
        locate(&self.exe_dir, name.as_bytes())
    }
}

/// `dir` joined with `name`, found case-insensitively as on Windows; the
/// plain join when nothing is there.
fn locate(dir: &Path, name: &[u8]) -> PathBuf {
    neuron_core::vfs::Host::new(dir)
        .locate(name)
        .ok()
        .flatten()
        .map_or_else(
            || dir.join(String::from_utf8_lossy(name).as_ref()),
            |e| e.path,
        )
}

/// What the client logs in with (`SLoginInfo`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LoginInfo {
    /// The account name.
    pub account_name: Vec<u16>,
    /// Create the account when it does not exist.
    pub create: bool,
}

/// A `.fl` template next to the executable (or any save) as the readers
/// load it: `None` when it is missing or an `FLSG` file.
fn read_text(path: &Path) -> Option<Vec<u8>> {
    let bytes = std::fs::read(path).ok()?;
    read_save(&bytes).text().map(<[u8]>::to_vec)
}

/// `PlayerDB::create_restart_file`: `Restart.fl` in the account folder, the
/// executable's `NewPlayer.fl` followed by the initial world file. On a
/// failure the file is removed and the warning reported. The C++ prints
/// the template through `fprintf` as its format; the port writes it as it
/// is (D605).
pub fn create_restart_file(cx: &mut Cx<'_>, files: &ServerFiles, account_dir: &Path) -> bool {
    let restart = account_dir.join("Restart.fl");
    let Ok(()) = std::fs::write(&restart, b"") else {
        cx.logic.reports.push(
            PLAYER_DB,
            0x156c,
            format!(
                "*** WARNING: Failed to open '{}' for writing.",
                restart.display()
            ),
        );
        return false;
    };
    let template_path = files.exe_file("NewPlayer.fl");
    let Some(template) = read_text(&template_path) else {
        cx.logic.reports.push(
            PLAYER_DB,
            0x1565,
            format!(
                "*** WARNING: Failed to open '{}' for reading.",
                template_path.display()
            ),
        );
        let _ = std::fs::remove_file(&restart);
        return false;
    };
    let world_path = files.initial_world_path();
    let Ok(world) = std::fs::read(&world_path) else {
        cx.logic.reports.push(
            PLAYER_DB,
            0x155e,
            format!(
                "*** WARNING: Failed to open '{}' for reading.",
                world_path.display()
            ),
        );
        let _ = std::fs::remove_file(&restart);
        return false;
    };
    let mut text = template;
    text.extend_from_slice(&text_mode_read(&world));
    std::fs::write(&restart, text_mode_write(&text)).is_ok()
}

/// `PlayerDB::create_account`: in single player the account folder is made
/// and given a restart file (0, or -1 when that fails). On a multiplayer
/// server, when new players are allowed, the registry makes the account
/// (`PlayerDB::CreateAccount`): 0, else -1.
pub fn create_account(cx: &mut Cx<'_>, files: &ServerFiles, info: &LoginInfo) -> i32 {
    let dir = files.accounts.account_dir(&info.account_name);
    if cx.world.config().single_player {
        if !dir.exists() {
            let _ = std::fs::create_dir_all(&dir);
        }
        return if create_restart_file(cx, files, &dir) {
            0
        } else {
            -1
        };
    }
    if !cx.logic.allow_new_players {
        return -1;
    }
    let name = until_nul(&info.account_name).to_vec();
    if cx.logic.accounts.create_account(&files.accounts, &name) {
        cx.logic.reports.push(
            "",
            0,
            format!(
                "LOG: Create account {} \"{}\"",
                String::from_utf16_lossy(&name),
                dir.display()
            ),
        );
        0
    } else {
        -1
    }
}

/// `PlayerDB::login`: 1 when an active slot is logged in on the account
/// already (`_wcsicmp` of the names), else `load_user_data`'s answer.
pub fn login(cx: &mut Cx<'_>, files: &ServerFiles, info: &LoginInfo, session: u32) -> u8 {
    let name = until_nul(&info.account_name);
    let taken = cx.logic.players.active_ids().any(|id| {
        cx.logic
            .players
            .get(id)
            .is_some_and(|slot| wcsicmp_eq(&slot.account_name, name))
    });
    if taken {
        return 1;
    }
    load_user_data(cx, files, info, session)
}

/// `_wcsicmp(a, b) == 0` in the "C" locale: A to Z fold to a to z.
fn wcsicmp_eq(a: &[u16], b: &[u16]) -> bool {
    let fold = |c: u16| {
        if (u16::from(b'A')..=u16::from(b'Z')).contains(&c) {
            c + 32
        } else {
            c
        }
    };
    let a = until_nul(a);
    a.len() == b.len() && a.iter().zip(b).all(|(&x, &y)| fold(x) == fold(y))
}

/// `PlayerDB::load_user_data`: the result code the login answers with (2
/// for no account or a name that does not match a multiplayer account's
/// `name` file, else `PlayerData_LoginAccount`'s).
pub fn load_user_data(cx: &mut Cx<'_>, files: &ServerFiles, info: &LoginInfo, session: u32) -> u8 {
    let dir = files.accounts.account_dir(&info.account_name);
    if !dir.exists() && (!info.create || create_account(cx, files, info) != 0) {
        return 2;
    }
    if !cx.world.config().single_player {
        let Ok(bytes) = std::fs::read(dir.join("name")) else {
            return 2;
        };
        let stored = decode_name_file(&bytes[..bytes.len().min(0x800)]);
        let name = until_nul(&info.account_name);
        if stored.len() != name.len() || stored != name {
            return 2;
        }
    }
    login_account(cx, files, &dir, info, session)
}

fn until_nul(name: &[u16]) -> &[u16] {
    let end = name.iter().position(|&c| c == 0).unwrap_or(name.len());
    &name[..end]
}

/// `PlayerData_LoginAccount`: the slot takes the session and the account
/// name, and every `*.fl` of the account folder is loaded as a character
/// (3 when one loads, else 6). Single player makes the restart file when
/// the folder has none; a multiplayer login answers 3 once the folder
/// exists. The places are cleared.
pub fn login_account(
    cx: &mut Cx<'_>,
    files: &ServerFiles,
    dir: &Path,
    info: &LoginInfo,
    session: u32,
) -> u8 {
    let name = until_nul(&info.account_name).to_vec();
    let account = cx.logic.accounts.find_by_name(&name);
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.account = account;
    }
    if let Some(id) = account {
        let registered = cx.logic.accounts.get(id);
        if registered.is_some_and(|a| a.locked) {
            return 7;
        }
        if registered.is_some_and(|a| !a.not_banned) {
            return 0;
        }
    }
    {
        let Some(slot) = cx.logic.players.get_mut(session) else {
            return 6;
        };
        slot.client_id = session;
        slot.account_name.clone_from(&name);
    }
    if let Some(registered) = account.and_then(|id| cx.logic.accounts.get_mut(id)) {
        registered.client_id = session;
    }
    let mut result = 6;
    if dir.exists() {
        let mut found_restart = false;
        for name in list_save_files(dir) {
            if load_saved_character(cx, session, dir, name.as_bytes()) {
                result = 3;
                if !found_restart && eq_ignore_case(name.as_bytes(), b"Restart.fl") {
                    found_restart = true;
                }
            }
        }
        if cx.world.config().single_player {
            if !found_restart
                && create_restart_file(cx, files, dir)
                && load_saved_character(cx, session, dir, b"Restart.fl")
            {
                result = 3;
            }
        } else {
            result = 3;
            let shown = String::from_utf16_lossy(&name);
            cx.logic.reports.push(
                "",
                0,
                format!("LOG: Login on account {shown} (\"{}\")", dir.display()),
            );
            // The C++ stores the folder through the account pointer without
            // a test; a login whose account is not registered skips it (D607).
            if let Some(registered) = account.and_then(|id| cx.logic.accounts.get_mut(id)) {
                registered.directory_path = dir.to_path_buf();
            }
        }
    }
    let account = cx.logic.accounts.find_by_name(&name);
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.account = account;
        slot.system_id = 0;
        slot.ship_object_id = 0;
        slot.created_ship_id = 0;
        slot.current_base = 0;
        slot.last_base_id = 0;
        slot.current_location = 0;
        slot.field_3fc = 0;
        slot.exited_base_id = 0;
    }
    result
}

/// Reads `path` into the slot's record under `key` (made when missing);
/// on success the record's file name is `key`, otherwise the record is
/// removed. `true` when a `[Player]` section was read.
fn load_into(cx: &mut Cx<'_>, session: u32, key: &[u8], path: &Path) -> bool {
    let data: std::sync::Arc<GameData> = cx.world.data_arc();
    let id = CharacterId::new(key);
    let text = read_text(path).unwrap_or_default();
    let shown = path.display().to_string();
    let mut record = cx
        .logic
        .players
        .get_mut(session)
        .and_then(|slot| slot.saved_characters.remove(&id))
        .unwrap_or_default();
    let save = SaveData {
        universe: &data.universe,
        archetypes: &data.archetypes,
        reputation: &data.reputation,
        costumes: &data.costumes,
    };
    let saw = load_save_record(
        &text,
        &shown,
        &mut record,
        save,
        cx.world.strings_mut(),
        &mut cx.logic.reports,
    );
    if saw {
        record.file_name = id.as_bytes().to_vec();
        if let Some(slot) = cx.logic.players.get_mut(session) {
            slot.saved_characters.insert(id, record);
        }
    }
    saw
}

/// `PlayerData_LoadSavedCharacter(slot, dir, file_name)`.
pub fn load_saved_character(cx: &mut Cx<'_>, session: u32, dir: &Path, file_name: &[u8]) -> bool {
    let path = dir.join(String::from_utf8_lossy(file_name).as_ref());
    if load_into(cx, session, file_name, &path) {
        return true;
    }
    cx.logic.reports.push(
        PLAYER_DB,
        0x4a8,
        format!("*** WARNING: Failed to load saved game {}", path.display()),
    );
    false
}

/// `PlayerData_ReloadSavedCharacter(slot, file_name)`: the key is the
/// name with `.fl` added outside a multiplayer server (so a name that
/// already ends in `.fl` gets a second one, as `_makepath` makes it).
pub fn reload_saved_character(
    cx: &mut Cx<'_>,
    files: &ServerFiles,
    session: u32,
    file_name: &[u8],
) {
    reload_saved_character_in(cx, &files.accounts, session, file_name);
}

/// [`reload_saved_character`] in the accounts folder `accounts`.
pub fn reload_saved_character_in(
    cx: &mut Cx<'_>,
    accounts: &AccountsRoot,
    session: u32,
    file_name: &[u8],
) {
    let mut key = file_name.to_vec();
    if !cx.logic.multiplayer_server {
        key.extend_from_slice(b".fl");
    }
    let Some(account) = cx
        .logic
        .players
        .get(session)
        .map(|s| s.account_name.clone())
    else {
        return;
    };
    let path = accounts
        .account_dir(&account)
        .join(String::from_utf8_lossy(&key).as_ref());
    load_into(cx, session, &key, &path);
}

/// One entry of the character list (`CCharacterSet::CharacterInfo`).
#[derive(Clone, Debug, PartialEq)]
pub struct CharacterInfo {
    /// The character's id.
    pub id: CharacterId,
    /// The account's restart file.
    pub is_restart: bool,
    /// The autosave.
    pub is_autosave: bool,
    /// The name.
    pub name: Vec<u16>,
    /// The description (empty for the autosave).
    pub description: Vec<u16>,
    /// The description's text id (0x65c for the autosave).
    pub descrip_str_id: u32,
    /// The save time, low half.
    pub timestamp_low: u32,
    /// The save time, high half.
    pub timestamp_high: u32,
    /// The ship.
    pub ship_archetype: u32,
    /// The cash.
    pub money: i32,
    /// `num_kills`.
    pub num_kills: i32,
    /// `num_misn_successes`.
    pub num_mission_successes: i32,
    /// `num_misn_failures`.
    pub num_mission_failures: i32,
    /// The hull.
    pub hull_status: f32,
    /// The equipment.
    pub equipment: Vec<game_sim::data::equip_desc::EquipDesc>,
    /// The collision groups.
    pub collision_groups: Vec<CollisionGroupDesc>,
    /// The system.
    pub system_id: u32,
    /// The base.
    pub base_id: u32,
    /// The room.
    pub location_id: u32,
    /// `CreateID` of the voice.
    pub voice_id: u32,
    /// The rank.
    pub rank: u32,
    /// The base costume.
    pub base_costume: game_sim::data::costume::Costume,
    /// The comm costume.
    pub com_costume: game_sim::data::costume::Costume,
}

/// The character list (`CCharacterSet`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CharacterSet {
    /// The characters: the restart file first, then the autosave, then the
    /// newest save first.
    pub characters: Vec<CharacterInfo>,
    /// Saves that failed since the last list.
    pub save_failure_count: i32,
}

/// `CharacterInfo_GoesBefore`.
fn goes_before(new: &CharacterInfo, existing: &CharacterInfo) -> bool {
    if new.is_restart != existing.is_restart {
        return new.is_restart;
    }
    if new.is_autosave != existing.is_autosave {
        return new.is_autosave;
    }
    let time = |c: &CharacterInfo| (u64::from(c.timestamp_high) << 32) | u64::from(c.timestamp_low);
    time(new) > time(existing)
}

/// `PlayerData_BuildCharacterSet`: one entry per saved character in id
/// order, each placed before the first entry it goes before; the slot's
/// save failure count moves to the list.
///
/// On a multiplayer server a slot with a selected character is first
/// autosaved ([`crate::saves::auto_save`]) and a trade it is in ends with
/// reason 4 ([`end_trade`]). The rest of that prologue (leaving the system
/// and the base, `SystemExit`) is not ported yet.
pub fn build_character_set(cx: &mut Cx<'_>, session: u32) -> Option<CharacterSet> {
    if cx.logic.multiplayer_server
        && cx
            .logic
            .players
            .get(session)
            .is_some_and(|s| s.selected_character.is_valid())
    {
        let _ = crate::saves::auto_save(cx, session);
        let client = cx.logic.players.get(session).map_or(0, |s| s.client_id);
        end_trade(cx, client);
    }
    let slot = cx.logic.players.get_mut(session)?;
    let mut set = CharacterSet::default();
    for (id, saved) in &slot.saved_characters {
        let is_restart = stricmp(b"Restart.fl", &saved.file_name).is_eq();
        let is_autosave = stricmp(b"AutoSave.fl", &saved.file_name).is_eq();
        let info = CharacterInfo {
            id: id.clone(),
            is_restart,
            is_autosave,
            name: saved.name.clone(),
            description: if is_autosave {
                Vec::new()
            } else {
                saved.description.clone()
            },
            descrip_str_id: if is_autosave {
                0x65c
            } else {
                saved.descrip_str_id
            },
            timestamp_low: saved.timestamp_low,
            timestamp_high: saved.timestamp_high,
            ship_archetype: saved.ship_archetype,
            money: saved.money,
            num_kills: saved.num_kills,
            num_mission_successes: saved.num_mission_successes,
            num_mission_failures: saved.num_mission_failures,
            hull_status: saved.hull_status,
            equipment: saved.equipment.clone(),
            collision_groups: saved.collision_groups.clone(),
            system_id: saved.system_id,
            base_id: saved.base_id,
            location_id: saved.location_id,
            voice_id: create_id(&saved.voice),
            rank: saved.rank,
            base_costume: saved.base_costume.clone(),
            com_costume: saved.com_costume.clone(),
        };
        let at = set
            .characters
            .iter()
            .position(|existing| goes_before(&info, existing))
            .unwrap_or(set.characters.len());
        set.characters.insert(at, info);
    }
    set.save_failure_count = slot.save_failure_count;
    slot.save_failure_count = 0;
    Some(set)
}

/// `PlayerData_DestroyCharacter`: the restart file is kept (`false`); any
/// other character's file is deleted, its id leaves the account registry
/// (`PlayerDB::DeleteCharacterFromID`) and its record is removed. Leaving
/// the game first when the character is the selected one
/// (`PlayerData_DeselectCharacter`) arrives with character selection.
pub fn destroy_character(
    cx: &mut Cx<'_>,
    files: &ServerFiles,
    session: u32,
    id: &CharacterId,
) -> bool {
    let Some(slot) = cx.logic.players.get_mut(session) else {
        return false;
    };
    let Some(record) = slot.saved_characters.get(id) else {
        return false;
    };
    if stricmp(&record.file_name, b"Restart.fl").is_eq() {
        return false;
    }
    let path = files
        .accounts
        .account_dir(&slot.account_name)
        .join(String::from_utf8_lossy(&record.file_name).as_ref());
    let key = record.file_name.to_ascii_lowercase();
    let account = slot.account_name.clone();
    slot.saved_characters.remove(id);
    cx.logic
        .accounts
        .delete_character_from_id(&files.accounts, &key);
    let _ = std::fs::remove_file(&path);
    if !cx.world.config().single_player {
        cx.logic.reports.push(
            "",
            0,
            format!(
                "LOG: Delete character on account {} (\"{}\")",
                String::from_utf16_lossy(&account),
                path.display()
            ),
        );
    }
    true
}

/// `PlayerDB::create_new_character`: `false` when a registered account
/// already has a character of that name; `true` without making one when
/// the slot's account has five characters (as the C++ answers); otherwise
/// [`create_new_character`], and on success the name joins the account and
/// its id the registry. A slot without a registered account is refused,
/// where the C++ reads through the null account (D607).
pub fn create_character(
    cx: &mut Cx<'_>,
    files: &ServerFiles,
    session: u32,
    info: &CreateCharacterInfo,
) -> bool {
    let name = until_nul(&info.name).to_vec();
    let key = neuron_server::registry::character_id(&name);
    if cx.logic.accounts.find_by_character_id(&key).is_some() {
        return false;
    }
    let Some(account) = cx.logic.players.get(session).and_then(|s| s.account) else {
        return false;
    };
    if cx
        .logic
        .accounts
        .get(account)
        .is_some_and(|a| a.characters.len() >= 5)
    {
        return true;
    }
    if !create_new_character(cx, files, session, info) {
        return false;
    }
    if let Some(registered) = cx.logic.accounts.get_mut(account) {
        registered.characters.push(name);
    }
    cx.logic.accounts.insert_character_id(&key, account);
    true
}

/// Phases (D) and (E) of `PlayerDB::init` on a multiplayer server: the
/// registry of every account folder under the root, with the character
/// names its saves hold (`ReadCharacterName`).
#[must_use]
pub fn scan_registry(files: &ServerFiles) -> neuron_server::registry::AccountRegistry {
    let folders = neuron_server::account::scan_accounts(&files.accounts);
    neuron_server::registry::AccountRegistry::from_folders(&folders, &files.accounts, |path| {
        std::fs::read(path)
            .map(|bytes| crate::character_file::read_character_name(&bytes))
            .unwrap_or_default()
    })
}

/// What the client creates a character with (`SCreateCharacterInfo`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CreateCharacterInfo {
    /// The name.
    pub name: Vec<u16>,
    /// The faction (+0x30).
    pub faction: u32,
    /// The home base (+0x34): the base and its `FNewCharExtra`.
    pub base: u32,
    /// The package (+0x38).
    pub package: u32,
    /// The pilot (+0x3c).
    pub pilot: u32,
}

/// `PlayerData_CreateNewCharacter`: the character's file in the account
/// folder, named by the hash of its name, made from the executable's
/// `NewCharacter.fl` (`MPNewCharacter.fl` outside single player) with the
/// `%%...%%` values filled in and `%%Package%%` replaced by the package's
/// ship and loadout; then the file is loaded as a character. `false`
/// when none of the four choices is known, or when one is missing or the
/// template cannot be read (the file opened for writing is left empty
/// then, as in the C++).
pub fn create_new_character(
    cx: &mut Cx<'_>,
    files: &ServerFiles,
    session: u32,
    info: &CreateCharacterInfo,
) -> bool {
    let data = cx.world.data_arc();
    let Some(account) = cx
        .logic
        .players
        .get(session)
        .map(|s| s.account_name.clone())
    else {
        return false;
    };
    let hash = hashed_name(&info.name);
    let file_name = format!("{hash}.fl");
    let path = files.accounts.account_dir(&account).join(&file_name);
    let db = &data.new_char;
    let faction = db.factions.get(&info.faction);
    let package = db.packages.get(&info.package);
    let home_base = db.extras.get(&info.base);
    let pilot = db.pilots.get(&info.pilot);
    if faction.is_none() && package.is_none() && home_base.is_none() && pilot.is_none() {
        return false;
    }
    if std::fs::write(&path, b"").is_err() {
        return false;
    }
    let template = files.exe_file(if cx.world.config().single_player {
        "NewCharacter.fl"
    } else {
        "MPNewCharacter.fl"
    });
    let Some(template) = read_text(&template) else {
        return false;
    };
    let (Some(faction), Some(package), Some(home_base), Some(pilot)) =
        (faction, package, home_base, pilot)
    else {
        return false;
    };
    let home_system = data
        .universe
        .bases
        .get(&info.base)
        .and_then(|b| data.universe.systems.get(&b.system))
        .map(|s| cx.world.strings().get(s.nickname).to_vec());
    let strings = cx.world.strings();
    let mut reader = IniReader::new();
    reader.open_memory(&template);
    let mut out = Vec::new();
    // The joined values persist from one key to the next: a key without
    // values prints the previous key's.
    let mut joined: Vec<u8> = Vec::new();
    while reader.read_header() {
        out.extend_from_slice(b"\n[");
        out.extend_from_slice(&reader.header());
        out.extend_from_slice(b"]\n");
        while reader.read_value() {
            let key = reader.name();
            if eq_ignore_case(&key, b"%%Package%%") {
                put(&mut out, b"ship_archetype", &package.ship);
                let loadout = data.loadouts.get(create_id(&package.loadout));
                for item in loadout.map(|l| l.items.as_slice()).unwrap_or_default() {
                    let Some(equipment) = data.archetypes.equipment.get(&item.arch_id) else {
                        continue;
                    };
                    let nickname = equipment.root.nickname;
                    if item.equipped != 0 {
                        let mut line = format!("{nickname}, ").into_bytes();
                        line.extend_from_slice(strings.get(item.hardpoint));
                        put(&mut out, b"equip", &line);
                    } else {
                        put(
                            &mut out,
                            b"cargo",
                            format!("{nickname}, {}", item.count).as_bytes(),
                        );
                    }
                }
                continue;
            }
            let count = reader.num_parameters();
            for index in 0..count {
                if index == 0 {
                    joined.clear();
                } else {
                    joined.extend_from_slice(b", ");
                }
                let value = reader.value_string_at(index);
                let filled: Vec<u8> = if value.first() == Some(&b'%') {
                    if eq_ignore_case(&value, b"%%Name%%") {
                        wide_to_hex(until_nul(&info.name)).into_bytes()
                    } else if eq_ignore_case(&value, b"%%Faction%%") {
                        faction.rep_group.clone()
                    } else if eq_ignore_case(&value, b"%%Money%%") {
                        (package.money as u32).to_string().into_bytes()
                    } else if eq_ignore_case(&value, b"%%Base_Costume%%") {
                        pilot.body.clone()
                    } else if eq_ignore_case(&value, b"%%Comm_Costume%%") {
                        pilot.comm.clone()
                    } else if eq_ignore_case(&value, b"%%Voice%%") {
                        pilot.voice.clone()
                    } else if eq_ignore_case(&value, b"%%Home_System%%") {
                        home_system.clone().unwrap_or(value)
                    } else if eq_ignore_case(&value, b"%%Home_Base%%") {
                        home_base.clone()
                    } else {
                        value
                    }
                } else {
                    value
                };
                joined.extend_from_slice(&filled);
            }
            put(&mut out, &key, &joined);
        }
        out.push(b'\n');
    }
    if std::fs::write(&path, text_mode_write(&out)).is_err() {
        return false;
    }
    reload_saved_character(cx, files, session, file_name.as_bytes());
    if !cx.world.config().single_player {
        let name = String::from_utf16_lossy(&account);
        cx.logic.reports.push(
            "",
            0,
            format!(
                "LOG: Create character on account {name} (\"{}\")",
                path.display()
            ),
        );
    }
    true
}

/// `fprintf(f, "%s = %s\n", key, value)`.
fn put(out: &mut Vec<u8>, key: &[u8], value: &[u8]) {
    out.extend_from_slice(key);
    out.extend_from_slice(b" = ");
    out.extend_from_slice(value);
    out.push(b'\n');
}

/// The record of the slot's character `id`, if any.
#[must_use]
pub fn saved_character<'a>(
    cx: &'a Cx<'_>,
    session: u32,
    id: &CharacterId,
) -> Option<&'a SavedCharacter> {
    cx.logic.players.get(session)?.saved_characters.get(id)
}

/// `PlayerDB::logout(session)` (`Server/server_classes.cpp:2521-2626`):
/// a multiplayer server autosaves the slot; the slot leaves its system
/// (`PlayerDB_LeaveSystem`: the system's observer and ships go, the
/// pending visits are committed, `SystemExit`) and its base
/// (`PlayerDB_LeaveBase`: the room and the base are left in the base
/// database); `PlayerLogout` goes to the bus and the counter counts it;
/// the player's group hears of it; every other player in use loses the
/// player from its list (on a dedicated server only for a selected
/// character); the account's logout is reported; then the slot is torn
/// down (`PlayerData_Teardown`): its vibe is freed, its character, ship,
/// cargo, costumes, places and selection are wiped, cash is the C++'s
/// fill value 666, the account is marked logged out and the slot's client
/// id is 0, which frees it. A trade the player is in ends with reason 4
/// before `PlayerLogout`, and the teardown withdraws the trade requests
/// ([`crate::trade::teardown_requests`]).
pub fn logout(cx: &mut Cx<'_>, session: u32) {
    let max = cx.logic.players.max_players() as u32;
    if session == 0 || session > max {
        return;
    }
    let single_player = cx.world.config().single_player;
    if !single_player {
        let _ = crate::saves::auto_save(cx, session);
    }
    // PlayerDB_LeaveSystem.
    let system = cx.logic.players.get(session).map_or(0, |s| s.system_id);
    if system & 0x3FFF_FFFF != 0 {
        crate::world::remove_player(cx, system, session);
        if let Some(slot) = cx.logic.players.get_mut(session) {
            crate::player::flush_pending_visits(slot);
        }
        if cx
            .logic
            .players
            .get(session)
            .is_some_and(|s| s.system_id != 0)
        {
            if let Some(slot) = cx.logic.players.get_mut(session) {
                slot.system_id = 0;
            }
            let _ = cx.dispatch(
                neuron_server::controller::ControllerId::BROADCAST,
                &mut crate::events::ControllerMessage::SystemExit {
                    system,
                    destination: 0,
                    player: session,
                },
            );
        }
    }
    // PlayerDB_LeaveBase.
    let (base, location) = cx
        .logic
        .players
        .get(session)
        .map_or((0, 0), |s| (s.current_base, s.current_location));
    if base & 0x3FFF_FFFF != 0 {
        if location & 0x3FFF_FFFF != 0 {
            crate::bases::exit_location(cx, session, location);
        }
        crate::bases::unload_base(cx, base);
    }
    if let Some(slot) = cx.logic.players.get_mut(session) {
        slot.current_base = 0;
        slot.current_location = 0;
    }
    // The vestigial SystemExit of a system id still set: LeaveSystem
    // cleared it, so it never fires.
    end_trade(cx, session);
    let _ = cx.dispatch(
        neuron_server::controller::ControllerId::BROADCAST,
        &mut crate::events::ControllerMessage::PlayerLogout { player: session },
    );
    cx.logic.stats.logouts += 1;
    if let Some(group) = cx.logic.players.get(session).and_then(|s| s.group) {
        crate::group::handle_client_logout(cx, group, session);
    }
    let selected = cx
        .logic
        .players
        .get(session)
        .is_some_and(|s| s.selected_character.is_valid());
    if !cx.logic.multiplayer_server || selected {
        for other in 1..=max {
            if other == session || cx.logic.players.get(other).is_none_or(|s| s.client_id == 0) {
                continue;
            }
            crate::client::player_list_remove(cx, other, session);
        }
    }
    let name = cx
        .logic
        .players
        .get(session)
        .map(|s| s.account_name.clone())
        .unwrap_or_default();
    let account = cx.logic.accounts.find_by_name(&name);
    if let Some(id) = account
        && !single_player
    {
        let dir = cx
            .logic
            .accounts
            .get(id)
            .map(|a| a.directory_path.display().to_string())
            .unwrap_or_default();
        let shown = String::from_utf16_lossy(&name);
        cx.logic
            .reports
            .push("", 0, format!("LOG: Logout on account {shown} (\"{dir}\")"));
    }
    teardown(cx, session);
}

/// The `TerminateTrade(session, 4)` of a player leaving the game in a
/// trade (`PlayerDB::logout` step 4, `PlayerData_BuildCharacterSet`,
/// `PlayerData_SelectCharacter`).
pub fn end_trade(cx: &mut Cx<'_>, session: u32) {
    if cx
        .logic
        .players
        .get(session)
        .is_some_and(|s| s.trade_offer.is_some())
    {
        crate::trade::terminate_trade(cx, session, 4);
    }
}

/// `PlayerData_Teardown`: the trade requests are withdrawn
/// ([`crate::trade::teardown_requests`]), then the vibe and the fields.
/// The C++ fills the account name and the costumes with 0xcd; the port
/// empties them.
fn teardown(cx: &mut Cx<'_>, session: u32) {
    crate::trade::teardown_requests(cx, session);
    let vibe = cx.logic.players.get(session).map_or(-1, |s| s.rep_vibe);
    if vibe != -1 {
        let _ = cx.world.vibes().0.free(vibe);
    }
    let no_where = create_id(b"NO WHERE");
    let Some(slot) = cx.logic.players.get_mut(session) else {
        return;
    };
    let account = slot.account;
    slot.rep_vibe = -1;
    slot.account_name.clear();
    slot.saved_characters.clear();
    slot.ship_archetype = 0;
    slot.relative_health = 0.0;
    slot.equipment.clear();
    slot.collision_groups.clear();
    slot.com_costume = game_sim::data::costume::Costume::default();
    slot.voice.clear();
    slot.costume = game_sim::data::costume::Costume::default();
    slot.initial_pos = [0.0; 3];
    slot.initial_orient = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]];
    slot.cash = 0x29a;
    slot.num_kills = 0;
    slot.num_mission_successes = 0;
    slot.num_mission_failures = 0;
    slot.skip_next_autosave = false;
    slot.save_failure_count = 0;
    slot.client_id = 0;
    slot.cheat_reported = false;
    slot.equip_ids.reset();
    slot.system_id = 0;
    slot.ship_object_id = 0;
    slot.created_ship_id = 0;
    slot.current_base = 0;
    slot.entered_base_id = 0;
    slot.last_base_id = 0;
    slot.current_location = 0;
    slot.field_3fc = 0;
    slot.exited_base_id = 0;
    slot.starting_ring = no_where;
    slot.selected_character.clear();
    slot.group = None;
    slot.group_mission_set_by = 0;
    slot.visits.clear();
    if let Some(registered) = account.and_then(|id| cx.logic.accounts.get_mut(id)) {
        registered.client_id = 0;
    }
}
