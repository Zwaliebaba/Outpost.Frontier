//! One module's text catalog and its file format.

use std::collections::BTreeMap;
use std::fmt;
use std::path::Path;

/// The first four bytes of a catalog file.
pub const MAGIC: [u8; 4] = *b"OFTC";
/// The format version this module reads and writes.
pub const VERSION: u16 = 1;
/// The extension of a catalog file, without the dot.
pub const EXTENSION: &str = "tcat";

const HEADER_BYTES: usize = 16;
const INDEX_ENTRY_BYTES: usize = 12;
/// An `RT_STRING` entry stores its length in a 16-bit word.
const MAX_STRING_UNITS: usize = 0xFFFF;

/// The two kinds of entry a catalog holds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EntryKind {
    /// A string-table entry (`RT_STRING`), UTF-16.
    String,
    /// A rich-text card (resource type 23), raw bytes.
    Card,
}

impl fmt::Display for EntryKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::String => "string",
            Self::Card => "card",
        })
    }
}

/// Why a catalog could not be built, written or read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CatalogError {
    /// The file does not start with [`MAGIC`].
    BadMagic,
    /// The file has a version this module does not read.
    UnsupportedVersion(u16),
    /// The file ends inside its header, an index or an entry.
    Truncated,
    /// A reserved field is not zero.
    ReservedNotZero,
    /// The IDs of an index are not strictly ascending.
    IdsNotAscending { kind: EntryKind, id: u16 },
    /// A string's byte length is odd.
    OddStringLength { id: u16 },
    /// A string is longer than an `RT_STRING` entry can be.
    StringTooLong { id: u16, units: usize },
    /// An ID was added twice.
    DuplicateId { kind: EntryKind, id: u16 },
    /// The catalog does not fit the 32-bit offsets of the format.
    TooLarge,
}

impl fmt::Display for CatalogError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::BadMagic => f.write_str("not a text catalog (bad magic)"),
            Self::UnsupportedVersion(version) => {
                write!(f, "unsupported text catalog version {version}")
            }
            Self::Truncated => f.write_str("text catalog is truncated"),
            Self::ReservedNotZero => f.write_str("reserved field is not zero"),
            Self::IdsNotAscending { kind, id } => {
                write!(f, "{kind} IDs are not ascending at {id}")
            }
            Self::OddStringLength { id } => write!(f, "string {id} has an odd byte length"),
            Self::StringTooLong { id, units } => {
                write!(f, "string {id} has {units} UTF-16 units, more than 65535")
            }
            Self::DuplicateId { kind, id } => write!(f, "{kind} {id} is defined twice"),
            Self::TooLarge => f.write_str("text catalog is larger than 4 GiB"),
        }
    }
}

impl std::error::Error for CatalogError {}

/// Why [`TextCatalog::read`] failed.
#[derive(Debug)]
pub enum CatalogReadError {
    /// The file could not be read.
    Io(std::io::Error),
    /// The file is not a valid catalog.
    Format(CatalogError),
}

impl fmt::Display for CatalogReadError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io(error) => error.fmt(f),
            Self::Format(error) => error.fmt(f),
        }
    }
}

impl std::error::Error for CatalogReadError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io(error) => Some(error),
            Self::Format(error) => Some(error),
        }
    }
}

/// The text resources of one module (one resource DLL of the C++), keyed
/// by the local 16-bit ID: string-table entries as UTF-16 and rich-text
/// cards as the bytes of the resource.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TextCatalog {
    strings: BTreeMap<u16, Vec<u16>>,
    cards: BTreeMap<u16, Vec<u8>>,
}

impl TextCatalog {
    /// An empty catalog.
    pub fn new() -> Self {
        Self::default()
    }

    /// Adds string `id`. An empty string is an entry too: it makes its
    /// block exist (see [`TextCatalog::string_entry`]).
    pub fn add_string(&mut self, id: u16, units: Vec<u16>) -> Result<(), CatalogError> {
        if units.len() > MAX_STRING_UNITS {
            return Err(CatalogError::StringTooLong {
                id,
                units: units.len(),
            });
        }
        if self.strings.contains_key(&id) {
            return Err(CatalogError::DuplicateId {
                kind: EntryKind::String,
                id,
            });
        }
        self.strings.insert(id, units);
        Ok(())
    }

