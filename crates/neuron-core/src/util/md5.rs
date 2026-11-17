//! The MD5 text form of `NeuronRuntime`'s `MD5Hash` (`md5hash.cpp`), the
//! form account names take (decision T16).
//!
//! The digest is standard MD5 (RFC 1321; the C++ constants and round order
//! were checked against it). The text is not the usual hex digest: it is
//! the four state words, each a little-endian `u32` of the digest, printed
//! with `"%08x-%08x-%08x-%08x"`. [`Md5Words::parse`] reads it back as the
//! C++ `sscanf(text, "%x-%x-%x-%x")` does.

use md5::{Digest, Md5};

/// The MD5 initial state; the words a parse leaves where the text has no
/// field.
pub const MD5_INITIAL: [u32; 4] = [0x6745_2301, 0xEFCD_AB89, 0x98BA_DCFE, 0x1032_5476];

/// The four state words of an `MD5Hash`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Md5Words(pub [u32; 4]);

impl Default for Md5Words {
    /// The state of a fresh `MD5Hash`.
    fn default() -> Self {
        Self(MD5_INITIAL)
    }
}

impl Md5Words {
    /// The words of a digest: bytes 0 to 3 are the first word, low byte
    /// first.
    #[must_use]
    pub fn from_digest(digest: [u8; 16]) -> Self {
        let (chunks, _) = digest.as_chunks::<4>();
        let mut words = [0u32; 4];
        for (word, bytes) in words.iter_mut().zip(chunks) {
            *word = u32::from_le_bytes(*bytes);
        }
        Self(words)
    }

    /// The bytes `GetStatePtr` exposes: the words, low byte first.
    #[must_use]
    pub fn to_bytes(self) -> [u8; 16] {
        let mut bytes = [0u8; 16];
        let (chunks, _) = bytes.as_chunks_mut::<4>();
        for (chunk, word) in chunks.iter_mut().zip(self.0) {
            *chunk = word.to_le_bytes();
        }
        bytes
    }

    /// `AsString`: `"%08x-%08x-%08x-%08x"`, lower case.
    #[must_use]
    pub fn to_text(self) -> String {
        let [a, b, c, d] = self.0;
        format!("{a:08x}-{b:08x}-{c:08x}-{d:08x}")
    }

    /// `FromString` on a fresh `MD5Hash`: the words of `text`, with
    /// [`MD5_INITIAL`] words where it has no field.
    #[must_use]
    pub fn parse(text: &[u8]) -> Self {
        let mut words = Self::default();
        words.parse_into(text);
        words
    }

    /// `FromString`: `sscanf(text, "%x-%x-%x-%x")` into these words. A field
    /// that does not convert ends the scan; the words from there on keep
    /// their values. Returns the number of words assigned.
    ///
    /// Each `%x` skips leading white space, takes an optional sign and an
    /// optional `0x` prefix, and accumulates hex digits in 64 bits: an
    /// overflow gives all ones and a minus sign negates. The low 32 bits are
    /// stored. Between fields, the next byte must be `-`.
    pub fn parse_into(&mut self, text: &[u8]) -> usize {
        let text = crate::hash::c_str(text);
        let mut pos = 0;
        for (index, word) in self.0.iter_mut().enumerate() {
            if index > 0 {
                if text.get(pos) != Some(&b'-') {
                    return index;
                }
                pos += 1;
            }
            let Some((value, next)) = scan_hex(text, pos) else {
                return index;
            };
            *word = value;
            pos = next;
        }
        4
    }
}

