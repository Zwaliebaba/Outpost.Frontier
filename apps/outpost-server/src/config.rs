//! The dedicated server's TOML configuration (decision T22), which
//! replaces the configuration dialog (`OutpostServer/ServerConfigDlg.cpp`),
//! its binary `OutpostServer.cfg` and the command-line switches
//! (`OutpostServerCmdLine.cpp`). When the file does not exist, the
//! defaults with the old `OutpostServer.cfg` of the accounts folder
//! imported over them are written to it once ([`load_or_create`]).
//!
//! ```toml
//! [server]
//! name = "Frontier"          # the session name; required
//! description = ""
//! password = ""              # empty for none
//! max_players = 16           # 1 to 128
//! allow_new_players = true
//! allow_pvp = false
//!
//! [network]
//! address = "0.0.0.0"
//! port = 14041
//!
//! [paths]
//! install_dir = ""           # OutpostFrontier.ini; empty: the executable's folder
//! user_data = ""             # empty: Documents/My Games/OutpostFrontier
//!
//! [log]
//! level = 4                  # 1 to 7
//! categories = []            # categories shown besides 0x10000
//! file = ""                  # a log file, truncated at start
//! error_log = ""             # empty: OutpostServerErrors.log in the accounts folder
//! ```

use std::fmt::{self, Write as _};
use std::net::IpAddr;
use std::path::{Path, PathBuf};

use neuron_server::admin_files::{LEGACY_CONFIG_FILE, LegacyServerConfig};
use neuron_server::net::MAX_CLIENTS;
use toml::{Table, Value};

/// The configuration's default file name.
pub const CONFIG_FILE: &str = "OutpostServer.toml";

/// The default port (`FServerControl`'s and the client's `0x36d9`).
pub const DEFAULT_PORT: u16 = 0x36d9;

/// The session name of `CClientImpl::Startup`.
pub const DEFAULT_NAME: &str = "OutpostFrontier Default Session Name";

/// The dedicated server's settings.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ServerConfig {
    /// The session name.
    pub name: String,
    /// The description.
    pub description: String,
    /// The session password; empty for none.
    pub password: String,
    /// Players admitted at once, 1 to 128.
    pub max_players: u16,
    /// New accounts may be made.
    pub allow_new_players: bool,
    /// Players may fight each other.
    pub allow_pvp: bool,
    /// The address to listen on.
    pub address: IpAddr,
    /// The UDP port; 0 lets the system choose.
    pub port: u16,
    /// The folder of `OutpostFrontier.ini`; `None` for the executable's.
    pub install_dir: Option<PathBuf>,
    /// The user-data folder; `None` for the default.
    pub user_data: Option<PathBuf>,
    /// The log level (`/S`).
    pub log_level: i32,
    /// Categories shown besides the general one (`/E`).
    pub log_categories: Vec<i32>,
    /// The log file (`/L`).
    pub log_file: Option<PathBuf>,
    /// The error log; `None` for the default.
    pub error_log: Option<PathBuf>,
}

impl Default for ServerConfig {
    /// The dialog's defaults (16 players, new players, no PvP), the
    /// default session name, port 0x36d9 on every address, level 4.
    fn default() -> Self {
        Self {
            name: DEFAULT_NAME.to_owned(),
            description: String::new(),
            password: String::new(),
            max_players: 16,
            allow_new_players: true,
            allow_pvp: false,
            address: IpAddr::from([0, 0, 0, 0]),
            port: DEFAULT_PORT,
            install_dir: None,
            user_data: None,
            log_level: neuron_server::log::DEFAULT_LOG_LEVEL,
            log_categories: Vec::new(),
            log_file: None,
            error_log: None,
        }
    }
}

/// Why a configuration was refused.
#[derive(Debug)]
pub enum ConfigError {
    /// The file cannot be read or written.
    Io(PathBuf, std::io::Error),
    /// The text is not TOML.
    Syntax(String),
    /// A key has the wrong type, an unknown name, or a value out of range.
    Value(String),
}

impl fmt::Display for ConfigError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(path, error) => write!(f, "{}: {error}", path.display()),
            Self::Syntax(message) => write!(f, "configuration: {message}"),
            Self::Value(message) => write!(f, "configuration: {message}"),
        }
    }
}

impl std::error::Error for ConfigError {}

fn value_error(message: impl Into<String>) -> ConfigError {
    ConfigError::Value(message.into())
}

fn string(table: &Table, section: &str, key: &str) -> Result<Option<String>, ConfigError> {
    match table.get(key) {
        None => Ok(None),
        Some(Value::String(s)) => Ok(Some(s.clone())),
        Some(_) => Err(value_error(format!("[{section}] {key} must be a string"))),
    }
}

