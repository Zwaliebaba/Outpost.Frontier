//! The `VMesh` meshes at run time (`VMeshLibrary.cpp`, `VMeshPart.cpp`):
//! the `VMeshData` registry by name CRC with reference counts and the
//! face groups' materials, the shared-buffer groups and their device
//! buffers, `VMeshLibrary`'s load and unload, the `VMeshPart` archetype
//! (material hook-up, `Prepare`, `Draw`) and the `VMesh` plugin.
//!
//! The reference keeps the registry in a process static map
//! (`G_VmeshDataMap`) that every `VMeshLibrary`, part and wire shares;
//! here it is a [`VMeshRegistry`] value they share through
//! [`SharedRegistry`] (divergence D351).

use std::cell::{Cell, RefCell};
use std::collections::BTreeMap;
use std::rc::Rc;

use neuron_core::hash::name_crc32;
use neuron_core::math::mul_transform;
use neuron_core::mesh_data::fvf::Channel;
use neuron_core::mesh_data::part;
use neuron_core::mesh_data::vmesh::{VMeshData, VMeshError, VMeshFaceGroup, VMeshRef};
use neuron_core::vfs::{ATTRIBUTE_DIRECTORY, FileSystem};

use super::face::{FaceSource, MeshFace};
use super::manager::{DrawRequest, GeometryQuery, RenderComponent};
use super::services::{MeshCamera, MeshServices};
use super::{
    HANDLE_NONE, Handle, Instance, MeshIid, child_fs, dump, file, open_node, read_leaf, transform,
};
use crate::render::material::{
    DrawArgs, IbScratch, MaterialId, SubmitContext, VbListing, VertexSource, VertexStream,
};

/// A registry shared by the libraries, the parts and the wires.
pub type SharedRegistry = Rc<RefCell<VMeshRegistry>>;

/// One `VMeshData` (`FVmeshData`).
#[derive(Clone, Debug, PartialEq)]
pub struct VMeshEntry {
    /// The `.vms` directory name, case kept.
    pub name: Vec<u8>,
    /// Its name CRC, the registry key.
    pub crc: u32,
    /// The leaf.
    pub data: VMeshData,
    /// One material per face group, each with the reference the parts
    /// hooked it up with.
    pub materials: Vec<Option<MaterialId>>,
    /// The vertex buffer of its group once built.
    pub vertex_buffer: u32,
    /// The group's format.
    pub fvf: u32,
    /// Its first vertex in the group's vertex buffer.
    pub base_vertex: u32,
    /// The index buffer of its group once built.
    pub index_buffer: u32,
    /// Its first index in the group's index buffer.
    pub start_index: u32,
    /// Its group.
    pub group: Option<usize>,
    /// The reference count.
    pub refs: i32,
    /// The leaf size in KiB (`SizeKib`, the `Memory_VMeshData` event).
    pub size_kib: f32,
}

/// A shared-buffer group (`FVmeshGroup`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct VMeshGroup {
    /// The sum of the members' vertices.
    pub vertex_total: u32,
    /// The sum of the members' indices.
    pub index_total: u32,
    /// The members' format; 0 until the first joins.
    pub fvf: u32,
    /// The vertex buffer, 0 for none.
    pub vertex_buffer: u32,
    /// The index buffer, 0 for none.
    pub index_buffer: u32,
    /// The members in list order.
    pub members: Vec<usize>,
}

/// The `VMeshData` registry and the groups.
#[derive(Clone, Debug, Default)]
pub struct VMeshRegistry {
    entries: Vec<Option<VMeshEntry>>,
    by_crc: BTreeMap<u32, usize>,
    groups: Vec<Option<VMeshGroup>>,
}

/// What `VMeshLibrary::Load` returns: the meshes it holds a reference on,
/// in load order (`FVmeshDataList`). `None` is the null handle.
pub type VMeshLibraryLoad = Option<Vec<usize>>;

impl VMeshRegistry {
    /// An empty registry.
    #[must_use]
    pub fn new() -> VMeshRegistry {
        VMeshRegistry::default()
    }

    /// A registry to share.
    #[must_use]
    pub fn shared() -> SharedRegistry {
        Rc::new(RefCell::new(VMeshRegistry::new()))
    }

    /// `VMeshDataFind`: the mesh of a name CRC.
    #[must_use]
    pub fn find(&self, crc: u32) -> Option<usize> {
        self.by_crc.get(&crc).copied()
    }

    /// A mesh.
    #[must_use]
    pub fn entry(&self, id: usize) -> Option<&VMeshEntry> {
        self.entries.get(id).and_then(Option::as_ref)
    }

    fn entry_mut(&mut self, id: usize) -> Option<&mut VMeshEntry> {
        self.entries.get_mut(id).and_then(Option::as_mut)
    }

    /// A group.
    #[must_use]
    pub fn group(&self, id: usize) -> Option<&VMeshGroup> {
        self.groups.get(id).and_then(Option::as_ref)
    }

