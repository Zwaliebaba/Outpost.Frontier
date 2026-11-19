//! The typed messages the toolkit passes: what the Win3D input handler of
//! `win3d.cpp` reads from the key-map event record and hands to the
//! objects' virtual slots.
//!
//! The reference registers one handler for twelve key-map events: 1 (a
//! key), 2 (a character), 4 (the wheel), 5 (the mouse moved) and 6 to 13
//! (mouse buttons 0 to 7). Each becomes one of [`Input`]'s variants here;
//! [`Input::from_mouse_event`] and [`Input::from_key_event`] convert the
//! events of [`crate::input`].

use crate::input::keyboard::{KeyEvent, KeyEventKind};
use crate::input::mouse::{MouseEvent, WHEEL_BUTTON};

/// The broadcast message that shows a control (`WinControl` slot 23).
pub const MSG_SHOW: u32 = 0xEF01;
/// The broadcast message that hides a control.
pub const MSG_HIDE: u32 = 0xEF02;

/// What a mouse button did.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ButtonAction {
    Up,
    Down,
    /// A press within the double-click time and slop of the last one.
    DoubleClick,
}

/// `FWin3DMouseEvent`: a mouse button event of slot 19.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct MouseButton {
    /// 0 to 7.
    pub index: u32,
    pub action: ButtonAction,
}

impl MouseButton {
    /// The left button going down (`Field0 == -1 && Field1 == 2`, the click
    /// `UITextMsgButton` reacts to).
    pub const LEFT_DOWN: MouseButton = MouseButton {
        index: 0,
        action: ButtonAction::Down,
    };

    /// `Field0`: `-1 - index`.
    #[must_use]
    pub fn field0(self) -> i32 {
        -1 - self.index.cast_signed()
    }

    /// `Field1`: 1 up, 2 down, 3 double click.
    #[must_use]
    pub fn field1(self) -> i32 {
        match self.action {
            ButtonAction::Up => 1,
            ButtonAction::Down => 2,
            ButtonAction::DoubleClick => 3,
        }
    }

    /// The left button (`Field0 == -1`).
    #[must_use]
    pub fn is_left(self) -> bool {
        self.index == 0
    }
}

/// The modifier byte `Win3DDispatchKey` builds for slot 20: bit 0 key up,
/// bit 1 key down, bit 2 Shift, bit 3 Ctrl, bit 4 Alt. A handler may change
/// it (it is passed by pointer); later handlers see the change.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct KeyMods(pub u8);

impl KeyMods {
    pub const UP: u8 = 0x01;
    pub const DOWN: u8 = 0x02;
    pub const SHIFT: u8 = 0x04;
    pub const CONTROL: u8 = 0x08;
    pub const ALT: u8 = 0x10;

    /// The byte for a key record: the down flag, and the record's modifier
    /// bits (Shift 3, Ctrl 0xC, Alt 0x30). The reference builds the byte in
    /// the argument's own stack slot, so bits 5 to 7 are the low byte of the
    /// record's address; they are 0 here (divergence D342).
    #[must_use]
    pub fn from_record(down: bool, modifiers: u32) -> KeyMods {
        let mut bits = if down { Self::DOWN } else { Self::UP };
        if modifiers & 0x03 != 0 {
            bits |= Self::SHIFT;
        }
        if modifiers & 0x0C != 0 {
            bits |= Self::CONTROL;
        }
        if modifiers & 0x30 != 0 {
            bits |= Self::ALT;
        }
        KeyMods(bits)
    }

    #[must_use]
    pub fn down(self) -> bool {
        self.0 & Self::DOWN != 0
    }
}

/// One event of the Win3D input handler.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Input {
    /// Event 5: the software cursor moved; the position in surface pixels.
    Move { x: i32, y: i32 },
    /// Events 6 to 13.
    Button(MouseButton),
    /// Event 4: the wheel delta of the frame (`GetClientMouseWheel`), the
    /// same for every notch event of the frame.
    Wheel(i32),
    /// Event 1: a key with its virtual-key code.
    Key { key: u32, mods: KeyMods },
    /// Event 2: a character, the message's `wParam` and `lParam`.
    Char { wparam: u32, lparam: u32 },
}

impl Input {
    /// The event a queued mouse event posts: a move at the cursor, a
    /// button 0 to 7, or a wheel notch with the frame's wheel delta. `None`
    /// for a button index past 7 that is not the wheel.
    #[must_use]
    pub fn from_mouse_event(event: MouseEvent, cursor: (i32, i32), wheel: i32) -> Option<Input> {
        match event {
            MouseEvent::Moved => Some(Input::Move {
                x: cursor.0,
                y: cursor.1,
            }),
            MouseEvent::Button { index, .. } if index == WHEEL_BUTTON => Some(Input::Wheel(wheel)),
            MouseEvent::Button {
                index,
                down,
                double_click,
            } if index < 8 => Some(Input::Button(MouseButton {
                index,
                action: match (down, double_click) {
                    (false, _) => ButtonAction::Up,
                    (true, false) => ButtonAction::Down,
                    (true, true) => ButtonAction::DoubleClick,
                },
            })),
            MouseEvent::Button { .. } => None,
        }
    }

    /// The event a routed key message posts: a key (event 1) or a
    /// character (event 2).
    #[must_use]
    pub fn from_key_event(event: &KeyEvent) -> Input {
        match event.kind {
            KeyEventKind::Key => Input::Key {
                key: event.key,
                mods: KeyMods::from_record(event.down, event.modifiers.0),
            },
            KeyEventKind::Char => Input::Char {
                wparam: event.wparam,
                lparam: event.lparam,
            },
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::input::keyboard::Modifiers;

    #[test]
    fn buttons_carry_the_reference_fields() {
        let up = MouseButton {
            index: 2,
            action: ButtonAction::Up,
        };
        assert_eq!((up.field0(), up.field1()), (-3, 1));
        assert_eq!(
            (
                MouseButton::LEFT_DOWN.field0(),
                MouseButton::LEFT_DOWN.field1()
            ),
            (-1, 2)
        );
        let double = Input::from_mouse_event(
            MouseEvent::Button {
                index: 0,
                down: true,
                double_click: true,
            },
            (0, 0),
            0,
        );
        assert_eq!(
            double,
            Some(Input::Button(MouseButton {
                index: 0,
                action: ButtonAction::DoubleClick
            }))
        );
        assert_eq!(
            Input::from_mouse_event(
                MouseEvent::Button {
                    index: WHEEL_BUTTON,
                    down: true,
                    double_click: false
                },
                (0, 0),
                -2
            ),
            Some(Input::Wheel(-2))
        );
    }

    #[test]
    fn key_modifier_bytes() {
        let mods = KeyMods::from_record(true, Modifiers::SHIFT.0 | Modifiers::ALT.0);
        assert_eq!(mods.0, KeyMods::DOWN | KeyMods::SHIFT | KeyMods::ALT);
        assert_eq!(KeyMods::from_record(false, Modifiers::CONTROL.0).0, 0x09);
    }
}
