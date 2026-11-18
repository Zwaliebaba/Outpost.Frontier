//! The IME's state as the edit boxes and the window see it: the
//! composition copy, the caret the IME windows follow, the interface the
//! boxes call (`ImeUi_SetState`, `ImeUi_FinalizeString`,
//! `ImeUi_SetCaretPosition`), and the platform-free parts of
//! `ImeUi_ProcessMessage`, `ImeUi_IgnoreHotKey` and `ImeUi_GetState`.

use crate::input::keyboard::{WM_KEYUP, WM_SYSKEYUP, vk};

/// A composition character's attribute (`GCS_COMPATTR`, the `ATTR_*`
/// values).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum CompositionAttribute {
    /// `ATTR_INPUT`: typed, not converted.
    Input,
    /// `ATTR_TARGET_CONVERTED`: converted, and the conversion target.
    TargetConverted,
    /// `ATTR_CONVERTED`.
    Converted,
    /// `ATTR_TARGET_NOTCONVERTED`: the target, not converted yet.
    TargetNotConverted,
    /// `ATTR_INPUT_ERROR`.
    InputError,
    /// `ATTR_FIXEDCONVERTED`.
    FixedConverted,
    /// Any other byte.
    Other(u8),
}

impl From<u8> for CompositionAttribute {
    fn from(value: u8) -> Self {
        match value {
            0 => CompositionAttribute::Input,
            1 => CompositionAttribute::TargetConverted,
            2 => CompositionAttribute::Converted,
            3 => CompositionAttribute::TargetNotConverted,
            4 => CompositionAttribute::InputError,
            5 => CompositionAttribute::FixedConverted,
            other => CompositionAttribute::Other(other),
        }
    }
}

/// The composition in progress (`GComposition`, `GCompositionAttributes`,
/// `GClientCaret`): UTF-16 text, one attribute per unit, and the IME's
/// cursor within it (`GCS_CURSORPOS`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Composition {
    pub text: Vec<u16>,
    pub attributes: Vec<CompositionAttribute>,
    pub cursor: usize,
}

impl Composition {
    /// A composition from the strings the IME returned: the text, the
    /// attribute bytes (cut or padded with `Input` to the text's length)
    /// and the cursor (kept within the text).
    #[must_use]
    pub fn new(text: Vec<u16>, attribute_bytes: &[u8], cursor: i32) -> Composition {
        let mut attributes: Vec<CompositionAttribute> = attribute_bytes
            .iter()
            .take(text.len())
            .map(|&b| CompositionAttribute::from(b))
            .collect();
        attributes.resize(text.len(), CompositionAttribute::Input);
        let cursor = usize::try_from(cursor).unwrap_or(0).min(text.len());
        Composition {
            text,
            attributes,
            cursor,
        }
    }

    /// `ImeUi_IsCompositionEmpty`.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.text.is_empty()
    }

    /// `ResetComposition`.
    pub fn clear(&mut self) {
        self.text.clear();
        self.attributes.clear();
        self.cursor = 0;
    }
}

/// Where the IME windows go: the focused box's caret in surface pixels
/// (`ImeUi_SetCaretPosition`), with the line height below which the
/// candidate list opens.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CaretRect {
    pub x: i32,
    pub y: i32,
    pub height: i32,
}

/// What the edit boxes ask of the IME.
pub trait ImeContext {
    /// `ImeUi_SetState`: give the window its input context (`true`) or
    /// none, so the IME is on only while a box has the focus.
    fn set_open(&mut self, open: bool);
    /// `ImeUi_FinalizeString`: close the candidate list and cancel the
    /// composition; with `send`, return the composition's text, which the
    /// focus manager hands to the box that is losing the focus. Nothing
    /// happens while the window has no input context.
    fn finalize(&mut self, send: bool) -> Vec<u16>;
    /// `ImeUi_SetCaretPosition`: the caret the IME windows follow.
    fn set_caret(&mut self, caret: CaretRect);
    /// Whether any box is registered (`IMEFocus::m_NumBoxes != 0`): the
    /// window procedure and the hot-key test act only then.
    fn set_boxes_registered(&mut self, registered: bool);
}

