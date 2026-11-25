//! The account registry of a multiplayer server (work package P3.2):
//! `CAccount` and the two maps of `PlayerDB`, `m_AccountNameMap` (by
//! account name) and `m_CharacterIDMap` (by character file name), with
//! `PlayerDB::CreateAccount`, `DeleteAccount`, `DeleteCharacterFromID`,
//! `FindAccountFromName`, `FindAccountFromCharacterID`,
//! `FindAccountFromCharacterName`, the admin rights and `LockAccountAccess`
//! (`Server/server_classes.cpp:3221-3445`, `:3712`, `:4255-4380`,
//! `:4949-5140`).
//!
//! Both maps compare their keys as the C++'s `ci_wchar_traits` and
//! `ci_char_traits` do: `_wcsnicmp` / `_strnicmp` over the shorter length
//! (A to Z folded, the "C" locale), the shorter key first when that ties.
//! Accounts live in a slot vector; the maps hold slot indices, as the C++
//! maps hold `CAccount*`.

use std::cmp::Ordering;
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use crate::account::{AccountFolder, AccountsRoot, encode_name_file, hashed_name};

/// A to Z folded to a to z.
fn fold(c: u32) -> u32 {
    if (u32::from(b'A')..=u32::from(b'Z')).contains(&c) {
        c + 32
    } else {
        c
    }
}

fn ci_cmp(a: impl Iterator<Item = u32> + Clone, b: impl Iterator<Item = u32> + Clone) -> Ordering {
    let (la, lb) = (a.clone().count(), b.clone().count());
    for (x, y) in a.zip(b) {
        match fold(x).cmp(&fold(y)) {
            Ordering::Equal => {}
            other => return other,
        }
    }
    la.cmp(&lb)
}

/// An account name as `m_AccountNameMap` orders it (`CiWString`).
#[derive(Clone, Debug, Default)]
pub struct CiWName(pub Vec<u16>);

impl PartialEq for CiWName {
    fn eq(&self, other: &Self) -> bool {
        self.cmp(other).is_eq()
    }
}
impl Eq for CiWName {}
impl PartialOrd for CiWName {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for CiWName {
    fn cmp(&self, other: &Self) -> Ordering {
        ci_cmp(
            self.0.iter().map(|&c| u32::from(c)),
            other.0.iter().map(|&c| u32::from(c)),
        )
    }
}

/// A character id as `m_CharacterIDMap` orders it (`CiString`): the
/// lower-case file name, `"<len>-<crc>.fl"`.
#[derive(Clone, Debug, Default)]
pub struct CiName(pub Vec<u8>);

impl PartialEq for CiName {
    fn eq(&self, other: &Self) -> bool {
        self.cmp(other).is_eq()
    }
}
impl Eq for CiName {}
impl PartialOrd for CiName {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for CiName {
    fn cmp(&self, other: &Self) -> Ordering {
        ci_cmp(
            self.0.iter().map(|&c| u32::from(c)),
            other.0.iter().map(|&c| u32::from(c)),
        )
    }
}

/// One account (`CAccount`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Account {
    /// The name from the `name` file.
    pub name: Vec<u16>,
    /// The folder, set at login (`m_DirectoryPath`).
    pub directory_path: PathBuf,
    /// The names of its characters, in the order they were found or made.
    pub characters: Vec<Vec<u16>>,
    /// Logins are refused (`LockAccountAccess`).
    pub locked: bool,
    /// No `banned` file (`m_bNotBanned`).
    pub not_banned: bool,
    /// An `admin` file.
    pub admin: bool,
    /// The session logged in on it, 0 for none.
    pub client_id: u32,
}

impl Default for Account {
    /// `CAccount::CAccount`: not locked, not banned, no rights, no session.
    fn default() -> Self {
        Self {
            name: Vec::new(),
            directory_path: PathBuf::new(),
            characters: Vec::new(),
            locked: false,
            not_banned: true,
            admin: false,
            client_id: 0,
        }
    }
}

impl Account {
    /// `CAccount::InitFromFolder` as [`AccountFolder::read`] reads it.
    #[must_use]
    pub fn from_folder(folder: &AccountFolder) -> Self {
        Self {
            name: folder.name.clone(),
            not_banned: !folder.banned,
            admin: folder.admin,
            ..Self::default()
        }
    }
}

/// The handle of an account in the registry (the C++'s `CAccount*`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct AccountId(usize);

/// What a multiplayer server knows of its accounts.
#[derive(Clone, Debug, Default)]
pub struct AccountRegistry {
    accounts: Vec<Option<Account>>,
    by_name: BTreeMap<CiWName, AccountId>,
    by_character: BTreeMap<CiName, AccountId>,
}

/// `"<hash>.fl"` lowered, the character id of a character name
/// (`ComputeHashedCharacterID`, `_makepath(.., "fl")`, `_strlwr`).
#[must_use]
pub fn character_id(name: &[u16]) -> Vec<u8> {
    format!("{}.fl", hashed_name(name))
        .to_ascii_lowercase()
        .into_bytes()
}

impl AccountRegistry {
    /// An empty registry.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The account of a handle.
    #[must_use]
    pub fn get(&self, id: AccountId) -> Option<&Account> {
        self.accounts.get(id.0)?.as_ref()
    }

