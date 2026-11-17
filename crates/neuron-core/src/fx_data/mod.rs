//! Particle effect data (work package P1.7): the node libraries and effect
//! libraries of `.ale` files.
//!
//! Ported from the loading side of `CppSource/particlefx/`
//! (`particlefx_nodelibrary.cpp`, `particlefx_effectlibrary.cpp`,
//! `particlefx_effectarchetype.cpp`, `particlefx_types.cpp`, the readers in
//! `particlefx_common.cpp`, the keyed ramps of `particlefx_ramp.cpp` and
//! the property sets of the node classes). Simulation and drawing are the
//! client's (work package P4.8).
//!
//! An `.ale` file is a UTF archive with two directories, each holding one
//! file of its own name:
//!
//! * `ParticleFxNodeLibrary`: the node prototypes ([`NodeLibrary`]): emitters,
//!   fields, appearances and folders, each with its typed properties
//!   ([`PropertySet`]): scalars, names, flags, nested ramps and animated
//!   transforms.
//! * `ALEffectLib`: the effects ([`EffectLibrary`]): per effect a tree of
//!   node references, by library name hash, and the emitter/appearance and
//!   appearance/field pairs.
//!
//! The client loads the node library first and then the effect library,
//! through one file system ([`AleFile::parse`] does the same). The loaders
//! enter and leave the directories with `open_node`, so a node library that
//! fails before leaving its directory also makes the effect library fail.
//!
//! All values are stored as read (float bit patterns included); the only
//! arithmetic is the colour packing of [`ramp::pack_rgb`].

mod chunk;
mod effect_library;
mod node_library;
pub mod props;
pub mod ramp;

#[cfg(test)]
mod tests;

use std::sync::Arc;

pub use effect_library::{EffectArchetype, EffectLibrary, EffectNodeRef, EffectPair, ROOT_PARENT};
pub use node_library::{FxNode, IgnoredProperty, NodeLibrary, remap_hash, remap_kind};
pub use props::{
    Billboard, BillboardSlot, BillboardTables, ClassData, Emitter, EmitterShape, Field,
    MeshAppearance, NestedSingle, NodeClass, ParticleAppearance, PropertyError, PropertyRow,
    PropertySet, billboard_tables,
};
pub use ramp::{
    AnimatedKey, AnimatedSingle, AnimatedSingleRamp, AnimatedTrack, AnimatedTransform,
    ColorRampRamp, NestedRamp, RampColor, RampKey, RampSingle, RampSingleRamp,
};

use crate::vfs::{UtfFileSystem, VfsError};

/// The effect library's directory and file.
pub const ALE_NODE_NAME: &[u8] = b"ALEffectLib";
/// The node library's directory and file.
///
/// The `.ale` files named it `AlchemyNodeLibrary` until decision O17
/// renamed it in the data to the name the reference reads.
pub const NODE_LIBRARY_NAME: &[u8] = b"ParticleFxNodeLibrary";
/// The `open_node` argument that leaves a directory.
pub const LEAVE: &[u8] = b"..";
/// Version 1.0 as float bits: nested ramps hold one key and no mode, and
/// the effects have no sixteen header bytes.
pub const VERSION_1_0: u32 = 0x3F80_0000;
/// Version 1.1 as float bits, the version the editor writes.
pub const VERSION_1_1: u32 = 0x3F8C_CCCD;
/// Longest string a reader accepts (`kAleNameMax`), terminator included.
pub const NAME_MAX: usize = 0x100;

/// The node and effect name hash (`HashNameCrc`): the engine CRC without
/// case folding, over the bytes up to the first NUL.
#[must_use]
pub fn name_hash(name: &[u8]) -> u32 {
    crate::hash::exact_name_crc32(name)
}

/// Why a library load failed. Nodes and effects read before the failure
/// stay in the library.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum FxError {
    /// The directory could not be entered.
    NoDirectory,
    /// The version or the count could not be read; the directory is not
    /// left.
    ShortHeader,
    /// An effect's sixteen header bytes could not be read; the directory is
    /// not left.
    ShortEffectHeader,
    /// A node's class name could not be read.
    ShortName,
    /// A class name that names no node class (the reference cannot create
    /// it, or it is not a node).
    UnknownClass(Vec<u8>),
}

impl FxError {
    /// The reference's result code: -6 (`GR_ALE_LOAD_FAIL`) for the header
    /// failures, -1 for a short class name, -3 for an unknown class.
    #[must_use]
    pub const fn code(&self) -> i32 {
        match self {
            Self::NoDirectory | Self::ShortHeader | Self::ShortEffectHeader => -6,
            Self::ShortName => -1,
            Self::UnknownClass(_) => -3,
        }
    }
}

impl core::fmt::Display for FxError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::NoDirectory => f.write_str("effect data directory missing"),
            Self::ShortHeader => f.write_str("effect data header truncated"),
            Self::ShortEffectHeader => f.write_str("effect header truncated"),
            Self::ShortName => f.write_str("node class name truncated"),
            Self::UnknownClass(name) => {
                write!(f, "unknown node class {}", String::from_utf8_lossy(name))
            }
        }
    }
}

impl std::error::Error for FxError {}

/// Both libraries of one `.ale` file, loaded as the client loads them.
#[derive(Clone, Debug, PartialEq)]
pub struct AleFile {
    /// The node library.
    pub nodes: NodeLibrary,
    /// The node library load's result.
    pub nodes_result: Result<(), FxError>,
    /// The effect library.
    pub effects: EffectLibrary,
    /// The effect library load's result.
    pub effects_result: Result<(), FxError>,
}

impl AleFile {
    /// Opens `bytes` as a UTF archive and loads the node library, then the
    /// effect library, into fresh libraries.
    ///
    /// # Errors
    ///
    /// The bytes are not a UTF archive.
    pub fn parse(bytes: impl Into<Arc<[u8]>>) -> Result<Self, VfsError> {
        let mut fs = UtfFileSystem::from_bytes(bytes, b"")?;
        let mut nodes = NodeLibrary::new();
        let nodes_result = nodes.load(&mut fs);
        let mut effects = EffectLibrary::new();
        let effects_result = effects.load(&mut fs);
        Ok(Self {
            nodes,
            nodes_result,
            effects,
            effects_result,
        })
    }
}