/// An IME context that does nothing: hosts without an IME, and tests.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct NoIme;

impl ImeContext for NoIme {
    fn set_open(&mut self, _open: bool) {}
    fn finalize(&mut self, _send: bool) -> Vec<u16> {
        Vec::new()
    }
    fn set_caret(&mut self, _caret: CaretRect) {}
    fn set_boxes_registered(&mut self, _registered: bool) {}
}

/// The IME hot keys `ImeUi_IgnoreHotKey` swallows when an IME took the key
/// (`VK_PROCESSKEY`, the real key from `ImmGetVirtualKey`): (Ctrl, Shift,
/// key).
pub const HOT_KEYS: [(bool, bool, u32); 20] = [
    (false, false, vk::APPS),
    (true, false, b'8' as u32),
    (true, false, b'Y' as u32),
    (true, false, vk::DELETE),
    (true, false, vk::F7),
    (true, false, vk::F9),
    (true, false, vk::F10),
    (true, false, vk::F11),
    (true, false, vk::F12),
    (false, false, vk::F2),
    (false, false, vk::F3),
    (false, false, vk::F4),
    (false, false, vk::F5),
    (false, false, vk::F10),
    (false, true, vk::F6),
    (false, true, vk::F7),
    (false, true, vk::F8),
    (true, true, vk::F10),
    (true, false, vk::SPACE),
    (true, false, vk::BACK),
];

/// Whether the key an IME took is one of its hot keys with this Ctrl and
/// Shift state. A message for it is neither routed, translated nor
/// dispatched.
#[must_use]
pub fn is_hot_key(key: u32, control: bool, shift: bool) -> bool {
    HOT_KEYS
        .iter()
        .any(|&(c, s, k)| c == control && s == shift && k == key)
}

/// `LANG_CHINESE`, the primary language of a Chinese keyboard layout.
pub const LANG_CHINESE: u16 = 0x04;
/// `IME_CMODE_NATIVE`.
pub const IME_CMODE_NATIVE: u32 = 0x01;

/// `ImeUi_GetState`: 0 when the window has no input context or the layout
/// is not an IME; for a Chinese IME 1 in native conversion mode and 2
/// otherwise; for others 1 while the IME is open.
#[must_use]
pub fn ime_state(
    has_context: bool,
    is_ime: bool,
    language: u16,
    open: bool,
    conversion: u32,
) -> u32 {
    if !has_context || !is_ime {
        return 0;
    }
    if language & 0x3ff == LANG_CHINESE {
        if conversion & IME_CMODE_NATIVE != 0 {
            1
        } else {
            2
        }
    } else {
        u32::from(open)
    }
}

/// The modifier release repair of `ImeUi_ProcessMessage`: an IME can eat
/// the release of Alt, Ctrl or Shift. On each key release, the first of
/// the three that was last seen pressed, is not the key being released and
/// is no longer down gets a synthetic `WM_KEYUP` posted to the focus
/// window. Each key message for one of the three records its `lParam`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ModifierRepair {
    /// The last `lParam` of Alt, Ctrl and Shift; bit 31 set means
    /// released.
    state: [u32; 3],
}

impl Default for ModifierRepair {
    fn default() -> Self {
        ModifierRepair {
            state: [0x8000_0000; 3],
        }
    }
}

impl ModifierRepair {
    const KEYS: [u32; 3] = [vk::MENU, vk::CONTROL, vk::SHIFT];