    /// The account of a handle, to change.
    pub fn get_mut(&mut self, id: AccountId) -> Option<&mut Account> {
        self.accounts.get_mut(id.0)?.as_mut()
    }

    /// Adds an account under its name; an account already under that name
    /// stays in the slots but is no longer found by it (`map[name] = acct`).
    pub fn insert(&mut self, account: Account) -> AccountId {
        let id = AccountId(self.accounts.len());
        self.by_name.insert(CiWName(account.name.clone()), id);
        self.accounts.push(Some(account));
        id
    }

    /// Records a character file as the account's (`m_CharacterIDMap[id] =
    /// acct`); the id is lowered first.
    pub fn insert_character_id(&mut self, file_name: &[u8], account: AccountId) {
        self.by_character
            .insert(CiName(file_name.to_ascii_lowercase()), account);
    }

    /// Phases (D) and (E) of `PlayerDB::init`: every scanned folder becomes
    /// an account, and each of its `*.fl` files a character id; `names`
    /// gives a file's character name (`ReadCharacterName`), and only a
    /// non-empty one is added to the account's characters.
    #[must_use]
    pub fn from_folders(
        folders: &[AccountFolder],
        root: &AccountsRoot,
        mut names: impl FnMut(&Path) -> Vec<u16>,
    ) -> Self {
        let mut registry = Self::new();
        for folder in folders {
            let id = registry.insert(Account::from_folder(folder));
            for file in &folder.character_files {
                registry.insert_character_id(file.as_bytes(), id);
                let name = names(&root.path().join(&folder.dir_name).join(file));
                if !name.is_empty()
                    && let Some(account) = registry.get_mut(id)
                {
                    account.characters.push(name);
                }
            }
        }
        registry
    }

    /// `FindAccountFromName`.
    #[must_use]
    pub fn find_by_name(&self, name: &[u16]) -> Option<AccountId> {
        self.by_name.get(&CiWName(name.to_vec())).copied()
    }

    /// `FindAccountFromCharacterID`.
    #[must_use]
    pub fn find_by_character_id(&self, file_name: &[u8]) -> Option<AccountId> {
        self.by_character.get(&CiName(file_name.to_vec())).copied()
    }

    /// `FindAccountFromCharacterName`: through the name's character id.
    #[must_use]
    pub fn find_by_character_name(&self, name: &[u16]) -> Option<AccountId> {
        self.find_by_character_id(&character_id(name))
    }

