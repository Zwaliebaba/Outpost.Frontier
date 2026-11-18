//! Reads a registry subtree of the current user into a
//! [`RegKey`](crate::user_state::RegKey), for the one-time import of the
//! C++ client's values (P4.1). Read-only: the import never writes the
//! registry.

use windows::Win32::Foundation::ERROR_SUCCESS;
use windows::Win32::System::Registry::{
    HKEY, HKEY_CURRENT_USER, KEY_READ, REG_BINARY, REG_DWORD, REG_EXPAND_SZ, REG_SZ, RegCloseKey,
    RegEnumKeyExW, RegEnumValueW, RegOpenKeyExW, RegQueryInfoKeyW,
};
use windows::core::{HSTRING, PWSTR};

use crate::user_state::{MAX_KEY_DEPTH, RegKey, RegValue};

/// An open key, closed on drop.
struct Key(HKEY);

impl Drop for Key {
    fn drop(&mut self) {
        // SAFETY: the key this value opened, closed once.
        let _ = unsafe { RegCloseKey(self.0) };
    }
}

fn open(parent: HKEY, name: &str) -> Option<Key> {
    let mut key = HKEY::default();
    let name = HSTRING::from(name);
    // SAFETY: a valid parent key and name; the result is closed by `Key`.
    let status = unsafe { RegOpenKeyExW(parent, &name, None, KEY_READ, &mut key) };
    (status == ERROR_SUCCESS).then_some(Key(key))
}

fn utf16_to_string(units: &[u16]) -> String {
    let end = units.iter().position(|&u| u == 0).unwrap_or(units.len());
    String::from_utf16_lossy(&units[..end])
}

fn value(kind: u32, bytes: Vec<u8>) -> RegValue {
    match kind {
        k if k == REG_DWORD.0 && bytes.len() >= 4 => {
            RegValue::Dword(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
        }
        k if k == REG_SZ.0 || k == REG_EXPAND_SZ.0 => {
            let units: Vec<u16> = bytes
                .as_chunks::<2>()
                .0
                .iter()
                .map(|p| u16::from_le_bytes(*p))
                .collect();
            RegValue::Sz(utf16_to_string(&units))
        }
        k if k == REG_BINARY.0 => RegValue::Binary(bytes),
        kind => RegValue::Other { kind, bytes },
    }
}

fn read(key: &Key, depth: usize) -> RegKey {
    let mut out = RegKey::default();
    let (mut subkeys, mut max_subkey, mut values, mut max_name, mut max_data) = (0, 0, 0, 0, 0);
    // SAFETY: an open key and valid out pointers for the counts.
    let status = unsafe {
        RegQueryInfoKeyW(
            key.0,
            None,
            None,
            None,
            Some(&mut subkeys),
            Some(&mut max_subkey),
            None,
            Some(&mut values),
            Some(&mut max_name),
            Some(&mut max_data),
            None,
            None,
        )
    };
    if status != ERROR_SUCCESS {
        return out;
    }
    for index in 0..values {
        let mut name = vec![0u16; max_name as usize + 1];
        let mut name_len = name.len() as u32;
        let mut data = vec![0u8; max_data as usize];
        let mut data_len = data.len() as u32;
        let mut kind = 0u32;
        // SAFETY: buffers of the sizes the key reported, with their lengths.
        let status = unsafe {
            RegEnumValueW(
                key.0,
                index,
                Some(PWSTR(name.as_mut_ptr())),
                &mut name_len,
                None,
                Some(&mut kind),
                Some(data.as_mut_ptr()),
                Some(&mut data_len),
            )
        };
        if status == ERROR_SUCCESS {
            data.truncate(data_len as usize);
            out.values.insert(
                utf16_to_string(&name[..name_len as usize]),
                value(kind, data),
            );
        }
    }
    if depth + 1 >= MAX_KEY_DEPTH {
        return out;
    }
    for index in 0..subkeys {
        let mut name = vec![0u16; max_subkey as usize + 1];
        let mut name_len = name.len() as u32;
        // SAFETY: a buffer of the size the key reported, with its length.
        let status = unsafe {
            RegEnumKeyExW(
                key.0,
                index,
                Some(PWSTR(name.as_mut_ptr())),
                &mut name_len,
                None,
                None,
                None,
                None,
            )
        };
        if status != ERROR_SUCCESS {
            continue;
        }
        let child_name = utf16_to_string(&name[..name_len as usize]);
        if let Some(child) = open(key.0, &child_name) {
            out.keys.insert(child_name, read(&child, depth + 1));
        }
    }
    out
}

/// The subtree of `HKEY_CURRENT_USER\path`, or `None` when the key does not
/// exist.
#[must_use]
pub fn read_current_user(path: &str) -> Option<RegKey> {
    let key = open(HKEY_CURRENT_USER, path)?;
    Some(read(&key, 0))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_a_subtree_of_the_current_user() {
        // A key every Windows user has.
        let Some(tree) = read_current_user("Control Panel\\Desktop") else {
            println!("no such key here; nothing to test");
            return;
        };
        assert!(!tree.values.is_empty());
        assert!(read_current_user("Software\\OutpostFrontierDoesNotExist\\x").is_none());
    }

    #[test]
    fn values_convert_by_type() {
        assert_eq!(value(REG_DWORD.0, vec![1, 0, 0, 0]), RegValue::Dword(1));
        assert_eq!(
            value(REG_SZ.0, vec![b'h', 0, b'i', 0, 0, 0]),
            RegValue::Sz("hi".into())
        );
        assert_eq!(value(REG_BINARY.0, vec![9]), RegValue::Binary(vec![9]));
    }
}
