//! The `IMesh` and `IRenderableMesh` faces of the mesh kinds: the
//! `VMeshPart`'s (`FVMeshPartMesh`, `FVMeshPartRenderable` in
//! `VMeshPart.cpp`), the `TriMesh` archetype's (`FTriMeshArch`'s `IMesh`
//! slots and `FRenderableFacet` in `TriMesh.cpp`) and the `TriMesh`
//! instance's, which forwards to its live archetype (`FTriMeshAnim` in
//! `Plugins.cpp`).
//!
//! The reference hands these out as interface pointers into the part or
//! the archetype (`QueryHandle`, `QueryMeshInstance`); a [`MeshFace`] holds
//! the plugin's shared part or archetype instead, so that a material can
//! use it while the mesh draws (the wire materials ask the instance being
//! drawn for its edges). The plugins give the render manager a
//! [`FaceSource`] for that lookup (divergence D356).
//!
//! Each method is one slot, with the reference's results: 0 or a negative
//! `GENRESULT`, the outputs as values. A pointer the reference writes into
//! the mesh's own memory is the bytes from that pointer to the end of the
//! array it points into ([`FaceData`], [`FaceIndices`]), with its offset in
//! the `VMeshData` leaf where it points there.
//!
//! The `TriMesh`'s editing mode (mode 2 of `OpenFaceGroup`: the face group
//! made private, written, compacted and shared again on close) is not
//! ported: opening a face group in mode 2 fails, so the slots that need it
//! answer as the reference does outside that mode (divergence D358).

use std::cell::RefCell;
use std::rc::Rc;

use neuron_core::hash::name_crc32;
use neuron_core::mesh_data::fvf::Channel;

use super::services::MeshServices;
use super::trimesh::{TriMeshArchetype, TriMeshGroup};
use super::vmesh::{SharedRegistry, VMeshPart};
use super::{Handle, Instance, dump, file};
use crate::render::material::{GR_GENERIC, GR_OK, GR_UNSUPPORTED, MaterialId, VertexStream};

/// The result of a channel the face group lacks (`0xFFFFFFF8`).
pub const GR_NO_CHANNEL: i32 = -8;

/// Where the plugins' faces come from: `QueryHandle(handle, IID_IMesh)`
/// and `QueryMeshInstance(instance, handle, IID_IMesh)`.
pub trait FaceSource {
    /// The face of an archetype handle.
    fn face(&self, handle: Handle) -> Option<MeshFace>;
    /// The face of an instance on `handle`; by default the archetype's.
    fn instance_face(&self, instance: Instance, handle: Handle) -> Option<MeshFace> {
        let _ = instance;
        self.face(handle)
    }
}

/// `GetFaceGroupData`'s outputs: the channel's array (`*A`), its stride
/// (`*B`), element size (`*C`) and count (`*D`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct FaceData {
    /// The bytes from the pointer to the end of its array; `None` for a
    /// null pointer.
    pub bytes: Option<Vec<u8>>,
    /// The pointer's offset in the `VMeshData` leaf, for a `VMesh` part.
    pub leaf_offset: Option<usize>,
    /// `*B`.
    pub stride: u32,
    /// `*C`.
    pub element_bytes: u32,
    /// `*D`.
    pub count: u32,
}

impl FaceData {
    /// The array as little-endian words (what the surface materials read).
    #[must_use]
    pub fn words(&self) -> Option<Vec<u32>> {
        self.bytes.as_ref().map(|bytes| {
            bytes
                .as_chunks::<4>()
                .0
                .iter()
                .map(|w| u32::from_le_bytes(*w))
                .collect()
        })
    }
}

/// `GetFaceGroupIndices`' outputs: the index array (`*A`) and the count
/// (`*B`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct FaceIndices {
    /// The indices from the pointer to the end of its array; `None` for a
    /// null pointer.
    pub indices: Option<Vec<u16>>,
    /// The pointer's offset in the `VMeshData` leaf, for a `VMesh` part.
    pub leaf_offset: Option<usize>,
    /// `*B`.
    pub count: u32,
}

/// The editing state of a `TriMesh` archetype's `IMesh` face: the cursor
/// (`+0x1E8`, the group count meaning none), the mode (`+0x218`), the
/// locked channels (`+0x1EC`) and their index copies (`+0x1F0`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TriMeshEdit {
    /// The selected group; equal to the group count when none is.
    pub cursor: usize,
    /// The mode `OpenFaceGroup` was given.
    pub mode: i32,
    /// The locked channels.
    pub locked_mask: u32,
    /// The locked index copies, by `TriMeshLockedSlot`.
    pub locked: [Option<Vec<u16>>; 10],
}

