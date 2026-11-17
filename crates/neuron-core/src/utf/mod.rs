//! The UTF tree container (work package P1.3): the archive format behind
//! `.utf`, `.cmp`, `.3db`, `.mat`, `.txm` and the other engine data files.
//!
//! Ported from the walker in `CppSource/ReadFile/ReadFile.cpp`
//! (`FUtfWalker`, `FUtfFileSystem::OpenArchive`). The archive is read
//! whole into memory and every access is bounds-checked: where the
//! reference would read outside the archive (a node, name or chain that
//! points past the end, a sibling chain that loops), the port returns a
//! [`UtfError`] instead (divergence register).
//!
//! Layout, little-endian. The 0x38-byte header holds the magic `"UTF "`,
//! version 0x101, the tree offset (+0x08), the node size (+0x14, 0x2C or
//! 0x0C), the string-table offset (+0x18) and the data-section offset
//! (+0x24); the other dwords are not read. Node offsets count from the
//! start of the tree, whose first record is the root.
//!
//! * 0x2C records ([`Layout::Full`]): +0x00 next sibling (0 ends the
//!   chain), +0x04 name offset in the string table, +0x08 flags (0x10 =
//!   directory), +0x10 first child or data offset, +0x1C size, +0x20,
//!   +0x24 and +0x28 creation, access and write times (DOS date in the
//!   low word, DOS time in the high word).
//! * 0x0C records ([`Layout::Compact`]): +0x00 bit 31 = directory, low 31
//!   bits + 4 = name offset; +0x04 first child or data offset; +0x08 child
//!   count or size. Children are contiguous.
//!
//! File bytes start at the header's data offset plus the node's data
//! offset. Names compare with the ASCII [`stricmp`]; the find functions
//! match with [`match_wildcard`].

mod time;
mod wildcard;

use std::sync::Arc;

use crate::hash::{c_str, stricmp};

pub use time::{dos_date_time_to_file_time, packed_to_file_time, system_time_to_file_time};
pub use wildcard::{FOLD_BELOW_TABLE, fold_wild_char, match_wildcard};

/// `"UTF "` read as a little-endian dword.
pub const MAGIC: u32 = 0x2046_5455;
/// The only version the reader accepts.
pub const VERSION: u32 = 0x101;
/// Bytes of the header.
pub const HEADER_SIZE: usize = 0x38;
/// The directory bit of the find attributes.
pub const ATTRIBUTE_DIRECTORY: u32 = 0x10;
/// Longest path component the walker compares when a separator follows
/// it; longer components are cut (`WalkPath`'s 0x104-byte buffer).
pub const COMPONENT_LIMIT: usize = 0x103;
/// Bytes of a find pattern kept per search (`strncpy(..., 0x104)`).
pub const MASK_LIMIT: usize = 0x104;

/// Why an archive cannot be opened or a record cannot be read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UtfError {
    /// Fewer than 0x38 header bytes, or no byte after the header.
    TooShort,
    /// The first dword is not `"UTF "`.
    BadMagic,
    /// The version is not 0x101.
    BadVersion,
    /// The node size is neither 0x0C nor 0x2C.
    BadNodeSize(u32),
    /// The tree offset is not inside the archive.
    TreeOutOfRange,
    /// The string-table offset is not inside the archive.
    StringsOutOfRange,
    /// A record field lies past the end of the archive. The reference reads
    /// outside its mapping.
    RecordOutOfRange {
        /// Node offset from the tree start.
        offset: u32,
    },
    /// A name does not end inside the archive.
    NameOutOfRange {
        /// Node offset from the tree start.
        offset: u32,
    },
    /// A file's data starts past the end of the archive.
    DataOutOfRange,
    /// A sibling chain visits more records than the archive can hold, so it
    /// loops. The reference never returns.
    Cycle,
}

