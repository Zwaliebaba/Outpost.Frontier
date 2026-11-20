//! The server's account files (work package P3.1): the account CRC and the
//! names it makes, the accounts root and its folders, the obfuscated `name`
//! file, the `banned` and `admin` flags, the folder scan of `PlayerDB::init`
//! and the multiplayer server's own account in `accounts.cfg`.
//!
//! C++ sources: `Server/server_pub.cpp:776-935` (`Server_GetAccountsRoot`,
//! the CRC engine, `Server_AccountDirName`), `Server/server_classes.cpp`
//! (`InitServerAccount` `:388`, `PlayerDB::init`'s scan `:1876-1950`,
//! `BuildAccountFilePath` `:3266`, the name-file key `:3229`,
//! `PlayerDB::CreateAccount` `:4344`, `CAccount::InitFromFolder` `:5035`).
//!
//! The root is a parameter (decision O41): the executables give the C++'s
//! `Documents\My Games\OutpostFrontier` on Windows.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use neuron_core::account::{AccountKeys, MachineIdentity, generate_account};

/// The width of the account CRC.
const CRC_WIDTH: u32 = 31;
/// Its polynomial.
const CRC_POLY: u32 = 0x8005;

/// `AccountCrcReflect`: the low `bits` bits of `value` reversed; the bits
/// above are kept.
fn reflect(value: u32, bits: u32) -> u32 {
    let mut result = value;
    let mut source = value;
    for index in 0..bits {
        let mask = 1u32 << (bits - index - 1);
        if source & 1 == 0 {
            result &= !mask;
        } else {
            result |= mask;
        }
        source >>= 1;
    }
    result
}

/// The mask of the CRC's width.
const fn crc_mask() -> u32 {
    let top = 1u32 << (CRC_WIDTH - 1);
    ((top - 1) * 2 + 1) | 1
}

fn crc_table() -> [u32; 256] {
    let top = 1u32 << (CRC_WIDTH - 1);
    let mut table = [0u32; 256];
    for (index, entry) in table.iter_mut().enumerate() {
        let mut work = (reflect(index as u32, 8) & 0xFF) << (CRC_WIDTH - 8);
        for _ in 0..8 {
            work = if work & top != 0 {
                (work << 1) ^ CRC_POLY
            } else {
                work << 1
            };
        }
        *entry = reflect(work, CRC_WIDTH) & crc_mask();
    }
    table
}

/// `Server_AccountCrc`: the server's CRC engine (width 31, polynomial
/// 0x8005, reflected in and out, no XOR in or out) over `bytes`, with the
/// result's bytes gathered from the most significant end of the 32-bit
/// register as the engine's finalize does.
#[must_use]
pub fn account_crc(bytes: &[u8]) -> u32 {
    let table = crc_table();
    let mut crc = 0u32;
    for &byte in bytes {
        crc = table[((crc & 0xFF) ^ u32::from(byte)) as usize] ^ (crc >> 8);
    }
    let mut remain = crc & crc_mask();
    let mut folded = 0u32;
    let mut shift = 8;
    while shift <= 32 {
        folded |= (remain & 0xFF) << (32 - shift);
        remain >>= 8;
        shift += 8;
    }
    folded
}

/// `towlower` in the "C" locale, which the server never leaves for
/// `LC_CTYPE`: only `A` to `Z` change.
fn lower_wide(c: u16) -> u16 {
    if (u16::from(b'A')..=u16::from(b'Z')).contains(&c) {
        c + 0x20
    } else {
        c
    }
}

/// The little-endian bytes of a wide string, as the CRC reads `wchar_t`s.
fn wide_bytes(text: &[u16]) -> Vec<u8> {
    text.iter().flat_map(|c| c.to_le_bytes()).collect()
}