/// One `%x` conversion starting at `pos`: the low 32 bits and the position
/// after the field, or `None` when no digit is read.
fn scan_hex(text: &[u8], mut pos: usize) -> Option<(u32, usize)> {
    while text
        .get(pos)
        .is_some_and(|&c| matches!(c, b' ' | b'\t' | b'\n' | 0x0B | 0x0C | b'\r'))
    {
        pos += 1;
    }
    let mut negative = false;
    match text.get(pos) {
        Some(b'-') => {
            negative = true;
            pos += 1;
        }
        Some(b'+') => pos += 1,
        _ => {}
    }
    if text.get(pos) == Some(&b'0') && matches!(text.get(pos + 1), Some(b'x' | b'X')) {
        pos += 2;
    }
    let mut value: u64 = 0;
    let mut overflow = false;
    let mut digits = 0;
    while let Some(digit) = text.get(pos).and_then(|&c| char::from(c).to_digit(16)) {
        if let Some(next) = value
            .checked_mul(16)
            .and_then(|v| v.checked_add(u64::from(digit)))
        {
            value = next;
        } else {
            overflow = true;
            value = value.wrapping_mul(16).wrapping_add(u64::from(digit));
        }
        digits += 1;
        pos += 1;
    }
    if digits == 0 {
        return None;
    }
    if overflow {
        value = u64::MAX;
    } else if negative {
        value = value.wrapping_neg();
    }
    Some(((value & 0xFFFF_FFFF) as u32, pos))
}

/// An `MD5Hash` being fed: `AddData` until `CalcValue`.
#[derive(Clone, Debug, Default)]
pub struct Md5Hash {
    hasher: Md5,
}

impl Md5Hash {
    /// A fresh hash.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// `AddData`.
    pub fn add_data(&mut self, data: &[u8]) {
        self.hasher.update(data);
    }

    /// `CalcValue`: the final state words.
    #[must_use]
    pub fn calc_value(self) -> Md5Words {
        Md5Words::from_digest(self.hasher.finalize().into())
    }
}

/// The state words after hashing `data`.
#[must_use]
pub fn md5_words(data: &[u8]) -> Md5Words {
    let mut hash = Md5Hash::new();
    hash.add_data(data);
    hash.calc_value()
}

/// The text form of the MD5 of `data`.
#[must_use]
pub fn md5_text(data: &[u8]) -> String {
    md5_words(data).to_text()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn text_is_the_state_words() {
        // MD5("") = d41d8cd98f00b204e9800998ecf8427e.
        assert_eq!(md5_text(b""), "d98c1dd4-04b2008f-980980e9-7e42f8ec");
        // MD5("abc") = 900150983cd24fb0d6963f7d28e17f72.
        assert_eq!(md5_text(b"abc"), "98500190-b04fd23c-7d3f96d6-727fe128");
    }

    #[test]
    fn chunked_input_gives_the_same_digest() {
        let data: Vec<u8> = (0..1000u32).map(|i| (i * 7 % 251) as u8).collect();
        let mut hash = Md5Hash::new();
        for chunk in data.chunks(13) {
            hash.add_data(chunk);
        }
        assert_eq!(hash.calc_value(), md5_words(&data));
    }

    #[test]
    fn parse_round_trips_and_keeps_missing_words() {
        let words = md5_words(b"account");
        assert_eq!(Md5Words::parse(words.to_text().as_bytes()), words);
        assert_eq!(
            Md5Words::parse(b"1-2"),
            Md5Words([1, 2, MD5_INITIAL[2], MD5_INITIAL[3]])
        );
        assert_eq!(Md5Words::parse(b""), Md5Words::default());
        assert_eq!(
            Md5Words::parse(b" 0xA-+B--1-123456789"),
            Md5Words([10, 11, u32::MAX, 0x2345_6789])
        );
        assert_eq!(Md5Words::parse(b"1 -2").0[1], MD5_INITIAL[1]);
        assert_eq!(Md5Words::parse(b"11112222333344445").0[0], u32::MAX);
        assert_eq!(Md5Words::parse(b"FfFfFfFf").0[0], u32::MAX);
    }

    #[test]
    fn bytes_are_little_endian_words() {
        let words = Md5Words([0x0403_0201, 0, 0, 0x100F_0E0D]);
        let bytes = words.to_bytes();
        assert_eq!(&bytes[..4], &[1, 2, 3, 4]);
        assert_eq!(Md5Words::from_digest(bytes), words);
    }
}