impl core::fmt::Display for UtfError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::TooShort => f.write_str("UTF archive too short"),
            Self::BadMagic => f.write_str("not a UTF archive"),
            Self::BadVersion => f.write_str("unsupported UTF version"),
            Self::BadNodeSize(size) => write!(f, "unsupported UTF node size {size:#x}"),
            Self::TreeOutOfRange => f.write_str("UTF tree offset outside the archive"),
            Self::StringsOutOfRange => f.write_str("UTF string table outside the archive"),
            Self::RecordOutOfRange { offset } => {
                write!(f, "UTF record {offset:#x} outside the archive")
            }
            Self::NameOutOfRange { offset } => {
                write!(f, "name of UTF record {offset:#x} outside the archive")
            }
            Self::DataOutOfRange => f.write_str("UTF file data outside the archive"),
            Self::Cycle => f.write_str("UTF sibling chain loops"),
        }
    }
}

impl std::error::Error for UtfError {}

/// The two record layouts.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Layout {
    /// 0x0C-byte records with contiguous children.
    Compact,
    /// 0x2C-byte records with sibling chains and times.
    Full,
}

impl Layout {
    /// The record size the header names.
    #[must_use]
    pub const fn node_size(self) -> u32 {
        match self {
            Self::Compact => 0x0C,
            Self::Full => 0x2C,
        }
    }
}

/// The 0x38-byte header. Fields without evidence of use keep their offset
/// as their name.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Header {
    /// +0x00, [`MAGIC`].
    pub magic: u32,
    /// +0x04, [`VERSION`].
    pub version: u32,
    /// +0x08, offset of the root record.
    pub tree_offset: u32,
    /// +0x0C, not read by the reference.
    pub unknown_0c: u32,
    /// +0x10, not read by the reference.
    pub unknown_10: u32,
    /// +0x14, 0x0C or 0x2C.
    pub node_size: u32,
    /// +0x18, offset of the string table.
    pub string_offset: u32,
    /// +0x1C, not read by the reference.
    pub unknown_1c: u32,
    /// +0x20, not read by the reference.
    pub unknown_20: u32,
    /// +0x24, offset of the data section.
    pub data_offset: u32,
    /// +0x28, not read by the reference.
    pub unknown_28: u32,
    /// +0x2C, not read by the reference.
    pub unknown_2c: u32,
    /// +0x30, not read by the reference.
    pub unknown_30: u32,
    /// +0x34, not read by the reference.
    pub unknown_34: u32,
}

