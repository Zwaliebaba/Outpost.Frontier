//! The node library (`FNodePrototypeRegistry::Load` in
//! `particlefx_nodelibrary.cpp`, `ReadNodeLibProperties` in
//! `particlefx_common.cpp`).
//!
//! Stream (the file `ParticleFxNodeLibrary` in the UTF directory of the same
//! name): the version as float bits, a 32-bit node count, then per node
//! its class name (a [`Chunk::string`]) and its property records up to a
//! zero type word. A record is a 16-bit type, a 32-bit hash and the kind's
//! value. What a record consumes depends on whether the class owns the
//! hash: a scalar, string or pair is always read; a transform and a colour
//! ramp only when owned; a nested scalar or animated ramp is read into a
//! throw-away ramp when not owned; the flat ramp kinds 0x100..0x102 (owned
//! by no class) and unknown kinds read nothing. Records are applied in
//! order, so a later record of the same property wins.

use std::collections::BTreeMap;

use super::chunk::Chunk;
use super::props::{NodeClass, PropertySet, kind};
use super::ramp::{AnimatedSingleRamp, RampSingleRamp};
use super::{FxError, LEAVE, NAME_MAX, NODE_LIBRARY_NAME, VERSION_1_0, name_hash};
use crate::vfs::FileSystem;

/// A property record the node's set did not take (port-only diagnostics;
/// the reference drops them silently or with a log line).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IgnoredProperty {
    /// The hash after remapping.
    pub hash: u32,
    /// The type word after remapping (bit 15 is a bool's value).
    pub kind: u32,
}

/// One node of the library: a prototype the effects instantiate by the
/// hash of its instance name.
#[derive(Clone, Debug, PartialEq)]
pub struct FxNode {
    /// [`name_hash`] of the instance name, the library key.
    pub hash: u32,
    /// The node's properties; `props.class` is its class.
    pub props: PropertySet,
    /// Records the set did not take.
    pub ignored: Vec<IgnoredProperty>,
}

/// A node library: nodes in insertion order, keyed by name hash. Loading
/// several files into one library adds to it; a node whose name hash is
/// already present is dropped.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct NodeLibrary {
    nodes: Vec<FxNode>,
    index: BTreeMap<u32, usize>,
}

/// The two hash renames the reader applies to every record
/// (.data 0x062563a8): two old names of the LOD curves.
#[must_use]
pub const fn remap_hash(hash: u32) -> u32 {
    match hash {
        0xEE14_29BA => super::props::EMITTER_LOD_CURVE,
        0xF038_2D6B => super::props::APPEARANCE_LOD_CURVE,
        other => other,
    }
}

/// The type remap of a 1.0 stream: the flat ramp kinds 0x100..0x102
/// become the nested 0x200..0x202 (exact type words only).
#[must_use]
pub const fn remap_kind(kind: u32, version: u32) -> u32 {
    if version != VERSION_1_0 {
        return kind;
    }
    match kind {
        0x100 => 0x200,
        0x101 => 0x201,
        0x102 => 0x202,
        other => other,
    }
}

/// Reads one record's value into `set` (`ReadNodeLibProperty`). Returns
/// whether the set took it.
fn read_property(
    chunk: &mut Chunk,
    set: &mut PropertySet,
    hash: u32,
    ty: u32,
    version: u32,
) -> bool {
    match ty & 0x7FFF {
        kind::BOOL => set.set_bool(hash, u8::from(ty & 0x8000 != 0)).is_ok(),
        kind::INT => chunk.u32().is_ok_and(|v| set.set_int(hash, v).is_ok()),
        kind::FLOAT => chunk
            .u32()
            .is_ok_and(|v| set.set_float(hash, f32::from_bits(v)).is_ok()),
        kind::NAME => chunk
            .string(NAME_MAX)
            .is_ok_and(|text| set.set_name(hash, &text).is_ok()),
        kind::PAIR => chunk.bytes::<8>().is_ok_and(|b| {
            let first = u32::from_le_bytes([b[0], b[1], b[2], b[3]]);
            let second = u32::from_le_bytes([b[4], b[5], b[6], b[7]]);
            set.set_pair(hash, [first, second]).is_ok()
        }),
        kind::TRANSFORM => match set.transform_mut(hash) {
            Some(transform) => {
                let _ = transform.read(chunk, version);
                true
            }
            None => false,
        },
        kind::SINGLE_RAMP_RAMP => match set.single_ramp_ramp_mut(hash) {
            Some(ramp) => {
                ramp.read(chunk, version);
                true
            }
            None => {
                // A fresh "FxRampSingleRamp" from the factory takes the
                // value and is released.
                RampSingleRamp::new().read(chunk, version);
                false
            }
        },
        kind::COLOR_RAMP_RAMP => match set.color_ramp_ramp_mut(hash) {
            Some(ramp) => {
                ramp.read(chunk, version);
                true
            }
            None => false,
        },
        kind::ANIMATED_SINGLE_RAMP => match set.animated_single_ramp_mut(hash) {
            Some(ramp) => {
                let _ = ramp.read(chunk, version);
                true
            }
            None => {
                let _ = AnimatedSingleRamp::new().read(chunk, version);
                false
            }
        },
        // The flat ramp kinds (no class owns one, so nothing is read) and
        // unknown kinds ("Unknown type of property").
        _ => false,
    }
}