/// A `TriMesh` archetype as its plugin shares it with the faces.
#[derive(Debug)]
pub struct TriMeshShared {
    /// The archetype.
    pub arch: RefCell<TriMeshArchetype>,
    /// Its face's editing state.
    pub edit: RefCell<TriMeshEdit>,
}

impl TriMeshShared {
    /// An archetype shared with its faces, with the cursor at `cursor` (the
    /// group count after a load or a clone: no group open).
    #[must_use]
    pub fn new(arch: TriMeshArchetype, cursor: usize) -> Rc<TriMeshShared> {
        Rc::new(TriMeshShared {
            arch: RefCell::new(arch),
            edit: RefCell::new(TriMeshEdit {
                cursor,
                ..TriMeshEdit::default()
            }),
        })
    }
}

/// An `IMesh` face (with the `IRenderableMesh` face where the mesh has
/// one).
#[derive(Clone, Debug)]
pub enum MeshFace {
    /// A `VMeshPart` and the registry its mesh lives in.
    VMesh {
        part: Rc<VMeshPart>,
        registry: SharedRegistry,
    },
    /// A `TriMesh` archetype (`instance` false) or an instance's face,
    /// which forwards to its live archetype.
    TriMesh {
        mesh: Rc<TriMeshShared>,
        instance: bool,
    },
}

/// The channel order of `LockFaceGroupChannels` and
/// `UnlockFaceGroupChannels`.
const LOCK_ORDER: [u32; 9] = [0x1, 0x8, 0x2, 0x4, 0x10, 0x4000, 0x8000, 0x10000, 0x20000];

/// `TriMeshLockedSlot`.
fn locked_slot(channel: u32) -> Option<usize> {
    Some(match channel {
        0x1 => 0,
        0x8 => 1,
        0x2 => 2,
        0x4 => 3,
        0x10 => 4,
        0x100 => 5,
        0x4000 => 6,
        0x8000 => 7,
        0x10000 => 8,
        0x20000 => 9,
        _ => return None,
    })
}

/// The stream of a vertex channel of the shared source.
fn channel_stream(arch: &TriMeshArchetype, channel: u32) -> Option<&VertexStream> {
    let v = &arch.vertices;
    Some(match channel {
        0x1 | 0x80 => &v.position,
        0x2 => &v.tex[0],
        0x4 => &v.tex[1],
        0x8 => &v.normal,
        0x10 => &v.diffuse,
        _ => return None,
    })
}

/// `FTriMeshGroup::HasChannels`. A group `AddFaceGroup` made has no vertex
/// source, which the reference reads through (divergence D357): it has no
/// vertex channel here.
fn has_channels(arch: &TriMeshArchetype, group: &TriMeshGroup, channels: u32) -> bool {
    if channels & 0x80 != 0 && group.edge_indices.is_none() {
        return false;
    }
    if channels & 0x100 != 0 && group.edge_angles.is_none() {
        return false;
    }
    if channels & 0x3_C000 != 0 {
        // No group has extra streams outside the editing mode.
        return false;
    }
    let low = channels & 0xFF;
    if !group.vertices {
        return low & 0x1F == 0;
    }
    [0x1, 0x8, 0x2, 0x4, 0x10]
        .into_iter()
        .filter(|bit| low & bit != 0)
        .all(|bit| channel_stream(arch, bit).is_some_and(|s| s.index.is_some()))
}

impl MeshFace {
    // --- the VMesh part -----------------------------------------------------------------------

    fn notice<S: MeshServices + ?Sized>(services: &mut S, line: u32, what: &str) -> i32 {
        let message = format!("VMPartArch {what}: not implemented");
        services.dump(dump::NOTICE, file::VM_PART_ARCH, line, &message);
        GR_UNSUPPORTED
    }

    /// The part's cursor as a face-group index of its mesh, `None` when no
    /// group is selected.
    fn vmesh_group(part: &VMeshPart) -> Option<usize> {
        let cursor = usize::try_from(part.cursor.get()).ok()?;
        Some(usize::from(part.mesh_ref.face_group_start) + cursor)
    }

    /// `ChannelPresent` on the part's mesh (a part without a mesh, which
    /// the reference reads through, has no channel: divergence D357).
    fn vmesh_present(part: &VMeshPart, registry: &SharedRegistry, channel: u32) -> bool {
        let registry = registry.borrow();
        part.data
            .get()
            .and_then(|id| registry.entry(id))
            .is_some_and(|e| e.data.fvf().has_channel_id(channel))
    }

    // --- the TriMesh archetype ----------------------------------------------------------------