    /// The live meshes, by name CRC.
    pub fn meshes(&self) -> impl Iterator<Item = &VMeshEntry> {
        self.by_crc.values().filter_map(|&id| self.entry(id))
    }

    /// `VMeshDataAddRef`.
    pub fn add_ref(&mut self, id: usize) -> i32 {
        self.entry_mut(id).map_or(0, |e| {
            e.refs += 1;
            e.refs
        })
    }

    /// `VMeshDataRelease`: at 0 or below the mesh is destroyed.
    pub fn release<S: MeshServices + ?Sized>(&mut self, services: &mut S, id: usize) -> i32 {
        let Some(entry) = self.entry_mut(id) else {
            return 0;
        };
        entry.refs -= 1;
        let count = entry.refs;
        if count <= 0 {
            self.destruct(services, id);
        }
        count
    }

    /// `VMeshDataDestruct`: the key, the group membership and the leaf go,
    /// then the negative memory event.
    fn destruct<S: MeshServices + ?Sized>(&mut self, services: &mut S, id: usize) {
        let Some(entry) = self.entry(id) else {
            return;
        };
        let (crc, group, size) = (entry.crc, entry.group, entry.size_kib);
        if self.by_crc.get(&crc) == Some(&id) {
            self.by_crc.remove(&crc);
        }
        if let Some(group) = group {
            self.group_remove(services, group, id);
        }
        self.entries[id] = None;
        services.log_event("Memory_VMeshData", -size, 0x610);
    }

    fn new_group(&mut self) -> usize {
        self.groups.push(Some(VMeshGroup::default()));
        self.groups.len() - 1
    }

    /// `Fvf = 0` and both buffers destroyed (`VMeshGroupReleaseBuffers`).
    fn release_buffers<S: MeshServices + ?Sized>(&mut self, services: &mut S, group: usize) {
        if let Some(g) = self.groups.get_mut(group).and_then(Option::as_mut) {
            g.fvf = 0;
            services.destroy_vb(g.vertex_buffer);
            g.vertex_buffer = 0;
            services.destroy_ib(g.index_buffer);
            g.index_buffer = 0;
        }
    }

    /// The group's buffers destroyed (`VMeshGroupDestroyVb`,
    /// `VMeshGroupDestroyIb`).
    fn destroy_buffers<S: MeshServices + ?Sized>(&mut self, services: &mut S, group: usize) {
        if let Some(g) = self.groups.get_mut(group).and_then(Option::as_mut) {
            services.destroy_vb(g.vertex_buffer);
            g.vertex_buffer = 0;
            services.destroy_ib(g.index_buffer);
            g.index_buffer = 0;
        }
    }

    /// `VMeshGroupAdd`: the first member fixes the format; a different one
    /// is refused. A member of another group leaves it after the totals
    /// grew.
    fn group_add<S: MeshServices + ?Sized>(
        &mut self,
        services: &mut S,
        group: usize,
        id: usize,
    ) -> bool {
        let Some(entry) = self.entry(id) else {
            return false;
        };
        if entry.group == Some(group) {
            return true;
        }
        let header = entry.data.header;
        let old = entry.group;
        let Some(g) = self.groups.get_mut(group).and_then(Option::as_mut) else {
            return false;
        };
        if g.fvf == 0 {
            g.fvf = u32::from(header.fvf);
        }
        if g.fvf != u32::from(header.fvf) {
            return false;
        }
        g.vertex_total = g.vertex_total.wrapping_add(u32::from(header.vertex_count));
        g.index_total = g.index_total.wrapping_add(u32::from(header.index_count));
        if let Some(old) = old {
            self.group_remove(services, old, id);
        }
        if let Some(e) = self.entry_mut(id) {
            e.group = Some(group);
        }
        if let Some(g) = self.groups.get_mut(group).and_then(Option::as_mut) {
            g.members.push(id);
        }
        true
    }

    /// `VMeshGroupRemove`: an emptied group releases its buffers and goes.
    fn group_remove<S: MeshServices + ?Sized>(
        &mut self,
        services: &mut S,
        group: usize,
        id: usize,
    ) -> i32 {
        let Some(entry) = self.entry(id) else {
            return -1;
        };
        if entry.group != Some(group) {
            return -1;
        }
        let header = entry.data.header;
        let Some(g) = self.groups.get_mut(group).and_then(Option::as_mut) else {
            return -1;
        };
        g.vertex_total = g.vertex_total.wrapping_sub(u32::from(header.vertex_count));
        g.index_total = g.index_total.wrapping_sub(u32::from(header.index_count));
        let Some(at) = g.members.iter().position(|&m| m == id) else {
            return -1;
        };
        g.members.remove(at);
        let count = g.members.len();
        if let Some(e) = self.entry_mut(id) {
            e.group = None;
        }
        if count == 0 {
            self.release_buffers(services, group);
            self.groups[group] = None;
        }
        i32::try_from(count).unwrap_or(i32::MAX)
    }