    /// `GetAccountAdminRights`.
    #[must_use]
    pub fn admin_rights(&self, name: &[u16]) -> bool {
        self.find_by_name(name)
            .and_then(|id| self.get(id))
            .is_some_and(|a| a.admin)
    }

    /// `SetAccountAdminRights`: the `admin` file is made or removed in the
    /// account's folder and the right set; `false` for an unknown account.
    /// A file that cannot be made still sets the right, as in the C++.
    pub fn set_admin_rights(&mut self, root: &AccountsRoot, name: &[u16], admin: bool) -> bool {
        let Some(id) = self.find_by_name(name) else {
            return false;
        };
        let path = root.account_dir(name).join("admin");
        if admin {
            let _ = fs::write(&path, b"");
        } else {
            let _ = fs::remove_file(&path);
        }
        if let Some(account) = self.get_mut(id) {
            account.admin = admin;
        }
        true
    }

    /// `PlayerDB::BanAccount`: for a registered account, the `banned` file
    /// is made (or removed) in its folder and the flag set; `false` for an
    /// unknown account. A logged-in player stays (the next login is
    /// refused).
    pub fn ban_account(&mut self, root: &AccountsRoot, name: &[u16], ban: bool) -> bool {
        let Some(id) = self.find_by_name(name) else {
            return false;
        };
        let path = root.account_dir(name).join("banned");
        if ban {
            let _ = fs::write(&path, b"");
        } else {
            let _ = fs::remove_file(&path);
        }
        if let Some(account) = self.get_mut(id) {
            account.not_banned = !ban;
        }
        true
    }

    /// `PlayerDB::SetAccountPassword`: the `password` file of an existing
    /// account folder ([`crate::account::encode_password_file`]); `false`
    /// when the folder does not exist. A file that cannot be written still
    /// answers `true`, as in the C++.
    pub fn set_account_password(root: &AccountsRoot, name: &[u16], password: &[u16]) -> bool {
        let dir = root.account_dir(name);
        if !dir.is_dir() {
            return false;
        }
        let _ = fs::write(
            dir.join("password"),
            crate::account::encode_password_file(password),
        );
        true
    }

    /// `LockAccountAccess`: the account is locked; returns the session to
    /// disconnect, if one is logged in.
    pub fn lock(&mut self, name: &[u16]) -> Option<u32> {
        let id = self.find_by_name(name)?;
        let account = self.get_mut(id)?;
        account.locked = true;
        (account.client_id != 0).then_some(account.client_id)
    }

    /// The accounts in the name map's order (`m_AccountNameMap`'s walk).
    pub fn iter(&self) -> impl Iterator<Item = (AccountId, &Account)> + '_ {
        self.by_name
            .values()
            .filter_map(|&id| self.get(id).map(|account| (id, account)))
    }

    /// `UnlockAccountAccess`: logins are taken again.
    pub fn unlock(&mut self, name: &[u16]) {
        if let Some(account) = self.find_by_name(name).and_then(|id| self.get_mut(id)) {
            account.locked = false;
        }
    }

    /// `PlayerDB::CreateAccount`: `false` when the folder exists already or
    /// cannot be made; otherwise the folder gets the `name` file and the
    /// account is registered. A `name` file that cannot be written deletes
    /// the account again.
    pub fn create_account(&mut self, root: &AccountsRoot, name: &[u16]) -> bool {
        let dir = root.account_dir(name);
        if dir.exists() {
            return false;
        }
        let _ = fs::create_dir_all(&dir);
        if !dir.exists() {
            return false;
        }
        if fs::write(dir.join("name"), encode_name_file(name)).is_err() {
            self.delete_account(root, name);
            return false;
        }
        let folder = AccountFolder::read(&dir);
        self.insert(Account::from_folder(&folder));
        true
    }

