//! The platform-free core of `CPlayerAccount` (`Common/common_playeraccount.cpp`;
//! work package P4.1, moved here in P3.1 by decision O41): the install key
//! and the two MD5 texts that name an account to a server, which the client
//! and the multiplayer server's own account (`accounts.cfg`) share.
//!
//! [`seed_machine_hash`] is `SeedMachineHash` over the machine's queries
//! ([`MachineIdentity`]: the `DigitalProductId`, a UUID without one, the
//! user name, a millisecond count without that), [`generate_text_key`] is
//! `GenerateTextKey` (the key the account page offers), and
//! [`AccountKeys::from_text_key`] is `SetTextKey` (the key the player
//! submits, checked, and the two account texts made from it).
//! [`generate_account`] is `GenerateAccount`, which the server uses for its
//! own account. [`AccountKeys::text_key`], [`AccountKeys::verify`] and
//! [`AccountKeys::server_signature`] are `GetTextKey`, `LoadAccount` (or
//! `LoadAccountFromStrings`) with `VerifyAccount`, and `GetServerSignature`
//! over the stored values. The machine queries are answered by the platform
//! (`neuron-client::platform::machine` on Windows); the client's registry
//! import and state file stay in `game-app::account_keys`.
//!
//! `SetAcctIndex` is an empty function in the C++, so the account index
//! that `GenerateTextKey` would add to the seed is always 0 and never
//! added.

use crate::util::md5::{Md5Hash, Md5Words, md5_text, md5_words};

/// The two keys under `HKEY_LOCAL_MACHINE` that `SeedMachineHash`
/// reads `DigitalProductId` from, the Windows 9x key first.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProductIdKey {
    /// `Software\Microsoft\Windows\CurrentVersion\`.
    Windows9x,
    /// `Software\Microsoft\Windows NT\CurrentVersion\`.
    WindowsNt,
}

impl ProductIdKey {
    /// The key's path, with the C++'s trailing backslash.
    #[must_use]
    pub fn path(self) -> &'static str {
        match self {
            ProductIdKey::Windows9x => "Software\\Microsoft\\Windows\\CurrentVersion\\",
            ProductIdKey::WindowsNt => "Software\\Microsoft\\Windows NT\\CurrentVersion\\",
        }
    }
}

/// The size of `SeedMachineHash`'s product-id buffer, in bytes.
pub const PRODUCT_ID_CAPACITY: u32 = 0x400;

/// The size `SeedMachineHash` gives `GetUserNameA`, in characters with the
/// terminator.
pub const USER_NAME_CAPACITY: u32 = 0x101;

/// The machine queries an account is derived from (`SeedMachineHash`;
/// decision O27). On Windows `neuron-client::platform::machine::WinMachine`
/// asks the system; tests give fixed answers.
pub trait MachineIdentity {
    /// `RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, KEY_QUERY_VALUE)` and, when
    /// it opens, `RegQueryValueExA(key, "DigitalProductId")` into a buffer
    /// of [`PRODUCT_ID_CAPACITY`] bytes whose size `size` claims, updated
    /// as the call updates it. `None` when the key does not open (nothing
    /// is queried and `size` is kept); `Some(None)` when the query fails.
    fn product_id(&mut self, key: ProductIdKey, size: &mut u32) -> Option<Option<Vec<u8>>>;
    /// `UuidCreate`: a new UUID, as its 16 bytes in memory.
    fn uuid(&mut self) -> [u8; 16];
    /// `GetUserNameA` into [`USER_NAME_CAPACITY`] characters: the name's
    /// bytes in the system code page, without the terminator; `None` when
    /// the call fails.
    fn user_name(&mut self) -> Option<Vec<u8>>;
    /// The millisecond counter the C++ reads with `timeGetTime` when the
    /// user name cannot be had.
    fn milliseconds(&mut self) -> u32;
}

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

impl AccountKeys {
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