    /// `VMeshGroupEnsureVb`: a buffer that still selects is kept; else it is
    /// made again for the group's vertices and every member copies its
    /// vertices in at a running base (a failed copy stops the walk). The
    /// final `SelectVb` result, or the create failure.
    pub fn ensure_vb<S: MeshServices + ?Sized>(&mut self, services: &mut S, group: usize) -> i32 {
        let Some(g) = self.group(group) else {
            return -1;
        };
        let result = services.select_vb(g.vertex_buffer);
        if result >= 0 {
            return result;
        }
        let (old, fvf, total) = (g.vertex_buffer, g.fvf, g.vertex_total);
        services.destroy_vb(old);
        let vb = match services.create_vb(fvf, total) {
            Ok(vb) => vb,
            Err(result) => {
                self.set_group_vb(group, 0);
                return result;
            }
        };
        self.set_group_vb(group, vb);
        if services.lock_vb(vb, total) >= 0 {
            let members = self
                .group(group)
                .map(|g| g.members.clone())
                .unwrap_or_default();
            let mut base = 0u32;
            for id in members {
                let Some(entry) = self.entry(id) else {
                    continue;
                };
                let count = u32::from(entry.data.header.vertex_count);
                let source = vertex_source(&entry.data, 0, count);
                let (result, out_base) = services.copy_vertices(vb, base, &source, 0, source.count);
                if result < 0 {
                    break;
                }
                if let Some(e) = self.entry_mut(id) {
                    e.vertex_buffer = vb;
                    e.fvf = fvf;
                    e.base_vertex = out_base;
                }
                base = base.wrapping_add(count);
            }
            services.unlock_vb(vb);
        }
        services.select_vb(vb)
    }

    fn set_group_vb(&mut self, group: usize, vb: u32) {
        if let Some(g) = self.groups.get_mut(group).and_then(Option::as_mut) {
            g.vertex_buffer = vb;
        }
    }

    fn set_group_ib(&mut self, group: usize, ib: u32) {
        if let Some(g) = self.groups.get_mut(group).and_then(Option::as_mut) {
            g.index_buffer = ib;
        }
    }

    /// `VMeshGroupEnsureIb`: as [`VMeshRegistry::ensure_vb`] for the index
    /// buffer; each member's indices get its face groups' first vertex
    /// added (flag bit 0, which every loaded mesh has).
    pub fn ensure_ib<S: MeshServices + ?Sized>(
        &mut self,
        services: &mut S,
        group: usize,
        base_vertex: u32,
    ) -> i32 {
        let Some(g) = self.group(group) else {
            return -1;
        };
        let result = services.select_ib(g.index_buffer, base_vertex);
        if result >= 0 {
            return result;
        }
        let (old, total) = (g.index_buffer, g.index_total);
        services.destroy_ib(old);
        let ib = match services.create_ib(total) {
            Ok(ib) => ib,
            Err(result) => {
                self.set_group_ib(group, 0);
                return result;
            }
        };
        self.set_group_ib(group, ib);
        if services.lock_ib(ib, total) >= 0 {
            let members = self
                .group(group)
                .map(|g| g.members.clone())
                .unwrap_or_default();
            let mut written: Vec<u16> = Vec::with_capacity(total as usize);
            let mut start = 0u32;
            for id in members {
                let Some(entry) = self.entry(id) else {
                    continue;
                };
                let data = &entry.data;
                if data.header.flags & 1 != 0 {
                    let mut source = data.indices.iter();
                    for face_group in &data.face_groups {
                        for _ in 0..face_group.index_count {
                            let index = source.next().copied().unwrap_or(0);
                            written.push(face_group.vertex_begin.wrapping_add(index));
                        }
                    }
                } else {
                    written.extend_from_slice(&data.indices);
                }
                let count = u32::from(data.header.index_count);
                if let Some(e) = self.entry_mut(id) {
                    e.index_buffer = ib;
                    e.start_index = start;
                }
                start = start.wrapping_add(count);
            }
            services.unlock_ib(ib, &written);
        }
        services.select_ib(ib, base_vertex)
    }