    /// `PlayerDB::DeleteAccount`: every file of the account's folder and the
    /// folder are removed; a registered account loses its character ids and
    /// its entry. Returns the session to disconnect, if one is logged in.
    pub fn delete_account(&mut self, root: &AccountsRoot, name: &[u16]) -> Option<u32> {
        let id = self.find_by_name(name);
        let client = id.and_then(|id| self.get(id)).map_or(0, |a| a.client_id);
        let dir = root.account_dir(name);
        if let Ok(entries) = fs::read_dir(&dir) {
            for entry in entries.filter_map(Result::ok) {
                let file = entry.file_name();
                let file = file.to_string_lossy();
                if !file.starts_with('.') && entry.path().is_file() {
                    let _ = fs::remove_file(entry.path());
                }
            }
        }
        let _ = fs::remove_dir(&dir);
        if let Some(id) = id {
            self.by_character.retain(|_, account| *account != id);
            self.by_name.remove(&CiWName(name.to_vec()));
            if let Some(slot) = self.accounts.get_mut(id.0) {
                *slot = None;
            }
        }
        (client != 0).then_some(client)
    }

    /// `PlayerDB::DeleteCharacterFromID`: the id leaves the map, and its
    /// account drops the first character whose id it is, deleting that
    /// character's file (`CAccount::DeleteCharacterFromID`).
    pub fn delete_character_from_id(&mut self, root: &AccountsRoot, file_name: &[u8]) {
        let key = CiName(file_name.to_vec());
        let Some(&id) = self.by_character.get(&key) else {
            return;
        };
        if let Some(account) = self.accounts.get_mut(id.0).and_then(Option::as_mut) {
            let found = account
                .characters
                .iter()
                .position(|c| CiName(character_id(c)) == key);
            if let Some(index) = found {
                let path = root
                    .account_dir(&account.name)
                    .join(format!("{}.fl", hashed_name(&account.characters[index])));
                let _ = fs::remove_file(path);
                account.characters.remove(index);
            }
        }
        self.by_character.remove(&key);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn w(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    #[test]
    fn names_compare_without_case_and_shorter_first() {
        assert_eq!(CiWName(w("Alice")), CiWName(w("aLICE")));
        assert!(CiWName(w("ali")) < CiWName(w("Alice")));
        assert!(
            CiWName(w("[")) < CiWName(w("A")),
            "'[' is below 'a' once folded to lower case, above 'A' unfolded"
        );
        assert!(CiName(b"0C-AB.FL".to_vec()) == CiName(b"0c-ab.fl".to_vec()));
    }

    #[test]
    fn accounts_are_created_found_and_deleted() {
        let dir = std::env::temp_dir().join(format!("outpost-registry-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let root = AccountsRoot::new(&dir, true);
        let mut registry = AccountRegistry::new();
        let name = w("Pilot");
        assert!(registry.create_account(&root, &name));
        assert!(!registry.create_account(&root, &name), "the folder exists");
        let id = registry.find_by_name(&w("PILOT")).expect("found");
        assert!(registry.get(id).unwrap().not_banned);
        registry.get_mut(id).unwrap().characters.push(w("Hero"));
        registry.insert_character_id(&character_id(&w("Hero")), id);
        assert_eq!(registry.find_by_character_name(&w("hero")), Some(id));
        assert!(registry.set_admin_rights(&root, &name, true));
        assert!(root.account_dir(&name).join("admin").is_file());
        assert!(registry.admin_rights(&name));
        registry.get_mut(id).unwrap().client_id = 3;
        assert_eq!(registry.lock(&name), Some(3));
        registry.delete_character_from_id(&root, &character_id(&w("Hero")));
        assert!(registry.get(id).unwrap().characters.is_empty());
        assert_eq!(registry.find_by_character_name(&w("Hero")), None);
        assert_eq!(registry.delete_account(&root, &name), Some(3));
        assert_eq!(registry.find_by_name(&name), None);
        assert!(!root.account_dir(&name).exists());
        let _ = fs::remove_dir_all(&dir);
    }
}