/// `Server_AccountDirName` (and `ComputeHashedCharacterID`): the name up to
/// its first NUL, lowered, its CRC over `2 * len` bytes, as
/// `"%02x-%08x"` of the length and the CRC. A multiplayer account's folder
/// and every character file are named so.
#[must_use]
pub fn hashed_name(name: &[u16]) -> String {
    let name = &name[..name.iter().position(|&c| c == 0).unwrap_or(name.len())];
    let lowered: Vec<u16> = name.iter().map(|&c| lower_wide(c)).collect();
    let crc = account_crc(&wide_bytes(&lowered));
    format!("{:02x}-{crc:08x}", name.len())
}

/// The file name of a character's save: [`hashed_name`] and `.fl`, lower
/// case (`_strlwr`, which leaves the hex digits as they are).
#[must_use]
pub fn character_file_name(character: &[u16]) -> String {
    format!("{}.fl", hashed_name(character))
}

/// The constant the `name` file's key is made from.
const NAME_KEY_SOURCE: &str = "AKL!8167fGBA0712";

/// The `name` file's XOR key: the low 16 bits of the account CRC of
/// `L"AKL!8167fGBA0712"`.
#[must_use]
pub fn name_file_key() -> u16 {
    let wide: Vec<u16> = NAME_KEY_SOURCE.encode_utf16().collect();
    account_crc(&wide_bytes(&wide)) as u16
}

/// `PlayerDB::CreateAccount`'s `name` file: each wide character of the name
/// (up to its first NUL) XORed with the key, little endian, no terminator.
#[must_use]
pub fn encode_name_file(name: &[u16]) -> Vec<u8> {
    let key = name_file_key();
    name.iter()
        .take_while(|&&c| c != 0)
        .flat_map(|&c| (c ^ key).to_le_bytes())
        .collect()
}

/// The most wide characters `CAccount::InitFromFolder` reads.
pub const NAME_FILE_MAX: usize = 0x400;

/// `CAccount::InitFromFolder`'s read of the `name` file: up to 0x400 whole
/// wide characters (a trailing odd byte is not read), each XORed with the
/// key, the name ending at the first decoded NUL.
#[must_use]
pub fn decode_name_file(bytes: &[u8]) -> Vec<u16> {
    let key = name_file_key();
    bytes
        .as_chunks::<2>()
        .0
        .iter()
        .take(NAME_FILE_MAX)
        .map(|&pair| u16::from_le_bytes(pair) ^ key)
        .take_while(|&c| c != 0)
        .collect()
}

/// The folder under the user-data root that holds the accounts:
/// `Accts`, or `Accts\MultiPlayer` for a multiplayer server.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AccountsRoot {
    root: PathBuf,
    multiplayer: bool,
}

impl AccountsRoot {
    /// The accounts folder of the user-data folder `user_data` (the C++'s
    /// `GetUserDataPath`, `Documents\My Games\OutpostFrontier`).
    #[must_use]
    pub fn new(user_data: &Path, multiplayer: bool) -> Self {
        let mut root = user_data.join("Accts");
        if multiplayer {
            root.push("MultiPlayer");
        }
        Self { root, multiplayer }
    }

    /// `Server_GetAccountsRoot`.
    #[must_use]
    pub fn path(&self) -> &Path {
        &self.root
    }

    /// Whether this is a multiplayer server's root.
    #[must_use]
    pub fn multiplayer(&self) -> bool {
        self.multiplayer
    }

    /// The folder name of an account: [`hashed_name`] on a multiplayer
    /// server, the name itself in single player (`wcstombs` in the "C"
    /// locale, where only characters up to 0xFF convert; the name ends at
    /// the first other one, D597).
    #[must_use]
    pub fn account_dir_name(&self, account: &[u16]) -> String {
        if self.multiplayer {
            hashed_name(account)
        } else {
            account
                .iter()
                .take_while(|&&c| c != 0 && c <= 0xFF)
                .map(|&c| char::from(c as u8))
                .collect()
        }
    }

    /// An account's folder.
    #[must_use]
    pub fn account_dir(&self, account: &[u16]) -> PathBuf {
        self.root.join(self.account_dir_name(account))
    }

    /// `PlayerDB::MakeLocalUserPath`: `"%s\%s"` of the root and a file
    /// name.
    #[must_use]
    pub fn local_user_path(&self, file_name: &str) -> PathBuf {
        self.root.join(file_name)
    }