    /// `VMeshDataLoad(fs, name)`: a name CRC already registered gains a
    /// reference; else the `VMeshData` leaf of the mesh's node is read and
    /// registered with one. `None` where the read fails or the leaf is in
    /// the old format, each with the reference's `FDUMP`.
    pub fn load_data<S: MeshServices + ?Sized>(
        &mut self,
        services: &mut S,
        fs: &dyn FileSystem,
        name: &[u8],
    ) -> Option<usize> {
        let crc = name_crc32(name);
        if let Some(id) = self.find(crc) {
            self.add_ref(id);
            return Some(id);
        }
        let Some(bytes) = read_leaf(fs, b"VMeshData") else {
            services.dump(
                dump::ERROR,
                file::VMESH_DATA,
                0x2A,
                "couldnt load vmesh persistant data; bad version?",
            );
            return None;
        };
        let size = i32::try_from(bytes.len()).unwrap_or(i32::MAX);
        let size_kib = (f64::from(size) * 0.000_976_562_5) as f32;
        let parsed = VMeshData::parse(&bytes);
        let id = self.entries.len();
        self.by_crc.insert(crc, id);
        services.log_event("Memory_VMeshData", size_kib, 0x610);
        match parsed {
            Ok(data) => {
                let groups = data.face_groups.len();
                self.entries.push(Some(VMeshEntry {
                    name: name.to_vec(),
                    crc,
                    data,
                    materials: vec![None; groups],
                    vertex_buffer: 0,
                    fvf: 0,
                    base_vertex: 0,
                    index_buffer: 0,
                    start_index: 0,
                    group: None,
                    refs: 1,
                    size_kib,
                }));
                Some(id)
            }
            Err(error) => {
                if error == VMeshError::OldFormat {
                    let message = format!("mesh in old format: {}", String::from_utf8_lossy(name));
                    services.dump(dump::WARNING, file::VMESH_DATA, 0x3A, &message);
                }
                self.by_crc.remove(&crc);
                self.entries.push(None);
                services.log_event("Memory_VMeshData", -size_kib, 0x610);
                None
            }
        }
    }

    /// `FVMeshLibrary::Load(fs, lod_filter)`: every directory of the
    /// `VMeshLibrary` node through `FindFirst("*.*")` (names starting with
    /// `.` skipped, and with a filter of 0 or more the `.lod<n>` names
    /// below it), each mesh registered, referenced by the returned list and
    /// added to the current group (a mesh of another format starts a new
    /// one).
    pub fn load_library<S: MeshServices + ?Sized>(
        &mut self,
        services: &mut S,
        fs: &mut dyn FileSystem,
        lod_filter: i32,
    ) -> VMeshLibraryLoad {
        if !open_node(fs, b"VMeshLibrary") {
            return None;
        }
        let mut list: Option<Vec<usize>> = None;
        let mut group: Option<usize> = None;
        for entry in fs.find(b"*.*").unwrap_or_default() {
            let name = neuron_core::hash::c_str(&entry.data.name).to_vec();
            if entry.data.attributes & ATTRIBUTE_DIRECTORY == 0 || name.first() == Some(&b'.') {
                continue;
            }
            if lod_filter >= 0 && lod_filtered(&name, lod_filter) {
                continue;
            }
            let Some(child) = child_fs(fs, &name, Some(&entry)) else {
                continue;
            };
            let Some(id) = self.load_data(services, child.as_ref(), &name) else {
                let message = format!("couldnt load {}", String::from_utf8_lossy(&name));
                services.dump(dump::WARNING, file::VMESH_LIBRARY, 0x8B, &message);
                continue;
            };
            self.add_ref(id);
            list.get_or_insert_with(Vec::new).push(id);
            let current = *group.get_or_insert_with(|| self.new_group());
            if !self.group_add(services, current, id) {
                if self.group(current).is_some_and(|g| g.members.is_empty()) {
                    self.release_buffers(services, current);
                    self.groups[current] = None;
                }
                let fresh = self.new_group();
                group = Some(fresh);
                if !self.group_add(services, fresh, id) {
                    let message = format!(
                        "couldnt add vmesh to vmeshlibrary? {}",
                        String::from_utf8_lossy(&name)
                    );
                    services.dump(dump::WARNING, file::VMESH_LIBRARY, 0xA5, &message);
                }
            }
            self.release(services, id);
        }
        let _ = open_node(fs, b"..");
        if let Some(current) = group
            && self.group(current).is_some_and(|g| g.members.is_empty())
        {
            self.release_buffers(services, current);
            self.groups[current] = None;
        }
        list
    }

    /// `FVMeshLibrary::Unload`: every mesh of the list released.
    pub fn unload_library<S: MeshServices + ?Sized>(
        &mut self,
        services: &mut S,
        list: VMeshLibraryLoad,
    ) {
        for id in list.unwrap_or_default() {
            self.release(services, id);
        }
    }
}

/// Whether a `.lod<n>` name falls below the filter (the character after
/// `.lod` minus `'0'` is below it).
fn lod_filtered(name: &[u8], lod_filter: i32) -> bool {
    let Some(at) = name.windows(4).position(|w| w == b".lod") else {
        return false;
    };
    let next = name.get(at + 4).copied().unwrap_or(0);
    i32::from(next as i8) - i32::from(b'0') < lod_filter
}