    /// Adds card `id` with the resource's bytes.
    pub fn add_card(&mut self, id: u16, bytes: Vec<u8>) -> Result<(), CatalogError> {
        if self.cards.contains_key(&id) {
            return Err(CatalogError::DuplicateId {
                kind: EntryKind::Card,
                id,
            });
        }
        self.cards.insert(id, bytes);
        Ok(())
    }

    /// The defined strings in ascending ID order.
    pub fn strings(&self) -> impl Iterator<Item = (u16, &[u16])> {
        self.strings
            .iter()
            .map(|(&id, units)| (id, units.as_slice()))
    }

    /// The cards in ascending ID order.
    pub fn cards(&self) -> impl Iterator<Item = (u16, &[u8])> {
        self.cards.iter().map(|(&id, bytes)| (id, bytes.as_slice()))
    }

    /// The number of defined strings.
    pub fn string_count(&self) -> usize {
        self.strings.len()
    }

    /// The number of cards.
    pub fn card_count(&self) -> usize {
        self.cards.len()
    }

    /// The string defined with `id`, if any.
    pub fn string(&self, id: u16) -> Option<&[u16]> {
        self.strings.get(&id).map(Vec::as_slice)
    }

    /// Whether the `RT_STRING` block that holds `id` exists. `rc.exe`
    /// writes a block of 16 entries, `(id >> 4) + 1`, for every 16 IDs
    /// of which at least one is defined.
    pub fn block_exists(&self, id: u16) -> bool {
        let first = id & !0xF;
        self.strings.range(first..=first | 0xF).next().is_some()
    }

    /// The string-table entry the C++ finds for `id`: `None` when its
    /// block does not exist, otherwise the entry, empty when `id` itself
    /// is not defined.
    pub fn string_entry(&self, id: u16) -> Option<&[u16]> {
        if self.block_exists(id) {
            Some(self.string(id).unwrap_or_default())
        } else {
            None
        }
    }

    /// The bytes of card `id`, if any.
    pub fn card(&self, id: u16) -> Option<&[u8]> {
        self.cards.get(&id).map(Vec::as_slice)
    }