fn dword(bytes: &[u8], at: usize) -> Option<u32> {
    let end = at.checked_add(4)?;
    let b = bytes.get(at..end)?;
    Some(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

impl Header {
    /// The header at the start of `bytes`, or `None` when fewer than
    /// [`HEADER_SIZE`] bytes are given.
    #[must_use]
    pub fn read(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < HEADER_SIZE {
            return None;
        }
        let d = |at| dword(bytes, at).unwrap_or(0);
        Some(Self {
            magic: d(0x00),
            version: d(0x04),
            tree_offset: d(0x08),
            unknown_0c: d(0x0C),
            unknown_10: d(0x10),
            node_size: d(0x14),
            string_offset: d(0x18),
            unknown_1c: d(0x1C),
            unknown_20: d(0x20),
            data_offset: d(0x24),
            unknown_28: d(0x28),
            unknown_2c: d(0x2C),
            unknown_30: d(0x30),
            unknown_34: d(0x34),
        })
    }
}

/// A record, named by its offset from the start of the tree. The root is
/// offset 0. A reference is only meaningful for the archive it came from.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct NodeRef(u32);

impl NodeRef {
    /// The root record.
    pub const ROOT: Self = Self(0);

    /// The record at `offset` from the tree start.
    #[must_use]
    pub const fn from_offset(offset: u32) -> Self {
        Self(offset)
    }

    /// Offset from the tree start.
    #[must_use]
    pub const fn offset(self) -> u32 {
        self.0
    }
}

/// Every field of one record, decoded.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Record<'a> {
    /// The record.
    pub node: NodeRef,
    /// The name, without its NUL.
    pub name: &'a [u8],
    /// The directory bit.
    pub is_dir: bool,
    /// First child (directories) or data offset (files).
    pub data_offset: u32,
    /// Size (files; full records store one for directories too), or child
    /// count (compact directories).
    pub size: u32,
    /// The find attributes: the raw flags dword of a full record, 0x10 or 0
    /// for a compact one.
    pub attributes: u32,
    /// Next sibling offset (full records only).
    pub sibling: Option<u32>,
    /// Packed creation, access and write times (full records only).
    pub times: Option<[u32; 3]>,
}

/// A parsed archive. Cloning shares the bytes.
#[derive(Clone, Debug)]
pub struct UtfArchive {
    bytes: Arc<[u8]>,
    header: Header,
    layout: Layout,
}

fn is_separator(byte: u8) -> bool {
    byte == b'\\' || byte == b'/'
}

impl UtfArchive {
    /// Checks the header as `OpenArchive` and `InitializeArchiveView` do:
    /// at least 0x38 header bytes, the magic and version, a node size of
    /// 0x0C or 0x2C, more than 0x38 bytes in all, and tree and string
    /// offsets inside the archive. The data offset is not checked.
    ///
    /// # Errors
    ///
    /// The first check that fails.
    pub fn parse(bytes: impl Into<Arc<[u8]>>) -> Result<Self, UtfError> {
        let bytes = bytes.into();
        let header = Header::read(&bytes).ok_or(UtfError::TooShort)?;
        if header.magic != MAGIC {
            return Err(UtfError::BadMagic);
        }
        if header.version != VERSION {
            return Err(UtfError::BadVersion);
        }
        let layout = match header.node_size {
            0x0C => Layout::Compact,
            0x2C => Layout::Full,
            other => return Err(UtfError::BadNodeSize(other)),
        };
        if bytes.len() <= HEADER_SIZE {
            return Err(UtfError::TooShort);
        }
        if header.tree_offset as usize >= bytes.len() {
            return Err(UtfError::TreeOutOfRange);
        }
        if header.string_offset as usize >= bytes.len() {
            return Err(UtfError::StringsOutOfRange);
        }
        Ok(Self {
            bytes,
            header,
            layout,
        })
    }

    /// The whole archive.
    #[must_use]
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }

    /// The shared archive bytes.
    #[must_use]
    pub fn shared_bytes(&self) -> &Arc<[u8]> {
        &self.bytes
    }

    /// The header.
    #[must_use]
    pub fn header(&self) -> &Header {
        &self.header
    }

    /// The record layout.
    #[must_use]
    pub fn layout(&self) -> Layout {
        self.layout
    }

    /// The root record.
    #[must_use]
    pub fn root(&self) -> NodeRef {
        NodeRef::ROOT
    }

    fn tree_len(&self) -> usize {
        self.bytes.len() - self.header.tree_offset as usize
    }

    fn field(&self, node: NodeRef, at: u32) -> Result<u32, UtfError> {
        let pos = self.header.tree_offset as usize + node.0 as usize + at as usize;
        dword(&self.bytes, pos).ok_or(UtfError::RecordOutOfRange { offset: node.0 })
    }

    /// The directory bit: flags bit 4 of a full record (the reference reads
    /// the single byte at +8), bit 31 of a compact one.
    ///
    /// # Errors
    ///
    /// The field lies outside the archive.
    pub fn is_dir(&self, node: NodeRef) -> Result<bool, UtfError> {
        match self.layout {
            Layout::Full => {
                let pos = self.header.tree_offset as usize + node.0 as usize + 8;
                let flags = self
                    .bytes
                    .get(pos)
                    .ok_or(UtfError::RecordOutOfRange { offset: node.0 })?;
                Ok(flags & 0x10 != 0)
            }
            Layout::Compact => Ok(self.field(node, 0)? >> 31 != 0),
        }
    }

    /// The size field (+0x1C full, +0x08 compact).
    ///
    /// # Errors
    ///
    /// The field lies outside the archive.
    pub fn size(&self, node: NodeRef) -> Result<u32, UtfError> {
        self.field(node, self.size_at())
    }

    fn size_at(&self) -> u32 {
        match self.layout {
            Layout::Full => 0x1C,
            Layout::Compact => 0x08,
        }
    }

    /// The first-child or data offset (+0x10 full, +0x04 compact).
    ///
    /// # Errors
    ///
    /// The field lies outside the archive.
    pub fn data_offset(&self, node: NodeRef) -> Result<u32, UtfError> {
        self.field(
            node,
            match self.layout {
                Layout::Full => 0x10,
                Layout::Compact => 0x04,
            },
        )
    }

    /// The find attributes: the raw flags of a full record, or 0x10 / 0 for
    /// a compact one.
    ///
    /// # Errors
    ///
    /// The field lies outside the archive.
    pub fn attributes(&self, node: NodeRef) -> Result<u32, UtfError> {
        match self.layout {
            Layout::Full => self.field(node, 0x08),
            Layout::Compact => Ok(if self.is_dir(node)? {
                ATTRIBUTE_DIRECTORY
            } else {
                0
            }),
        }
    }

    /// The packed creation, access and write times of a full record; `None`
    /// for a compact one, whose times are the archive file's.
    ///
    /// # Errors
    ///
    /// A field lies outside the archive.
    pub fn times(&self, node: NodeRef) -> Result<Option<[u32; 3]>, UtfError> {
        match self.layout {
            Layout::Full => Ok(Some([
                self.field(node, 0x20)?,
                self.field(node, 0x24)?,
                self.field(node, 0x28)?,
            ])),
            Layout::Compact => Ok(None),
        }
    }

    /// The name, read from the string table up to its NUL.
    ///
    /// # Errors
    ///
    /// The name offset field, or the name and its NUL, lie outside the
    /// archive.
    pub fn name(&self, node: NodeRef) -> Result<&[u8], UtfError> {
        let offset = match self.layout {
            Layout::Full => u64::from(self.field(node, 0x04)?),
            Layout::Compact => u64::from(self.field(node, 0)? & 0x7FFF_FFFF) + 4,
        };
        let start = u64::from(self.header.string_offset) + offset;
        let tail = usize::try_from(start)
            .ok()
            .and_then(|s| self.bytes.get(s..))
            .ok_or(UtfError::NameOutOfRange { offset: node.0 })?;
        let end = tail
            .iter()
            .position(|&b| b == 0)
            .ok_or(UtfError::NameOutOfRange { offset: node.0 })?;
        Ok(&tail[..end])
    }

    /// Every field of `node`.
    ///
    /// # Errors
    ///
    /// Any field lies outside the archive.
    pub fn record(&self, node: NodeRef) -> Result<Record<'_>, UtfError> {
        let sibling = match self.layout {
            Layout::Full => Some(self.field(node, 0)?),
            Layout::Compact => None,
        };
        Ok(Record {
            node,
            name: self.name(node)?,
            is_dir: self.is_dir(node)?,
            data_offset: self.data_offset(node)?,
            size: self.size(node)?,
            attributes: self.attributes(node)?,
            sibling,
            times: self.times(node)?,
        })
    }

    /// The children of `dir` in the order `FindChild` visits them: the
    /// sibling chain from +0x10 of a full record, or the +0x08 records from
    /// +0x04 of a compact one (the end offset wraps at 32 bits, as there).
    /// `dir` is not checked to be a directory.
    #[must_use]
    pub fn children(&self, dir: NodeRef) -> Children<'_> {
        Children {
            archive: self,
            state: ChildState::Start(dir),
            steps: 0,
        }
    }

    /// `FindChild(name, dir)`: the first child whose name equals `name`
    /// under the ASCII [`stricmp`].
    ///
    /// # Errors
    ///
    /// A record on the way lies outside the archive, or the chain loops.
    pub fn find_child(&self, dir: NodeRef, name: &[u8]) -> Result<Option<NodeRef>, UtfError> {
        for child in self.children(dir) {
            let child = child?;
            if stricmp(name, self.name(child)?).is_eq() {
                return Ok(Some(child));
            }
        }
        Ok(None)
    }

    /// `WalkPath(start, path)`: one leading `\` or `/` is skipped and an
    /// empty rest names `start`. Each component must be found in a
    /// directory; a component followed by a separator is cut to
    /// [`COMPONENT_LIMIT`] bytes. A trailing separator ends the walk at the
    /// record just found, file or not. `None` when a component is missing
    /// or a file is walked through.
    ///
    /// # Errors
    ///
    /// A record on the way lies outside the archive, or a chain loops.
    pub fn walk(&self, start: NodeRef, path: &[u8]) -> Result<Option<NodeRef>, UtfError> {
        let path = c_str(path);
        let mut cursor = usize::from(path.first().is_some_and(|&b| is_separator(b)));
        if cursor == path.len() {
            return Ok(Some(start));
        }
        let mut node = start;
        loop {
            if !self.is_dir(node)? {
                return Ok(None);
            }
            let rest = &path[cursor..];
            let Some(len) = rest.iter().position(|&b| is_separator(b)) else {
                return self.find_child(node, rest);
            };
            match self.find_child(node, &rest[..len.min(COMPONENT_LIMIT)])? {
                Some(found) => node = found,
                None => return Ok(None),
            }
            cursor += len + 1;
            if cursor == path.len() {
                return Ok(Some(node));
            }
        }
    }

    /// The records a `FindFirst` / `FindNext` loop of the walker yields.
    ///
    /// `pattern` is split at its last separator: a non-empty directory part
    /// (cut to [`COMPONENT_LIMIT`] bytes) is walked from `base`, the rest is
    /// the mask, cut to [`MASK_LIMIT`] bytes. `*` and `*.*` take every
    /// record; any other mask goes through [`match_wildcard`]. A compact
    /// directory yields at least its first record even when its count is 0,
    /// as the reference's cursor does; a first-child offset of 0 yields
    /// nothing. `Ok(None)` where the reference fails with error 0xA1: the
    /// directory part is missing or names a file.
    ///
    /// # Errors
    ///
    /// A record on the way lies outside the archive, or a chain loops.
    pub fn find(&self, base: NodeRef, pattern: &[u8]) -> Result<Option<Vec<NodeRef>>, UtfError> {
        let pattern = c_str(pattern);
        let last_separator = pattern.iter().rposition(|&b| is_separator(b));
        let mut node = base;
        let mask = match last_separator {
            Some(index) => {
                if index != 0 {
                    let directory = &pattern[..index.min(COMPONENT_LIMIT)];
                    match self.walk(node, directory)? {
                        Some(found) => node = found,
                        None => return Ok(None),
                    }
                }
                &pattern[index + 1..]
            }
            None => pattern,
        };
        if !self.is_dir(node)? {
            return Ok(None);
        }
        let mask = &mask[..mask.len().min(MASK_LIMIT)];
        let all = mask == b"*" || mask == b"*.*";

        let mut next = self.data_offset(node)?;
        let mut remaining = match self.layout {
            Layout::Full => 0,
            Layout::Compact => self.size(node)?,
        };
        let cap = self.tree_len() + 1;
        let mut steps = 0usize;
        let mut advance = |at: u32, remaining: &mut u32| -> Result<u32, UtfError> {
            steps += 1;
            if steps > cap {
                return Err(UtfError::Cycle);
            }
            match self.layout {
                Layout::Full => self.field(NodeRef(at), 0),
                Layout::Compact => Ok(if *remaining > 1 {
                    *remaining -= 1;
                    at.wrapping_add(0x0C)
                } else {
                    0
                }),
            }
        };

        let mut found = Vec::new();
        'records: while next != 0 {
            if !all && !match_wildcard(self.name(NodeRef(next))?, mask) {
                loop {
                    next = advance(next, &mut remaining)?;
                    if next == 0 {
                        break 'records;
                    }
                    if match_wildcard(self.name(NodeRef(next))?, mask) {
                        break;
                    }
                }
            }
            // FindNext copies the name of every record it returns.
            self.name(NodeRef(next))?;
            found.push(NodeRef(next));
            next = advance(next, &mut remaining)?;
        }
        Ok(Some(found))
    }

    /// Where a file's bytes start: the header's data offset plus the node's,
    /// wrapping at 32 bits.
    ///
    /// # Errors
    ///
    /// The field lies outside the archive.
    pub fn data_start(&self, node: NodeRef) -> Result<u32, UtfError> {
        Ok(self
            .header
            .data_offset
            .wrapping_add(self.data_offset(node)?))
    }

    /// The bytes one whole read of `node` returns: its size, cut at the end
    /// of the archive.
    ///
    /// # Errors
    ///
    /// The data starts past the end of the archive (the reference's read
    /// fails with error 0x1E), or a field lies outside it.
    pub fn data(&self, node: NodeRef) -> Result<&[u8], UtfError> {
        let start = self.data_start(node)? as usize;
        if start > self.bytes.len() {
            return Err(UtfError::DataOutOfRange);
        }
        let count = (self.size(node)? as usize).min(self.bytes.len() - start);
        Ok(&self.bytes[start..start + count])
    }
}