/// `VMeshBuildVertexDesc(header, start, count)`: the five channel streams
/// over the mesh's interleaved vertices, flags 2, the mesh's format and
/// `count` vertices.
#[must_use]
pub fn vertex_source(data: &VMeshData, start: u32, count: u32) -> VertexSource {
    let stream = |channel: Channel| -> VertexStream {
        match data.channel_record(channel, start, count) {
            Some(record) => {
                let at = usize::try_from(record.offset).unwrap_or(usize::MAX);
                VertexStream {
                    data: Some(data.vertices.get(at..).unwrap_or_default().to_vec()),
                    byte_size: record.element_bytes,
                    stride: record.stride,
                    index: None,
                    count: record.count,
                }
            }
            None => VertexStream::default(),
        }
    };
    VertexSource {
        flags: 2,
        position: stream(Channel::Position),
        normal: stream(Channel::Normal),
        diffuse: stream(Channel::Diffuse),
        tex: [stream(Channel::Tex0), stream(Channel::Tex1)],
        fvf: u32::from(data.header.fvf),
        count,
    }
}

/// A `VMeshPart` archetype (`FVMeshPart`): a range of a library mesh.
///
/// The plugin shares its parts with the `IMesh` faces it hands out
/// ([`MeshFace`]), which a material may use while the part draws; the
/// state those faces and the plugin change lives in cells (divergence
/// D356).
#[derive(Clone, Debug, PartialEq)]
pub struct VMeshPart {
    /// The selected face group, -1 for none.
    pub cursor: Cell<i32>,
    /// The instances made on it.
    pub use_count: Cell<i32>,
    /// The library mesh, once hooked up.
    pub data: Cell<Option<usize>>,
    /// The `VMeshRef` record.
    pub mesh_ref: VMeshRef,
    /// The vertex source of its range, built by the first hook-up.
    pub vertices: RefCell<VertexSource>,
}

impl VMeshPart {
    /// A part over a `VMeshRef`, not hooked up.
    #[must_use]
    pub fn new(mesh_ref: VMeshRef) -> VMeshPart {
        VMeshPart {
            cursor: Cell::new(-1),
            use_count: Cell::new(0),
            data: Cell::new(None),
            mesh_ref,
            vertices: RefCell::new(VertexSource::default()),
        }
    }