    /// The catalog in the file format of the module documentation.
    pub fn to_bytes(&self) -> Result<Vec<u8>, CatalogError> {
        let count = |n: usize| u32::try_from(n).map_err(|_| CatalogError::TooLarge);
        let string_count = count(self.strings.len())?;
        let card_count = count(self.cards.len())?;
        let index_bytes = (self.strings.len() + self.cards.len()) * INDEX_ENTRY_BYTES;

        let mut index = Vec::with_capacity(index_bytes);
        let mut data = Vec::new();
        let mut push = |id: u16, bytes: &[u8], data: &mut Vec<u8>| {
            let offset = count(data.len())?;
            let length = count(bytes.len())?;
            index.extend_from_slice(&id.to_le_bytes());
            index.extend_from_slice(&0u16.to_le_bytes());
            index.extend_from_slice(&offset.to_le_bytes());
            index.extend_from_slice(&length.to_le_bytes());
            data.extend_from_slice(bytes);
            count(data.len()).map(|_| ())
        };
        for (&id, units) in &self.strings {
            let bytes: Vec<u8> = units.iter().flat_map(|u| u.to_le_bytes()).collect();
            push(id, &bytes, &mut data)?;
        }
        for (&id, bytes) in &self.cards {
            push(id, bytes, &mut data)?;
        }

        let mut out = Vec::with_capacity(HEADER_BYTES + index.len() + data.len());
        out.extend_from_slice(&MAGIC);
        out.extend_from_slice(&VERSION.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
        out.extend_from_slice(&string_count.to_le_bytes());
        out.extend_from_slice(&card_count.to_le_bytes());
        out.extend_from_slice(&index);
        out.extend_from_slice(&data);
        Ok(out)
    }

    /// Reads a catalog from the bytes of a file.
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, CatalogError> {
        let header = bytes.get(..HEADER_BYTES).ok_or(CatalogError::Truncated)?;
        if header[0..4] != MAGIC {
            return Err(CatalogError::BadMagic);
        }
        let version = le_u16(header, 4);
        if version != VERSION {
            return Err(CatalogError::UnsupportedVersion(version));
        }
        if le_u16(header, 6) != 0 {
            return Err(CatalogError::ReservedNotZero);
        }
        let string_count = to_usize(le_u32(header, 8));
        let card_count = to_usize(le_u32(header, 12));
        let index_bytes = string_count
            .checked_add(card_count)
            .and_then(|n| n.checked_mul(INDEX_ENTRY_BYTES))
            .ok_or(CatalogError::Truncated)?;
        let data_start = HEADER_BYTES
            .checked_add(index_bytes)
            .ok_or(CatalogError::Truncated)?;
        let index = bytes
            .get(HEADER_BYTES..data_start)
            .ok_or(CatalogError::Truncated)?;
        let data = &bytes[data_start..];

        let mut catalog = Self::new();
        let mut previous: Option<u16> = None;
        for (position, entry) in index.as_chunks::<INDEX_ENTRY_BYTES>().0.iter().enumerate() {
            let kind = if position < string_count {
                EntryKind::String
            } else {
                EntryKind::Card
            };
            if position == string_count {
                previous = None;
            }
            let id = le_u16(entry, 0);
            if le_u16(entry, 2) != 0 {
                return Err(CatalogError::ReservedNotZero);
            }
            if previous.is_some_and(|p| p >= id) {
                return Err(CatalogError::IdsNotAscending { kind, id });
            }
            previous = Some(id);
            let offset = to_usize(le_u32(entry, 4));
            let length = to_usize(le_u32(entry, 8));
            let payload = offset
                .checked_add(length)
                .and_then(|end| data.get(offset..end))
                .ok_or(CatalogError::Truncated)?;
            match kind {
                EntryKind::String => {
                    if payload.len() % 2 != 0 {
                        return Err(CatalogError::OddStringLength { id });
                    }
                    let units = payload
                        .as_chunks::<2>()
                        .0
                        .iter()
                        .map(|&pair| u16::from_le_bytes(pair))
                        .collect();
                    catalog.add_string(id, units)?;
                }
                EntryKind::Card => catalog.add_card(id, payload.to_vec())?,
            }
        }
        Ok(catalog)
    }

    /// Reads the catalog file at `path`.
    pub fn read(path: &Path) -> Result<Self, CatalogReadError> {
        let bytes = std::fs::read(path).map_err(CatalogReadError::Io)?;
        Self::from_bytes(&bytes).map_err(CatalogReadError::Format)
    }
}

fn le_u16(bytes: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([bytes[at], bytes[at + 1]])
}

fn le_u32(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

fn to_usize(value: u32) -> usize {
    usize::try_from(value).unwrap_or(usize::MAX)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> TextCatalog {
        let mut catalog = TextCatalog::new();
        catalog.add_string(0x21, vec![0x48, 0x69]).unwrap();
        catalog.add_string(0x03, vec![]).unwrap();
        catalog.add_card(7, vec![0xFF, 0xFE, 0x41, 0x00]).unwrap();
        catalog
    }

    #[test]
    fn layout_is_the_documented_one() {
        let bytes = sample().to_bytes().unwrap();
        let want: Vec<u8> = [
            &b"OFTC"[..],
            &[1, 0, 0, 0],
            &[2, 0, 0, 0],
            &[1, 0, 0, 0],
            // string 3: offset 0, 0 bytes
            &[3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
            // string 0x21: offset 0, 4 bytes
            &[0x21, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0],
            // card 7: offset 4, 4 bytes
            &[7, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0],
            &[0x48, 0, 0x69, 0],
            &[0xFF, 0xFE, 0x41, 0x00],
        ]
        .concat();
        assert_eq!(bytes, want);
    }

    #[test]
    fn round_trip() {
        let catalog = sample();
        let bytes = catalog.to_bytes().unwrap();
        assert_eq!(TextCatalog::from_bytes(&bytes).unwrap(), catalog);
        let empty = TextCatalog::new();
        assert_eq!(
            TextCatalog::from_bytes(&empty.to_bytes().unwrap()).unwrap(),
            empty
        );
    }

    #[test]
    fn blocks_follow_rc() {
        let catalog = sample();
        // 0x03 is defined (empty): block 1 (IDs 0..=15) exists.
        assert_eq!(catalog.string_entry(0x03), Some(&[][..]));
        assert_eq!(catalog.string_entry(0x0F), Some(&[][..]));
        assert_eq!(catalog.string_entry(0x10), None);
        assert_eq!(catalog.string_entry(0x20), Some(&[][..]));
        assert_eq!(catalog.string_entry(0x21), Some(&[0x48, 0x69][..]));
        assert_eq!(catalog.string_entry(0x30), None);
        assert!(!catalog.block_exists(0xFFFF));
    }

    #[test]
    fn rejects_bad_files() {
        let good = sample().to_bytes().unwrap();
        assert_eq!(
            TextCatalog::from_bytes(&good[..10]),
            Err(CatalogError::Truncated)
        );
        let mut bad = good.clone();
        bad[0] = b'X';
        assert_eq!(TextCatalog::from_bytes(&bad), Err(CatalogError::BadMagic));
        let mut bad = good.clone();
        bad[4] = 2;
        assert_eq!(
            TextCatalog::from_bytes(&bad),
            Err(CatalogError::UnsupportedVersion(2))
        );
        let mut bad = good.clone();
        bad[6] = 1;
        assert_eq!(
            TextCatalog::from_bytes(&bad),
            Err(CatalogError::ReservedNotZero)
        );
        // Swap the IDs of the two strings.
        let mut bad = good.clone();
        bad[16] = 0x21;
        bad[28] = 0x03;
        assert_eq!(
            TextCatalog::from_bytes(&bad),
            Err(CatalogError::IdsNotAscending {
                kind: EntryKind::String,
                id: 0x03
            })
        );
        // Odd string length.
        let mut bad = good.clone();
        bad[36] = 3;
        assert_eq!(
            TextCatalog::from_bytes(&bad),
            Err(CatalogError::OddStringLength { id: 0x21 })
        );
        // A card past the end.
        let mut bad = good.clone();
        bad[48] = 5;
        assert_eq!(TextCatalog::from_bytes(&bad), Err(CatalogError::Truncated));
        assert_eq!(
            TextCatalog::from_bytes(&good[..good.len() - 1]),
            Err(CatalogError::Truncated)
        );
    }

    #[test]
    fn card_ids_restart_the_order() {
        let mut catalog = TextCatalog::new();
        catalog.add_string(9, vec![1]).unwrap();
        catalog.add_card(1, vec![1]).unwrap();
        let bytes = catalog.to_bytes().unwrap();
        assert_eq!(TextCatalog::from_bytes(&bytes).unwrap(), catalog);
    }

    #[test]
    fn add_rejects_duplicates_and_long_strings() {
        let mut catalog = sample();
        assert_eq!(
            catalog.add_string(3, vec![1]),
            Err(CatalogError::DuplicateId {
                kind: EntryKind::String,
                id: 3
            })
        );
        assert_eq!(
            catalog.add_card(7, vec![]),
            Err(CatalogError::DuplicateId {
                kind: EntryKind::Card,
                id: 7
            })
        );
        assert_eq!(
            catalog.add_string(4, vec![0; 0x10000]),
            Err(CatalogError::StringTooLong {
                id: 4,
                units: 0x10000
            })
        );
        assert!(catalog.add_string(4, vec![0; 0xFFFF]).is_ok());
    }
}
