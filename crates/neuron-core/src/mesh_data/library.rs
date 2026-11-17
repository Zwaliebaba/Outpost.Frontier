//! A `VMeshLibrary` node (`FVMeshLibrary::Load`, `VMeshDataLoad` and the
//! shared-buffer groups of `VMeshLibrary.cpp`).
//!
//! Each subdirectory of `VMeshLibrary` whose name does not start with `.`
//! is one mesh: its name (the `.vms` path the exporter wrote, case kept)
//! and its `VMeshData` leaf. Meshes are keyed by the name CRC
//! ([`name_crc32`]); the parts and wireframes find them by that key.
//! Consecutive meshes with the same FVF share one vertex and one index
//! buffer, a [`VMeshGroup`].
//!
//! The reference keeps every loaded mesh in a process-wide map by CRC, so
//! a name loaded before (from any library still held) is shared instead of
//! read again. Within one load this port does the same; the map across
//! loads belongs to the client's mesh cache.

use std::collections::HashMap;

use super::vmesh::{VMeshData, VMeshError};
use super::{Malformed, Whole, child, find, open_node, read_whole};
use crate::hash::name_crc32;
use crate::vfs::{ATTRIBUTE_DIRECTORY, FileSystem};

/// Why a mesh of the library did not load. Each costs the reference two
/// warnings: one from `VMeshDataLoad` and "couldnt load %s".
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MeshLoadError {
    /// The `VMeshData` leaf is missing or its read failed ("couldnt load
    /// vmesh persistant data; bad version?").
    ReadFailed,
    /// The leaf does not parse ([`VMeshError::OldFormat`] is the reference's
    /// "mesh in old format").
    Parse(VMeshError),
}

/// Something the load reported.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum LibraryWarning {
    /// A mesh did not load.
    MeshFailed {
        /// The directory name.
        name: Vec<u8>,
        /// Why.
        error: MeshLoadError,
    },
    /// "couldnt add vmesh to vmeshlibrary? %s": a mesh joined no group.
    NotGrouped {
        /// The directory name.
        name: Vec<u8>,
    },
}

impl LibraryWarning {
    /// The number of `FDUMP` calls the reference makes for it.
    #[must_use]
    pub fn dump_count(&self) -> u32 {
        match self {
            Self::MeshFailed { .. } => 2,
            Self::NotGrouped { .. } => 1,
        }
    }
}

/// One loaded mesh.
#[derive(Clone, Debug, PartialEq)]
pub struct LibraryMesh {
    /// The directory name, as found.
    pub name: Vec<u8>,
    /// Its name CRC, the key parts and wireframes use.
    pub crc: u32,
    /// The parsed leaf.
    pub data: VMeshData,
    /// The group in [`VMeshLibrary::groups`] that holds it.
    pub group: Option<usize>,
}

/// Meshes that share one vertex buffer of `fvf` and one index buffer.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VMeshGroup {
    /// The FVF, fixed by the first member with a non-zero one.
    pub fvf: u32,
    /// The members' vertex counts, summed (32-bit wrapping).
    pub vertex_total: u32,
    /// The members' index counts, summed (32-bit wrapping).
    pub index_total: u32,
    /// Members in buffer order, as indices into [`VMeshLibrary::meshes`].
    pub members: Vec<usize>,
}

impl VMeshGroup {
    /// The first vertex of each member in the group's vertex buffer, as
    /// `VMeshGroupEnsureVb` places them.
    #[must_use]
    pub fn base_vertices(&self, library: &VMeshLibrary) -> Vec<u32> {
        let mut base = 0u32;
        self.members
            .iter()
            .map(|&m| {
                let at = base;
                base = base.wrapping_add(u32::from(library.meshes[m].data.header.vertex_count));
                at
            })
            .collect()
    }

