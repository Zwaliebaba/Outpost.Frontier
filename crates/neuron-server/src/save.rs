//! The `FLS1` save container (`Server/server_savefile.cpp`) and the way the
//! server's readers tell it from plain text (`PlayerData_LoadSaveRecord`,
//! `PlayerDB::ReadCharacterName`, the new-character template;
//! `Server/server_savegame.cpp:353-380`, `server_classes.cpp:3508-3527`,
//! `:3660-3680`).
//!
//! Saves are written as plain text (decision O12); both builds still read
//! `FLS1`, so existing saves and the shipped templates load.

use neuron_core::msvcrt::text_mode_read;

/// The four bytes that open an `FLS1` file (`Server_FLS1Magic`).
pub const FLS1_MAGIC: [u8; 4] = *b"FLS1";

/// The four bytes of the container every reader refuses.
pub const FLSG_MAGIC: [u8; 4] = *b"FLSG";

/// The key of the cipher. The encoder indexes the literal `"Genesis - In
/// the beginning ..."` with `i & 3`, so only its first four bytes are used.
const KEY: [u8; 4] = *b"Gene";

/// `Server_FLS1Transform`: each byte XORed with `(key[i & 3] + i) | 0x80`,
/// the addition in 8 bits. The transform is its own inverse.
pub fn fls1_transform(data: &mut [u8]) {
    for (i, byte) in data.iter_mut().enumerate() {
        // `(unsigned char)I`: the low byte of the index.
        let mask = (KEY[i & 3].wrapping_add(i as u8)) | 0x80;
        *byte ^= mask;
    }
}

/// Encodes plain text as an `FLS1` file: the magic, then the transformed
/// text.
#[must_use]
pub fn encode_fls1(text: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(text.len() + 4);
    out.extend_from_slice(&FLS1_MAGIC);
    let start = out.len();
    out.extend_from_slice(text);
    fls1_transform(&mut out[start..]);
    out
}

/// What a server reader makes of a save file's bytes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SaveContents {
    /// Plain text, as `fread` gives it from the file opened in text mode
    /// (`ReadTail(0)`): CR LF read as LF, the input ending at a Ctrl-Z.
    Plain(Vec<u8>),
    /// An `FLS1` file, reopened in binary mode, read from offset 4 and
    /// decoded (`ReadTail(4)`, `Encode`).
    Fls1(Vec<u8>),
    /// An `FLSG` file, which every reader refuses.
    Refused,
}

impl SaveContents {
    /// The text the reader hands to `INI_Reader::open_memory`, or `None`
    /// for a refused file.
    #[must_use]
    pub fn text(&self) -> Option<&[u8]> {
        match self {
            Self::Plain(text) | Self::Fls1(text) => Some(text),
            Self::Refused => None,
        }
    }
}

/// Reads a save file's bytes as the server's readers do.
///
/// The readers take the magic with a text-mode `fread` of four bytes. Text
/// mode cannot turn other bytes into `FLS1` or `FLSG` (it only removes CRs
/// and stops at Ctrl-Z), so the raw prefix decides. A file shorter than four
/// bytes matches neither, as the zeroed magic buffers of the reference do.
#[must_use]
pub fn read_save(bytes: &[u8]) -> SaveContents {
    match bytes.get(..4) {
        Some(magic) if magic == FLS1_MAGIC => {
            let mut text = bytes[4..].to_vec();
            fls1_transform(&mut text);
            SaveContents::Fls1(text)
        }
        Some(magic) if magic == FLSG_MAGIC => SaveContents::Refused,
        _ => SaveContents::Plain(text_mode_read(bytes)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn transform_matches_the_key_schedule() {
        // Byte 0: 'G' + 0 = 0x47 | 0x80 = 0xC7; byte 1: 'e' + 1 = 0x66 | 0x80
        // = 0xE6; byte 4: 'G' + 4 = 0x4B | 0x80 = 0xCB.
        let mut data = [0u8; 6];
        fls1_transform(&mut data);
        assert_eq!(data, [0xC7, 0xE6, 0xF0, 0xE8, 0xCB, 0xEA]);
        // The 8-bit addition wraps: 'n' (0x6E) + 0xFE = 0x6C | 0x80.
        let mut long = vec![0u8; 0x100];
        fls1_transform(&mut long);
        assert_eq!(long[0xFE], 0xEC);
    }

    #[test]
    fn round_trip_and_magic() {
        let text = b"[Player]\nname = test\n";
        let file = encode_fls1(text);
        assert_eq!(&file[..4], b"FLS1");
        assert_eq!(read_save(&file), SaveContents::Fls1(text.to_vec()));
        assert_eq!(read_save(b"FLSGxyz"), SaveContents::Refused);
        assert_eq!(
            read_save(b"[Player]\r\nname = a\r\n\x1ajunk"),
            SaveContents::Plain(b"[Player]\nname = a\n".to_vec())
        );
        assert_eq!(read_save(b"FLS"), SaveContents::Plain(b"FLS".to_vec()));
        assert_eq!(read_save(b"FLS1"), SaveContents::Fls1(Vec::new()));
    }
}