    /// Runs `f` on the archetype and the editing state when a group is
    /// selected (the group's index), else -1. An archetype the face cannot
    /// read (while it is being changed) answers -1 too (divergence D356).
    fn tri_open<R>(
        mesh: &TriMeshShared,
        f: impl FnOnce(&TriMeshArchetype, &mut TriMeshEdit, usize) -> Result<R, i32>,
    ) -> Result<R, i32> {
        let Ok(arch) = mesh.arch.try_borrow() else {
            return Err(GR_GENERIC);
        };
        let Ok(mut edit) = mesh.edit.try_borrow_mut() else {
            return Err(GR_GENERIC);
        };
        let cursor = edit.cursor;
        if cursor == arch.groups.len() || cursor > arch.groups.len() {
            // At the end, or past it, where the reference reads outside the
            // group vector (divergence D357).
            return Err(GR_GENERIC);
        }
        f(&arch, &mut edit, cursor)
    }

    fn result(r: Result<(), i32>) -> i32 {
        match r {
            Ok(()) => GR_OK,
            Err(e) => e,
        }
    }

    /// The archetype's own `UnlockFaceGroupChannels` (outside the editing
    /// mode the copies are dropped).
    fn tri_unlock(edit: &mut TriMeshEdit) {
        if edit.locked_mask == 0 {
            return;
        }
        for bit in LOCK_ORDER {
            if edit.locked_mask & bit != 0
                && let Some(slot) = locked_slot(bit)
            {
                edit.locked[slot] = None;
            }
        }
        if edit.locked_mask & 0x100 != 0 {
            edit.locked[5] = None;
        }
        edit.locked_mask = 0;
    }

    // --- IMesh ---------------------------------------------------------------------------------

    /// Slot 3, `ResetMesh`: 0 for a part; a `TriMesh` archetype is emptied
    /// (its open group closed, every group's material released, the
    /// vertices, the UV animation and the cached bounds dropped).
    pub fn reset_mesh<S: MeshServices + ?Sized>(&self, services: &mut S) -> i32 {
        match self {
            MeshFace::VMesh { .. } => GR_OK,
            MeshFace::TriMesh { mesh, .. } => {
                let Ok(mut arch) = mesh.arch.try_borrow_mut() else {
                    return GR_GENERIC;
                };
                let mut edit = mesh.edit.borrow_mut();
                if edit.cursor != arch.groups.len() {
                    Self::tri_unlock(&mut edit);
                    edit.cursor = arch.groups.len();
                }
                arch.reset(services);
                edit.mode = 0;
                GR_OK
            }
        }
    }

    /// Slot 4: 0.
    #[must_use]
    pub fn no_op(&self) -> i32 {
        GR_OK
    }

    /// Slot 5, `RefreshMaterials`: 0; a `TriMesh` hooks its materials up
    /// again (the result dropped).
    pub fn refresh_materials<S: MeshServices + ?Sized>(&self, services: &mut S, flags: u32) -> i32 {
        let _ = flags;
        if let MeshFace::TriMesh { mesh, .. } = self
            && let Ok(mut arch) = mesh.arch.try_borrow_mut()
        {
            arch.hook_up(services);
        }
        GR_OK
    }

    /// Slots 6 and 7: -1 (-7 on a `TriMesh` instance).
    #[must_use]
    pub fn reject_operation(&self) -> i32 {
        match self {
            MeshFace::TriMesh { instance: true, .. } => GR_UNSUPPORTED,
            _ => GR_GENERIC,
        }
    }

    /// Slot 8, `GetFaceGroupCount`.
    #[must_use]
    pub fn face_group_count(&self) -> i32 {
        match self {
            MeshFace::VMesh { part, .. } => i32::from(part.mesh_ref.face_group_count),
            MeshFace::TriMesh { mesh, .. } => mesh
                .arch
                .try_borrow()
                .map_or(0, |a| i32::try_from(a.groups.len()).unwrap_or(i32::MAX)),
        }
    }

    /// Slot 9, `OpenFaceGroup(index, mode)`: -1 while a group is open or
    /// for an index past the count; a `TriMesh` refuses mode 2 (divergence
    /// D358).
    #[must_use]
    pub fn open_face_group(&self, index: i32, mode: i32) -> i32 {
        match self {
            MeshFace::VMesh { part, .. } => {
                if part.cursor.get() >= 0
                    || index.cast_unsigned() >= u32::from(part.mesh_ref.face_group_count)
                {
                    return GR_GENERIC;
                }
                part.cursor.set(index);
                GR_OK
            }
            MeshFace::TriMesh { mesh, .. } => {
                let Ok(arch) = mesh.arch.try_borrow() else {
                    return GR_GENERIC;
                };
                let mut edit = mesh.edit.borrow_mut();
                if edit.cursor != arch.groups.len()
                    || index.cast_unsigned() as usize >= arch.groups.len()
                    || mode == 2
                {
                    return GR_GENERIC;
                }
                edit.mode = mode;
                edit.cursor = index.cast_unsigned() as usize;
                GR_OK
            }
        }
    }