    /// One key message (`WM_KEYDOWN`, `WM_KEYUP`, `WM_SYSKEYDOWN`,
    /// `WM_SYSKEYUP`) with its virtual key; `down` says whether a key is
    /// down now (`GetAsyncKeyState`). Returns the key and `lParam` of the
    /// `WM_KEYUP` to post, if any.
    pub fn on_key_message(
        &mut self,
        message: u32,
        key: u32,
        lparam: u32,
        down: impl Fn(u32) -> bool,
    ) -> Option<(u32, u32)> {
        let mut post = None;
        if message == WM_KEYUP || message == WM_SYSKEYUP {
            for (state, modifier) in self.state.iter().zip(Self::KEYS) {
                if state & 0x8000_0000 == 0 && key != modifier && !down(modifier) {
                    post = Some((modifier, (state & 0x01ff_0000) | 0xc000_0001));
                    break;
                }
            }
        }
        if let Some(i) = Self::KEYS.iter().position(|&k| k == key) {
            self.state[i] = lparam;
        }
        post
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::input::keyboard::{WM_KEYDOWN, WM_SYSKEYDOWN};

    #[test]
    fn compositions_keep_one_attribute_per_unit() {
        let c = Composition::new(vec![0x304b, 0x306a], &[1, 0, 3], 9);
        assert_eq!(
            c.attributes,
            [
                CompositionAttribute::TargetConverted,
                CompositionAttribute::Input
            ]
        );
        assert_eq!(c.cursor, 2);
        let mut c = Composition::new(vec![0x41, 0x42, 0x43], &[2], -1);
        assert_eq!(c.attributes[0], CompositionAttribute::Converted);
        assert_eq!(c.attributes[2], CompositionAttribute::Input);
        assert_eq!(c.cursor, 0);
        assert_eq!(
            CompositionAttribute::from(9),
            CompositionAttribute::Other(9)
        );
        assert!(!c.is_empty());
        c.clear();
        assert!(c.is_empty());
    }

    #[test]
    fn hot_keys_match_ctrl_and_shift_exactly() {
        assert!(is_hot_key(vk::SPACE, true, false));
        assert!(!is_hot_key(vk::SPACE, false, false));
        assert!(!is_hot_key(vk::SPACE, true, true));
        assert!(is_hot_key(vk::F10, false, false));
        assert!(is_hot_key(vk::F10, true, true));
        assert!(!is_hot_key(vk::F10, false, true));
        assert!(is_hot_key(u32::from(b'Y'), true, false));
        assert!(is_hot_key(vk::APPS, false, false));
    }

    #[test]
    fn the_state_follows_the_language() {
        assert_eq!(ime_state(false, true, 0x0411, true, 0), 0);
        assert_eq!(ime_state(true, false, 0x0411, true, 0), 0);
        assert_eq!(ime_state(true, true, 0x0411, true, 0), 1);
        assert_eq!(ime_state(true, true, 0x0412, false, 0), 0);
        // Chinese reads the conversion mode, not the open status.
        assert_eq!(ime_state(true, true, 0x0804, false, 1), 1);
        assert_eq!(ime_state(true, true, 0x0404, true, 0), 2);
    }

    #[test]
    fn an_eaten_modifier_release_is_repaired() {
        let mut r = ModifierRepair::default();
        let up = |_k: u32| false;
        // Ctrl down, then 'A' released while Ctrl is up again: repair.
        assert_eq!(
            r.on_key_message(WM_KEYDOWN, vk::CONTROL, 0x001d_0001, up),
            None
        );
        assert_eq!(
            r.on_key_message(WM_KEYUP, 0x41, 0xc01e_0001, up),
            Some((vk::CONTROL, 0xc01d_0001))
        );
        // Still recorded as down until a message for Ctrl arrives.
        assert!(r.on_key_message(WM_KEYUP, 0x41, 0xc01e_0001, up).is_some());
        assert_eq!(
            r.on_key_message(WM_KEYUP, vk::CONTROL, 0xc01d_0001, up),
            None
        );
        assert_eq!(r.on_key_message(WM_KEYUP, 0x41, 0xc01e_0001, up), None);
        // A modifier that is really down is left alone; Alt comes first.
        r.on_key_message(WM_SYSKEYDOWN, vk::MENU, 0x2038_0001, up);
        r.on_key_message(WM_KEYDOWN, vk::SHIFT, 0x002a_0001, up);
        let alt_down = |k: u32| k == vk::MENU;
        assert_eq!(
            r.on_key_message(WM_KEYUP, 0x41, 0xc01e_0001, alt_down),
            Some((vk::SHIFT, 0xc02a_0001))
        );
        assert_eq!(
            r.on_key_message(WM_KEYUP, 0x41, 0xc01e_0001, up),
            Some((vk::MENU, 0xc038_0001))
        );
        // Key downs never post.
        assert_eq!(r.on_key_message(WM_KEYDOWN, 0x41, 0x001e_0001, up), None);
    }
}