enum ChildState {
    Start(NodeRef),
    Full(u32),
    FullAfter(u32),
    Compact { offset: u32, end: u32 },
    Done,
}

/// Iterator over a directory's children; see [`UtfArchive::children`].
pub struct Children<'a> {
    archive: &'a UtfArchive,
    state: ChildState,
    steps: usize,
}

impl Iterator for Children<'_> {
    type Item = Result<NodeRef, UtfError>;

    fn next(&mut self) -> Option<Self::Item> {
        loop {
            match self.state {
                ChildState::Done => return None,
                ChildState::Start(dir) => {
                    let start = match self.archive.layout {
                        Layout::Full => self.archive.field(dir, 0x10).map(ChildState::Full),
                        Layout::Compact => self.archive.field(dir, 0x04).and_then(|offset| {
                            let count = self.archive.field(dir, 0x08)?;
                            Ok(ChildState::Compact {
                                offset,
                                end: offset.wrapping_add(count.wrapping_mul(0x0C)),
                            })
                        }),
                    };
                    match start {
                        Ok(state) => self.state = state,
                        Err(e) => {
                            self.state = ChildState::Done;
                            return Some(Err(e));
                        }
                    }
                }
                ChildState::Full(0) => {
                    self.state = ChildState::Done;
                }
                ChildState::Full(at) => {
                    self.steps += 1;
                    if self.steps > self.archive.tree_len() + 1 {
                        self.state = ChildState::Done;
                        return Some(Err(UtfError::Cycle));
                    }
                    // The sibling is read only when the caller asks for the
                    // next child, as FindChild reads it after the compare.
                    self.state = ChildState::FullAfter(at);
                    return Some(Ok(NodeRef(at)));
                }
                ChildState::FullAfter(at) => match self.archive.field(NodeRef(at), 0) {
                    Ok(next) => self.state = ChildState::Full(next),
                    Err(e) => {
                        self.state = ChildState::Done;
                        return Some(Err(e));
                    }
                },
                ChildState::Compact { offset, end } => {
                    if offset >= end {
                        self.state = ChildState::Done;
                        return None;
                    }
                    if self.archive.field(NodeRef(offset), 0).is_err() {
                        self.state = ChildState::Done;
                        return Some(Err(UtfError::RecordOutOfRange { offset }));
                    }
                    self.state = ChildState::Compact {
                        offset: offset.wrapping_add(0x0C),
                        end,
                    };
                    return Some(Ok(NodeRef(offset)));
                }
            }
        }
    }
}

#[cfg(test)]
pub(crate) mod tests;