    /// Slot 10, `CloseFaceGroup`: the channels closed and unlocked, the
    /// cursor cleared; -1 without an open group.
    #[must_use]
    pub fn close_face_group(&self) -> i32 {
        match self {
            MeshFace::VMesh { part, .. } => {
                if part.cursor.get() < 0 {
                    return GR_GENERIC;
                }
                part.cursor.set(-1);
                GR_OK
            }
            MeshFace::TriMesh { mesh, .. } => {
                let Ok(arch) = mesh.arch.try_borrow() else {
                    return GR_GENERIC;
                };
                let mut edit = mesh.edit.borrow_mut();
                if edit.cursor == arch.groups.len() {
                    return GR_GENERIC;
                }
                if edit.cursor < arch.groups.len() {
                    Self::tri_unlock(&mut edit);
                }
                edit.cursor = arch.groups.len();
                GR_OK
            }
        }
    }

    /// Slot 11, `AddFaceGroup(prim)`: a part refuses with a notice; a
    /// `TriMesh` without an open group appends an empty group.
    pub fn add_face_group<S: MeshServices + ?Sized>(&self, services: &mut S, prim: u32) -> i32 {
        match self {
            MeshFace::VMesh { .. } => Self::notice(services, 0x9E, "add_facegroup"),
            MeshFace::TriMesh { mesh, .. } => {
                let Ok(mut arch) = mesh.arch.try_borrow_mut() else {
                    return GR_GENERIC;
                };
                let mut edit = mesh.edit.borrow_mut();
                if edit.cursor != arch.groups.len() {
                    return GR_GENERIC;
                }
                arch.groups.push(TriMeshGroup::empty(prim));
                edit.cursor = arch.groups.len();
                GR_OK
            }
        }
    }

    /// Slot 12, `RemoveFaceGroup(index)`: a part refuses with a notice; a
    /// `TriMesh` erases the group without releasing its material (as the
    /// reference leaks it); an index past the groups erases nothing
    /// (divergence D357).
    pub fn remove_face_group<S: MeshServices + ?Sized>(&self, services: &mut S, index: i32) -> i32 {
        match self {
            MeshFace::VMesh { .. } => Self::notice(services, 0xA5, "remove_facegroup"),
            MeshFace::TriMesh { mesh, .. } => {
                let Ok(mut arch) = mesh.arch.try_borrow_mut() else {
                    return GR_GENERIC;
                };
                let mut edit = mesh.edit.borrow_mut();
                edit.cursor = arch.groups.len();
                if arch.groups.is_empty() {
                    return GR_GENERIC;
                }
                if let Ok(at) = usize::try_from(index)
                    && at < arch.groups.len()
                {
                    arch.groups.remove(at);
                }
                edit.cursor = arch.groups.len();
                GR_OK
            }
        }
    }

    /// Slot 13, `GetFaceGroupPrimitiveType`.
    ///
    /// # Errors
    ///
    /// -1 without an open group.
    pub fn primitive_type(&self) -> Result<u32, i32> {
        match self {
            MeshFace::VMesh { part, registry } => {
                if part.cursor.get() < 0 {
                    return Err(GR_GENERIC);
                }
                let registry = registry.borrow();
                part.data
                    .get()
                    .and_then(|id| registry.entry(id))
                    .map(|e| e.data.header.primitive_type)
                    .ok_or(GR_GENERIC)
            }
            MeshFace::TriMesh { mesh, .. } => {
                Self::tri_open(mesh, |arch, _, cursor| Ok(arch.groups[cursor].prim))
            }
        }
    }

    /// Slot 14, `GetFaceGroupMaterial`: the open group's material with a
    /// reference for the caller (`None` for none, or for a `TriMesh` group
    /// without a material CRC).
    ///
    /// # Errors
    ///
    /// -1 without an open group.
    pub fn face_group_material<S: MeshServices + ?Sized>(
        &self,
        services: &mut S,
    ) -> Result<Option<MaterialId>, i32> {
        let material = match self {
            MeshFace::VMesh { part, registry } => {
                let group = Self::vmesh_group(part).ok_or(GR_GENERIC)?;
                let registry = registry.borrow();
                let id = part.data.get().ok_or(GR_GENERIC)?;
                registry
                    .entry(id)
                    .and_then(|e| e.materials.get(group).copied().flatten())
            }
            MeshFace::TriMesh { mesh, .. } => Self::tri_open(mesh, |arch, _, cursor| {
                let group = &arch.groups[cursor];
                Ok(if group.material_crc == 0 {
                    None
                } else {
                    group.material
                })
            })?,
        };
        if let Some(material) = material {
            services.add_ref_material(material);
        }
        Ok(material)
    }