fn boolean(table: &Table, section: &str, key: &str) -> Result<Option<bool>, ConfigError> {
    match table.get(key) {
        None => Ok(None),
        Some(Value::Boolean(b)) => Ok(Some(*b)),
        Some(_) => Err(value_error(format!(
            "[{section}] {key} must be true or false"
        ))),
    }
}

fn integer(table: &Table, section: &str, key: &str) -> Result<Option<i64>, ConfigError> {
    match table.get(key) {
        None => Ok(None),
        Some(Value::Integer(i)) => Ok(Some(*i)),
        Some(_) => Err(value_error(format!("[{section}] {key} must be an integer"))),
    }
}

fn path(table: &Table, section: &str, key: &str) -> Result<Option<PathBuf>, ConfigError> {
    Ok(string(table, section, key)?
        .filter(|s| !s.is_empty())
        .map(PathBuf::from))
}

fn check_keys(table: &Table, section: &str, known: &[&str]) -> Result<(), ConfigError> {
    match table.keys().find(|k| !known.contains(&k.as_str())) {
        Some(key) => Err(value_error(format!("unknown key [{section}] {key}"))),
        None => Ok(()),
    }
}

fn section<'t>(root: &'t Table, name: &str) -> Result<Option<&'t Table>, ConfigError> {
    match root.get(name) {
        None => Ok(None),
        Some(Value::Table(t)) => Ok(Some(t)),
        Some(_) => Err(value_error(format!("[{name}] must be a table"))),
    }
}

impl ServerConfig {
    /// Parses a configuration; missing keys keep their defaults.
    ///
    /// # Errors
    ///
    /// [`ConfigError::Syntax`] for text that is not TOML,
    /// [`ConfigError::Value`] for an unknown section or key, a value of
    /// the wrong type, a player limit outside 1 to 128 or an empty name
    /// (the dialog's two refusals, strings 0xf006 and 0xf007).
    pub fn parse(text: &str) -> Result<Self, ConfigError> {
        let root: Table = text
            .parse()
            .map_err(|e: toml::de::Error| ConfigError::Syntax(e.to_string()))?;
        check_keys(&root, "", &["server", "network", "paths", "log"]).map_err(|_| {
            value_error("unknown section; the sections are [server], [network], [paths] and [log]")
        })?;
        let mut config = Self::default();
        if let Some(t) = section(&root, "server")? {
            check_keys(
                t,
                "server",
                &[
                    "name",
                    "description",
                    "password",
                    "max_players",
                    "allow_new_players",
                    "allow_pvp",
                ],
            )?;
            if let Some(v) = string(t, "server", "name")? {
                config.name = v;
            }
            if let Some(v) = string(t, "server", "description")? {
                config.description = v;
            }
            if let Some(v) = string(t, "server", "password")? {
                config.password = v;
            }
            if let Some(v) = integer(t, "server", "max_players")? {
                config.max_players = u16::try_from(v)
                    .ok()
                    .filter(|n| (1..=MAX_CLIENTS as u16).contains(n))
                    .ok_or_else(|| {
                        value_error("[server] max_players: that number must be between 1 and 128")
                    })?;
            }
            if let Some(v) = boolean(t, "server", "allow_new_players")? {
                config.allow_new_players = v;
            }
            if let Some(v) = boolean(t, "server", "allow_pvp")? {
                config.allow_pvp = v;
            }
        }
        if config.name.is_empty() {
            return Err(value_error(
                "[server] name: you must provide a name for your server",
            ));
        }
        if let Some(t) = section(&root, "network")? {
            check_keys(t, "network", &["address", "port"])?;
            if let Some(v) = string(t, "network", "address")? {
                config.address = v.parse().map_err(|_| {
                    value_error(format!("[network] address: {v} is not an address"))
                })?;
            }
            if let Some(v) = integer(t, "network", "port")? {
                config.port = u16::try_from(v)
                    .map_err(|_| value_error("[network] port must be 0 to 65535"))?;
            }
        }
        if let Some(t) = section(&root, "paths")? {
            check_keys(t, "paths", &["install_dir", "user_data"])?;
            config.install_dir = path(t, "paths", "install_dir")?;
            config.user_data = path(t, "paths", "user_data")?;
        }
        if let Some(t) = section(&root, "log")? {
            check_keys(t, "log", &["level", "categories", "file", "error_log"])?;
            if let Some(v) = integer(t, "log", "level")? {
                config.log_level = i32::try_from(v)
                    .ok()
                    .filter(|l| (1..8).contains(l))
                    .ok_or_else(|| value_error("[log] level must be 1 to 7"))?;
            }
            match t.get("categories") {
                None => {}
                Some(Value::Array(values)) => {
                    for value in values {
                        let category = value
                            .as_integer()
                            .and_then(|i| i32::try_from(i).ok())
                            .ok_or_else(|| value_error("[log] categories must be integers"))?;
                        config.log_categories.push(category);
                    }
                }
                Some(_) => return Err(value_error("[log] categories must be a list")),
            }
            config.log_file = path(t, "log", "file")?;
            config.error_log = path(t, "log", "error_log")?;
        }
        Ok(config)
    }

