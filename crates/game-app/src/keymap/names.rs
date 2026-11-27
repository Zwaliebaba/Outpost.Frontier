//! A binding's display name (`KeyMapFormatBindingName`, `menu_keymap.cpp`):
//! the modifiers' names and the key's, as the preferences screen and the
//! tooltips show them.
//!
//! The key names are the keyboard layout's (`MapVirtualKeyA` and
//! `GetKeyNameTextW`, Windows calls): the caller gives them as a
//! [`KeyNames`] table, which the client fills from the platform layer. A
//! key the table does not name falls back to the reference's text ids, as
//! a failed `GetKeyNameTextW` does.

use std::collections::BTreeMap;

use neuron_core::catalog::TextCatalogs;

use super::{MOD_ALT, MOD_CONTROL, MOD_SHIFT};

/// `Dest`'s size in the reference's callers (`wchar_t Name[0x100]`).
pub const NAME_CHARS: usize = 0x100;

/// `IDS` of an unnamed key.
const IDS_UNKNOWN_KEY: u32 = 0x3ae;
/// `IDS` of the Shift, Ctrl and Alt fallbacks.
const IDS_MODIFIERS: [u32; 3] = [0x4eb, 0x4ec, 0x4ed];

/// The names the keyboard layout gives the virtual keys.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct KeyNames {
    /// By virtual key below 0x101: `GetKeyNameTextW` of the key's scan
    /// code (`MapVirtualKeyA(vk, MAPVK_VK_TO_VSC) << 16`), for the ten
    /// extended keys (the arrows, Home, End, Insert, Delete, Page Up and
    /// Page Down) with the extended bit, for the others first without it
    /// and then with it. A key without a scan code or a name is absent.
    pub keys: BTreeMap<u32, Vec<u16>>,
    /// Shift (0x10), Ctrl (0x11) and Alt (0x12) as the modifier prefix asks
    /// for them: without the extended bit, `None` when that has no name.
    pub modifiers: [Option<Vec<u16>>; 3],
}

/// `GetIdsStringWWithRawFallback(ids, buffer, room)`: at most `room - 1`
/// units.
fn ids_text(catalogs: Option<&TextCatalogs>, ids: u32, room: usize) -> Vec<u16> {
    let Some(c) = catalogs else {
        return Vec::new();
    };
    let mut buffer = vec![0u16; room];
    let n = c.string_with_card_fallback_into(ids, &mut buffer);
    buffer.truncate(n);
    buffer
}

/// The text id of a mouse button or wheel "key" (negative virtual keys).
fn special_key_ids(virtual_key: i32) -> u32 {
    match virtual_key {
        -9 => 0x4ea,
        -8 => 0x3ad,
        -7 => 0x3ac,
        -6 => 0x3ab,
        -5 => 0x3aa,
        -4 => 0x3a9,
        -3 => 0x3a8,
        -2 => 0x3a7,
        -1 => 0x3a6,
        _ => IDS_UNKNOWN_KEY,
    }
}

/// `KeyMapFormatBindingName(binding, dest, dest_chars)` with `dest_chars`
/// of [`NAME_CHARS`]: the key's name, after "Shift + ", "Ctrl + " and
/// "Alt + " for its modifiers (a modifier without a name gives its text id's
/// text, with no " + " after it); a name too long for the buffer is the
/// unknown key's text.
#[must_use]
pub fn format_binding_name(
    virtual_key: i32,
    modifiers: u32,
    names: &KeyNames,
    catalogs: Option<&TextCatalogs>,
) -> Vec<u16> {
    let vk = virtual_key.cast_unsigned();
    let key: Vec<u16> = if vk < 0x101 {
        match names.keys.get(&vk).filter(|n| !n.is_empty()) {
            Some(name) => name.iter().copied().take(255).collect(),
            None => ids_text(catalogs, IDS_UNKNOWN_KEY, 256),
        }
    } else {
        ids_text(catalogs, special_key_ids(virtual_key), 256)
    };

    if modifiers == 0 {
        let mut out = key;
        out.truncate(NAME_CHARS - 1);
        return out;
    }

    let mut prefix: Vec<u16> = Vec::new();
    for (index, bit) in [MOD_SHIFT, MOD_CONTROL, MOD_ALT].into_iter().enumerate() {
        if modifiers & bit == 0 {
            continue;
        }
        let room = 256usize.saturating_sub(prefix.len());
        match names.modifiers[index].as_ref().filter(|n| !n.is_empty()) {
            Some(name) => {
                prefix.extend(name.iter().copied().take(room.saturating_sub(1)));
                prefix.extend(" + ".encode_utf16());
            }
            None => prefix.extend(ids_text(catalogs, IDS_MODIFIERS[index], room)),
        }
    }

    if prefix.len() + key.len() < NAME_CHARS {
        prefix.extend(key);
        prefix
    } else {
        ids_text(catalogs, IDS_UNKNOWN_KEY, NAME_CHARS)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn w(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    #[test]
    fn modifiers_come_before_the_key() {
        let mut names = KeyNames::default();
        names.keys.insert(0x57, w("W"));
        names.modifiers[0] = Some(w("Shift"));
        names.modifiers[2] = Some(w("Alt"));
        assert_eq!(format_binding_name(0x57, 0, &names, None), w("W"));
        assert_eq!(
            format_binding_name(0x57, MOD_SHIFT | MOD_ALT, &names, None),
            w("Shift + Alt + W")
        );
        // Without a catalog the fallbacks are empty.
        assert_eq!(format_binding_name(0x58, 0, &names, None), w(""));
        assert_eq!(format_binding_name(0x57, MOD_CONTROL, &names, None), w("W"));
    }
}