    /// Slot 15, `SetFaceGroupMaterial`: a part refuses with a notice. A
    /// `TriMesh` releases the group's material twice (as the reference
    /// does), clears its CRC, and for a new material takes two references
    /// and the CRC of its name.
    pub fn set_face_group_material<S: MeshServices + ?Sized>(
        &self,
        services: &mut S,
        material: Option<MaterialId>,
    ) -> i32 {
        match self {
            MeshFace::VMesh { .. } => Self::notice(services, 0xD2, "set_facegroup_material"),
            MeshFace::TriMesh { mesh, .. } => {
                let Ok(mut arch) = mesh.arch.try_borrow_mut() else {
                    return GR_GENERIC;
                };
                let cursor = mesh.edit.borrow().cursor;
                let Some(group) = arch.groups.get_mut(cursor) else {
                    return GR_GENERIC;
                };
                if let Some(old) = group.material.take() {
                    services.release_material(old);
                    services.release_material(old);
                }
                group.material_crc = 0;
                let Some(new) = material else {
                    return GR_OK;
                };
                group.material = Some(new);
                services.add_ref_material(new);
                services.add_ref_material(new);
                if let Some(name) = services.material_name(new) {
                    group.material_crc = name_crc32(&name);
                }
                GR_OK
            }
        }
    }

    /// Slot 16, `GetFaceGroupMaterialContext`: a part refuses with a notice
    /// (after the cursor test); a `TriMesh` group's `+0x0C`.
    ///
    /// # Errors
    ///
    /// -1 without an open group, -7 for a part.
    pub fn material_context<S: MeshServices + ?Sized>(
        &self,
        services: &mut S,
    ) -> Result<usize, i32> {
        match self {
            MeshFace::VMesh { part, .. } => {
                if part.cursor.get() < 0 {
                    return Err(GR_GENERIC);
                }
                Err(Self::notice(
                    services,
                    0xE0,
                    "get_facegroup_material_context",
                ))
            }
            MeshFace::TriMesh { mesh, .. } => {
                Self::tri_open(mesh, |arch, _, cursor| Ok(arch.groups[cursor].field20))
            }
        }
    }

    /// Slot 17, `SetFaceGroupMaterialContext`.
    pub fn set_material_context<S: MeshServices + ?Sized>(
        &self,
        services: &mut S,
        value: usize,
    ) -> i32 {
        match self {
            MeshFace::VMesh { part, .. } => {
                if part.cursor.get() < 0 {
                    return GR_GENERIC;
                }
                Self::notice(services, 0xEE, "set_facegroup_material_context")
            }
            MeshFace::TriMesh { mesh, .. } => {
                let Ok(mut arch) = mesh.arch.try_borrow_mut() else {
                    return GR_GENERIC;
                };
                let cursor = mesh.edit.borrow().cursor;
                match arch.groups.get_mut(cursor) {
                    Some(group) => {
                        group.field20 = value;
                        GR_OK
                    }
                    None => GR_GENERIC,
                }
            }
        }
    }

    /// Slot 18, `OpenFaceGroupChannels(flags)`: 0, -8 for a channel the
    /// group lacks, -1 without an open group.
    #[must_use]
    pub fn open_channels(&self, flags: u32) -> i32 {
        match self {
            MeshFace::VMesh { part, registry } => {
                if part.cursor.get() < 0 {
                    GR_GENERIC
                } else if Self::vmesh_present(part, registry, flags) {
                    GR_OK
                } else {
                    GR_NO_CHANNEL
                }
            }
            MeshFace::TriMesh { mesh, .. } => Self::result(Self::tri_open(mesh, |arch, _, c| {
                if has_channels(arch, &arch.groups[c], flags) {
                    Ok(())
                } else {
                    Err(GR_NO_CHANNEL)
                }
            })),
        }
    }

    /// Slots 19 and 21 of a part, 19 of a `TriMesh`: 0 while a group is
    /// open, else -1.
    #[must_use]
    pub fn close_channels(&self) -> i32 {
        match self {
            MeshFace::VMesh { part, .. } => {
                if part.cursor.get() >= 0 {
                    GR_OK
                } else {
                    GR_GENERIC
                }
            }
            MeshFace::TriMesh { mesh, .. } => Self::result(Self::tri_open(mesh, |_, _, _| Ok(()))),
        }
    }

