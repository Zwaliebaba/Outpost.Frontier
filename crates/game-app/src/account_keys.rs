//! The multiplayer account values (`CPlayerAccount` in
//! `Common/common_playeraccount.cpp`; work package P4.1, decision O27): the
//! install key and the two MD5 texts that name the account to the server.
//!
//! The C++ keeps `InstallKey`, `MPAccountName` and `MPAccountNameSig` as
//! `REG_SZ` values of the client's registry key. The port keeps them in
//! `account.toml`, imported once by [`AccountKeys::from_registry`].
//!
//! A new install derives them as the C++ does (O27): [`seed_machine_hash`]
//! is `SeedMachineHash` over the machine's queries
//! ([`MachineIdentity`]: the `DigitalProductId`, a UUID without one, the
//! user name, a millisecond count without that), [`generate_text_key`] is
//! `GenerateTextKey` (the key the account page offers), and
//! [`AccountKeys::from_text_key`] is `SetTextKey` (the key the player
//! submits, checked, and the two account texts made from it).
//! [`generate_account`] is `GenerateAccount`, which the server uses for its
//! own account. [`AccountKeys::text_key`], [`AccountKeys::verify`] and
//! [`AccountKeys::server_signature`] are `GetTextKey`, `LoadAccount` with
//! `VerifyAccount`, and `GetServerSignature` over the stored values. The
//! account page and the login that call them are the game flow's (P5).
//!
//! `SetAcctIndex` is an empty function in the C++, so the account index
//! that `GenerateTextKey` would add to the seed is always 0 and never
//! added.

use neuron_client::user_state::{MachineIdentity, PRODUCT_ID_CAPACITY, ProductIdKey, RegKey};
use neuron_core::util::md5::{Md5Hash, Md5Words, md5_text, md5_words};

/// The length of an MD5 text (`"%08x-%08x-%08x-%08x"`), the part of a
/// text key the check hashes.
pub const RAW_KEY_LEN: usize = 0x23;

/// The length of a text key: the MD5 text, `-` and four check characters.
pub const TEXT_KEY_LEN: usize = 0x28;

/// The size `GetTextKey` reads `InstallKey` into, terminator included.
pub const TEXT_KEY_READ: usize = 0x29;

/// `SeedMachineHash`: a hash fed with the machine's `DigitalProductId`
/// (the Windows 9x key first, then the NT key, sharing one size that is
/// set to the buffer's once; 16 bytes of a new UUID when neither gives
/// one), then the user name up to its first NUL (the millisecond count as
/// `%u` when there is none).
pub fn seed_machine_hash(machine: &mut dyn MachineIdentity) -> Md5Hash {
    let mut size = PRODUCT_ID_CAPACITY;
    let mut product_id = machine
        .product_id(ProductIdKey::Windows9x, &mut size)
        .flatten();
    if product_id.is_none() {
        // A failed first query may leave a size larger than the buffer;
        // the C++ would then let the second write past it (D530).
        size = size.min(PRODUCT_ID_CAPACITY);
        product_id = machine
            .product_id(ProductIdKey::WindowsNt, &mut size)
            .flatten();
    }
    let mut hash = Md5Hash::new();
    match product_id {
        Some(bytes) => hash.add_data(&bytes),
        None => hash.add_data(&machine.uuid()),
    }
    let name = match machine.user_name() {
        Some(name) => name,
        None => machine.milliseconds().to_string().into_bytes(),
    };
    let end = name.iter().position(|&b| b == 0).unwrap_or(name.len());
    hash.add_data(&name[..end]);
    hash
}

/// `GenerateAccount`: the account name (the machine seed, then `name`'s
/// bytes when there is a name) and its signature (the MD5 of the name's
/// text).
pub fn generate_account(machine: &mut dyn MachineIdentity, name: Option<&[u8]>) -> AccountKeys {
    let mut hash = seed_machine_hash(machine);
    if let Some(name) = name {
        let end = name.iter().position(|&b| b == 0).unwrap_or(name.len());
        hash.add_data(&name[..end]);
    }
    let name = hash.calc_value().to_text();
    let signature = md5_text(name.as_bytes());
    AccountKeys {
        install_key: None,
        name: Some(name),
        signature: Some(signature),
    }
}