    /// `accounts.cfg` of a multiplayer server.
    #[must_use]
    pub fn server_account_path(&self) -> PathBuf {
        self.root.join("accounts.cfg")
    }

    /// `cheaters.log`.
    #[must_use]
    pub fn cheaters_log_path(&self) -> PathBuf {
        self.root.join("cheaters.log")
    }
}

/// What `CAccount::InitFromFolder` reads from an account's folder.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct AccountFolder {
    /// The folder's name.
    pub dir_name: String,
    /// The name from the `name` file; empty when it is missing.
    pub name: Vec<u16>,
    /// A `banned` file exists.
    pub banned: bool,
    /// An `admin` file exists.
    pub admin: bool,
    /// The `*.fl` files in listing order, as found (`m_CharacterIDMap` keys
    /// are their lower-case form).
    pub character_files: Vec<String>,
}

impl AccountFolder {
    /// `CAccount::InitFromFolder` on `dir`.
    #[must_use]
    pub fn read(dir: &Path) -> Self {
        let name = fs::read(dir.join("name"))
            .map(|bytes| decode_name_file(&bytes))
            .unwrap_or_default();
        Self {
            dir_name: dir
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default(),
            name,
            banned: dir.join("banned").is_file(),
            admin: dir.join("admin").is_file(),
            character_files: Vec::new(),
        }
    }
}

/// The entries of a directory in byte order of their names, as the port
/// lists directories (decision O14, D37).
fn sorted_entries(dir: &Path) -> Vec<fs::DirEntry> {
    let mut entries: Vec<fs::DirEntry> = match fs::read_dir(dir) {
        Ok(read) => read.filter_map(Result::ok).collect(),
        Err(_) => return Vec::new(),
    };
    entries.sort_by_key(fs::DirEntry::file_name);
    entries
}

/// `*.fl` as `_findfirst` matches it: an extension of `fl` in any case.
fn is_save_file(name: &str) -> bool {
    name.len() > 3 && name[name.len() - 3..].eq_ignore_ascii_case(".fl")
}

/// Phases (D) and (E) of `PlayerDB::init` on a multiplayer server: every
/// subfolder of the root whose name does not start with `.`, read as an
/// account, with its `*.fl` files.
#[must_use]
pub fn scan_accounts(root: &AccountsRoot) -> Vec<AccountFolder> {
    let mut accounts = Vec::new();
    for entry in sorted_entries(root.path()) {
        let name = entry.file_name().to_string_lossy().into_owned();
        if name.starts_with('.') || !entry.path().is_dir() {
            continue;
        }
        let mut folder = AccountFolder::read(&entry.path());
        folder.character_files = sorted_entries(&entry.path())
            .into_iter()
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| is_save_file(n))
            .collect();
        accounts.push(folder);
    }
    accounts
}

/// The size of each account text in `accounts.cfg`: 35 characters and a
/// NUL.
pub const SERVER_ACCOUNT_TEXT: usize = 0x24;

/// The contents of `accounts.cfg`: two words, then the account name and its
/// signature as NUL-padded texts.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ServerAccountFile {
    /// The first word (a random salt; never read back).
    pub salt: u32,
    /// The second word (a provisional server id; never read back).
    pub stored_id: u32,
    /// The account name text.
    pub name: String,
    /// The signature text.
    pub signature: String,
}

/// A text as `fwrite(text, 0x24)` stores it from a string of 35 characters:
/// padded or cut to 0x24 bytes.
fn fixed_text(text: &str) -> [u8; SERVER_ACCOUNT_TEXT] {
    let mut out = [0u8; SERVER_ACCOUNT_TEXT];
    let bytes = text.as_bytes();
    let n = bytes.len().min(SERVER_ACCOUNT_TEXT);
    out[..n].copy_from_slice(&bytes[..n]);
    out
}

/// A text read as `fread(buffer, 0x24)` into a zeroed 0x25-byte buffer:
/// up to the first NUL.
fn read_text(bytes: &[u8]) -> String {
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).into_owned()
}

