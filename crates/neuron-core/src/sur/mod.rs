//! `.sur` collision files (work package P1.7): the per-part collision
//! surfaces an archetype's `.cmp` or `.3db` mesh is paired with.
//!
//! Ported from `PhyArch::LoadSurfaces` (`CppSource/Common/common_phyarch.cpp`),
//! which fills `Archetype::Root::PhyParts`. The surfaces themselves are IVP
//! compact surfaces, decoded by [`compact`]; the loader keeps them as the
//! bytes it read, as the reference does.
//!
//! Layout, little-endian. A file starts with the tag `"vers"` and the `f32`
//! version 2.0; anything else is rejected (files without the tag are an
//! older layout the reference refuses as "old format"). Parts follow until
//! the end of the file: a `u32` part id, an `i32` chunk count, and that many
//! chunks, each a 4-byte tag and its payload:
//!
//! * `surf`: a `u32` byte count and a compact surface of that many bytes.
//! * `exts`: the minimum and maximum corners, two times three `f32`.
//! * `!fxd`: no payload; marks the part as fixed.
//! * `hpid`: an `i32` count and that many `u32` hardpoint ids.
//!
//! Tags compare with the ASCII [`stricmp`](crate::hash::stricmp) as C
//! strings. An unknown tag ends the load with an error; the parts read
//! before it stay loaded. A part id that repeats is reported and the
//! earlier part is reset, then the chunks of the new one apply to it.
//! Fewer than four bytes where a part id would start end the file
//! successfully.
//!
//! Where the reference reads past the end of the file in the middle of a
//! part, it carries on with values left from an earlier read or never
//! initialised; the port stops with [`SurError::Truncated`] instead
//! (divergence register).

pub mod compact;

use std::collections::{BTreeMap, HashSet};

use crate::hash::eq_ignore_case;
use crate::math::Vector;

pub use compact::{
    CompactEdge, CompactError, CompactLedge, CompactSurface, CompactTriangle, EdgeRef,
    LedgeTreeNode, PolyPoint,
};

/// The only version the loader accepts.
pub const VERSION: f32 = 2.0;

/// Why a `.sur` file does not load.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SurError {
    /// Fewer than four bytes: the reference fails silently.
    NoTag,
    /// The file does not start with `"vers"`: the older layout. The
    /// reference reports it as a file to rebuild.
    OldFormat,
    /// `"vers"` is not followed by four bytes: the reference fails silently.
    TruncatedVersion,
    /// The version is not 2.0 (its bits). The reference reports it.
    BadVersion(u32),
    /// A chunk tag the loader does not know. The reference reports it.
    UnknownChunk {
        /// The part being read.
        part_id: u32,
        /// The tag as read.
        tag: [u8; 4],
    },
    /// The file ends inside a part. The reference reads on with stale values.
    Truncated {
        /// Where the read that ran short starts.
        offset: usize,
    },
}

impl SurError {
    /// Whether the reference reports this failure through its diagnostic
    /// channel (`FDUMP`); the other failures are silent.
    #[must_use]
    pub fn is_reported(&self) -> bool {
        matches!(
            self,
            Self::OldFormat | Self::BadVersion(_) | Self::UnknownChunk { .. }
        )
    }
}

impl core::fmt::Display for SurError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::NoTag => f.write_str(".sur file shorter than its tag"),
            Self::OldFormat => f.write_str(".sur file uses the old format"),
            Self::TruncatedVersion => f.write_str(".sur file ends inside its version"),
            Self::BadVersion(bits) => write!(
                f,
                ".sur file version {} is not {VERSION}",
                f32::from_bits(*bits)
            ),
            Self::UnknownChunk { part_id, tag } => write!(
                f,
                ".sur part {part_id} has an unknown chunk {:?}",
                String::from_utf8_lossy(tag)
            ),
            Self::Truncated { offset } => write!(f, ".sur file ends inside a part at {offset:#x}"),
        }
    }
}

impl std::error::Error for SurError {}

/// The two corners of a part's `exts` chunk.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Extents {
    /// The minimum corner.
    pub min: Vector,
    /// The maximum corner.
    pub max: Vector,
}

/// One part of a `.sur` file (`PhyArch::Part`).
#[derive(Clone, Debug, PartialEq)]
pub struct SurPart {
    /// `bNotFixed`: true unless a `!fxd` chunk was read.
    pub not_fixed: bool,
    /// The bytes of the last `surf` chunk: an IVP compact surface.
    pub surface: Option<Vec<u8>>,
    /// The last `exts` chunk. `None` when the part has none: the reference
    /// leaves the corners unwritten (indeterminate). A part reset by a
    /// repeated id has zero corners.
    pub extents: Option<Extents>,
    /// The `hpid` ids in first-seen order, each once.
    pub hardpoint_ids: Vec<u32>,
}

impl Default for SurPart {
    fn default() -> Self {
        Self {
            not_fixed: true,
            surface: None,
            extents: None,
            hardpoint_ids: Vec::new(),
        }
    }
}

impl SurPart {
    /// `Part::clear`, applied to a part whose id repeats.
    fn clear(&mut self) {
        self.not_fixed = true;
        self.surface = None;
        self.extents = Some(Extents::default());
        self.hardpoint_ids.clear();
    }

    /// The decoded surface, if the part has one.
    #[must_use]
    pub fn compact_surface(&self) -> Option<Result<CompactSurface, CompactError>> {
        self.surface.as_deref().map(CompactSurface::parse)
    }
}

