//! The space class of the server's chat commands (`ChatCommand_SpaceClass`,
//! `Server/server_chat.cpp:324`): 0x09 to 0x0D, 0x20, and every code point
//! from U+0100 to U+FFFF that Windows' `GetStringTypeW(CT_CTYPE1)` reports
//! as `C1_SPACE`. The table is checked against Windows by the `chat_space`
//! oracle suite.

/// The code points above U+00FF that `GetStringTypeW` reports as
/// `C1_SPACE`, ascending.
pub const WIDE_SPACES: &[u16] = &[
    0x1680, 0x180e, 0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008,
    0x2009, 0x200a, 0x200b, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0xfeff,
];

/// Whether a unit is in the chat commands' space class.
#[must_use]
pub fn is_chat_space(unit: u16) -> bool {
    (0x09..=0x0d).contains(&unit) || unit == 0x20 || WIDE_SPACES.binary_search(&unit).is_ok()
}
