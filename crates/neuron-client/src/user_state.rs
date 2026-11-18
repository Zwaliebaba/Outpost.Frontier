//! Per-user state (work package P4.1; decision O27): the folder the client
//! keeps its files in, a TOML state file written atomically, the registry
//! tree a one-time import reads, and the protection of saved secrets.
//!
//! The C++ keeps login history, table layouts and the account values in the
//! registry under `HKCU\Software\Microsoft\Microsoft Games\OutpostFrontier\1.0`
//! (`reg_key_stack.cpp`, `login_mgr.cpp`, `ui_fast_table.cpp`,
//! `common_playeraccount.cpp`). Here they live in files under
//! `Documents\My Games\OutpostFrontier` (migration plan, P4.1). The first
//! run without state files reads that registry key once
//! (`platform::registry`) into a [`RegKey`] and converts it; the registry is
//! never written. Saved passwords are sealed with the Windows user's data
//! protection (`platform::dpapi`, O27) through a [`Protector`].

use std::collections::BTreeMap;
use std::fmt;
use std::io;
use std::path::{Path, PathBuf};

/// The registry key of the C++ client's per-user values, under
/// `HKEY_CURRENT_USER`.
pub const LEGACY_REGISTRY_KEY: &str = "Software\\Microsoft\\Microsoft Games\\OutpostFrontier\\1.0";

/// The deepest key nesting the import follows (the C++ key stack holds
/// 32).
pub const MAX_KEY_DEPTH: usize = 32;

/// Where the client keeps its per-user files.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UserPaths {
    root: PathBuf,
}

impl UserPaths {
    /// The folder below the user's Documents folder.
    pub const SUBDIRECTORY: [&'static str; 2] = ["My Games", "OutpostFrontier"];

    /// The paths below `documents`, the user's Documents folder.
    #[must_use]
    pub fn from_documents(documents: &Path) -> Self {
        let mut root = documents.to_path_buf();
        for part in Self::SUBDIRECTORY {
            root.push(part);
        }
        UserPaths { root }
    }

    /// The paths below `root` itself (tests, a portable install).
    #[must_use]
    pub fn at(root: PathBuf) -> Self {
        UserPaths { root }
    }

    #[must_use]
    pub fn root(&self) -> &Path {
        &self.root
    }

    /// The state file `name` (for example `login.toml`).
    #[must_use]
    pub fn file(&self, name: &str) -> PathBuf {
        self.root.join(name)
    }

    /// Creates the folder and its parents.
    pub fn create(&self) -> io::Result<()> {
        std::fs::create_dir_all(&self.root)
    }
}

/// Why a state file could not be read or written.
#[derive(Debug)]
pub enum StateError {
    Io(io::Error),
    /// The file is not valid TOML.
    Parse(String),
    /// The table could not be written as TOML.
    Serialize(String),
}

impl fmt::Display for StateError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(e) => write!(f, "state file: {e}"),
            Self::Parse(e) => write!(f, "state file is not valid TOML: {e}"),
            Self::Serialize(e) => write!(f, "state could not be written: {e}"),
        }
    }
}

impl std::error::Error for StateError {}

impl From<io::Error> for StateError {
    fn from(e: io::Error) -> Self {
        Self::Io(e)
    }
}

/// Reads a state file: `None` when it does not exist.
pub fn load(path: &Path) -> Result<Option<toml::Table>, StateError> {
    match std::fs::read_to_string(path) {
        Ok(text) => text
            .parse::<toml::Table>()
            .map(Some)
            .map_err(|e| StateError::Parse(e.to_string())),
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(e.into()),
    }
}

/// Writes a state file atomically: into a temporary file beside it, then
/// renamed over it, so a crash leaves the old file or the new one.
pub fn save(path: &Path, table: &toml::Table) -> Result<(), StateError> {
    let text = toml::to_string(table).map_err(|e| StateError::Serialize(e.to_string()))?;
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let mut temporary = path.as_os_str().to_owned();
    temporary.push(".tmp");
    let temporary = PathBuf::from(temporary);
    std::fs::write(&temporary, text)?;
    std::fs::rename(&temporary, path)?;
    Ok(())
}

/// A registry value as the import reads it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RegValue {
    /// `REG_DWORD`.
    Dword(u32),
    /// `REG_SZ` (and `REG_EXPAND_SZ`, unexpanded), without its terminator.
    Sz(String),
    /// `REG_BINARY`.
    Binary(Vec<u8>),
    /// Any other type, with its raw bytes.
    Other { kind: u32, bytes: Vec<u8> },
}

/// A registry key: its values and subkeys by name. Registry names compare
/// without case; the lookups here do too.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RegKey {
    pub values: BTreeMap<String, RegValue>,
    pub keys: BTreeMap<String, RegKey>,
}

impl RegKey {
    /// The value `name`, compared without ASCII case.
    #[must_use]
    pub fn value(&self, name: &str) -> Option<&RegValue> {
        self.values
            .iter()
            .find(|(k, _)| k.eq_ignore_ascii_case(name))
            .map(|(_, v)| v)
    }