    /// The configuration as TOML, every key written.
    #[must_use]
    pub fn to_toml(&self) -> String {
        fn quoted(s: &str) -> String {
            Value::String(s.to_owned()).to_string()
        }
        fn path_text(p: Option<&PathBuf>) -> String {
            quoted(&p.map(|p| p.display().to_string()).unwrap_or_default())
        }
        let mut out = String::new();
        let _ = writeln!(
            out,
            "# The Outpost:Frontier dedicated server (OutpostServer)."
        );
        let _ = writeln!(out, "\n[server]");
        let _ = writeln!(out, "name = {}", quoted(&self.name));
        let _ = writeln!(out, "description = {}", quoted(&self.description));
        let _ = writeln!(out, "password = {}", quoted(&self.password));
        let _ = writeln!(out, "max_players = {}", self.max_players);
        let _ = writeln!(out, "allow_new_players = {}", self.allow_new_players);
        let _ = writeln!(out, "allow_pvp = {}", self.allow_pvp);
        let _ = writeln!(out, "\n[network]");
        let _ = writeln!(out, "address = {}", quoted(&self.address.to_string()));
        let _ = writeln!(out, "port = {}", self.port);
        let _ = writeln!(out, "\n[paths]");
        let _ = writeln!(
            out,
            "install_dir = {}",
            path_text(self.install_dir.as_ref())
        );
        let _ = writeln!(out, "user_data = {}", path_text(self.user_data.as_ref()));
        let _ = writeln!(out, "\n[log]");
        let _ = writeln!(out, "level = {}", self.log_level);
        let categories: Vec<String> = self
            .log_categories
            .iter()
            .map(|c| format!("0x{c:x}"))
            .collect();
        let _ = writeln!(out, "categories = [{}]", categories.join(", "));
        let _ = writeln!(out, "file = {}", path_text(self.log_file.as_ref()));
        let _ = writeln!(out, "error_log = {}", path_text(self.error_log.as_ref()));
        out
    }

    /// The old dialog's settings over these (decision T22). The "internet
    /// accessible" flag has no use without the list server (decision O8);
    /// a player limit outside 1 to 128 keeps the current one.
    pub fn import_legacy(&mut self, legacy: &LegacyServerConfig) {
        let text = |w: &[u16]| String::from_utf16_lossy(w);
        if !legacy.name.is_empty() {
            self.name = text(&legacy.name);
        }
        self.description = text(&legacy.description);
        self.password = text(&legacy.password);
        self.allow_new_players = legacy.allow_new_players;
        self.allow_pvp = legacy.allow_pvp;
        if let Ok(n) = u16::try_from(legacy.max_players)
            && (1..=MAX_CLIENTS as u16).contains(&n)
        {
            self.max_players = n;
        }
    }

    /// The user-data folder: the configured one, or
    /// `Documents\My Games\OutpostFrontier` under the user's profile
    /// (`USERPROFILE`, else `HOME`), or the working folder without either.
    #[must_use]
    pub fn user_data_dir(&self) -> PathBuf {
        self.user_data.clone().unwrap_or_else(default_user_data)
    }

    /// The install folder: the configured one or the executable's.
    #[must_use]
    pub fn install_dir(&self) -> PathBuf {
        self.install_dir.clone().unwrap_or_else(|| {
            std::env::current_exe()
                .ok()
                .and_then(|p| p.parent().map(Path::to_path_buf))
                .unwrap_or_default()
        })
    }
}

/// `Documents\My Games\OutpostFrontier` under the user's profile.
#[must_use]
pub fn default_user_data() -> PathBuf {
    std::env::var_os("USERPROFILE")
        .or_else(|| std::env::var_os("HOME"))
        .map(PathBuf::from)
        .unwrap_or_default()
        .join("Documents")
        .join("My Games")
        .join("OutpostFrontier")
}

