//! Key messages as the C++ client's message pump hands them to the key map
//! (`KeyMapRouteInputMessage` in `menu_keymap.cpp`) before
//! `TranslateMessage`: the record it fills from `WM_KEYDOWN`, `WM_KEYUP`,
//! `WM_SYSKEYDOWN`, `WM_SYSKEYUP` and `WM_CHAR`, with the modifier state
//! read when the message was taken from the queue.
//!
//! What the record is then used for (the handler tree, the held-key list
//! and the accelerator groups of `KeyMap.ini`) is the game's: GameApp's
//! router receives each [`KeyEvent`] and answers whether it consumed it,
//! and a consumed message is neither translated nor dispatched. The C++
//! router always consumes a key event for Alt itself (`VK_MENU`), so Alt
//! never reaches the system menu; the game's router keeps that rule.

/// `WM_KEYDOWN`.
pub const WM_KEYDOWN: u32 = 0x100;
/// `WM_KEYUP`.
pub const WM_KEYUP: u32 = 0x101;
/// `WM_CHAR`.
pub const WM_CHAR: u32 = 0x102;
/// `WM_SYSKEYDOWN`.
pub const WM_SYSKEYDOWN: u32 = 0x104;
/// `WM_SYSKEYUP`.
pub const WM_SYSKEYUP: u32 = 0x105;

/// Virtual-key codes the input code names.
pub mod vk {
    pub const BACK: u32 = 0x08;
    pub const TAB: u32 = 0x09;
    pub const RETURN: u32 = 0x0d;
    pub const SHIFT: u32 = 0x10;
    pub const CONTROL: u32 = 0x11;
    pub const MENU: u32 = 0x12;
    pub const ESCAPE: u32 = 0x1b;
    pub const SPACE: u32 = 0x20;
    pub const END: u32 = 0x23;
    pub const HOME: u32 = 0x24;
    pub const LEFT: u32 = 0x25;
    pub const RIGHT: u32 = 0x27;
    pub const INSERT: u32 = 0x2d;
    pub const DELETE: u32 = 0x2e;
    pub const APPS: u32 = 0x5d;
    pub const F2: u32 = 0x71;
    pub const F3: u32 = 0x72;
    pub const F4: u32 = 0x73;
    pub const F5: u32 = 0x74;
    pub const F6: u32 = 0x75;
    pub const F7: u32 = 0x76;
    pub const F8: u32 = 0x77;
    pub const F9: u32 = 0x78;
    pub const F10: u32 = 0x79;
    pub const F11: u32 = 0x7a;
    pub const F12: u32 = 0x7b;
    /// The key an IME took (`VK_PROCESSKEY`).
    pub const PROCESSKEY: u32 = 0xe5;
}

/// The modifier bits of the record (`EKeyModifier`): Shift 1, Ctrl 4,
/// Alt 0x10.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct Modifiers(pub u32);

impl Modifiers {
    pub const NONE: Modifiers = Modifiers(0);
    pub const SHIFT: Modifiers = Modifiers(1);
    pub const CONTROL: Modifiers = Modifiers(4);
    pub const ALT: Modifiers = Modifiers(0x10);

    /// The bits for the three keys' "down" states (`GetKeyState` high bit).
    #[must_use]
    pub fn from_keys(shift: bool, control: bool, alt: bool) -> Modifiers {
        let mut bits = 0;
        if shift {
            bits = Modifiers::SHIFT.0;
        }
        if control {
            bits |= Modifiers::CONTROL.0;
        }
        if alt {
            bits |= Modifiers::ALT.0;
        }
        Modifiers(bits)
    }

    #[must_use]
    pub fn contains(self, other: Modifiers) -> bool {
        self.0 & other.0 == other.0
    }
}

/// The record's event kind: 1 for a key, 2 for a character.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum KeyEventKind {
    Key = 1,
    Char = 2,
}

/// One routed key message: the fields `KeyMapRouteInputMessage` writes into
/// the HUD command record.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct KeyEvent {
    pub kind: KeyEventKind,
    /// The virtual key (`wParam`) for a key, 0 for a character.
    pub key: u32,
    /// Key down (record byte 0x18); always set for a character.
    pub down: bool,
    /// Auto-repeat (record byte 0x19): see [`is_repeat`].
    pub repeat: bool,
    pub modifiers: Modifiers,
    pub message: u32,
    pub wparam: u32,
    pub lparam: u32,
}

