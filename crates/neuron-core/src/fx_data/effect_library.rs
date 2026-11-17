//! The effect library (`FFxEffectLibrary::LoadAle` in
//! `particlefx_effectlibrary.cpp`, `FFxEffectArchetype` in
//! `particlefx_effectarchetype.cpp`).
//!
//! Stream (the file `ALEffectLib` in the UTF directory of the same name):
//! the version as float bits, a 32-bit effect count, then per effect its
//! name (a [`Chunk::string`]), sixteen bytes nobody interprets (only when
//! the version compares at least 1.1 as a float), a 32-bit count of node
//! references of sixteen bytes each and a 32-bit count of node pairs of
//! eight bytes each.

use std::collections::BTreeMap;

use super::chunk::Chunk;
use super::{ALE_NODE_NAME, FxError, LEAVE, NAME_MAX, VERSION_1_1, name_hash};
use crate::vfs::FileSystem;

/// [`EffectNodeRef::parent_id`] of a node at the effect's root.
pub const ROOT_PARENT: u32 = 0x8000;

/// One node of an effect (the archetype's 16-byte record, in file order).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EffectNodeRef {
    /// 0: the library node with [`node_hash`](Self::node_hash); otherwise a
    /// fresh `FxNode` folder.
    pub kind: u32,
    /// The name hash of the library node.
    pub node_hash: u32,
    /// The [`id`](Self::id) of the parent node, or [`ROOT_PARENT`].
    pub parent_id: u32,
    /// The node's id within the effect; 0 in the stream asks for the next
    /// free one.
    pub id: u32,
}

/// Two node ids of an effect: an emitter and the appearance it feeds, or
/// an appearance and a field acting on it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EffectPair {
    /// The emitter or appearance.
    pub first: u32,
    /// The appearance or field.
    pub second: u32,
}

/// An effect: its node tree and pairs.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct EffectArchetype {
    /// The effect name.
    pub name: Vec<u8>,
    /// The sixteen bytes after the name (zero in 1.0 streams) as four
    /// little-endian words; stored and handed back, never interpreted.
    pub unknown: [u32; 4],
    /// The nodes in stream order.
    pub nodes: Vec<EffectNodeRef>,
    /// The pairs in stream order.
    pub pairs: Vec<EffectPair>,
    next_id: u32,
}

impl EffectArchetype {
    /// An empty effect named `name`.
    #[must_use]
    pub fn new(name: Vec<u8>) -> Self {
        Self {
            name,
            ..Self::default()
        }
    }

    /// `AddRec16(rec, out, rec.id)`. With an id of 0 the next id is
    /// assigned; a record with a non-zero id equal in all four words to one
    /// already present is refused. The id counter follows the largest id
    /// seen (a signed comparison). Returns the record's id.
    pub fn add_node(&mut self, node: EffectNodeRef) -> Option<u32> {
        if node.id != 0 && self.nodes.contains(&node) {
            return None;
        }
        let id = if node.id == 0 {
            self.next_id = self.next_id.wrapping_add(1);
            self.next_id
        } else {
            node.id
        };
        self.nodes.push(EffectNodeRef { id, ..node });
        if self.next_id.cast_signed() <= id.cast_signed() {
            self.next_id = id;
        }
        Some(id)
    }

    /// `AddRec8`: a pair already present is refused.
    pub fn add_pair(&mut self, pair: EffectPair) -> bool {
        if self.pairs.contains(&pair) {
            return false;
        }
        self.pairs.push(pair);
        true
    }
}

/// An effect library: effects in insertion order, keyed by name hash.
/// Loading several files into one library adds to it; an effect whose
/// name hash is already present is dropped.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct EffectLibrary {
    effects: Vec<EffectArchetype>,
    index: BTreeMap<u32, usize>,
}

fn words<const N: usize>(bytes: &[u8]) -> [u32; N] {
    core::array::from_fn(|i| {
        u32::from_le_bytes([
            bytes[4 * i],
            bytes[4 * i + 1],
            bytes[4 * i + 2],
            bytes[4 * i + 3],
        ])
    })
}

impl EffectLibrary {
    /// An empty library.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The effects in insertion order.
    #[must_use]
    pub fn effects(&self) -> &[EffectArchetype] {
        &self.effects
    }

    /// The effect whose name has `hash` ([`name_hash`]; the game's effect
    /// CRCs).
    #[must_use]
    pub fn find(&self, hash: u32) -> Option<&EffectArchetype> {
        self.index.get(&hash).map(|&i| &self.effects[i])
    }

    /// `InsertArchetype`: refused when the name hash is present (the
    /// reference logs "Different name generates same Hash" when the names
    /// differ).
    pub fn insert(&mut self, effect: EffectArchetype) -> bool {
        let hash = name_hash(&effect.name);
        if self.index.contains_key(&hash) {
            return false;
        }
        self.index.insert(hash, self.effects.len());
        self.effects.push(effect);
        true
    }

    /// `LoadAle`: enters the directory `ALEffectLib` of `fs` and reads the
    /// leaf of the same name, then leaves the directory.
    ///
    /// An effect whose name cannot be read is skipped and reading goes on
    /// from where the read stopped. A short record ends that record list;
    /// a short record count ends the load (the effect being read is
    /// dropped) with success.
    ///
    /// # Errors
    ///
    /// [`FxError::NoDirectory`], [`FxError::ShortHeader`] and
    /// [`FxError::ShortEffectHeader`]; after the last two the directory is
    /// not left.
    pub fn load(&mut self, fs: &mut dyn FileSystem) -> Result<(), FxError> {
        if fs.open_node(ALE_NODE_NAME).is_err() {
            return Err(FxError::NoDirectory);
        }
        let mut chunk = Chunk::open(fs, ALE_NODE_NAME);
        let header = chunk.u32().and_then(|version| Ok((version, chunk.i32()?)));
        let Ok((version, count)) = header else {
            return Err(FxError::ShortHeader);
        };
        // An x87 comparison of the two floats: NaN is not at least 1.1.
        let has_header = f32::from_bits(version) >= f32::from_bits(VERSION_1_1);
        'effects: for _ in 0..count.max(0) {
            let Ok(name) = chunk.string(NAME_MAX) else {
                // The reference goes on with the next effect; at the end of
                // the stream every remaining one fails the same way.
                if chunk.exhausted() {
                    break;
                }
                continue;
            };
            let mut effect = EffectArchetype::new(name);
            if has_header {
                let read = chunk
                    .bytes::<12>()
                    .and_then(|first| Ok((first, chunk.bytes::<4>()?)));
                let Ok((first, last)) = read else {
                    return Err(FxError::ShortEffectHeader);
                };
                let mut header = [0; 16];
                header[..12].copy_from_slice(&first);
                header[12..].copy_from_slice(&last);
                effect.unknown = words(&header);
            }
            let Ok(nodes) = chunk.i32() else { break };
            for _ in 0..nodes.max(0) {
                let Ok(record) = chunk.bytes::<16>() else {
                    break;
                };
                let [kind, node_hash, parent_id, id] = words(&record);
                effect.add_node(EffectNodeRef {
                    kind,
                    node_hash,
                    parent_id,
                    id,
                });
            }
            let Ok(pairs) = chunk.i32() else {
                break 'effects;
            };
            for _ in 0..pairs.max(0) {
                let Ok(record) = chunk.bytes::<8>() else {
                    break;
                };
                let [first, second] = words(&record);
                effect.add_pair(EffectPair { first, second });
            }
            self.insert(effect);
        }
        let _ = fs.open_node(LEAVE);
        Ok(())
    }
}
