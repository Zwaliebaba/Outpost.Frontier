//! The player's per-user state (work package P4.1, decision O27): the login
//! history, the table layouts and the account values, kept in the per-user
//! folder. When none of their files exists, the C++ client's registry key
//! is imported once and the files are written; the registry is never
//! written.

use neuron_client::user_state::{self, Protector, RegKey, StateError, UserPaths};

use crate::account_keys::{self, AccountKeys};
use crate::login_history::{self, LoginFileError, LoginHistory};
use crate::table_layouts::{self, TableLayouts};

/// The player's per-user state.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Profile {
    pub logins: LoginHistory,
    pub tables: TableLayouts,
    pub account: AccountKeys,
}

/// Why the profile could not be loaded or saved.
#[derive(Debug)]
pub enum ProfileError {
    State(StateError),
    Login(LoginFileError),
    /// `tables.toml` or `account.toml` is of another version or malformed.
    Malformed(&'static str),
    Protect(user_state::ProtectError),
}

impl std::fmt::Display for ProfileError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::State(e) => e.fmt(f),
            Self::Login(e) => e.fmt(f),
            Self::Malformed(file) => write!(f, "{file} is malformed or of another version"),
            Self::Protect(e) => e.fmt(f),
        }
    }
}

impl std::error::Error for ProfileError {}

impl From<StateError> for ProfileError {
    fn from(e: StateError) -> Self {
        Self::State(e)
    }
}

/// How the profile was found.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Origin {
    /// From the state files.
    Files,
    /// Imported from the C++ client's registry key, and saved.
    Imported,
    /// Neither files nor a registry key: a new player.
    New,
}

const FILES: [&str; 3] = [
    login_history::FILE_NAME,
    table_layouts::FILE_NAME,
    account_keys::FILE_NAME,
];

impl Profile {
    /// The profile from `paths`, or, when none of its files exists, from
    /// the registry tree `legacy` returns (the C++ client's key), saved to
    /// the files at once.
    pub fn load_or_import(
        paths: &UserPaths,
        legacy: impl FnOnce() -> Option<RegKey>,
        protector: &dyn Protector,
    ) -> Result<(Profile, Origin), ProfileError> {
        if FILES.iter().any(|f| paths.file(f).exists()) {
            return Ok((Self::load(paths, protector)?, Origin::Files));
        }
        let Some(root) = legacy() else {
            return Ok((Profile::default(), Origin::New));
        };
        let profile = Profile {
            logins: LoginHistory::from_registry(&root),
            tables: TableLayouts::from_registry(&root),
            account: account_keys::from_registry(&root),
        };
        profile.save(paths, protector)?;
        Ok((profile, Origin::Imported))
    }

    /// Reads the state files; a missing file gives its empty value.
    pub fn load(paths: &UserPaths, protector: &dyn Protector) -> Result<Profile, ProfileError> {
        let mut profile = Profile::default();
        if let Some(t) = user_state::load(&paths.file(login_history::FILE_NAME))? {
            profile.logins = LoginHistory::from_toml(&t, protector).map_err(ProfileError::Login)?;
        }
        if let Some(t) = user_state::load(&paths.file(table_layouts::FILE_NAME))? {
            profile.tables = TableLayouts::from_toml(&t)
                .ok_or(ProfileError::Malformed(table_layouts::FILE_NAME))?;
        }
        if let Some(t) = user_state::load(&paths.file(account_keys::FILE_NAME))? {
            profile.account = account_keys::from_toml(&t)
                .ok_or(ProfileError::Malformed(account_keys::FILE_NAME))?;
        }
        Ok(profile)
    }

    /// Writes the three state files.
    pub fn save(&self, paths: &UserPaths, protector: &dyn Protector) -> Result<(), ProfileError> {
        let logins = self
            .logins
            .to_toml(protector)
            .map_err(ProfileError::Protect)?;
        user_state::save(&paths.file(login_history::FILE_NAME), &logins)?;
        user_state::save(
            &paths.file(table_layouts::FILE_NAME),
            &self.tables.to_toml(),
        )?;
        user_state::save(
            &paths.file(account_keys::FILE_NAME),
            &account_keys::to_toml(&self.account),
        )?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_client::user_state::{ProtectError, RegValue};

    struct Plain;

    impl Protector for Plain {
        fn protect(&self, plain: &[u8]) -> Result<Vec<u8>, ProtectError> {
            Ok(plain.to_vec())
        }
        fn unprotect(&self, sealed: &[u8]) -> Result<Vec<u8>, ProtectError> {
            Ok(sealed.to_vec())
        }
    }

    fn temp(name: &str) -> UserPaths {
        let dir =
            std::env::temp_dir().join(format!("outpost-profile-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        UserPaths::at(dir)
    }

    #[test]
    fn the_registry_is_imported_once_then_the_files_win() {
        let paths = temp("import");
        let mut root = RegKey::default();
        root.values
            .insert("MPAccountName".into(), RegValue::Sz("abcd".into()));
        let mut calls = 0;
        let (p, origin) = Profile::load_or_import(
            &paths,
            || {
                calls += 1;
                Some(root.clone())
            },
            &Plain,
        )
        .unwrap();
        assert_eq!(origin, Origin::Imported);
        assert_eq!(p.account.name.as_deref(), Some("abcd"));
        for f in FILES {
            assert!(paths.file(f).exists(), "{f}");
        }
        // The second run reads the files and never asks the registry.
        let (q, origin) =
            Profile::load_or_import(&paths, || panic!("registry read"), &Plain).unwrap();
        assert_eq!(origin, Origin::Files);
        assert_eq!(q, p);
        assert_eq!(calls, 1);
        std::fs::remove_dir_all(paths.root()).unwrap();
    }

    #[test]
    fn without_files_or_registry_the_player_is_new() {
        let paths = temp("new");
        let (p, origin) = Profile::load_or_import(&paths, || None, &Plain).unwrap();
        assert_eq!(origin, Origin::New);
        assert_eq!(p, Profile::default());
        assert!(!paths.root().exists());
    }
}
