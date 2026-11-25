//! A dedicated server's start-up (work package P3.9):
//! `COutpostServerApp::ReadOutpostFrontierIni` (`OutpostServer.cpp:681-735`)
//! for the data path, then `IServerImpl::Startup` (`Server/IServerImpl.cpp:298-357`)
//! in its multiplayer form: `PlayerDB::init` (the slots, the server's own
//! account, the account scan, the cheat message), `DataConfig::startup`,
//! `Server_ReadWorldIni`, `Server_LoadInitialDlls` (`[Initial MP DLLs]`),
//! `Server_LoadWorldState`, `Server_SubsystemInit` and
//! `Server_PlayerSubsystemInit` (PvP and the system database). The base
//! and fuse databases are made on first use. The transport's start
//! (`IClient::Startup`) is the caller's.

use std::fmt;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use neuron_core::account::MachineIdentity;
use neuron_core::ini::IniReader;
use neuron_core::vfs::Host;
use neuron_server::account::{AccountsRoot, ServerAccount, ServerAccountSeed, init_server_account};

use crate::login::ServerFiles;
use crate::server::{Cx, Logic, RecordingSink};

/// The engine INI's name (`FServerControl::IniFileName`).
pub const INI_FILE: &str = "OutpostFrontier.ini";

/// The local server INI read after it (`"SrvLocal-" + IniFileName`).
pub const LOCAL_INI_FILE: &str = "SrvLocal-OutpostFrontier.ini";

/// What the server starts with (`SStartupInfo` and the settings the
/// configuration dialog gave `FServerControl`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StartupInfo {
    /// The folder of `OutpostFrontier.ini` and the character templates
    /// (the executable's folder in the C++).
    pub install_dir: PathBuf,
    /// The user-data folder (`Documents\My Games\OutpostFrontier`); the
    /// accounts are in its `Accts\MultiPlayer` (decision O41).
    pub user_data: PathBuf,
    /// The player limit (`MaxPlayers`).
    pub max_players: usize,
    /// New accounts may be made (`bAllowNewPlayers`).
    pub allow_new_players: bool,
    /// Players may fight each other (`bAllowPvp`).
    pub allow_pvp: bool,
    /// The CRT generator's seed (`srand(timeGetTime())`).
    pub seed: u32,
}

/// Why the server did not start.
#[derive(Debug)]
pub enum StartupError {
    /// `OutpostFrontier.ini` cannot be read.
    NoIni(PathBuf),
    /// No `[OutpostFrontier] data path` names a folder.
    NoDataPath,
    /// The accounts folder cannot be made.
    Accounts(std::io::Error),
    /// `InitServerAccount` found no valid account and signature (string
    /// 0xf010 of the C++, "Unable to find valid account name and
    /// signature").
    NoServerAccount,
}

impl fmt::Display for StartupError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NoIni(path) => write!(f, "cannot read {}", path.display()),
            Self::NoDataPath => write!(f, "{INI_FILE} names no data path"),
            Self::Accounts(error) => write!(f, "cannot make the accounts folder: {error}"),
            Self::NoServerAccount => {
                write!(
                    f,
                    "Unable to find valid account name and signature.  Aborting..."
                )
            }
        }
    }
}

impl std::error::Error for StartupError {}

/// A started server.
pub struct Started {
    /// The world over the loaded data.
    pub world: World,
    /// The server's state; its client sink records until a transport is
    /// given ([`crate::host::ServerHost::new`]).
    pub logic: Logic,
    /// The files outside the data tree.
    pub files: ServerFiles,
    /// The server's own account (`GetServerID`, `GetServerSig`).
    pub account: ServerAccount,
}

/// `[OutpostFrontier] data path` of an INI's text: the last one found
/// (`ReadOutpostFrontierIni` opens the data path for each it meets).
#[must_use]
pub fn read_data_path(ini: &[u8]) -> Option<Vec<u8>> {
    let mut reader = IniReader::new();
    if !reader.open_memory(ini) {
        return None;
    }
    let mut found = None;
    while reader.read_header() {
        if reader.is_header(b"OutpostFrontier") {
            while reader.read_value() {
                if reader.is_value(b"Data path") {
                    found = Some(reader.value_string());
                }
            }
        }
    }
    found
}

