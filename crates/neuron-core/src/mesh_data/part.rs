//! The `VMeshPart` and `VMeshWire` nodes of an archetype
//! (`FVMeshPlugin::Claim` and `FVMeshPart::Claim` in `VMeshPart.cpp`,
//! `FVMeshWire::CreateArchetype` in `VMeshWire.cpp`).

use super::vmesh::{VMeshError, VMeshRef, VWireData};
use super::{Malformed, Whole, open_node, read_whole};
use crate::vfs::FileSystem;

/// Why a `VMeshPart` node was not claimed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PartError {
    /// There is no `VMeshPart` node: the plugin returns -1.
    NoNode,
    /// The `VMeshRef` leaf is missing or its read fails. The reference
    /// keeps a null record and then reads through it (divergence
    /// register); the port does not claim the node.
    NoRef,
    /// The leaf is shorter than a record (divergence register).
    Ref(VMeshError),
}

impl core::fmt::Display for PartError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::NoNode => f.write_str("no VMeshPart node"),
            Self::NoRef => f.write_str("no readable VMeshRef"),
            Self::Ref(e) => write!(f, "VMeshRef: {e}"),
        }
    }
}

impl std::error::Error for PartError {}

/// A claimed `VMeshPart`: the range of a library mesh one level draws.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct VMeshPart {
    /// The first record of the `VMeshRef` leaf.
    pub mesh_ref: VMeshRef,
    /// Bytes of the leaf; the reference reads all of them and uses the
    /// first 0x3C.
    pub ref_size: u32,
}

/// `FVMeshPlugin::Claim`: `OpenNode("VMeshPart")`, the `VMeshRef` leaf read
/// whole, then `OpenNode("..")`. The material hook-up that follows needs the
/// material library and is left to the client.
///
/// # Errors
///
/// [`PartError`] inside `Ok` when the node is not claimed; `Err` for a
/// malformed archive.
pub fn claim(fs: &mut dyn FileSystem) -> Result<Result<VMeshPart, PartError>, Malformed> {
    if !open_node(fs, b"VMeshPart")? {
        return Ok(Err(PartError::NoNode));
    }
    let part = match read_whole(fs, b"VMeshRef")? {
        Whole::Read(bytes) => VMeshRef::parse(&bytes)
            .map(|mesh_ref| VMeshPart {
                mesh_ref,
                ref_size: u32::try_from(bytes.len()).unwrap_or(u32::MAX),
            })
            .map_err(PartError::Ref),
        Whole::Missing | Whole::Failed => Err(PartError::NoRef),
    };
    let _ = open_node(fs, b"..")?;
    Ok(part)
}

/// Why a `VMeshWire` node gave no wireframe.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WireError {
    /// "couldnt load vmeshwire persistant wire_data; bad version?": the
    /// `VWireData` leaf is missing or its read failed. The reference
    /// returns without `OpenNode("..")`, so the current node stays
    /// `VMeshWire`; so does the port's.
    ReadFailed,
    /// The leaf is shorter than its header and indices (divergence
    /// register). The current node is back where it was.
    Parse(VMeshError),
}

impl core::fmt::Display for WireError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::ReadFailed => f.write_str("no readable VWireData"),
            Self::Parse(e) => write!(f, "VWireData: {e}"),
        }
    }
}

impl std::error::Error for WireError {}

/// `FVMeshWire::CreateArchetype`: `OpenNode("VMeshWire")` and the
/// `VWireData` leaf. `Ok(None)` when there is no node.
///
/// # Errors
///
/// [`WireError`] inside `Ok(Some)`; `Err` for a malformed archive.
pub fn load_wire(
    fs: &mut dyn FileSystem,
) -> Result<Option<Result<VWireData, WireError>>, Malformed> {
    if !open_node(fs, b"VMeshWire")? {
        return Ok(None);
    }
    let Whole::Read(bytes) = read_whole(fs, b"VWireData")? else {
        return Ok(Some(Err(WireError::ReadFailed)));
    };
    let wire = VWireData::parse(&bytes).map_err(WireError::Parse);
    let _ = open_node(fs, b"..")?;
    Ok(Some(wire))
}
