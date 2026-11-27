//! What the data tests share.

use game_app::keymap::names::KeyNames;

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().collect()
}

/// The names a US keyboard layout gives the keys (`GetKeyNameTextW`), as
/// the C++ client-oracle runs' client reads them.
#[must_use]
pub fn us_key_names() -> KeyNames {
    let mut names = KeyNames::default();
    for c in b'A'..=b'Z' {
        names.keys.insert(u32::from(c), vec![u16::from(c)]);
    }
    for c in b'0'..=b'9' {
        names.keys.insert(u32::from(c), vec![u16::from(c)]);
    }
    for (vk, name) in [
        (0x08, "Backspace"),
        (0x09, "Tab"),
        (0x0d, "Enter"),
        (0x10, "Shift"),
        (0x11, "Ctrl"),
        (0x12, "Alt"),
        (0x14, "Caps Lock"),
        (0x1b, "Esc"),
        (0x20, "Space"),
        (0x21, "Page Up"),
        (0x22, "Page Down"),
        (0x23, "End"),
        (0x24, "Home"),
        (0x25, "Left"),
        (0x26, "Up"),
        (0x27, "Right"),
        (0x28, "Down"),
        (0x2d, "Insert"),
        (0x2e, "Delete"),
    ] {
        names.keys.insert(vk, wide(name));
    }
    for (i, n) in (0x70..=0x7b).enumerate() {
        names.keys.insert(n, wide(&format!("F{}", i + 1)));
    }
    names.modifiers = [Some(wide("Shift")), Some(wide("Ctrl")), Some(wide("Alt"))];
    names
}