/// The C++'s auto-repeat test: `(lParam & 0xf0000000) == 0x40000000`, the
/// previous-state bit set and the transition, context and reserved bits
/// clear. An Alt-held repeat (context bit set) does not count.
#[must_use]
pub fn is_repeat(lparam: u32) -> bool {
    lparam & 0xf000_0000 == 0x4000_0000
}

/// Whether the router sees this message. `WM_DEADCHAR`, `WM_SYSCHAR` and
/// everything else go straight to translation.
#[must_use]
pub fn is_routed(message: u32) -> bool {
    matches!(
        message,
        WM_KEYDOWN | WM_KEYUP | WM_SYSKEYDOWN | WM_SYSKEYUP | WM_CHAR
    )
}

impl KeyEvent {
    /// The record for one pumped message, or `None` when the router does
    /// not see it.
    #[must_use]
    pub fn from_message(
        message: u32,
        wparam: u32,
        lparam: u32,
        modifiers: Modifiers,
    ) -> Option<KeyEvent> {
        let (kind, key, down, repeat) = match message {
            WM_KEYDOWN | WM_SYSKEYDOWN => (KeyEventKind::Key, wparam, true, is_repeat(lparam)),
            WM_KEYUP | WM_SYSKEYUP => (KeyEventKind::Key, wparam, false, is_repeat(lparam)),
            WM_CHAR => (KeyEventKind::Char, 0, true, false),
            _ => return None,
        };
        Some(KeyEvent {
            kind,
            key,
            down,
            repeat,
            modifiers,
            message,
            wparam,
            lparam,
        })
    }

    /// The UTF-16 unit of a character message (the record's `wParam`
    /// word).
    #[must_use]
    pub fn character(&self) -> Option<u16> {
        (self.kind == KeyEventKind::Char).then_some(self.wparam as u16)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn key_messages_fill_the_record() {
        let shift_ctrl = Modifiers::from_keys(true, true, false);
        assert_eq!(shift_ctrl, Modifiers(5));
        let e = KeyEvent::from_message(WM_KEYDOWN, 0x41, 0x001e_0001, shift_ctrl).unwrap();
        assert_eq!(e.kind, KeyEventKind::Key);
        assert_eq!((e.key, e.down, e.repeat), (0x41, true, false));
        assert_eq!(
            (e.message, e.wparam, e.lparam),
            (WM_KEYDOWN, 0x41, 0x001e_0001)
        );
        assert!(e.modifiers.contains(Modifiers::CONTROL));
        // A held key repeats with the previous-state bit.
        let e = KeyEvent::from_message(WM_KEYDOWN, 0x41, 0x401e_0001, Modifiers::NONE).unwrap();
        assert!(e.repeat);
        // With Alt held (context bit) it is not a repeat for the router.
        let e = KeyEvent::from_message(WM_SYSKEYDOWN, 0x41, 0x601e_0001, Modifiers::ALT).unwrap();
        assert!(e.down && !e.repeat);
        // Releases carry the transition bit: never a repeat.
        let e = KeyEvent::from_message(WM_KEYUP, 0x41, 0xc01e_0001, Modifiers::NONE).unwrap();
        assert!(!e.down && !e.repeat);
        let e =
            KeyEvent::from_message(WM_SYSKEYUP, vk::MENU, 0xc038_0001, Modifiers::NONE).unwrap();
        assert_eq!(
            (e.kind, e.key, e.down),
            (KeyEventKind::Key, vk::MENU, false)
        );
    }

    #[test]
    fn characters_have_no_key() {
        let e = KeyEvent::from_message(WM_CHAR, 0x00e9, 0x401e_0001, Modifiers::NONE).unwrap();
        assert_eq!(
            (e.kind, e.key, e.down, e.repeat),
            (KeyEventKind::Char, 0, true, false)
        );
        assert_eq!(e.character(), Some(0x00e9));
        assert_eq!(
            KeyEvent::from_message(WM_KEYDOWN, 1, 0, Modifiers::NONE)
                .unwrap()
                .character(),
            None
        );
    }

    #[test]
    fn other_messages_are_not_routed() {
        for message in [0x103, 0x106, 0x107, 0x200, 0x0f] {
            assert!(!is_routed(message));
            assert_eq!(KeyEvent::from_message(message, 0, 0, Modifiers::NONE), None);
        }
        for message in [WM_KEYDOWN, WM_KEYUP, WM_CHAR, WM_SYSKEYDOWN, WM_SYSKEYUP] {
            assert!(is_routed(message));
        }
    }
}