/// `sscanf(text, "%x")`: white space, an optional sign and `0x`, then hex
/// digits, accumulated modulo 2^32; 0 without digits.
fn scan_hex(text: &str) -> u32 {
    let mut s = text.trim_start_matches([' ', '\t', '\n', '\r', '\x0b', '\x0c']);
    let negative = s.starts_with('-');
    if s.starts_with('-') || s.starts_with('+') {
        s = &s[1..];
    }
    if (s.starts_with("0x") || s.starts_with("0X"))
        && s[2..].starts_with(|c: char| c.is_ascii_hexdigit())
    {
        s = &s[2..];
    }
    let mut value = 0u32;
    for c in s.chars() {
        let Some(d) = c.to_digit(16) else {
            break;
        };
        value = value.wrapping_mul(16).wrapping_add(d);
    }
    if negative {
        value.wrapping_neg()
    } else {
        value
    }
}

impl ServerAccountFile {
    /// The file's bytes: the two words little endian, then the two texts of
    /// 0x24 bytes each.
    #[must_use]
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(8 + 2 * SERVER_ACCOUNT_TEXT);
        out.extend_from_slice(&self.salt.to_le_bytes());
        out.extend_from_slice(&self.stored_id.to_le_bytes());
        out.extend_from_slice(&fixed_text(&self.name));
        out.extend_from_slice(&fixed_text(&self.signature));
        out
    }

    /// Reads the file as `InitServerAccount` does: each field as far as the
    /// file has it, the rest zero.
    #[must_use]
    pub fn from_bytes(bytes: &[u8]) -> Self {
        let mut padded = bytes.to_vec();
        padded.resize(8 + 2 * SERVER_ACCOUNT_TEXT, 0);
        let word = |at: usize| u32::from_le_bytes(padded[at..at + 4].try_into().expect("4 bytes"));
        Self {
            salt: word(0),
            stored_id: word(4),
            name: read_text(&padded[8..8 + SERVER_ACCOUNT_TEXT]),
            signature: read_text(&padded[8 + SERVER_ACCOUNT_TEXT..]),
        }
    }

    /// The server id the C++ uses: the leading hex run of the signature
    /// text (`sscanf("%x")`); the stored word is ignored.
    #[must_use]
    pub fn server_id(&self) -> u32 {
        scan_hex(&self.signature)
    }

    /// `LoadAccountFromStrings` and `VerifyAccount`.
    #[must_use]
    pub fn verify(&self) -> bool {
        self.keys().verify()
    }

    /// The account values.
    #[must_use]
    pub fn keys(&self) -> AccountKeys {
        AccountKeys {
            install_key: None,
            name: Some(self.name.clone()),
            signature: Some(self.signature.clone()),
        }
    }
}

/// The C runtime's `rand()` and the clock `InitServerAccount` seeds it from.
pub trait ServerAccountSeed {
    /// `srand(timeGetTime())`.
    fn reseed_from_clock(&mut self);
    /// `rand()`.
    fn rand(&mut self) -> i32;
}

/// `(rand() << 16) | (rand() & 0xffff)`, the left draw first.
fn rand_word(seed: &mut dyn ServerAccountSeed) -> u32 {
    let high = (seed.rand() as u32) << 16;
    high | (seed.rand() as u32 & 0xFFFF)
}

/// A fresh `accounts.cfg`: `srand(timeGetTime())`, three words (salt, then
/// two whose XOR is the provisional id) and the account `GenerateAccount`
/// makes for `"SERVER"` on this machine.
pub fn new_server_account(
    seed: &mut dyn ServerAccountSeed,
    machine: &mut dyn MachineIdentity,
) -> ServerAccountFile {
    seed.reseed_from_clock();
    let salt = rand_word(seed);
    let a = rand_word(seed);
    let b = rand_word(seed);
    let keys = generate_account(machine, Some(b"SERVER"));
    ServerAccountFile {
        salt,
        stored_id: b ^ a,
        name: keys.name.unwrap_or_default(),
        signature: keys.signature.unwrap_or_default(),
    }
}

