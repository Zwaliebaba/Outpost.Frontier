//! The login history (`FLoginHistory` in `Client/login_mgr.cpp`; work
//! package P4.1, decision O27): the accounts the player logged in with,
//! keyed by id, with the saved password and the time of the last login.
//!
//! The C++ keeps it in the registry under `MP\Logins\Login1`, `Login2`, …
//! of the client's key, names and passwords as `WideToHex` text. The port
//! keeps it in `login.toml` in the per-user folder, the password sealed for
//! the Windows user (DPAPI) and written as hex. [`LoginHistory::from_registry`]
//! reads the C++'s keys once, as `FLoginHistory::Load` does.

use std::collections::BTreeMap;

use neuron_client::user_state::{Protector, RegKey, RegValue, from_hex, to_hex};
use neuron_core::msvcrt::atoi;
use neuron_core::util::wide_hex::hex_to_wide;

/// The state file of the login history.
pub const FILE_NAME: &str = "login.toml";

/// The format version of `login.toml`.
pub const VERSION: i64 = 1;

/// One account of the history (`FLoginRecord`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LoginRecord {
    /// The account name, UTF-16 code units as the C++ keeps them.
    pub account: Vec<u16>,
    /// The saved password; empty when none is saved.
    pub password: Vec<u16>,
    /// The time of the last login, as the C++ stores it.
    pub time: u32,
}

/// The history: records by id, in id order as the C++'s `std::map`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LoginHistory {
    pub records: BTreeMap<u32, LoginRecord>,
}

/// Why a `login.toml` could not be read.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum LoginFileError {
    /// The file's version is not [`VERSION`].
    Version(Option<i64>),
    /// A record lacks a field or has one of the wrong type.
    Record(String),
    /// A password could not be opened (another Windows user, or damaged).
    Password(String),
}

impl std::fmt::Display for LoginFileError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Version(v) => write!(f, "login.toml has version {v:?}, not {VERSION}"),
            Self::Record(r) => write!(f, "login.toml record: {r}"),
            Self::Password(e) => write!(f, "login.toml password: {e}"),
        }
    }
}

impl std::error::Error for LoginFileError {}

/// `ReadRegHexString`: a `REG_SZ` value decoded by `HexToWide`; `None` when
/// it is missing or of another type.
fn hex_string(key: &RegKey, name: &str) -> Option<Vec<u16>> {
    key.string(name).map(|s| hex_to_wide(s.as_bytes()))
}

/// A `REG_DWORD` value as is, a `REG_SZ` value through `atoi`.
fn number(key: &RegKey, name: &str) -> Option<u32> {
    match key.value(name)? {
        RegValue::Dword(v) => Some(*v),
        RegValue::Sz(s) => Some(atoi(s.as_bytes()) as u32),
        _ => None,
    }
}

impl LoginHistory {
    /// `FLoginHistory::Load` over the client's registry key: `Login1`,
    /// `Login2`, … under `MP\Logins` until one is missing. A record without
    /// a readable `id` takes the previous record's id (the first one 0, as
    /// the port of the C++ starts it) and so lands on that record; a field
    /// that is missing leaves what the record had, but `time` becomes 0.
    #[must_use]
    pub fn from_registry(root: &RegKey) -> LoginHistory {
        let mut history = LoginHistory::default();
        let Some(logins) = root.path("MP\\Logins") else {
            return history;
        };
        let mut id = 0u32;
        for index in 1.. {
            let Some(key) = logins.key(&format!("Login{index}")) else {
                break;
            };
            if let Some(v) = number(key, "id") {
                id = v;
            }
            let record = history.records.entry(id).or_default();
            if let Some(account) = hex_string(key, "account") {
                record.account = account;
            }
            if let Some(password) = hex_string(key, "password") {
                record.password = password;
            }
            record.time = number(key, "time").unwrap_or(0);
        }
        history
    }

    /// The history as the TOML of `login.toml`: names as text, passwords
    /// sealed by `protector` and written as hex (absent when none is
    /// saved).
    pub fn to_toml(
        &self,
        protector: &dyn Protector,
    ) -> Result<toml::Table, neuron_client::user_state::ProtectError> {
        let mut records = Vec::new();
        for (id, r) in &self.records {
            let mut t = toml::Table::new();
            t.insert("id".into(), toml::Value::Integer(i64::from(*id)));
            t.insert(
                "account".into(),
                toml::Value::String(String::from_utf16_lossy(&r.account)),
            );
            if !r.password.is_empty() {
                let bytes: Vec<u8> = r.password.iter().flat_map(|u| u.to_le_bytes()).collect();
                let sealed = protector.protect(&bytes)?;
                t.insert("password".into(), toml::Value::String(to_hex(&sealed)));
            }
            t.insert("time".into(), toml::Value::Integer(i64::from(r.time)));
            records.push(toml::Value::Table(t));
        }
        let mut table = toml::Table::new();
        table.insert("version".into(), toml::Value::Integer(VERSION));
        table.insert("login".into(), toml::Value::Array(records));
        Ok(table)
    }