    /// Slot 20, `LockFaceGroupChannels(mask)`: a part answers as slot 18. A
    /// `TriMesh` copies each vertex channel's corner indices of the group
    /// (`LockChannel`), marks the edges and numbers the edge angles; 0 when
    /// every channel of the mask is locked.
    #[must_use]
    pub fn lock_channels(&self, mask: u32) -> i32 {
        match self {
            MeshFace::VMesh { .. } => self.open_channels(mask),
            MeshFace::TriMesh { mesh, .. } => {
                Self::result(Self::tri_open(mesh, |arch, edit, cursor| {
                    let group = &arch.groups[cursor];
                    if !has_channels(arch, group, mask) {
                        return Err(GR_NO_CHANNEL);
                    }
                    for bit in LOCK_ORDER {
                        if mask & bit != 0 && edit.locked_mask & bit == 0 {
                            let copy = channel_stream(arch, bit)
                                .and_then(|s| s.index.as_ref())
                                .map(|source| {
                                    group
                                        .indices
                                        .iter()
                                        .map(|&i| {
                                            let at = group.min_index as usize + usize::from(i);
                                            source.get(at).copied().unwrap_or(0) as u16
                                        })
                                        .collect::<Vec<u16>>()
                                })
                                .ok_or(GR_GENERIC)?;
                            let slot = locked_slot(bit).ok_or(GR_GENERIC)?;
                            edit.locked[slot] = Some(copy);
                            edit.locked_mask |= bit;
                        }
                    }
                    if mask & 0x80 != 0 {
                        edit.locked_mask |= 0x80;
                    }
                    if mask & 0x100 != 0 && edit.locked_mask & 0x100 == 0 {
                        let angles = group.edge_angles.as_ref().ok_or(GR_GENERIC)?;
                        let count = group.edge_count() >> 1;
                        let _ = angles;
                        edit.locked[5] = Some((0..count).map(|i| i as u16).collect());
                        edit.locked_mask |= 0x100;
                    }
                    if edit.locked_mask & mask == mask {
                        Ok(())
                    } else {
                        Err(GR_GENERIC)
                    }
                }))
            }
        }
    }

    /// Slot 21, `UnlockFaceGroupChannels`.
    #[must_use]
    pub fn unlock_channels(&self) -> i32 {
        match self {
            MeshFace::VMesh { .. } => self.close_channels(),
            MeshFace::TriMesh { mesh, .. } => Self::result(Self::tri_open(mesh, |_, edit, _| {
                Self::tri_unlock(edit);
                Ok(())
            })),
        }
    }

    /// Slot 22, `GetFaceGroupData(channel)`: a part's channel over its
    /// vertex range; a `TriMesh` group's vertex stream, its edge angles
    /// (0x100) or extra stream.
    ///
    /// # Errors
    ///
    /// -1 without an open group, for a channel the group lacks or one the
    /// face does not name.
    pub fn face_group_data(&self, channel: u32) -> Result<FaceData, i32> {
        match self {
            MeshFace::VMesh { part, registry } => {
                if part.cursor.get() < 0 || !Self::vmesh_present(part, registry, channel) {
                    return Err(GR_GENERIC);
                }
                let registry = registry.borrow();
                let entry = part
                    .data
                    .get()
                    .and_then(|id| registry.entry(id))
                    .ok_or(GR_GENERIC)?;
                let id = Channel::from_id(channel).ok_or(GR_GENERIC)?;
                let record = entry
                    .data
                    .channel_record(
                        id,
                        u32::from(part.mesh_ref.vertex_start),
                        u32::from(part.mesh_ref.vertex_count),
                    )
                    .ok_or(GR_GENERIC)?;
                let at = usize::try_from(record.offset).unwrap_or(usize::MAX);
                Ok(FaceData {
                    bytes: Some(entry.data.vertices.get(at..).unwrap_or_default().to_vec()),
                    leaf_offset: Some(entry.data.header.vertices_offset().wrapping_add(at)),
                    stride: record.stride,
                    element_bytes: record.element_bytes,
                    count: record.count,
                })
            }
            MeshFace::TriMesh { mesh, .. } => Self::tri_open(mesh, |arch, _, cursor| {
                let group = &arch.groups[cursor];
                if !has_channels(arch, group, channel) {
                    return Err(GR_GENERIC);
                }
                if channel == 0x100 {
                    return Ok(FaceData {
                        bytes: group
                            .edge_angles
                            .as_ref()
                            .map(|a| a.iter().flat_map(|v| v.to_le_bytes()).collect()),
                        leaf_offset: None,
                        stride: 4,
                        element_bytes: 4,
                        count: group.edge_count() >> 1,
                    });
                }
                let stream = channel_stream(arch, channel).ok_or(GR_GENERIC)?;
                Ok(FaceData {
                    bytes: stream.data.clone(),
                    leaf_offset: None,
                    stride: stream.stride,
                    element_bytes: stream.byte_size,
                    count: stream.count,
                })
            }),
        }
    }