    /// The group's index buffer as `VMeshGroupEnsureIb` fills it, and the
    /// start index each member records. The buffer holds `index_total`
    /// entries; each member writes its [`VMeshData::buffer_indices`] where
    /// the previous one stopped, while its start index advances by its
    /// header's index count. Entries no member writes stay 0 (the
    /// reference leaves them as the device returned them).
    ///
    /// # Errors
    ///
    /// [`VMeshError::OutOfRange`] when a member's face groups name more
    /// indices than it has or the writes pass `index_total`.
    pub fn index_buffer(&self, library: &VMeshLibrary) -> Result<(Vec<u16>, Vec<u32>), VMeshError> {
        let mut buffer = vec![0u16; self.index_total as usize];
        let mut cursor = 0usize;
        let mut start = 0u32;
        let mut starts = Vec::with_capacity(self.members.len());
        for &m in &self.members {
            let data = &library.meshes[m].data;
            let indices = data.buffer_indices()?;
            buffer
                .get_mut(cursor..cursor + indices.len())
                .ok_or(VMeshError::OutOfRange)?
                .copy_from_slice(&indices);
            cursor += indices.len();
            starts.push(start);
            start = start.wrapping_add(u32::from(data.header.index_count));
        }
        Ok((buffer, starts))
    }
}

/// A loaded `VMeshLibrary` node.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct VMeshLibrary {
    /// The distinct meshes, in load order.
    pub meshes: Vec<LibraryMesh>,
    /// The handle's list: one entry per mesh directory that loaded, as an
    /// index into [`Self::meshes`]; a name whose CRC was loaded before
    /// repeats that mesh. Empty where the reference returns a null handle.
    pub entries: Vec<usize>,
    /// The groups that still have members, in creation order.
    pub groups: Vec<VMeshGroup>,
    /// What the load reported, in order.
    pub warnings: Vec<LibraryWarning>,
}

impl VMeshLibrary {
    /// The mesh whose name CRC is `crc` (`VMeshDataFind`).
    #[must_use]
    pub fn find(&self, crc: u32) -> Option<&LibraryMesh> {
        self.meshes.iter().find(|m| m.crc == crc)
    }

    /// The number of `FDUMP` calls the load made.
    #[must_use]
    pub fn dump_count(&self) -> u32 {
        self.warnings.iter().map(LibraryWarning::dump_count).sum()
    }
}

/// `strstr(name, ".lod")` and the filter test: a name with `.lod` in it
/// is skipped unless the character after it, minus `'0'` as a signed
/// `char`, is at least `lod_filter`. A negative filter keeps everything.
fn lod_filtered(name: &[u8], lod_filter: i32) -> bool {
    if lod_filter < 0 {
        return false;
    }
    let name = crate::hash::c_str(name);
    let Some(at) = name.windows(4).position(|w| w == b".lod") else {
        return false;
    };
    let next = name.get(at + 4).copied().unwrap_or(0);
    let digit = i32::from(i8::from_ne_bytes([next])) - i32::from(b'0');
    digit < lod_filter
}

#[derive(Default)]
struct Groups {
    slots: Vec<Option<VMeshGroup>>,
    of_mesh: Vec<Option<usize>>,
}

impl Groups {
    fn create(&mut self) -> usize {
        self.slots.push(Some(VMeshGroup {
            fvf: 0,
            vertex_total: 0,
            index_total: 0,
            members: Vec::new(),
        }));
        self.slots.len() - 1
    }

    fn count(&self, group: usize) -> usize {
        self.slots[group].as_ref().map_or(0, |g| g.members.len())
    }

    fn release_if_empty(&mut self, group: usize) {
        if self.count(group) == 0 {
            self.slots[group] = None;
        }
    }

    /// `VMeshGroupAdd`.
    fn add(&mut self, group: usize, mesh: usize, data: &VMeshData) -> bool {
        if self.of_mesh[mesh] == Some(group) {
            return true;
        }
        let fvf = u32::from(data.header.fvf);
        let Some(g) = self.slots[group].as_mut() else {
            return false;
        };
        if g.fvf == 0 {
            g.fvf = fvf;
        }
        if g.fvf != fvf {
            return false;
        }
        g.vertex_total = g
            .vertex_total
            .wrapping_add(u32::from(data.header.vertex_count));
        g.index_total = g
            .index_total
            .wrapping_add(u32::from(data.header.index_count));
        if let Some(old) = self.of_mesh[mesh] {
            self.remove(old, mesh, data);
        }
        self.of_mesh[mesh] = Some(group);
        if let Some(g) = self.slots[group].as_mut() {
            g.members.push(mesh);
        }
        true
    }