/// A path the INI names, relative to `base`, with Windows separators.
fn ini_path(base: &Path, value: &[u8]) -> PathBuf {
    let text = String::from_utf8_lossy(value).replace('\\', "/");
    let mut path = base.to_path_buf();
    for part in text.split('/').filter(|p| !p.is_empty() && *p != ".") {
        path.push(part);
    }
    path
}

/// The file `name` in `dir`, found case-insensitively as on Windows.
fn locate(dir: &Path, name: &str) -> PathBuf {
    Host::new(dir)
        .locate(name.as_bytes())
        .ok()
        .flatten()
        .map_or_else(|| dir.join(name), |e| e.path)
}

/// `IServerImpl::Startup` for a dedicated server.
///
/// # Errors
///
/// [`StartupError`] when the INI, the data path or the server's account
/// is missing, or the accounts folder cannot be made.
pub fn startup(
    info: &StartupInfo,
    machine: &mut dyn MachineIdentity,
    seed: &mut dyn ServerAccountSeed,
) -> Result<Started, StartupError> {
    let ini_file = locate(&info.install_dir, INI_FILE);
    let ini = std::fs::read(&ini_file).map_err(|_| StartupError::NoIni(ini_file.clone()))?;
    let local = std::fs::read(locate(&info.install_dir, LOCAL_INI_FILE)).unwrap_or_default();
    let data_path = read_data_path(&local)
        .or_else(|| read_data_path(&ini))
        .ok_or(StartupError::NoDataPath)?;
    let data_dir = ini_path(&info.install_dir, &data_path);

    // PlayerDB::init.
    let accounts = AccountsRoot::new(&info.user_data, true);
    let account = init_server_account(&accounts, seed, machine)
        .map_err(StartupError::Accounts)?
        .ok_or(StartupError::NoServerAccount)?;
    let files = ServerFiles {
        exe_dir: info.install_dir.clone(),
        data_dir: data_dir.clone(),
        initial_world: ServerFiles::read_initial_world(&ini),
        accounts: accounts.clone(),
    };
    let mut logic = Logic::new(Box::new(RecordingSink::default()), info.max_players, true);
    logic.allow_new_players = info.allow_new_players;
    logic.accounts = crate::login::scan_registry(&files);
    logic.cheat_message = crate::cheats::load_cheat_message(&info.install_dir, false);
    logic.saves.accounts = Some(accounts);

    // DataConfig::startup.
    let mut data = GameData::new();
    if let Some(config) = DataConfig::read(&Host::new(&info.install_dir), INI_FILE.as_bytes()) {
        data.load(&Host::new(&data_dir), &config, None);
    }
    logic.reports.0.append(&mut data.reports.0);
    let config = WorldConfig {
        seed: info.seed,
        ..WorldConfig::default()
    };
    let mut world = World::new(Arc::new(data), config).with_assets(Assets::new(&data_dir));

    {
        let mut cx = Cx {
            world: &mut world,
            logic: &mut logic,
        };
        // Server_LoadInitialDlls.
        let mut reader = IniReader::new();
        if reader.open_memory(&ini) {
            cx.load_initial_controllers(&mut reader, false);
        }
        // Server_LoadWorldState: a file that does not open reads as empty.
        let text = std::fs::read(files.initial_world_path()).unwrap_or_default();
        let mut reader = IniReader::new();
        reader.open_memory(&text);
        crate::selection::load_world_state(&mut cx, &mut reader);
        // Server_PlayerSubsystemInit.
        cx.logic.server_world.forbid_pvp = !info.allow_pvp;
        crate::world::initialize(&mut cx);
    }
    Ok(Started {
        world,
        logic,
        files,
        account,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_data_path_is_the_last_one() {
        let ini = b"[OutpostFrontier]\ndata path = .\\data\n[Other]\nData path = x\n[OutpostFrontier]\nDATA PATH = ..\\DATA\n";
        assert_eq!(read_data_path(ini), Some(b"..\\DATA".to_vec()));
        assert_eq!(read_data_path(b"[OutpostFrontier]\n"), None);
    }

    #[test]
    fn ini_paths_take_windows_separators() {
        let base = Path::new("/srv/outpost");
        assert_eq!(ini_path(base, b".\\data"), base.join("data"));
        assert_eq!(
            ini_path(base, b"a\\b/c"),
            base.join("a").join("b").join("c")
        );
    }
}