    /// Slot 23, `SetFaceGroupData`: a part refuses with a notice; a
    /// `TriMesh` needs the editing mode (-1 outside it).
    pub fn set_face_group_data<S: MeshServices + ?Sized>(&self, services: &mut S) -> i32 {
        match self {
            MeshFace::VMesh { .. } => Self::notice(services, 0x13F, "set_facegroup_data"),
            MeshFace::TriMesh { mesh, .. } => Self::result(Self::tri_open(mesh, |_, edit, _| {
                if edit.mode == 2 {
                    Ok(())
                } else {
                    Err(GR_GENERIC)
                }
            })),
        }
    }

    /// Slot 24, `GetFaceGroupIndices(channel)`. A part answers the open
    /// group's index count with the leaf's indices from index
    /// `3 * (FacegroupStart + Cursor)` on (the reference scales the group
    /// index by 6 bytes, whatever the channel). A `TriMesh` answers the
    /// edge indices (0x80) or a locked copy.
    ///
    /// # Errors
    ///
    /// -1 without an open group or for a channel without a lock slot.
    pub fn face_group_indices(&self, channel: u32) -> Result<FaceIndices, i32> {
        match self {
            MeshFace::VMesh { part, registry } => {
                let group = Self::vmesh_group(part).ok_or(GR_GENERIC)?;
                let registry = registry.borrow();
                let entry = part
                    .data
                    .get()
                    .and_then(|id| registry.entry(id))
                    .ok_or(GR_GENERIC)?;
                let count = entry
                    .data
                    .face_groups
                    .get(group)
                    .map_or(0, |g| u32::from(g.index_count));
                let first = group * 3;
                Ok(FaceIndices {
                    indices: Some(entry.data.indices.get(first..).unwrap_or_default().to_vec()),
                    leaf_offset: Some(entry.data.header.indices_offset() + first * 2),
                    count,
                })
            }
            MeshFace::TriMesh { mesh, .. } => Self::tri_open(mesh, |arch, edit, cursor| {
                let group = &arch.groups[cursor];
                if channel == 0x80 {
                    return Ok(FaceIndices {
                        indices: group.edge_indices.clone(),
                        leaf_offset: None,
                        count: group.edge_count(),
                    });
                }
                let slot = locked_slot(channel).ok_or(GR_GENERIC)?;
                Ok(FaceIndices {
                    indices: edit.locked[slot].clone(),
                    leaf_offset: None,
                    count: if channel == 0x100 {
                        group.edge_count() >> 1
                    } else {
                        u32::try_from(group.indices.len()).unwrap_or(u32::MAX)
                    },
                })
            }),
        }
    }

    /// Slot 25, `SetFaceGroupIndices`: a part refuses with a notice; a
    /// `TriMesh` needs the editing mode.
    pub fn set_face_group_indices<S: MeshServices + ?Sized>(&self, services: &mut S) -> i32 {
        self.edit_only(services, 0x188, "set_facegroup_indices")
    }

    /// Slot 26, `GetFaceGroupIndexCount(selector)`: selector 1 the index
    /// count, and on a `TriMesh` 2 the edge count.
    ///
    /// # Errors
    ///
    /// -1 without an open group or for another selector.
    pub fn index_count(&self, selector: i32) -> Result<u32, i32> {
        match self {
            MeshFace::VMesh { part, registry } => {
                let group = Self::vmesh_group(part).ok_or(GR_GENERIC)?;
                if selector != 1 {
                    return Err(GR_GENERIC);
                }
                let registry = registry.borrow();
                Ok(part
                    .data
                    .get()
                    .and_then(|id| registry.entry(id))
                    .and_then(|e| e.data.face_groups.get(group))
                    .map_or(0, |g| u32::from(g.index_count)))
            }
            MeshFace::TriMesh { mesh, .. } => Self::tri_open(mesh, |arch, _, cursor| {
                let group = &arch.groups[cursor];
                match selector {
                    1 => Ok(u32::try_from(group.indices.len()).unwrap_or(u32::MAX)),
                    2 => Ok(group.edge_count()),
                    _ => Err(GR_GENERIC),
                }
            }),
        }
    }

    /// Slot 27, `SetFaceGroupIndexCount`.
    pub fn set_index_count<S: MeshServices + ?Sized>(&self, services: &mut S) -> i32 {
        self.edit_only(services, 0x165, "set_facegroup_indices_count")
    }

    /// Slot 28, `AddFaceGroupChannels`.
    pub fn add_channels<S: MeshServices + ?Sized>(&self, services: &mut S) -> i32 {
        self.edit_only(services, 0x18F, "add_elements")
    }