/// The attempts `InitServerAccount` makes (the reference bounds the
/// original's endless retry at four).
pub const SERVER_ACCOUNT_ATTEMPTS: usize = 4;

/// The server's own account and id.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ServerAccount {
    /// The account values.
    pub keys: AccountKeys,
    /// The server id.
    pub id: u32,
}

/// `InitServerAccount` on a multiplayer server's root: creates the folder;
/// reads `accounts.cfg`, or writes a new one and reads it back; a file that
/// fails the check is deleted and made again. `Ok(None)` when no attempt
/// verified or the file could not be written.
///
/// # Errors
///
/// The folder could not be created.
pub fn init_server_account(
    root: &AccountsRoot,
    seed: &mut dyn ServerAccountSeed,
    machine: &mut dyn MachineIdentity,
) -> io::Result<Option<ServerAccount>> {
    fs::create_dir_all(root.path())?;
    let path = root.server_account_path();
    for _ in 0..SERVER_ACCOUNT_ATTEMPTS {
        let Ok(bytes) = fs::read(&path) else {
            let file = new_server_account(seed, machine);
            if fs::write(&path, file.to_bytes()).is_err() {
                return Ok(None);
            }
            continue;
        };
        let file = ServerAccountFile::from_bytes(&bytes);
        if file.verify() {
            return Ok(Some(ServerAccount {
                keys: file.keys(),
                id: file.server_id(),
            }));
        }
        let _ = fs::remove_file(&path);
    }
    Ok(None)
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_core::account::ProductIdKey;

    fn wide(text: &str) -> Vec<u16> {
        text.encode_utf16().collect()
    }

    #[test]
    fn hashed_names_fold_ascii_case_only() {
        assert_eq!(hashed_name(&wide("Trent")), hashed_name(&wide("tRENT")));
        assert_ne!(
            hashed_name(&wide("\u{c9}mile")),
            hashed_name(&wide("\u{e9}mile"))
        );
        assert!(hashed_name(&wide("Trent")).starts_with("05-"));
        assert_eq!(hashed_name(&[0x41, 0, 0x42]), hashed_name(&wide("a")));
        assert_eq!(
            character_file_name(&wide("x")),
            format!("{}.fl", hashed_name(&wide("X")))
        );
    }

    #[test]
    fn the_crc_table_stays_within_31_bits() {
        assert_eq!(account_crc(&[]), 0);
        assert_eq!(crc_mask(), 0x7FFF_FFFF);
        assert!(crc_table().iter().all(|&e| e <= crc_mask()));
    }

    #[test]
    fn the_name_file_round_trips() {
        let name = wide("Trent \u{4e2d}");
        let bytes = encode_name_file(&name);
        assert_eq!(bytes.len(), name.len() * 2);
        assert_eq!(decode_name_file(&bytes), name);
        let mut odd = bytes.clone();
        odd.push(0x55);
        assert_eq!(decode_name_file(&odd), name);
        let key = name_file_key().to_le_bytes();
        let mut with_nul = bytes;
        with_nul.extend_from_slice(&key);
        with_nul.extend_from_slice(&encode_name_file(&wide("junk")));
        assert_eq!(decode_name_file(&with_nul), name);
    }

    #[test]
    fn roots_and_folder_names() {
        let mp = AccountsRoot::new(Path::new("/u"), true);
        assert_eq!(mp.path(), Path::new("/u/Accts/MultiPlayer"));
        assert_eq!(mp.account_dir_name(&wide("Bob")), hashed_name(&wide("bob")));
        let sp = AccountsRoot::new(Path::new("/u"), false);
        assert_eq!(sp.path(), Path::new("/u/Accts"));
        assert_eq!(
            sp.account_dir_name(&wide("Bob\u{e9}\u{4e2d}x")),
            "Bob\u{e9}"
        );
    }

    #[test]
    fn scan_hex_reads_the_leading_run() {
        assert_eq!(scan_hex("1a2b3c4d-ffff"), 0x1a2b_3c4d);
        assert_eq!(scan_hex("  0x10"), 0x10);
        assert_eq!(scan_hex("-1"), u32::MAX);
        assert_eq!(scan_hex("zz"), 0);
        assert_eq!(scan_hex("123456789"), 0x2345_6789);
    }

    struct Seed(neuron_core::rng::CrtRand);

    impl ServerAccountSeed for Seed {
        fn reseed_from_clock(&mut self) {
            self.0.seed(1234);
        }
        fn rand(&mut self) -> i32 {
            self.0.rand()
        }
    }

    struct Machine;

    impl MachineIdentity for Machine {
        fn product_id(&mut self, _key: ProductIdKey, _size: &mut u32) -> Option<Option<Vec<u8>>> {
            Some(Some(b"product".to_vec()))
        }
        fn uuid(&mut self) -> [u8; 16] {
            [7; 16]
        }
        fn user_name(&mut self) -> Option<Vec<u8>> {
            Some(b"server".to_vec())
        }
        fn milliseconds(&mut self) -> u32 {
            0
        }
    }

    fn temp_dir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "neuron-server-account-{tag}-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&dir);
        dir
    }

    #[test]
    fn the_server_account_is_made_once_and_verifies() {
        let dir = temp_dir("cfg");
        let root = AccountsRoot::new(&dir, true);
        let mut seed = Seed(neuron_core::rng::CrtRand::default());
        let first = init_server_account(&root, &mut seed, &mut Machine)
            .unwrap()
            .expect("account");
        let bytes = fs::read(root.server_account_path()).unwrap();
        assert_eq!(bytes.len(), 8 + 2 * SERVER_ACCOUNT_TEXT);
        let file = ServerAccountFile::from_bytes(&bytes);
        assert_eq!(file.to_bytes(), bytes);
        assert_eq!(first.id, scan_hex(&file.signature));
        let again = init_server_account(&root, &mut seed, &mut Machine)
            .unwrap()
            .expect("account");
        assert_eq!(again, first);
        // A damaged file is replaced by a fresh one.
        let mut damaged = bytes;
        damaged[8] ^= 1;
        fs::write(root.server_account_path(), &damaged).unwrap();
        let fresh = init_server_account(&root, &mut seed, &mut Machine)
            .unwrap()
            .expect("account");
        assert!(
            ServerAccountFile::from_bytes(&fs::read(root.server_account_path()).unwrap()).verify()
        );
        assert_eq!(
            fresh.keys, first.keys,
            "the same machine makes the same account"
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_scan_reads_folders_in_byte_order() {
        let dir = temp_dir("scan");
        let root = AccountsRoot::new(&dir, true);
        let b = root.account_dir(&wide("Bob"));
        let a = root.account_dir(&wide("Al"));
        for (path, name) in [(&b, "Bob"), (&a, "Al")] {
            fs::create_dir_all(path).unwrap();
            fs::write(path.join("name"), encode_name_file(&wide(name))).unwrap();
        }
        fs::write(b.join("banned"), b"").unwrap();
        fs::write(b.join("ZZ.FL"), b"").unwrap();
        fs::write(b.join("01-00000000.fl"), b"").unwrap();
        fs::write(b.join("notes.txt"), b"").unwrap();
        fs::create_dir_all(root.path().join(".hidden")).unwrap();
        fs::write(root.server_account_path(), b"").unwrap();
        let scanned = scan_accounts(&root);
        let mut want = [
            root.account_dir_name(&wide("Al")),
            root.account_dir_name(&wide("Bob")),
        ];
        want.sort();
        let got: Vec<&str> = scanned.iter().map(|f| f.dir_name.as_str()).collect();
        assert_eq!(got, want);
        let bob = scanned.iter().find(|f| f.name == wide("Bob")).unwrap();
        assert!(bob.banned && !bob.admin);
        assert_eq!(bob.character_files, ["01-00000000.fl", "ZZ.FL"]);
        let _ = fs::remove_dir_all(&dir);
    }
}
