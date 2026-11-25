//! The dedicated server's two files in its local user folder: the message
//! of the day (`motd.dat`, `OutpostServer/Motd.cpp`, read by the server
//! thread, `OutpostServer.cpp:337-349`) and the configuration dialog's
//! binary settings (`OutpostServer.cfg`, `OutpostServer/ServerConfigDlg.cpp`),
//! which the TOML configuration imports once (decision T22).

use neuron_core::msvcrt::text_mode_write;

/// The message of the day's file name.
pub const MOTD_FILE: &str = "motd.dat";

/// The bytes the server thread reads (`fread(MotdText, 0x2800, 1, File)`).
pub const MOTD_READ_BYTES: usize = 0x2800;

/// The characters it keeps (`MotdText[0x13ff] = 0`).
pub const MOTD_READ_CHARS: usize = 0x13ff;

/// The characters the news dialog can return: `GetRichEditText` into a
/// buffer of 0x1400 bytes, its terminator included.
pub const MOTD_EDIT_CHARS: usize = 0x1400 / 2 - 1;

/// The message of `motd.dat`'s bytes: the first 0x2800 bytes as UTF-16
/// little endian in a zeroed buffer (an odd last byte is a unit's low
/// byte), up to the first NUL and at most 0x13ff characters.
#[must_use]
pub fn read_motd(bytes: &[u8]) -> Vec<u16> {
    let bytes = &bytes[..bytes.len().min(MOTD_READ_BYTES)];
    let mut text: Vec<u16> = bytes
        .chunks(2)
        .map(|pair| u16::from(pair[0]) | pair.get(1).map_or(0, |&b| u16::from(b) << 8))
        .take(MOTD_READ_CHARS)
        .collect();
    if let Some(end) = text.iter().position(|&c| c == 0) {
        text.truncate(end);
    }
    text
}

/// `SaveMotd`'s bytes: the message up to its first NUL as raw UTF-16
/// little endian, written through a text-mode `"w"` stream, so each 0x0a
/// byte gains a 0x0d before it as the C++'s file does (the reader opens it
/// `"rb"`).
#[must_use]
pub fn encode_motd(text: &[u16]) -> Vec<u8> {
    let end = text.iter().position(|&c| c == 0).unwrap_or(text.len());
    let raw: Vec<u8> = text[..end].iter().flat_map(|c| c.to_le_bytes()).collect();
    text_mode_write(&raw)
}

/// The settings file's name.
pub const LEGACY_CONFIG_FILE: &str = "OutpostServer.cfg";

/// `OutpostServer.cfg` as the configuration dialog reads it: a version
/// word (2 or 3), then fixed UTF-16 fields, two flags, the player limit
/// and, in version 3, the PvP flag.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LegacyServerConfig {
    /// The version word.
    pub version: i32,
    /// The server name (0x21 characters).
    pub name: Vec<u16>,
    /// The description (0x81 characters).
    pub description: Vec<u16>,
    /// The session password (0x11 characters).
    pub password: Vec<u16>,
    /// New accounts may be made.
    pub allow_new_players: bool,
    /// "Internet accessible": the list-server report (not ported, decision
    /// O8).
    pub internet_accessible: bool,
    /// The player limit.
    pub max_players: i32,
    /// Players may fight each other (version 3).
    pub allow_pvp: bool,
}

impl Default for LegacyServerConfig {
    /// `OnInitDialog`'s defaults before the file is read.
    fn default() -> Self {
        Self {
            version: 3,
            name: Vec::new(),
            description: Vec::new(),
            password: Vec::new(),
            allow_new_players: true,
            internet_accessible: true,
            max_players: 0x10,
            allow_pvp: false,
        }
    }
}

const NAME_CHARS: usize = 0x21;
const DESCRIPTION_CHARS: usize = 0x81;
const PASSWORD_CHARS: usize = 0x11;

/// A fixed field's characters up to the first NUL.
fn wide_field(bytes: &[u8]) -> Vec<u16> {
    bytes
        .chunks_exact(2)
        .map(|p| u16::from_le_bytes([p[0], p[1]]))
        .take_while(|&c| c != 0)
        .collect()
}

impl LegacyServerConfig {
    /// `CServerConfigDlg::OnInitDialog`'s read: the version word, and for
    /// version 2 or 3 the fields in order, each over the default as far as
    /// the file has it (a short `fread` keeps what it read). `None` for an
    /// empty file.
    #[must_use]
    pub fn from_bytes(bytes: &[u8]) -> Option<Self> {
        let mut config = Self::default();
        let mut at = 0usize;
        let mut take = |n: usize| -> Option<&[u8]> {
            if at >= bytes.len() {
                return None;
            }
            let end = (at + n).min(bytes.len());
            let field = &bytes[at..end];
            at = end;
            Some(field)
        };
        let word = |field: &[u8], old: i32| {
            let mut raw = old.to_le_bytes();
            raw[..field.len()].copy_from_slice(field);
            i32::from_le_bytes(raw)
        };
        config.version = word(take(4)?, config.version);
        if config.version != 2 && config.version != 3 {
            return Some(config);
        }
        let mut wide = |config_field: &mut Vec<u16>, chars: usize| {
            if let Some(field) = take(chars * 2) {
                *config_field = wide_field(field);
            }
        };
        wide(&mut config.name, NAME_CHARS);
        wide(&mut config.description, DESCRIPTION_CHARS);
        wide(&mut config.password, PASSWORD_CHARS);
        if let Some(b) = take(1) {
            config.allow_new_players = b[0] != 0;
        }
        if let Some(b) = take(1) {
            config.internet_accessible = b[0] != 0;
        }
        if let Some(b) = take(4) {
            config.max_players = word(b, config.max_players);
        }
        if config.version == 3
            && let Some(b) = take(1)
        {
            config.allow_pvp = b[0] != 0;
        }
        Some(config)
    }