    /// Slot 29, `RemoveFaceGroupChannels`.
    pub fn remove_channels<S: MeshServices + ?Sized>(&self, services: &mut S) -> i32 {
        self.edit_only(services, 0x196, "remove_elements")
    }

    /// A slot a part refuses with a notice and a `TriMesh` performs only in
    /// the editing mode, which is never open (D358): -1.
    fn edit_only<S: MeshServices + ?Sized>(&self, services: &mut S, line: u32, what: &str) -> i32 {
        match self {
            MeshFace::VMesh { .. } => Self::notice(services, line, what),
            MeshFace::TriMesh { mesh, .. } => Self::result(Self::tri_open(mesh, |_, edit, _| {
                if edit.mode == 2 {
                    Ok(())
                } else {
                    Err(GR_GENERIC)
                }
            })),
        }
    }

    // --- IRenderableMesh -----------------------------------------------------------------------

    /// Whether the face has `IRenderableMesh` (a `TriMesh` instance has
    /// not).
    #[must_use]
    pub fn is_renderable(&self) -> bool {
        !matches!(self, MeshFace::TriMesh { instance: true, .. })
    }

    /// `IRenderableMesh` slot 3, `GetVertexCount`: the part's
    /// `VMeshRef.VertexCount`, the `TriMesh`'s `SrcCount`.
    #[must_use]
    pub fn vertex_count(&self) -> i32 {
        match self {
            MeshFace::VMesh { part, .. } => i32::from(part.mesh_ref.vertex_count),
            MeshFace::TriMesh { mesh, .. } => mesh
                .arch
                .try_borrow()
                .map_or(0, |a| a.vertices.count.cast_signed()),
        }
    }

    /// `IRenderableMesh` slot 4, `CopyVertexData(dest, fvf, start,
    /// count)` through the vertex-buffer manager from the mesh's vertex
    /// source.
    pub fn copy_vertex_data<S: MeshServices + ?Sized>(
        &self,
        services: &mut S,
        dest: &mut [u8],
        fvf: u32,
        start: u32,
        count: u32,
    ) -> i32 {
        match self {
            MeshFace::VMesh { part, .. } => {
                let vertices = part.vertices.borrow();
                services.copy_vertex_data(dest, fvf, &vertices, start, count)
            }
            MeshFace::TriMesh { mesh, .. } => match mesh.arch.try_borrow() {
                Ok(arch) => services.copy_vertex_data(dest, fvf, &arch.vertices, start, count),
                Err(_) => GR_GENERIC,
            },
        }
    }

    /// `IRenderableMesh` slot 5, `GetIndexCount`: the part's
    /// `VMeshRef.IndexCount`, the first `TriMesh` group's count.
    ///
    /// # Errors
    ///
    /// -1 for a `TriMesh` without a group, which the reference reads
    /// through (divergence D357).
    pub fn total_index_count(&self) -> Result<i32, i32> {
        match self {
            MeshFace::VMesh { part, .. } => Ok(i32::from(part.mesh_ref.index_count)),
            MeshFace::TriMesh { mesh, .. } => {
                let arch = mesh.arch.try_borrow().map_err(|_| GR_GENERIC)?;
                arch.groups
                    .first()
                    .map(|g| i32::try_from(g.indices.len()).unwrap_or(i32::MAX))
                    .ok_or(GR_GENERIC)
            }
        }
    }

    /// `IRenderableMesh` slot 6, `CopyIndices(dest, count)`: `count`
    /// indices when it is the count slot 5 gives (a part's from the leaf's
    /// index `3 * FacegroupStart` on, the reference's scaling; missing ones
    /// read as 0, divergence D354).
    ///
    /// # Errors
    ///
    /// -1 for another count.
    pub fn copy_indices(&self, count: i32) -> Result<Vec<u16>, i32> {
        match self {
            MeshFace::VMesh { part, registry } => {
                let expected = u32::from(part.mesh_ref.index_count);
                if count.cast_unsigned() != expected {
                    return Err(GR_GENERIC);
                }
                let registry = registry.borrow();
                let entry = part
                    .data
                    .get()
                    .and_then(|id| registry.entry(id))
                    .ok_or(GR_GENERIC)?;
                let first = usize::from(part.mesh_ref.face_group_start) * 3;
                Ok((0..expected as usize)
                    .map(|i| entry.data.indices.get(first + i).copied().unwrap_or(0))
                    .collect())
            }
            MeshFace::TriMesh { mesh, .. } => {
                let arch = mesh.arch.try_borrow().map_err(|_| GR_GENERIC)?;
                let first = arch.groups.first().ok_or(GR_GENERIC)?;
                if count.cast_unsigned() as usize != first.indices.len() {
                    return Err(GR_GENERIC);
                }
                Ok(first.indices.clone())
            }
        }
    }
}
