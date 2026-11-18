//! The multiplayer account values (`CPlayerAccount` in
//! `Common/common_playeraccount.cpp`; work package P4.1, decision O27): the
//! install key and the two MD5 texts that name the account to the server.
//!
//! The C++ keeps `InstallKey`, `MPAccountName` and `MPAccountNameSig` as
//! `REG_SZ` values of the client's registry key. The port keeps them in
//! `account.toml`, imported once by [`AccountKeys::from_registry`]. Deriving
//! them for a new install (`GenerateAccount`, `GenerateTextKey` and
//! `SetTextKey` over the machine seed of `SeedMachineHash`) is unchanged by
//! O27 and comes with the login flow.

use neuron_client::user_state::RegKey;

/// The state file of the account values.
pub const FILE_NAME: &str = "account.toml";

/// The format version of `account.toml`.
pub const VERSION: i64 = 1;

/// The account values; each is absent until the C++ or the port set it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct AccountKeys {
    /// `InstallKey`: the text key, 32 hex digits, `-` and a four-digit
    /// check.
    pub install_key: Option<String>,
    /// `MPAccountName`: the MD5 text of the install key.
    pub name: Option<String>,
    /// `MPAccountNameSig`: the MD5 text of the name.
    pub signature: Option<String>,
}

const FIELDS: [(&str, &str); 3] = [
    ("InstallKey", "install_key"),
    ("MPAccountName", "name"),
    ("MPAccountNameSig", "signature"),
];

impl AccountKeys {
    fn slots(&mut self) -> [&mut Option<String>; 3] {
        [&mut self.install_key, &mut self.name, &mut self.signature]
    }

    /// The values the C++ wrote under the client's registry key.
    #[must_use]
    pub fn from_registry(root: &RegKey) -> AccountKeys {
        let mut keys = AccountKeys::default();
        for (slot, (registry, _)) in keys.slots().into_iter().zip(FIELDS) {
            *slot = root.string(registry).map(str::to_owned);
        }
        keys
    }

    /// The values as the TOML of `account.toml`.
    #[must_use]
    pub fn to_toml(&self) -> toml::Table {
        let mut table = toml::Table::new();
        table.insert("version".into(), toml::Value::Integer(VERSION));
        let values = [&self.install_key, &self.name, &self.signature];
        for (value, (_, field)) in values.into_iter().zip(FIELDS) {
            if let Some(v) = value {
                table.insert(field.into(), toml::Value::String(v.clone()));
            }
        }
        table
    }

    /// Reads an `account.toml` table; `None` for another version or a
    /// value that is not text.
    #[must_use]
    pub fn from_toml(table: &toml::Table) -> Option<AccountKeys> {
        if table.get("version").and_then(toml::Value::as_integer) != Some(VERSION) {
            return None;
        }
        let mut keys = AccountKeys::default();
        for (slot, (_, field)) in keys.slots().into_iter().zip(FIELDS) {
            *slot = match table.get(field) {
                None => None,
                Some(v) => Some(v.as_str()?.to_owned()),
            };
        }
        Some(keys)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_client::user_state::RegValue;

    #[test]
    fn imports_and_round_trips() {
        let mut root = RegKey::default();
        root.values.insert(
            "InstallKey".into(),
            RegValue::Sz("0123456789abcdef0123456789abcdef-1a2b".into()),
        );
        root.values
            .insert("MPAccountName".into(), RegValue::Sz("ffee".into()));
        // A value of another type is not taken.
        root.values
            .insert("MPAccountNameSig".into(), RegValue::Dword(1));
        let keys = AccountKeys::from_registry(&root);
        assert_eq!(keys.name.as_deref(), Some("ffee"));
        assert_eq!(keys.signature, None);
        let text = toml::to_string(&keys.to_toml()).unwrap();
        assert_eq!(AccountKeys::from_toml(&text.parse().unwrap()), Some(keys));
        assert_eq!(
            AccountKeys::from_toml(&"version = 1\nname = 3".parse().unwrap()),
            None
        );
    }
}