    /// The part's face groups of its mesh (the range clamped to the mesh's;
    /// divergence D354).
    fn face_groups<'a>(&self, entry: &'a VMeshEntry) -> &'a [VMeshFaceGroup] {
        let start = usize::from(self.mesh_ref.face_group_start).min(entry.data.face_groups.len());
        let end =
            (start + usize::from(self.mesh_ref.face_group_count)).min(entry.data.face_groups.len());
        &entry.data.face_groups[start..end]
    }

    /// `FVMeshPart::HookUp`: the mesh of the record's CRC (once), then a
    /// material for each of the part's face groups by its CRC; a CRC of 0
    /// or one the library lacks keeps the previous material with a notice.
    /// 0, -1 without the mesh, -8 when a material was not found.
    pub fn hook_up<S: MeshServices + ?Sized>(
        &self,
        registry: &mut VMeshRegistry,
        services: &mut S,
    ) -> i32 {
        if self.data.get().is_none() {
            let found = registry.find(self.mesh_ref.mesh_crc);
            if let Some(id) = found {
                registry.add_ref(id);
            }
            self.data.set(found);
            let Some(entry) = found.and_then(|id| registry.entry(id)) else {
                return -1;
            };
            *self.vertices.borrow_mut() = vertex_source(
                &entry.data,
                u32::from(self.mesh_ref.vertex_start),
                u32::from(self.mesh_ref.vertex_count),
            );
        }
        let Some(id) = self.data.get() else {
            return -1;
        };
        let start = usize::from(self.mesh_ref.face_group_start);
        let mut failed = false;
        for index in 0..usize::from(self.mesh_ref.face_group_count) {
            let group = start + index;
            let Some(entry) = registry.entry_mut(id) else {
                return -1;
            };
            let Some(face_group) = entry.data.face_groups.get(group).copied() else {
                continue;
            };
            let previous = entry.materials[group];
            let crc = face_group.material_crc;
            let mut found = false;
            let mut written = previous;
            if crc != 0 {
                let (result, material) = services.find_material(crc);
                written = material;
                found = result >= 0;
            }
            let Some(entry) = registry.entry_mut(id) else {
                return -1;
            };
            if found {
                entry.materials[group] = written;
                if let Some(previous) = previous {
                    services.release_material(previous);
                }
            } else {
                let message = format!(" VMESH: couldnt find material {crc}");
                services.dump(dump::NOTICE, file::VM_PART_ARCH, 0x2B9, &message);
                entry.materials[group] = previous;
                failed = true;
            }
        }
        if failed { -8 } else { 0 }
    }

    /// The triangles of the part's face groups (`IndexCount / 3` each).
    #[must_use]
    pub fn triangle_count(&self, registry: &VMeshRegistry) -> i32 {
        let Some(entry) = self.data.get().and_then(|id| registry.entry(id)) else {
            return 0;
        };
        self.face_groups(entry)
            .iter()
            .map(|g| i32::from(g.index_count) / 3)
            .sum()
    }

    /// `FVMeshPart::Prepare`: the group's buffers made current; the listing
    /// gets the mesh's buffer, format and base vertex plus the record's
    /// first vertex, the scratch the index buffer and the mesh's first
    /// index plus the record's.
    pub fn prepare<S: MeshServices + ?Sized>(
        &self,
        registry: &mut VMeshRegistry,
        services: &mut S,
        listing: &mut VbListing,
        scratch: &mut IbScratch,
    ) -> bool {
        let data = self.data.get();
        let Some(group) = data.and_then(|id| registry.entry(id)).and_then(|e| e.group) else {
            return false;
        };
        if registry.ensure_vb(services, group) < 0 {
            return false;
        }
        let Some(entry) = data.and_then(|id| registry.entry(id)) else {
            return false;
        };
        listing.vertex_buffer = entry.vertex_buffer;
        listing.fvf = entry.fvf;
        listing.base_vertex = entry
            .base_vertex
            .wrapping_add(u32::from(self.mesh_ref.vertex_start));
        let result = registry.ensure_ib(services, group, listing.base_vertex);
        if result >= 0 {
            let Some(entry) = data.and_then(|id| registry.entry(id)) else {
                return false;
            };
            scratch.index_buffer = entry.index_buffer;
            scratch.bias = entry
                .start_index
                .wrapping_add(u32::from(self.mesh_ref.index_start));
        }
        result >= 0
    }

    /// `FVMeshPart::Draw`: the transforms (unless flag 0x200), then every
    /// face group with a material (or the override) bound, submitted from
    /// the group's buffers and torn down.
    ///
    /// The registry is borrowed only around its own steps, never across a
    /// material's call, so that the material can reach the part's `IMesh`
    /// face.
    pub fn draw<S: MeshServices + ?Sized>(
        &self,
        registry: &RefCell<VMeshRegistry>,
        services: &mut S,
        camera: &mut dyn MeshCamera,
        instance: Instance,
        request: &DrawRequest,
    ) -> bool {
        let Some(id) = self.data.get() else {
            return false;
        };
        let view_words = camera.view_transform();
        let world_words = services.instance_transform(instance);
        let (view, world) = (transform(view_words), transform(world_words));
        let mut flags = request.flags;
        if flags & 0x200 == 0 {
            let state = services.pipeline_state(7).unwrap_or(1);
            if state != 0 {
                let product = match &request.matrix {
                    Some(matrix) => mul_transform(&view, &mul_transform(matrix, &world)),
                    None => mul_transform(&view, &world),
                };
                services.set_modelview(&product.to_words());
            } else if let Some(matrix) = &request.matrix {
                let _ = mul_transform(matrix, &world);
                services.set_world(&world_words);
            } else {
                services.set_world(&world_words);
                services.set_view(&view_words);
            }
        }
        flags |= 0x10C;
        let (face_groups, prim, indices) = {
            let registry = registry.borrow();
            let Some(entry) = registry.entry(id) else {
                return false;
            };
            let face_groups: Vec<(usize, VMeshFaceGroup)> = self
                .face_groups(entry)
                .iter()
                .enumerate()
                .map(|(i, g)| (usize::from(self.mesh_ref.face_group_start) + i, *g))
                .collect();
            let indices: Vec<u16> = entry
                .data
                .indices
                .get(usize::from(self.mesh_ref.index_start)..)
                .unwrap_or_default()
                .to_vec();
            (face_groups, entry.data.header.primitive_type, indices)
        };
        let vertices = self.vertices.borrow();
        let mut listing = VbListing::default();
        let mut scratch = IbScratch::default();
        let mut index_start = 0u32;
        for (group, face_group) in face_groups {
            let own = registry
                .borrow()
                .entry(id)
                .and_then(|e| e.materials.get(group).copied().flatten());
            let (material, replaced) = match request.extra {
                Some(extra) => (Some(extra), own),
                None => (own, None),
            };
            if let Some(material) = material {
                let prepared = self.prepare(
                    &mut registry.borrow_mut(),
                    services,
                    &mut listing,
                    &mut scratch,
                );
                if !prepared {
                    return false;
                }
                let context = SubmitContext {
                    instance,
                    world: &world_words,
                    view: &view_words,
                    vertices: Some(&vertices),
                    indices: &indices,
                    vb: Some(&listing),
                    ib: Some(&scratch),
                    replaced,
                    field20: 0,
                };
                services.bind(material, &context);
                let span = face_group
                    .vertex_end
                    .wrapping_sub(face_group.vertex_begin)
                    .wrapping_add(1);
                let args = DrawArgs {
                    prim_type: prim,
                    start: u32::from(face_group.vertex_begin),
                    count: u32::from(span),
                    ib_start: index_start,
                    index_span: u32::from(face_group.index_count),
                    flags,
                };
                services.submit(material, &context, &args);
                services.teardown(material, &context);
            }
            index_start = index_start.wrapping_add(u32::from(face_group.index_count));
        }
        true
    }

    /// The destructor: the part's materials released, then its mesh.
    fn destroy<S: MeshServices + ?Sized>(&self, registry: &mut VMeshRegistry, services: &mut S) {
        self.cursor.set(-1);
        let Some(id) = self.data.take() else {
            return;
        };
        let start = usize::from(self.mesh_ref.face_group_start);
        for index in 0..usize::from(self.mesh_ref.face_group_count) {
            let slot = registry
                .entry_mut(id)
                .and_then(|e| e.materials.get_mut(start + index))
                .and_then(Option::take);
            if let Some(material) = slot {
                services.release_material(material);
            }
        }
        registry.release(services, id);
    }
}

