//! The client's copy of the multiplayer account values (`CPlayerAccount` in
//! `Common/common_playeraccount.cpp`; work package P4.1, decision O27).
//!
//! The account core (the machine seed, `GenerateAccount`, the text key, the
//! check and the server signature) is [`neuron_core::account`] (decision
//! O41), re-exported here. The C++ keeps `InstallKey`, `MPAccountName` and
//! `MPAccountNameSig` as `REG_SZ` values of the client's registry key; the
//! port keeps them in `account.toml`, imported once by [`from_registry`].
//! The account page and the login that use them are the game flow's (P5).

use neuron_client::user_state::RegKey;
pub use neuron_core::account::{
    AccountKeys, RAW_KEY_LEN, TEXT_KEY_LEN, TEXT_KEY_READ, generate_account, generate_text_key,
    seed_machine_hash,
};

/// The state file of the account values.
pub const FILE_NAME: &str = "account.toml";

/// The format version of `account.toml`.
pub const VERSION: i64 = 1;

const FIELDS: [(&str, &str); 3] = [
    ("InstallKey", "install_key"),
    ("MPAccountName", "name"),
    ("MPAccountNameSig", "signature"),
];

fn slots(keys: &mut AccountKeys) -> [&mut Option<String>; 3] {
    [&mut keys.install_key, &mut keys.name, &mut keys.signature]
}

/// The values the C++ wrote under the client's registry key.
#[must_use]
pub fn from_registry(root: &RegKey) -> AccountKeys {
    let mut keys = AccountKeys::default();
    for (slot, (registry, _)) in slots(&mut keys).into_iter().zip(FIELDS) {
        *slot = root.string(registry).map(str::to_owned);
    }
    keys
}

/// The values as the TOML of `account.toml`.
#[must_use]
pub fn to_toml(keys: &AccountKeys) -> toml::Table {
    let mut table = toml::Table::new();
    table.insert("version".into(), toml::Value::Integer(VERSION));
    let values = [&keys.install_key, &keys.name, &keys.signature];
    for (value, (_, field)) in values.into_iter().zip(FIELDS) {
        if let Some(v) = value {
            table.insert(field.into(), toml::Value::String(v.clone()));
        }
    }
    table
}

/// Reads an `account.toml` table; `None` for another version or a value
/// that is not text.
#[must_use]
pub fn from_toml(table: &toml::Table) -> Option<AccountKeys> {
    if table.get("version").and_then(toml::Value::as_integer) != Some(VERSION) {
        return None;
    }
    let mut keys = AccountKeys::default();
    for (slot, (_, field)) in slots(&mut keys).into_iter().zip(FIELDS) {
        *slot = match table.get(field) {
            None => None,
            Some(v) => Some(v.as_str()?.to_owned()),
        };
    }
    Some(keys)
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
        let keys = from_registry(&root);
        assert_eq!(keys.name.as_deref(), Some("ffee"));
        assert_eq!(keys.signature, None);
        let text = toml::to_string(&to_toml(&keys)).unwrap();
        assert_eq!(from_toml(&text.parse().unwrap()), Some(keys));
        assert_eq!(from_toml(&"version = 1\nname = 3".parse().unwrap()), None);
    }
}