/// What [`load`] leaves, as `LoadSurfaces` leaves its map and result.
#[derive(Clone, Debug, PartialEq)]
pub struct SurLoad {
    /// The parts by id: everything read before a failure, too.
    pub parts: BTreeMap<u32, SurPart>,
    /// Each repeated part id, once per repetition, in file order.
    pub duplicate_part_ids: Vec<u32>,
    /// `Ok` where the reference returns true.
    pub result: Result<(), SurError>,
}

impl SurLoad {
    /// The diagnostics the reference reports while loading: one per repeated
    /// part id and one for a reported failure.
    #[must_use]
    pub fn reported(&self) -> usize {
        self.duplicate_part_ids.len()
            + usize::from(
                self.result
                    .as_ref()
                    .err()
                    .is_some_and(SurError::is_reported),
            )
    }

    /// The parts, or the error that stopped the load.
    ///
    /// # Errors
    ///
    /// The [`SurError`] of a failed load.
    pub fn into_parts(self) -> Result<BTreeMap<u32, SurPart>, SurError> {
        self.result.map(|()| self.parts)
    }
}

struct Cursor<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl<'a> Cursor<'a> {
    fn take(&mut self, n: usize) -> Option<&'a [u8]> {
        let end = self.at.checked_add(n)?;
        let slice = self.bytes.get(self.at..end)?;
        self.at = end;
        Some(slice)
    }

    fn array4(&mut self) -> Option<[u8; 4]> {
        self.take(4).map(|b| [b[0], b[1], b[2], b[3]])
    }

    fn u32(&mut self) -> Option<u32> {
        self.array4().map(u32::from_le_bytes)
    }

    fn i32(&mut self) -> Option<i32> {
        self.array4().map(i32::from_le_bytes)
    }

    fn vector(&mut self) -> Option<Vector> {
        let x = f32::from_bits(self.u32()?);
        let y = f32::from_bits(self.u32()?);
        let z = f32::from_bits(self.u32()?);
        Some(Vector::new(x, y, z))
    }

    /// A part-level read that must not run short.
    fn need<T>(&self, start: usize, value: Option<T>) -> Result<T, SurError> {
        value.ok_or(SurError::Truncated { offset: start })
    }
}

/// Loads a `.sur` file (`PhyArch::LoadSurfaces`).
#[must_use]
pub fn load(bytes: &[u8]) -> SurLoad {
    let mut parts = BTreeMap::new();
    let mut duplicate_part_ids = Vec::new();
    let result = load_into(bytes, &mut parts, &mut duplicate_part_ids);
    SurLoad {
        parts,
        duplicate_part_ids,
        result,
    }
}

fn load_into(
    bytes: &[u8],
    parts: &mut BTreeMap<u32, SurPart>,
    duplicates: &mut Vec<u32>,
) -> Result<(), SurError> {
    let mut cursor = Cursor { bytes, at: 0 };
    let tag = cursor.array4().ok_or(SurError::NoTag)?;
    if !eq_ignore_case(&tag, b"vers") {
        // The version stays at its preset 1.0.
        return Err(SurError::OldFormat);
    }
    let bits = cursor.u32().ok_or(SurError::TruncatedVersion)?;
    // An `f32` comparison: only 2.0 itself passes.
    if f32::from_bits(bits) != VERSION {
        return Err(SurError::BadVersion(bits));
    }
    loop {
        let Some(part_id) = cursor.u32() else {
            return Ok(());
        };
        let part = match parts.entry(part_id) {
            std::collections::btree_map::Entry::Vacant(entry) => entry.insert(SurPart::default()),
            std::collections::btree_map::Entry::Occupied(entry) => {
                duplicates.push(part_id);
                let part = entry.into_mut();
                part.clear();
                part
            }
        };
        let mut seen: HashSet<u32> = part.hardpoint_ids.iter().copied().collect();
        let start = cursor.at;
        let chunk_count = cursor.i32();
        let chunk_count = cursor.need(start, chunk_count)?;
        for _ in 0..chunk_count.max(0) {
            let start = cursor.at;
            let tag = cursor.array4();
            let tag = cursor.need(start, tag)?;
            if eq_ignore_case(&tag, b"surf") {
                let start = cursor.at;
                let size = cursor.u32();
                let size = cursor.need(start, size)?;
                let start = cursor.at;
                let blob = usize::try_from(size).ok().and_then(|n| cursor.take(n));
                part.surface = Some(cursor.need(start, blob)?.to_vec());
            } else if eq_ignore_case(&tag, b"exts") {
                let start = cursor.at;
                let min = cursor.vector();
                let min = cursor.need(start, min)?;
                let max = cursor.vector();
                let max = cursor.need(start, max)?;
                part.extents = Some(Extents { min, max });
            } else if eq_ignore_case(&tag, b"!fxd") {
                part.not_fixed = false;
            } else if eq_ignore_case(&tag, b"hpid") {
                let start = cursor.at;
                let count = cursor.i32();
                let count = cursor.need(start, count)?;
                for _ in 0..count.max(0) {
                    let start = cursor.at;
                    let id = cursor.u32();
                    let id = cursor.need(start, id)?;
                    if seen.insert(id) {
                        part.hardpoint_ids.push(id);
                    }
                }
            } else {
                return Err(SurError::UnknownChunk { part_id, tag });
            }
        }
    }
}

#[cfg(test)]
mod tests;