/// The parts of a `VMesh` plugin, shared with the faces it hands out.
pub type SharedParts = Rc<RefCell<Vec<Option<Rc<VMeshPart>>>>>;

/// The `VMesh` render plugin (`FVMeshPlugin`): parts by handle and the part
/// the last `BindMeshInstance` chose, which every `RenderInstance` draws.
#[derive(Debug)]
pub struct VMeshPlugin {
    registry: SharedRegistry,
    parts: SharedParts,
    bound: Option<Handle>,
}

impl VMeshPlugin {
    /// A plugin over the shared registry.
    #[must_use]
    pub fn new(registry: SharedRegistry) -> VMeshPlugin {
        VMeshPlugin {
            registry,
            parts: Rc::new(RefCell::new(Vec::new())),
            bound: None,
        }
    }

    /// A part by handle (handles are 1 and up).
    #[must_use]
    pub fn part(&self, handle: Handle) -> Option<Rc<VMeshPart>> {
        part_of(&self.parts, handle)
    }

    /// The faces of the plugin's parts, for the render manager's directory.
    #[must_use]
    pub fn faces(&self) -> VMeshFaces {
        VMeshFaces {
            parts: Rc::clone(&self.parts),
            registry: Rc::clone(&self.registry),
        }
    }
}

/// A part of a shared table by handle (handles are 1 and up).
fn part_of(parts: &SharedParts, handle: Handle) -> Option<Rc<VMeshPart>> {
    let index = usize::try_from(handle).ok()?.checked_sub(1)?;
    parts.borrow().get(index).and_then(Clone::clone)
}

/// The `IMesh` faces of a `VMesh` plugin's parts
/// (`QueryHandle(handle, IID_IMesh)`).
#[derive(Clone, Debug)]
pub struct VMeshFaces {
    parts: SharedParts,
    registry: SharedRegistry,
}

impl FaceSource for VMeshFaces {
    fn face(&self, handle: Handle) -> Option<MeshFace> {
        if !valid(handle) {
            return None;
        }
        Some(MeshFace::VMesh {
            part: part_of(&self.parts, handle)?,
            registry: Rc::clone(&self.registry),
        })
    }
}

/// `VMeshHandleValid`: neither -1 nor 0.
fn valid(handle: Handle) -> bool {
    handle != HANDLE_NONE && handle != 0
}

impl<S: MeshServices + ?Sized> RenderComponent<S> for VMeshPlugin {
    fn claim(&mut self, services: &mut S, fs: &mut dyn FileSystem) -> Handle {
        let Ok(Ok(claimed)) = part::claim(fs) else {
            return HANDLE_NONE;
        };
        let part = VMeshPart::new(claimed.mesh_ref);
        part.hook_up(&mut self.registry.borrow_mut(), services);
        let mut parts = self.parts.borrow_mut();
        parts.push(Some(Rc::new(part)));
        u32::try_from(parts.len()).unwrap_or(HANDLE_NONE)
    }

    fn clone_handle(&mut self, services: &mut S, _handle: Handle) -> Handle {
        services.dump(
            dump::NOTICE,
            file::VM_PART_ARCH,
            0x211,
            "VMPartArch clone: not implemented",
        );
        HANDLE_NONE
    }

    fn destroy_handle(&mut self, services: &mut S, handle: Handle) {
        if !valid(handle) {
            return;
        }
        let Some(index) = usize::try_from(handle).ok().and_then(|h| h.checked_sub(1)) else {
            return;
        };
        let part = self
            .parts
            .borrow_mut()
            .get_mut(index)
            .and_then(Option::take);
        if let Some(part) = part {
            part.destroy(&mut self.registry.borrow_mut(), services);
        }
    }

    fn query_handle(&mut self, handle: Handle, iid: MeshIid) -> i32 {
        if !valid(handle) || self.part(handle).is_none() {
            return -1;
        }
        match iid {
            MeshIid::VMeshLibrary
            | MeshIid::Mesh
            | MeshIid::MatHookUp
            | MeshIid::RenderableMesh
            | MeshIid::NeuronComponent => 0,
            _ => -1,
        }
    }