    /// The subkey `name`, compared without ASCII case.
    #[must_use]
    pub fn key(&self, name: &str) -> Option<&RegKey> {
        self.keys
            .iter()
            .find(|(k, _)| k.eq_ignore_ascii_case(name))
            .map(|(_, v)| v)
    }

    /// The subkey at a backslash-separated `path`.
    #[must_use]
    pub fn path(&self, path: &str) -> Option<&RegKey> {
        path.split('\\')
            .filter(|p| !p.is_empty())
            .try_fold(self, |key, part| key.key(part))
    }

    /// A `REG_DWORD` value.
    #[must_use]
    pub fn dword(&self, name: &str) -> Option<u32> {
        match self.value(name)? {
            RegValue::Dword(v) => Some(*v),
            _ => None,
        }
    }

    /// A `REG_SZ` value.
    #[must_use]
    pub fn string(&self, name: &str) -> Option<&str> {
        match self.value(name)? {
            RegValue::Sz(v) => Some(v),
            _ => None,
        }
    }

    /// A `REG_BINARY` value.
    #[must_use]
    pub fn binary(&self, name: &str) -> Option<&[u8]> {
        match self.value(name)? {
            RegValue::Binary(v) => Some(v),
            _ => None,
        }
    }
}

/// Why a secret could not be sealed or opened.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProtectError(pub String);

impl fmt::Display for ProtectError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "secret protection: {}", self.0)
    }
}

impl std::error::Error for ProtectError {}

/// Seals secrets for the current user (DPAPI on Windows, O27).
pub trait Protector {
    fn protect(&self, plain: &[u8]) -> Result<Vec<u8>, ProtectError>;
    fn unprotect(&self, sealed: &[u8]) -> Result<Vec<u8>, ProtectError>;
}

/// Lower-case hexadecimal text of `bytes`, as the state files keep binary
/// values.
#[must_use]
pub fn to_hex(bytes: &[u8]) -> String {
    use std::fmt::Write;
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        let _ = write!(out, "{b:02x}");
    }
    out
}

/// The bytes of hexadecimal text; `None` for an odd length or a non-hex
/// digit.
#[must_use]
pub fn from_hex(text: &str) -> Option<Vec<u8>> {
    let digits = text.as_bytes();
    if !digits.len().is_multiple_of(2) {
        return None;
    }
    digits
        .chunks(2)
        .map(|pair| u8::from_str_radix(std::str::from_utf8(pair).ok()?, 16).ok())
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn paths_sit_under_my_games() {
        let p = UserPaths::from_documents(Path::new("/home/u/Documents"));
        assert_eq!(
            p.root(),
            Path::new("/home/u/Documents/My Games/OutpostFrontier")
        );
        assert_eq!(
            p.file("login.toml"),
            Path::new("/home/u/Documents/My Games/OutpostFrontier/login.toml")
        );
    }

    #[test]
    fn state_files_round_trip_and_replace_atomically() {
        let dir = std::env::temp_dir().join(format!("outpost-state-{}", std::process::id()));
        let paths = UserPaths::at(dir.clone());
        let file = paths.file("state.toml");
        assert!(load(&file).unwrap().is_none());
        let mut t = toml::Table::new();
        t.insert("version".into(), toml::Value::Integer(1));
        save(&file, &t).unwrap();
        assert_eq!(load(&file).unwrap(), Some(t.clone()));
        t.insert("name".into(), toml::Value::String("pilot".into()));
        save(&file, &t).unwrap();
        assert_eq!(load(&file).unwrap(), Some(t));
        // No temporary file is left behind.
        let names: Vec<_> = std::fs::read_dir(&dir)
            .unwrap()
            .map(|e| e.unwrap().file_name())
            .collect();
        assert_eq!(names, vec![std::ffi::OsString::from("state.toml")]);
        std::fs::write(&file, "not = = toml").unwrap();
        assert!(matches!(load(&file), Err(StateError::Parse(_))));
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn registry_lookups_ignore_case() {
        let mut child = RegKey::default();
        child.values.insert("id".into(), RegValue::Dword(7));
        child
            .values
            .insert("Account".into(), RegValue::Sz("41004200".into()));
        child
            .values
            .insert("Columns".into(), RegValue::Binary(vec![1, 2]));
        let mut root = RegKey::default();
        root.keys.insert("Logins".into(), RegKey::default());
        root.keys
            .get_mut("Logins")
            .unwrap()
            .keys
            .insert("0".into(), child);
        let k = root.path("logins\\0").unwrap();
        assert_eq!(k.dword("ID"), Some(7));
        assert_eq!(k.string("account"), Some("41004200"));
        assert_eq!(k.binary("columns"), Some(&[1u8, 2][..]));
        assert_eq!(k.dword("account"), None);
        assert!(root.path("Logins\\1").is_none());
    }

    #[test]
    fn hex_round_trips() {
        assert_eq!(to_hex(&[0, 0xab, 0x10]), "00ab10");
        assert_eq!(from_hex("00AB10"), Some(vec![0, 0xab, 0x10]));
        assert_eq!(from_hex("abc"), None);
        assert_eq!(from_hex("zz"), None);
    }
}