/// The property records of one node, up to a zero type word or a short
/// read (`ReadNodeLibProperties`).
fn read_properties(chunk: &mut Chunk, set: &mut PropertySet, version: u32) -> Vec<IgnoredProperty> {
    let mut ignored = Vec::new();
    while let Ok(ty) = chunk.u16() {
        if ty == 0 {
            break;
        }
        let Ok(hash) = chunk.u32() else { break };
        let hash = remap_hash(hash);
        let ty = remap_kind(u32::from(ty), version);
        if !read_property(chunk, set, hash, ty, version) {
            ignored.push(IgnoredProperty { hash, kind: ty });
        }
    }
    ignored
}

/// `FixColideFieldAlias`: the misspelt class name some data uses.
fn fix_class_name(name: Vec<u8>) -> Vec<u8> {
    if name == b"FxColideField" {
        b"FxCollideField".to_vec()
    } else {
        name
    }
}

impl NodeLibrary {
    /// An empty library.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The nodes in insertion order.
    #[must_use]
    pub fn nodes(&self) -> &[FxNode] {
        &self.nodes
    }

    /// The node whose instance name has `hash`.
    #[must_use]
    pub fn find(&self, hash: u32) -> Option<&FxNode> {
        self.index.get(&hash).map(|&i| &self.nodes[i])
    }

    /// `Insert`: keyed by the name's hash; a present hash keeps the old node
    /// (the reference logs "Different name generates same Hash" when the
    /// names differ) and still succeeds.
    fn insert(&mut self, name: Vec<u8>, mut props: PropertySet, ignored: Vec<IgnoredProperty>) {
        let hash = name_hash(&name);
        if self.index.contains_key(&hash) {
            return;
        }
        props.name = Some(name);
        self.index.insert(hash, self.nodes.len());
        self.nodes.push(FxNode {
            hash,
            props,
            ignored,
        });
    }

    /// `LoadNodeLibrary`: enters the directory `ParticleFxNodeLibrary` of `fs`
    /// and reads the leaf of the same name, then leaves the directory.
    ///
    /// Nodes are added as they are read, so a failure keeps the nodes
    /// before it. A node whose instance name is empty is named `unnamed`,
    /// or `unnamed#N` with the first `N` from 1 whose hash is free.
    ///
    /// # Errors
    ///
    /// [`FxError::NoDirectory`] and [`FxError::ShortHeader`] (the directory
    /// is then not left), [`FxError::ShortName`] and
    /// [`FxError::UnknownClass`], which end the load.
    pub fn load(&mut self, fs: &mut dyn FileSystem) -> Result<(), FxError> {
        if fs.open_node(NODE_LIBRARY_NAME).is_err() {
            return Err(FxError::NoDirectory);
        }
        let mut chunk = Chunk::open(fs, NODE_LIBRARY_NAME);
        let header = chunk.u32().and_then(|version| Ok((version, chunk.i32()?)));
        let Ok((version, count)) = header else {
            return Err(FxError::ShortHeader);
        };
        let mut result = Ok(());
        for _ in 0..count.max(0) {
            let Ok(class_name) = chunk.string(NAME_MAX) else {
                result = Err(FxError::ShortName);
                break;
            };
            let class_name = fix_class_name(class_name);
            let Some(class) = NodeClass::from_factory_name(&class_name) else {
                result = Err(FxError::UnknownClass(class_name));
                break;
            };
            let mut props = PropertySet::new(class, &class_name);
            let ignored = read_properties(&mut chunk, &mut props, version);
            let name = match &props.name {
                Some(name) if !name.is_empty() => name.clone(),
                _ => self.unnamed(),
            };
            self.insert(name, props, ignored);
            result = Ok(());
        }
        let _ = fs.open_node(LEAVE);
        result
    }

    /// The name an unnamed node gets: `unnamed`, else `unnamed#1`,
    /// `unnamed#2` and so on, the first whose hash is not in the library.
    fn unnamed(&self) -> Vec<u8> {
        let mut name = b"unnamed".to_vec();
        let mut suffix = 0_u32;
        while self.index.contains_key(&name_hash(&name)) {
            suffix = suffix.wrapping_add(1);
            name = format!("unnamed#{}", suffix.cast_signed()).into_bytes();
        }
        name
    }
}
