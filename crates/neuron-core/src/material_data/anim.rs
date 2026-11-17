//! `MaterialAnim` nodes: per animation a key count, flags, `MADeltas`
//! (20-byte records: duration and four rates) and `MAKeys` (16-byte
//! records: four start values), as `ParseMaKeys` and `LoadAnimationGroup`
//! (`MaterialAnimation.cpp`) read them. Evaluating them into a texture
//! matrix is work package P4.5.

use crate::hash::name_crc32;
use crate::vfs::{FileSystem, FsDesc};

/// One `MADeltas` record (`FMaDelta`), floats as bit patterns.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MaDelta {
    /// Duration of the key.
    pub duration: u32,
    /// Rates of the four animated values.
    pub rates: [u32; 4],
}

/// One `MAKeys` record (`FMaKey`), floats as bit patterns.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MaKey {
    /// Start values of the four animated values.
    pub values: [u32; 4],
}

/// One animation (`FImaData`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MaterialAnimation {
    /// The child's name.
    pub name: Vec<u8>,
    /// Its name CRC, the key materials look it up by.
    pub crc: u32,
    /// `MACount`: exactly four bytes, else 0.
    pub count: u32,
    /// `MAFlags`: exactly four bytes, else 0. Not read when the count is 0.
    pub flags: u32,
    /// The bytes read from `MADeltas`: `count * 20` (wrapping at 32 bits)
    /// asked, at most the leaf's size read. `None` without the leaf or for
    /// a count of 0. The reference leaves the rest of its buffer
    /// uninitialised.
    pub deltas: Option<Vec<u8>>,
    /// The bytes asked from `MADeltas`.
    pub deltas_asked: u32,
    /// The bytes read from `MAKeys`: `(count - 1) * 16` asked. `None`
    /// without the leaf or for a count below 2.
    pub keys: Option<Vec<u8>>,
    /// The bytes asked from `MAKeys`.
    pub keys_asked: u32,
}

fn word(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

impl MaterialAnimation {
    /// The complete delta records read.
    #[must_use]
    pub fn delta_records(&self) -> Vec<MaDelta> {
        self.deltas
            .as_deref()
            .unwrap_or_default()
            .as_chunks::<20>()
            .0
            .iter()
            .map(|r| MaDelta {
                duration: word(r, 0),
                rates: [word(r, 4), word(r, 8), word(r, 12), word(r, 16)],
            })
            .collect()
    }

    /// The complete key records read.
    #[must_use]
    pub fn key_records(&self) -> Vec<MaKey> {
        self.keys
            .as_deref()
            .unwrap_or_default()
            .as_chunks::<16>()
            .0
            .iter()
            .map(|r| MaKey {
                values: [word(r, 0), word(r, 4), word(r, 8), word(r, 12)],
            })
            .collect()
    }
}

fn read_word(fs: &dyn FileSystem, name: &[u8]) -> Option<u32> {
    let mut file = fs.open_file(&FsDesc::read(name)).ok()?;
    let mut buf = [0u8; 4];
    match file.read(&mut buf) {
        Ok(4) => Some(u32::from_le_bytes(buf)),
        _ => None,
    }
}

/// A read of `asked` bytes from the start of a leaf: what one read
/// returns, `Some(empty)` when it fails.
fn read_array(fs: &dyn FileSystem, name: &[u8], asked: u32) -> Option<Vec<u8>> {
    let mut file = fs.open_file(&FsDesc::read(name)).ok()?;
    let size = file.size().unwrap_or(0);
    let mut buf = vec![0u8; asked.min(size) as usize];
    let read = file.read(&mut buf).unwrap_or(0);
    buf.truncate(read);
    Some(buf)
}

/// `ParseMaKeys` on an animation's node.
#[must_use]
pub fn parse_animation(fs: &dyn FileSystem, name: &[u8]) -> MaterialAnimation {
    let mut anim = MaterialAnimation {
        name: name.to_vec(),
        crc: name_crc32(name),
        count: 0,
        flags: 0,
        deltas: None,
        deltas_asked: 0,
        keys: None,
        keys_asked: 0,
    };
    if let Some(count) = read_word(fs, b"MACount") {
        anim.count = count;
    }
    if anim.count == 0 {
        return anim;
    }
    if let Some(flags) = read_word(fs, b"MAFlags") {
        anim.flags = flags;
    }
    anim.deltas_asked = anim.count.wrapping_mul(0x14);
    anim.deltas = read_array(fs, b"MADeltas", anim.deltas_asked);
    if anim.count <= 1 {
        return anim;
    }
    anim.keys_asked = (anim.count - 1) << 4;
    anim.keys = read_array(fs, b"MAKeys", anim.keys_asked);
    anim
}

/// `LoadAnimationGroup`: every directory child of `MaterialAnim` whose
/// name does not start with `.` and that opens as a child file system,
/// parsed from its own node, in find order (the group the reference
/// returns). The library's lookup map keeps the last of several children
/// with the same name CRC; the group keeps them all. `None` when
/// `MaterialAnim` cannot be opened.
pub fn load_material_animations(fs: &mut dyn FileSystem) -> Option<Vec<MaterialAnimation>> {
    fs.open_node(b"MaterialAnim").ok()?;
    let found = fs.find(b"*.*").unwrap_or_default();
    let mut anims = Vec::new();
    for entry in found {
        let name = &entry.data.name;
        if !entry.data.is_dir() || name.first() == Some(&b'.') {
            continue;
        }
        let Ok(child) = fs.create_child(&FsDesc::read(name).with_found(entry.token)) else {
            continue;
        };
        anims.push(parse_animation(&*child, name));
    }
    let _ = fs.open_node(b"..");
    Some(anims)
}