/// The last four characters of a text, as the key check takes them.
fn last_four(text: &str) -> &str {
    &text[text.len().saturating_sub(4)..]
}

/// `GenerateTextKey`: the MD5 text of the machine seed, `-`, and the last
/// four characters of the MD5 text of that text.
pub fn generate_text_key(machine: &mut dyn MachineIdentity) -> String {
    let raw = seed_machine_hash(machine).calc_value().to_text();
    let check = md5_text(raw.as_bytes());
    format!("{raw}-{}", last_four(&check))
}

/// The key check of `SetTextKey` and `GetTextKey` over a buffer: the MD5
/// text of its first [`RAW_KEY_LEN`] bytes ends in the text from byte
/// `0x24` to the first NUL.
fn key_check(buffer: &[u8]) -> bool {
    let check = md5_text(&buffer[..RAW_KEY_LEN]);
    let tail = &buffer[RAW_KEY_LEN + 1..];
    let tail = &tail[..tail.iter().position(|&b| b == 0).unwrap_or(tail.len())];
    tail == last_four(&check).as_bytes()
}

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

    /// `SetTextKey`: the key the player submits, accepted when the MD5
    /// text of its first [`RAW_KEY_LEN`] bytes ends in the key's text from
    /// byte `0x24` on; then `InstallKey` is the key, `MPAccountName` the
    /// MD5 text of the whole key and `MPAccountNameSig` the MD5 text of
    /// that. A key shorter than `0x24` bytes is refused (D531).
    #[must_use]
    pub fn from_text_key(key: &str) -> Option<AccountKeys> {
        let bytes = key.as_bytes();
        let bytes = &bytes[..bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len())];
        if bytes.len() <= RAW_KEY_LEN || !key_check(bytes) {
            return None;
        }
        let name = md5_text(bytes);
        let signature = md5_text(name.as_bytes());
        Some(AccountKeys {
            install_key: Some(String::from_utf8_lossy(bytes).into_owned()),
            name: Some(name),
            signature: Some(signature),
        })
    }

    /// `GetTextKey`: the install key when it fits the C++'s read
    /// ([`TEXT_KEY_READ`] bytes into a zeroed buffer) and passes
    /// `SetTextKey`'s check.
    #[must_use]
    pub fn text_key(&self) -> Option<&str> {
        let key = self.install_key.as_deref()?;
        if key.len() > TEXT_KEY_READ {
            return None;
        }
        let mut buffer = [0u8; 0x2c];
        buffer[..key.len()].copy_from_slice(key.as_bytes());
        if !key_check(&buffer) {
            return None;
        }
        let end = key.find('\0').unwrap_or(key.len());
        Some(&key[..end])
    }

    /// The two account hashes as `LoadAccount` makes them: each value read
    /// with `FromString`, a missing one left at the fresh hash's state.
    fn hashes(&self) -> (Md5Words, Md5Words) {
        let load = |v: &Option<String>| v.as_deref().map(|t| Md5Words::parse(t.as_bytes()));
        (
            load(&self.name).unwrap_or_default(),
            load(&self.signature).unwrap_or_default(),
        )
    }

    /// `LoadAccount` and `VerifyAccount`: the MD5 of the name hash's text
    /// equals the signature hash.
    #[must_use]
    pub fn verify(&self) -> bool {
        let (name, signature) = self.hashes();
        md5_words(name.to_text().as_bytes()) == signature
    }

    /// `GetServerSignature`: the hash read from `text` with its 16 bytes
    /// XORed with the signature hash's bytes taken backwards, as text (the
    /// login name for a server, P5).
    #[must_use]
    pub fn server_signature(&self, text: &[u8]) -> String {
        let (_, signature) = self.hashes();
        let verify = signature.to_bytes();
        let mut bytes = Md5Words::parse(text).to_bytes();
        for (index, byte) in bytes.iter_mut().enumerate() {
            *byte ^= verify[0x0f - index];
        }
        Md5Words::from_digest(bytes).to_text()
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