    fn create_mesh_instance(
        &mut self,
        services: &mut S,
        instance: Instance,
        handle: Handle,
    ) -> i32 {
        if !valid(handle) {
            return 0;
        }
        let Some(part) = self.part(handle) else {
            return 0;
        };
        part.use_count.set(part.use_count.get() + 1);
        let (radius, centre) = services
            .instance_extent(instance)
            .unwrap_or((f32::from_bits(handle), [0.0; 3]));
        if radius <= 0.0 {
            let mesh_ref = part.mesh_ref;
            let _ = centre;
            services.set_instance_extent(instance, mesh_ref.radius, mesh_ref.center);
        }
        1
    }

    fn destroy_mesh_instance(
        &mut self,
        services: &mut S,
        _instance: Instance,
        handle: Handle,
    ) -> i32 {
        if !valid(handle) {
            return 0;
        }
        let Some(part) = self.part(handle) else {
            return 0;
        };
        part.use_count.set(part.use_count.get() - 1);
        if part.use_count.get() <= 0 {
            let mut registry = self.registry.borrow_mut();
            if let Some(group) = part
                .data
                .get()
                .and_then(|id| registry.entry(id))
                .and_then(|e| e.group)
            {
                registry.destroy_buffers(services, group);
            }
        }
        1
    }

    fn update_mesh_instance(&mut self, _services: &mut S, _instance: Instance, _arg: u32) -> i32 {
        0
    }

    fn query_mesh_instance(&mut self, _instance: Instance, handle: Handle, iid: MeshIid) -> i32 {
        <Self as RenderComponent<S>>::query_handle(self, handle, iid)
    }

    fn update_mesh_instances(&mut self, _services: &mut S, _arg: u32) -> i32 {
        0
    }

    fn query_geometry(
        &mut self,
        handle: Handle,
        _lod_bits: u32,
        query: GeometryQuery,
    ) -> (i32, Option<i32>) {
        if !valid(handle) {
            return (0, None);
        }
        let registry = self.registry.borrow();
        let Some(part) = self.part(handle).filter(|p| p.data.get().is_some()) else {
            return (0, None);
        };
        if !query.can_write() {
            return (0, None);
        }
        let value = if query.mode == 3 {
            i32::from(part.mesh_ref.vertex_count)
        } else {
            part.triangle_count(&registry)
        };
        (1, Some(value))
    }

    fn bounds(&mut self, handle: Handle, _lod: f32) -> (i32, Option<[f32; 6]>) {
        match self.part(handle).filter(|_| valid(handle)) {
            Some(part) => (1, Some(part.mesh_ref.bounds)),
            None => (0, None),
        }
    }

    fn centroid(&mut self, handle: Handle, _lod: f32) -> (i32, Option<[f32; 3]>) {
        match self.part(handle).filter(|_| valid(handle)) {
            Some(part) => (1, Some(part.mesh_ref.center)),
            None => (0, None),
        }
    }

    fn render_instance(
        &mut self,
        services: &mut S,
        camera: &mut dyn MeshCamera,
        _handle: Handle,
        instance: Instance,
        request: &DrawRequest,
    ) -> i32 {
        let flags = remap_draw_flags(request.flags);
        let Some(part) = self.bound.and_then(|h| self.part(h)) else {
            return 0;
        };
        let request = DrawRequest { flags, ..*request };
        let drawn = part.draw(&self.registry, services, camera, instance, &request);
        i32::from(drawn)
    }

    fn bind_mesh_instance(&mut self, _instance: Instance, handle: Handle) -> i32 {
        if !valid(handle) {
            return -1;
        }
        self.bound = Some(handle);
        0
    }
}

/// The flag word the `VMesh` and `TriMesh` plugins hand their draw: 0x40
/// without 0x4000, 2 for 0x1000, 0x80 for 0x8000.
#[must_use]
pub fn remap_draw_flags(flags: u32) -> u32 {
    let mut out = if flags & 0x4000 == 0 { 0x40 } else { 0 };
    if flags & 0x1000 != 0 {
        out |= 2;
    }
    if flags & 0x8000 != 0 {
        out |= 0x80;
    }
    out
}

#[cfg(test)]
mod tests {
    use super::{lod_filtered, remap_draw_flags};

    #[test]
    fn draw_flags_are_remapped() {
        assert_eq!(remap_draw_flags(0), 0x40);
        assert_eq!(remap_draw_flags(0x4000), 0);
        assert_eq!(remap_draw_flags(0x1000 | 0x8000 | 0x4000), 0x82);
    }

    #[test]
    fn lod_filter_reads_the_digit_after_lod() {
        assert!(lod_filtered(b"ship.lod0.vms", 1));
        assert!(!lod_filtered(b"ship.lod1.vms", 1));
        assert!(!lod_filtered(b"ship.vms", 1));
    }
}