    /// Reads a `login.toml` table, opening the passwords with `protector`.
    pub fn from_toml(
        table: &toml::Table,
        protector: &dyn Protector,
    ) -> Result<LoginHistory, LoginFileError> {
        let version = table.get("version").and_then(toml::Value::as_integer);
        if version != Some(VERSION) {
            return Err(LoginFileError::Version(version));
        }
        let mut history = LoginHistory::default();
        let empty = Vec::new();
        let records = match table.get("login") {
            Some(toml::Value::Array(a)) => a,
            None => &empty,
            Some(_) => return Err(LoginFileError::Record("`login` is not an array".into())),
        };
        for value in records {
            let bad = |what: &str| LoginFileError::Record(what.to_owned());
            let t = value.as_table().ok_or_else(|| bad("not a table"))?;
            let int = |name: &str| {
                t.get(name)
                    .and_then(toml::Value::as_integer)
                    .and_then(|v| u32::try_from(v).ok())
                    .ok_or_else(|| bad(name))
            };
            let id = int("id")?;
            let time = int("time")?;
            let account = t
                .get("account")
                .and_then(toml::Value::as_str)
                .ok_or_else(|| bad("account"))?
                .encode_utf16()
                .collect();
            let password = match t.get("password").map(|v| v.as_str()) {
                None => Vec::new(),
                Some(Some(hex)) => {
                    let sealed = from_hex(hex).ok_or_else(|| bad("password"))?;
                    let bytes = protector
                        .unprotect(&sealed)
                        .map_err(|e| LoginFileError::Password(e.0))?;
                    bytes
                        .as_chunks::<2>()
                        .0
                        .iter()
                        .map(|p| u16::from_le_bytes(*p))
                        .collect()
                }
                Some(None) => return Err(bad("password")),
            };
            history.records.insert(
                id,
                LoginRecord {
                    account,
                    password,
                    time,
                },
            );
        }
        Ok(history)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_client::user_state::ProtectError;

    /// A stand-in for DPAPI: reverses the bytes behind a marker.
    struct Reverse;

    impl Protector for Reverse {
        fn protect(&self, plain: &[u8]) -> Result<Vec<u8>, ProtectError> {
            let mut v = vec![0xaa];
            v.extend(plain.iter().rev());
            Ok(v)
        }
        fn unprotect(&self, sealed: &[u8]) -> Result<Vec<u8>, ProtectError> {
            match sealed.split_first() {
                Some((0xaa, rest)) => Ok(rest.iter().rev().copied().collect()),
                _ => Err(ProtectError("not sealed".into())),
            }
        }
    }

    fn wide(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    fn login(fields: &[(&str, RegValue)]) -> RegKey {
        let mut k = RegKey::default();
        for (n, v) in fields {
            k.values.insert((*n).into(), v.clone());
        }
        k
    }

    fn registry(logins: Vec<RegKey>) -> RegKey {
        let mut list = RegKey::default();
        for (i, l) in logins.into_iter().enumerate() {
            list.keys.insert(format!("Login{}", i + 1), l);
        }
        let mut mp = RegKey::default();
        mp.keys.insert("Logins".into(), list);
        let mut root = RegKey::default();
        root.keys.insert("MP".into(), mp);
        root
    }

    #[test]
    fn the_registry_import_reads_as_load_does() {
        let root = registry(vec![
            login(&[
                ("id", RegValue::Dword(5)),
                ("account", RegValue::Sz("00500069006c006f0074".into())),
                ("password", RegValue::Sz("0078".into())),
                ("time", RegValue::Dword(1234)),
            ]),
            // An id as text through atoi; a time of another type gives 0.
            login(&[
                ("id", RegValue::Sz("2".into())),
                ("account", RegValue::Sz("0042".into())),
                ("time", RegValue::Binary(vec![1])),
            ]),
            // No id: the previous record's (2), whose account it replaces.
            login(&[
                ("account", RegValue::Sz("0043".into())),
                ("time", RegValue::Sz("77".into())),
            ]),
        ]);
        let h = LoginHistory::from_registry(&root);
        assert_eq!(h.records.len(), 2);
        assert_eq!(
            h.records[&5],
            LoginRecord {
                account: wide("Pilot"),
                password: wide("x"),
                time: 1234
            }
        );
        assert_eq!(
            h.records[&2],
            LoginRecord {
                account: wide("C"),
                password: Vec::new(),
                time: 77
            }
        );
        assert!(
            LoginHistory::from_registry(&RegKey::default())
                .records
                .is_empty()
        );
    }

    #[test]
    fn the_state_file_seals_passwords_and_round_trips() {
        let mut h = LoginHistory::default();
        h.records.insert(
            3,
            LoginRecord {
                account: wide("Ace"),
                password: wide("s3cret"),
                time: 9,
            },
        );
        h.records.insert(
            4,
            LoginRecord {
                account: wide("NoPass"),
                password: Vec::new(),
                time: 0,
            },
        );
        let t = h.to_toml(&Reverse).unwrap();
        let text = toml::to_string(&t).unwrap();
        assert!(!text.contains("s3cret"));
        assert!(text.contains("account = \"Ace\""));
        let back = LoginHistory::from_toml(&text.parse().unwrap(), &Reverse).unwrap();
        assert_eq!(back, h);
    }

    #[test]
    fn bad_files_are_refused() {
        let t: toml::Table = "version = 2".parse().unwrap();
        assert_eq!(
            LoginHistory::from_toml(&t, &Reverse),
            Err(LoginFileError::Version(Some(2)))
        );
        let t: toml::Table =
            "version = 1\n[[login]]\nid = 1\naccount = \"a\"\ntime = 0\npassword = \"00\""
                .parse()
                .unwrap();
        assert!(matches!(
            LoginHistory::from_toml(&t, &Reverse),
            Err(LoginFileError::Password(_))
        ));
        let t: toml::Table = "version = 1\n[[login]]\nid = -1\naccount = \"a\"\ntime = 0"
            .parse()
            .unwrap();
        assert!(matches!(
            LoginHistory::from_toml(&t, &Reverse),
            Err(LoginFileError::Record(_))
        ));
    }
}