    /// `VMeshGroupRemove`: an emptied group is released.
    fn remove(&mut self, group: usize, mesh: usize, data: &VMeshData) {
        if let Some(g) = self.slots[group].as_mut() {
            g.vertex_total = g
                .vertex_total
                .wrapping_sub(u32::from(data.header.vertex_count));
            g.index_total = g
                .index_total
                .wrapping_sub(u32::from(data.header.index_count));
            g.members.retain(|&m| m != mesh);
        }
        self.of_mesh[mesh] = None;
        self.release_if_empty(group);
    }
}

/// `FVMeshLibrary::Load(fs, lod_filter)`: `OpenNode("VMeshLibrary")`, every
/// subdirectory of it through `FindFirst("*.*")`, then `OpenNode("..")`.
/// `Ok(None)` when the node is missing (the reference returns a null
/// handle and leaves the current node alone). The client passes a
/// `lod_filter` of -1.
///
/// # Errors
///
/// A malformed archive.
pub fn load(fs: &mut dyn FileSystem, lod_filter: i32) -> Result<Option<VMeshLibrary>, Malformed> {
    if !open_node(fs, b"VMeshLibrary")? {
        return Ok(None);
    }
    let mut library = VMeshLibrary::default();
    let mut groups = Groups::default();
    let mut by_crc: HashMap<u32, usize> = HashMap::new();
    let mut current: Option<usize> = None;
    for entry in find(fs, b"*.*")? {
        let name = &entry.data.name;
        if entry.data.attributes & ATTRIBUTE_DIRECTORY == 0 || name.first() == Some(&b'.') {
            continue;
        }
        if lod_filtered(name, lod_filter) {
            continue;
        }
        let Some(mesh_fs) = child(fs, name, Some(&entry))? else {
            continue;
        };
        let crc = name_crc32(name);
        let mesh = if let Some(&mesh) = by_crc.get(&crc) {
            mesh
        } else {
            let data = match read_whole(mesh_fs.as_ref(), b"VMeshData")? {
                Whole::Read(bytes) => VMeshData::parse(&bytes).map_err(MeshLoadError::Parse),
                Whole::Missing | Whole::Failed => Err(MeshLoadError::ReadFailed),
            };
            match data {
                Ok(data) => {
                    library.meshes.push(LibraryMesh {
                        name: name.clone(),
                        crc,
                        data,
                        group: None,
                    });
                    groups.of_mesh.push(None);
                    let mesh = library.meshes.len() - 1;
                    by_crc.insert(crc, mesh);
                    mesh
                }
                Err(error) => {
                    library.warnings.push(LibraryWarning::MeshFailed {
                        name: name.clone(),
                        error,
                    });
                    continue;
                }
            }
        };
        library.entries.push(mesh);
        let group = *current.get_or_insert_with(|| groups.create());
        if !groups.add(group, mesh, &library.meshes[mesh].data) {
            groups.release_if_empty(group);
            let fresh = groups.create();
            current = Some(fresh);
            if !groups.add(fresh, mesh, &library.meshes[mesh].data) {
                library
                    .warnings
                    .push(LibraryWarning::NotGrouped { name: name.clone() });
            }
        }
    }
    let _ = open_node(fs, b"..")?;
    if let Some(group) = current {
        groups.release_if_empty(group);
    }
    let mut renumber = vec![None; groups.slots.len()];
    for (slot, group) in groups.slots.into_iter().enumerate() {
        if let Some(group) = group {
            renumber[slot] = Some(library.groups.len());
            library.groups.push(group);
        }
    }
    for (mesh, group) in library.meshes.iter_mut().zip(groups.of_mesh) {
        mesh.group = group.and_then(|g| renumber[g]);
    }
    Ok(Some(library))
}

#[cfg(test)]
mod tests {
    use super::lod_filtered;

    #[test]
    fn lod_filter_reads_the_character_after_lod() {
        assert!(!lod_filtered(b"ship.lod0.vms", -1));
        assert!(lod_filtered(b"ship.lod0.vms", 1));
        assert!(!lod_filtered(b"ship.lod1.vms", 1));
        assert!(lod_filtered(b"ship.lod", 0));
        assert!(!lod_filtered(b"ship.LOD0.vms", 1));
        assert!(lod_filtered(b"ship.lod\xff", 0));
    }
}