    /// The file `CServerConfigDlg::OnOK` writes (always version 3). Text
    /// longer than a field is cut, keeping its terminator.
    #[must_use]
    pub fn to_bytes(&self) -> Vec<u8> {
        fn field(out: &mut Vec<u8>, text: &[u16], chars: usize) {
            for i in 0..chars {
                let c = if i + 1 < chars {
                    text.get(i).copied().unwrap_or(0)
                } else {
                    0
                };
                out.extend_from_slice(&c.to_le_bytes());
            }
        }
        let mut out = Vec::new();
        out.extend_from_slice(&3i32.to_le_bytes());
        field(&mut out, &self.name, NAME_CHARS);
        field(&mut out, &self.description, DESCRIPTION_CHARS);
        field(&mut out, &self.password, PASSWORD_CHARS);
        out.push(u8::from(self.allow_new_players));
        out.push(u8::from(self.internet_accessible));
        out.extend_from_slice(&self.max_players.to_le_bytes());
        out.push(u8::from(self.allow_pvp));
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn w(text: &str) -> Vec<u16> {
        text.encode_utf16().collect()
    }

    #[test]
    fn the_motd_round_trips_through_text_mode() {
        let text = w("Welcome, pilots");
        let bytes = encode_motd(&text);
        assert_eq!(bytes.len(), text.len() * 2);
        assert_eq!(read_motd(&bytes), text);
        // A 0x0a byte gains a 0x0d, which the binary read keeps.
        let newline = encode_motd(&[0x0a, 0x41]);
        assert_eq!(newline, [0x0d, 0x0a, 0x00, 0x41, 0x00]);
        assert_eq!(read_motd(&newline), [0x0a0d, 0x4100]);
    }

    #[test]
    fn the_motd_read_stops_at_its_limits() {
        assert_eq!(read_motd(&[0x41, 0, 0, 0, 0x42, 0]), w("A"));
        assert_eq!(read_motd(&[0x41]), [0x41]);
        let long: Vec<u8> = std::iter::repeat_n([0x41u8, 0], 0x2000).flatten().collect();
        assert_eq!(read_motd(&long).len(), MOTD_READ_CHARS);
        assert!(read_motd(&[]).is_empty());
    }

    #[test]
    fn the_legacy_config_round_trips() {
        let config = LegacyServerConfig {
            version: 3,
            name: w("Frontier"),
            description: w("A server"),
            password: w("secret"),
            allow_new_players: false,
            internet_accessible: true,
            max_players: 32,
            allow_pvp: true,
        };
        let bytes = config.to_bytes();
        assert_eq!(bytes.len(), 4 + 0x42 + 0x102 + 0x22 + 1 + 1 + 4 + 1);
        assert_eq!(LegacyServerConfig::from_bytes(&bytes), Some(config));
    }

    #[test]
    fn a_version_2_file_has_no_pvp_byte() {
        let mut config = LegacyServerConfig {
            name: w("Old"),
            allow_pvp: true,
            ..LegacyServerConfig::default()
        };
        let mut bytes = config.to_bytes();
        bytes[0] = 2;
        config.version = 2;
        config.allow_pvp = false;
        assert_eq!(LegacyServerConfig::from_bytes(&bytes), Some(config));
    }

    #[test]
    fn an_unknown_version_keeps_the_defaults() {
        let read = LegacyServerConfig::from_bytes(&7i32.to_le_bytes()).unwrap();
        assert_eq!(read.version, 7);
        assert_eq!(read.max_players, 16);
        assert_eq!(LegacyServerConfig::from_bytes(&[]), None);
        // A file cut inside the description keeps what was read.
        let full = LegacyServerConfig {
            name: w("Cut"),
            ..LegacyServerConfig::default()
        }
        .to_bytes();
        let read = LegacyServerConfig::from_bytes(&full[..4 + 0x42 + 4]).unwrap();
        assert_eq!(read.name, w("Cut"));
        assert!(read.description.is_empty());
        assert_eq!(read.max_players, 16);
    }

    #[test]
    fn long_text_is_cut_to_its_field() {
        let config = LegacyServerConfig {
            password: w(&"p".repeat(40)),
            ..LegacyServerConfig::default()
        };
        let read = LegacyServerConfig::from_bytes(&config.to_bytes()).unwrap();
        assert_eq!(read.password.len(), PASSWORD_CHARS - 1);
    }
}