/// The configuration at `path`, or, when there is no file, the defaults
/// with the old `OutpostServer.cfg` of `legacy_dir` imported, written to
/// `path`. The flag says whether the old file was imported.
///
/// # Errors
///
/// [`ConfigError`] when the file cannot be read, parsed or written.
pub fn load_or_create(
    path: &Path,
    legacy_dir: impl FnOnce(&ServerConfig) -> PathBuf,
) -> Result<(ServerConfig, bool), ConfigError> {
    match std::fs::read_to_string(path) {
        Ok(text) => return ServerConfig::parse(&text).map(|c| (c, false)),
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => {
            return Err(ConfigError::Io(path.to_path_buf(), e));
        }
        Err(_) => {}
    }
    let mut config = ServerConfig::default();
    let legacy = std::fs::read(legacy_dir(&config).join(LEGACY_CONFIG_FILE))
        .ok()
        .and_then(|bytes| LegacyServerConfig::from_bytes(&bytes));
    let imported = legacy.is_some();
    if let Some(legacy) = legacy {
        config.import_legacy(&legacy);
    }
    if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
        std::fs::create_dir_all(parent).map_err(|e| ConfigError::Io(parent.to_path_buf(), e))?;
    }
    std::fs::write(path, config.to_toml()).map_err(|e| ConfigError::Io(path.to_path_buf(), e))?;
    Ok((config, imported))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_empty_file_is_the_defaults() {
        assert_eq!(ServerConfig::parse("").unwrap(), ServerConfig::default());
    }

    #[test]
    fn the_committed_example_is_the_defaults() {
        let text = include_str!("../OutpostServer.example.toml");
        assert_eq!(ServerConfig::parse(text).unwrap(), ServerConfig::default());
    }

    #[test]
    fn every_key_is_read() {
        let text = r#"
            [server]
            name = "Frontier"
            description = "Test"
            password = "pw"
            max_players = 128
            allow_new_players = false
            allow_pvp = true
            [network]
            address = "127.0.0.1"
            port = 0
            [paths]
            install_dir = "/srv/outpost"
            user_data = ""
            [log]
            level = 7
            categories = [0x10005, 65537]
            file = "server.log"
            error_log = "errors.log"
        "#;
        let config = ServerConfig::parse(text).unwrap();
        assert_eq!(config.name, "Frontier");
        assert_eq!(config.description, "Test");
        assert_eq!(config.password, "pw");
        assert_eq!(config.max_players, 128);
        assert!(!config.allow_new_players);
        assert!(config.allow_pvp);
        assert_eq!(config.address, IpAddr::from([127, 0, 0, 1]));
        assert_eq!(config.port, 0);
        assert_eq!(config.install_dir, Some(PathBuf::from("/srv/outpost")));
        assert_eq!(config.user_data, None);
        assert_eq!(config.log_level, 7);
        assert_eq!(config.log_categories, [0x10005, 0x10001]);
        assert_eq!(config.log_file, Some(PathBuf::from("server.log")));
        assert_eq!(config.error_log, Some(PathBuf::from("errors.log")));
        // What it writes reads back the same.
        assert_eq!(ServerConfig::parse(&config.to_toml()).unwrap(), config);
    }

    #[test]
    fn the_dialogs_refusals_are_errors() {
        for text in [
            "[server]\nmax_players = 0",
            "[server]\nmax_players = 129",
            "[server]\nname = \"\"",
            "[log]\nlevel = 8",
            "[network]\nport = 70000",
            "[network]\naddress = \"nowhere\"",
            "[server]\nmaxplayers = 3",
            "[sever]\nname = \"x\"",
            "[server]\nallow_pvp = 1",
            "not toml",
        ] {
            assert!(ServerConfig::parse(text).is_err(), "{text}");
        }
    }

    #[test]
    fn the_old_dialog_file_is_imported_once() {
        let dir = std::env::temp_dir().join(format!("outpost-config-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let legacy = LegacyServerConfig {
            name: "Old Server".encode_utf16().collect(),
            password: "old".encode_utf16().collect(),
            max_players: 40,
            allow_pvp: true,
            ..LegacyServerConfig::default()
        };
        std::fs::write(dir.join(LEGACY_CONFIG_FILE), legacy.to_bytes()).unwrap();
        let path = dir.join(CONFIG_FILE);
        let (config, imported) = load_or_create(&path, |_| dir.clone()).unwrap();
        assert!(imported);
        assert_eq!(config.name, "Old Server");
        assert_eq!(config.password, "old");
        assert_eq!(config.max_players, 40);
        assert!(config.allow_pvp);
        // The second start reads the TOML file.
        std::fs::write(dir.join(LEGACY_CONFIG_FILE), b"").unwrap();
        let (again, imported) = load_or_create(&path, |_| dir.clone()).unwrap();
        assert!(!imported);
        assert_eq!(again, config);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
