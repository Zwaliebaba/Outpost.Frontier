//! `FRenderManager` (`RenderManager.cpp`): the render archetypes by engine
//! handle, each a [`LodSet`] of plugin handles; their creation from an
//! archive node (`MultiLevel`, else the first plugin that claims the
//! node); instances; the `IRenderer2` queries; and `RenderInstance`.
//!
//! The plugins are the `IRenderComponent` objects `LoadPlugins` creates by
//! the names of the `[RenderManager]` section ([`plugin_names`]); here they
//! are [`RenderComponent`] values the caller makes and hands over in that
//! order ([`super::VMeshPlugin`], [`super::TriMeshPlugin`],
//! [`super::SpherePlugin`]).

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::rc::Rc;

use neuron_core::math::{
    Transform, Vector, mul_transform, rotate, transform_xform, transpose_transform,
};
use neuron_core::vfs::FileSystem;

use super::face::{FaceSource, MeshFace};
use super::lod::{LodEntry, LodSet};
use super::services::{MeshCamera, MeshServices};
use super::{
    Archetype, HANDLE_NONE, Handle, Instance, MeshIid, child_fs, dump, file, read_leaf, transform,
};
use crate::render::material::MaterialId;

/// The far distance of a single level (`G_DefaultLodDistance`).
pub const DEFAULT_LOD_DISTANCE: f32 = 1_000_000.0;

/// `RenderInstance` flag: pick the level by distance.
pub const DRAW_PICK_SWITCH: u32 = 0x08;
/// `RenderInstance` flag: rotate the matrix offset by its own rotation.
pub const DRAW_ROTATE_OFFSET: u32 = 0x10;
/// `RenderInstance` flag: the matrix becomes `world * matrix * world^-1`.
pub const DRAW_CONJUGATE: u32 = 0x20;
/// `RenderInstance` flag: the matrix is used as given.
pub const DRAW_MATRIX_READY: u32 = 0x40;
/// `RenderInstance` flag: draw the current level.
pub const DRAW_KEEP_CURRENT: u32 = 0x80;
/// `RenderInstance` flag: no `ClassifySphere`, rank 4.
pub const DRAW_NO_CULL: u32 = 0x100;
/// `RenderInstance` flag: no recursion into child instances.
pub const DRAW_NO_CHILDREN: u32 = 0x2000;

/// The geometry query of `IRenderComponent` slot 12 (`QueryGeometry`):
/// the mode (argument A), the flags (B) and whether the output pointer (D)
/// is set.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GeometryQuery {
    /// 0 or 2: triangles, 3: vertices.
    pub mode: i32,
    /// Low twelve bits 3 and bit 0x4000 are required.
    pub flags: u32,
    /// The output pointer is not null.
    pub out: bool,
}

impl GeometryQuery {
    /// `IrcSlot12CanWrite`: whether a plugin writes the count.
    #[must_use]
    pub fn can_write(&self) -> bool {
        matches!(self.mode, 0 | 2 | 3)
            && self.flags & 0xFFF == 3
            && self.flags & 0x4000 != 0
            && self.out
    }
}

/// What a draw passes down: the flags, the matrix and the override
/// material (`Extra`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DrawRequest {
    /// The flag word.
    pub flags: u32,
    /// The matrix, `None` for a null one.
    pub matrix: Option<Transform>,
    /// The material that replaces every face group's (`Extra`).
    pub extra: Option<MaterialId>,
}

/// An `IRenderComponent` (a render plugin). Handles are the plugin's;
/// [`HANDLE_NONE`] stands for -1 and is never a valid one, nor is 0.
pub trait RenderComponent<S: ?Sized> {
    /// `Claim(fs)`: the archetype handle, [`HANDLE_NONE`] when the plugin
    /// does not claim the node.
    fn claim(&mut self, services: &mut S, fs: &mut dyn FileSystem) -> Handle;
    /// `CloneHandle` (slot 4).
    fn clone_handle(&mut self, services: &mut S, handle: Handle) -> Handle;
    /// `DestroyHandle`.
    fn destroy_handle(&mut self, services: &mut S, handle: Handle);
    /// `QueryHandle` (slot 6): the result.
    fn query_handle(&mut self, handle: Handle, iid: MeshIid) -> i32;
    /// `CreateMeshInstance` (slot 7): non-zero on success.
    fn create_mesh_instance(&mut self, services: &mut S, instance: Instance, handle: Handle)
    -> i32;
    /// `DestroyMeshInstance` (slot 8).
    fn destroy_mesh_instance(
        &mut self,
        services: &mut S,
        instance: Instance,
        handle: Handle,
    ) -> i32;
    /// `UpdateMeshInstance(instance, arg)` (slot 9); `arg` is the dword
    /// the engine passes (a float's bits for the UV animation).
    fn update_mesh_instance(&mut self, services: &mut S, instance: Instance, arg: u32) -> i32;
    /// `QueryMeshInstance` (slot 10): the result.
    fn query_mesh_instance(&mut self, instance: Instance, handle: Handle, iid: MeshIid) -> i32;
    /// `UpdateMeshInstances(arg)` (slot 11).
    fn update_mesh_instances(&mut self, services: &mut S, arg: u32) -> i32;
    /// `QueryGeometry` (slot 12): the result and the count written.
    fn query_geometry(
        &mut self,
        handle: Handle,
        lod_bits: u32,
        query: GeometryQuery,
    ) -> (i32, Option<i32>);
    /// `GetBounds` (slot 13): the result and the box written (max x, min
    /// x, max y, min y, max z, min z).
    fn bounds(&mut self, handle: Handle, lod: f32) -> (i32, Option<[f32; 6]>);
    /// `GetCentroid` (slot 14): the result and the centre written.
    fn centroid(&mut self, handle: Handle, lod: f32) -> (i32, Option<[f32; 3]>);
    /// `RenderInstance` (slot 16).
    fn render_instance(
        &mut self,
        services: &mut S,
        camera: &mut dyn MeshCamera,
        handle: Handle,
        instance: Instance,
        request: &DrawRequest,
    ) -> i32;
    /// `BindMeshInstance` (slot 17).
    fn bind_mesh_instance(&mut self, instance: Instance, handle: Handle) -> i32;
    /// `QueryMeshInstance(instance, IID_ISphereMesh)`: the sphere instance,
    /// for the planet plugin.
    fn sphere_mesh(&mut self, instance: Instance) -> Option<&mut super::sphere::SphereInstance> {
        let _ = instance;
        None
    }
    /// The plugin's last `Release`: what its destructor reports.
    fn destroy(&mut self, services: &mut S) {
        let _ = services;
    }
}

/// The plugin names of the `[RenderManager]` section (`LoadPlugins`):
/// every line not starting with `;`, cut at its first tab, space or `;`.
#[must_use]
pub fn plugin_names(lines: &[&[u8]]) -> Vec<Vec<u8>> {
    lines
        .iter()
        .filter(|line| line.first() != Some(&b';'))
        .map(|line| {
            let end = line
                .iter()
                .position(|&c| matches!(c, 0 | b'\t' | b' ' | b';'))
                .unwrap_or(line.len());
            line[..end].to_vec()
        })
        .collect()
}

/// The archetypes' levels by engine handle, shared with the
/// [`MeshDirectory`].
pub type SharedLodSets = Rc<RefCell<BTreeMap<Archetype, LodSet>>>;

/// The render manager: archetypes and plugins, and the plugins' faces.
///
/// The levels are shared with the [`MeshDirectory`] the services use to
/// answer `QueryArchetype` and `QueryInstance` for `IID_IMesh` while a
/// mesh draws; no borrow of them is held across a plugin's draw
/// (divergence D356).
pub struct RenderManager<S: ?Sized> {
    archetypes: SharedLodSets,
    plugins: Vec<Box<dyn RenderComponent<S>>>,
    faces: Vec<Option<Rc<dyn FaceSource>>>,
}

/// What `QueryArchetype` and `QueryInstance` reach for `IID_IMesh`: the
/// current level of an archetype and its plugin's face (the reference
/// hands out the interface pointer of the part or archetype).
#[derive(Clone)]
pub struct MeshDirectory {
    archetypes: SharedLodSets,
    faces: Vec<Option<Rc<dyn FaceSource>>>,
}

impl std::fmt::Debug for MeshDirectory {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("MeshDirectory")
            .field("archetypes", &self.archetypes.borrow().len())
            .finish_non_exhaustive()
    }
}

impl MeshDirectory {
    /// The current level's entry and its face source.
    fn current(&self, archetype: Archetype) -> Result<(LodEntry, &dyn FaceSource), i32> {
        let archetypes = self.archetypes.borrow();
        let set = archetypes.get(&archetype).ok_or(GR_FAIL)?;
        let entry = usize::try_from(set.current)
            .ok()
            .and_then(|index| set.entries.get(index))
            .copied()
            .ok_or(-8)?;
        let plugin = entry.plugin.ok_or(-8)?;
        let source = self
            .faces
            .get(plugin)
            .and_then(Option::as_deref)
            .ok_or(GR_FAIL)?;
        Ok((entry, source))
    }

    /// `QueryArchetype(archetype, IID_IMesh)`: the current level's face.
    ///
    /// # Errors
    ///
    /// As the reference: -1 for an unknown archetype or a level without
    /// the face, -8 without a current level.
    pub fn archetype_face(&self, archetype: Archetype) -> Result<MeshFace, i32> {
        let (entry, source) = self.current(archetype)?;
        source.face(entry.handle).ok_or(GR_FAIL)
    }

    /// `QueryInstance(instance, IID_IMesh)` once the engine has named the
    /// instance's archetype: the current level's instance face.
    ///
    /// # Errors
    ///
    /// As [`Self::archetype_face`].
    pub fn instance_face(&self, archetype: Archetype, instance: Instance) -> Result<MeshFace, i32> {
        let (entry, source) = self.current(archetype)?;
        source.instance_face(instance, entry.handle).ok_or(GR_FAIL)
    }
}

/// `GR_GENERIC`.
const GR_FAIL: i32 = -1;

impl<S: ?Sized> std::fmt::Debug for RenderManager<S> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RenderManager")
            .field("archetypes", &self.archetypes.borrow())
            .field("plugins", &self.plugins.len())
            .finish()
    }
}

impl<S: MeshServices + ?Sized> RenderManager<S> {
    /// A manager with the plugins `LoadPlugins` made, in list order.
    #[must_use]
    pub fn new(plugins: Vec<Box<dyn RenderComponent<S>>>) -> RenderManager<S> {
        let faces = plugins.iter().map(|_| None).collect();
        RenderManager {
            archetypes: Rc::new(RefCell::new(BTreeMap::new())),
            plugins,
            faces,
        }
    }

    /// Gives plugin `plugin`'s faces to the directory.
    pub fn set_faces(&mut self, plugin: usize, source: Rc<dyn FaceSource>) {
        if let Some(slot) = self.faces.get_mut(plugin) {
            *slot = Some(source);
        }
    }

    /// The directory of the archetypes' faces, sharing the levels.
    #[must_use]
    pub fn directory(&self) -> MeshDirectory {
        MeshDirectory {
            archetypes: Rc::clone(&self.archetypes),
            faces: self.faces.clone(),
        }
    }

    /// The plugins.
    pub fn plugins_mut(&mut self) -> &mut [Box<dyn RenderComponent<S>>] {
        &mut self.plugins
    }

    /// The levels of an archetype.
    #[must_use]
    pub fn lod_set(&self, archetype: Archetype) -> Option<LodSet> {
        self.archetypes.borrow().get(&archetype).cloned()
    }

    /// The archetypes, ascending.
    #[must_use]
    pub fn archetypes(&self) -> Vec<(Archetype, LodSet)> {
        self.archetypes
            .borrow()
            .iter()
            .map(|(&id, set)| (id, set.clone()))
            .collect()
    }

    /// `FRenderManager::MultiLevel`: 0, or -1 when the node has no
    /// `MultiLevel` child or a level is not claimed.
    fn multi_level(&mut self, services: &mut S, id: Archetype, fs: &dyn FileSystem) -> i32 {
        let Self {
            archetypes,
            plugins,
            ..
        } = self;
        let mut archetypes = archetypes.borrow_mut();
        let Some(set) = archetypes.get_mut(&id) else {
            return -1;
        };
        let Some(child) = child_fs(fs, b"MultiLevel", None) else {
            return -1;
        };
        let values: Vec<f32> = read_leaf(child.as_ref(), b"Switch2")
            .map(|bytes| {
                bytes
                    .as_chunks::<4>()
                    .0
                    .iter()
                    .map(|b| f32::from_le_bytes(*b))
                    .collect()
            })
            .unwrap_or_default();
        if values.is_empty() {
            set.resize(1);
        } else {
            set.resize(values.len() - 1);
            let n = set.switch.len();
            set.switch.copy_from_slice(&values[..n]);
        }
        let count = set.entries.len();
        let mut failed_at = None;
        #[allow(clippy::needless_range_loop)] // the index names the level node
        for index in 0..count {
            let name = format!("Level{index}");
            let level = child_fs(child.as_ref(), name.as_bytes(), None);
            let Some(mut level) = level.filter(|_| !plugins.is_empty()) else {
                failed_at = Some(index);
                break;
            };
            let mut claimed = false;
            for (number, plugin) in plugins.iter_mut().enumerate() {
                let handle = plugin.claim(services, level.as_mut());
                if handle != HANDLE_NONE {
                    set.entries[index] = LodEntry {
                        handle,
                        plugin: Some(number),
                    };
                    claimed = true;
                }
            }
            if !claimed {
                failed_at = Some(index);
                break;
            }
        }
        let Some(failed) = failed_at else {
            return 0;
        };
        for undo in (0..failed).rev() {
            let entry = set.entries[undo];
            if let Some(plugin) = entry.plugin.and_then(|p| plugins.get_mut(p)) {
                plugin.destroy_handle(services, entry.handle);
            }
        }
        set.resize(0);
        -1
    }

    /// `FRenderManager::WalkPlugins`: the first plugin that claims the node
    /// is the only level, drawn from 0 to 1e6.
    fn walk_plugins(&mut self, services: &mut S, id: Archetype, fs: &mut dyn FileSystem) -> i32 {
        let Self {
            archetypes,
            plugins,
            ..
        } = self;
        let mut archetypes = archetypes.borrow_mut();
        let Some(set) = archetypes.get_mut(&id) else {
            return -1;
        };
        for (number, plugin) in plugins.iter_mut().enumerate() {
            let handle = plugin.claim(services, fs);
            if handle == HANDLE_NONE {
                continue;
            }
            set.resize(1);
            set.switch[0] = 0.0;
            set.switch[1] = DEFAULT_LOD_DISTANCE;
            set.entries[0] = LodEntry {
                handle,
                plugin: Some(number),
            };
            return 0;
        }
        -1
    }

    /// `CreateArchetype(id, fs)` (`IEngineComponent` slot 4): 1 when
    /// `MultiLevel` or the plugin walk made the levels; 0 for an id already
    /// present or a node nobody claims.
    pub fn create_archetype(
        &mut self,
        services: &mut S,
        id: Archetype,
        fs: &mut dyn FileSystem,
    ) -> i32 {
        if self.archetypes.borrow().contains_key(&id) {
            return 0;
        }
        self.archetypes.borrow_mut().insert(id, LodSet::default());
        if self.multi_level(services, id, fs) >= 0 {
            return 1;
        }
        if self.walk_plugins(services, id, fs) >= 0 {
            return 1;
        }
        self.archetypes.borrow_mut().remove(&id);
        0
    }

    /// `DuplicateArchetype(new, old)`: the old archetype's levels, each
    /// handle cloned by its plugin. Always 0.
    pub fn duplicate_archetype(&mut self, services: &mut S, new: Archetype, old: Archetype) -> i32 {
        let Some(source) = self.archetypes.borrow().get(&old).cloned() else {
            return 0;
        };
        if self.archetypes.borrow().contains_key(&new) {
            return 0;
        }
        let mut dest = LodSet::default();
        dest.assign(&source);
        for (entry, from) in dest.entries.iter_mut().zip(&source.entries) {
            if let Some(plugin) = entry.plugin.and_then(|p| self.plugins.get_mut(p)) {
                entry.handle = plugin.clone_handle(services, from.handle);
            }
        }
        self.archetypes.borrow_mut().insert(new, dest);
        0
    }

    /// `DestroyArchetype(id)`: every level's handle destroyed, then the
    /// archetype dropped. The port returns 0 (the reference returns what
    /// its erase helper leaves).
    pub fn destroy_archetype(&mut self, services: &mut S, id: Archetype) -> i32 {
        let removed = self.archetypes.borrow_mut().remove(&id);
        let Some(set) = removed else {
            return 0;
        };
        for entry in &set.entries {
            if let Some(plugin) = entry.plugin.and_then(|p| self.plugins.get_mut(p)) {
                plugin.destroy_handle(services, entry.handle);
            }
        }
        0
    }

    /// The current level's entry: `Err(-8)` when there is none.
    fn current_entry(set: &LodSet) -> Result<LodEntry, i32> {
        usize::try_from(set.current)
            .ok()
            .and_then(|index| set.entries.get(index))
            .copied()
            .ok_or(-8)
    }

    /// `QueryArchetype(id, iid)`: the current level's `QueryHandle`.
    pub fn query_archetype(&mut self, id: Archetype, iid: MeshIid) -> i32 {
        let archetypes = self.archetypes.borrow();
        let Some(set) = archetypes.get(&id) else {
            return -1;
        };
        match Self::current_entry(set) {
            Ok(entry) => match entry.plugin.and_then(|p| self.plugins.get_mut(p)) {
                Some(plugin) => plugin.query_handle(entry.handle, iid),
                None => -8,
            },
            Err(result) => result,
        }
    }

    /// `CreateInstance(instance, archetype)`: every level's
    /// `CreateMeshInstance`; a failure undoes the levels before it. 1 on
    /// success, else 0.
    pub fn create_instance(
        &mut self,
        services: &mut S,
        instance: Instance,
        archetype: Archetype,
    ) -> i32 {
        let Some(entries) = self
            .archetypes
            .borrow()
            .get(&archetype)
            .map(|set| set.entries.clone())
        else {
            return 0;
        };
        for (index, entry) in entries.iter().enumerate() {
            let Some(plugin) = entry.plugin.and_then(|p| self.plugins.get_mut(p)) else {
                continue;
            };
            if plugin.create_mesh_instance(services, instance, entry.handle) != 0 {
                continue;
            }
            for undo in entries[..index].iter().rev() {
                if let Some(plugin) = undo.plugin.and_then(|p| self.plugins.get_mut(p)) {
                    plugin.destroy_mesh_instance(services, instance, undo.handle);
                }
            }
            return 0;
        }
        1
    }

    /// The instance's archetype as the `IRenderer` instance slots find it:
    /// `GetInstanceArchetype`, -1 for none, then `ReleaseHandle`.
    fn archetype_of(services: &mut S, instance: Instance) -> Option<Archetype> {
        let archetype = services.instance_archetype(instance);
        if archetype == -1 {
            return None;
        }
        services.release_handle(archetype);
        Some(archetype)
    }

    /// `DestroyInstance(instance)`: every level's `DestroyMeshInstance`.
    pub fn destroy_instance(&mut self, services: &mut S, instance: Instance) -> i32 {
        let Some(archetype) = Self::archetype_of(services, instance) else {
            return -1;
        };
        let Some(entries) = self
            .archetypes
            .borrow()
            .get(&archetype)
            .map(|set| set.entries.clone())
        else {
            return 0;
        };
        for entry in entries {
            if let Some(plugin) = entry.plugin.and_then(|p| self.plugins.get_mut(p)) {
                plugin.destroy_mesh_instance(services, instance, entry.handle);
            }
        }
        0
    }

    /// `UpdateInstance(instance, arg)`: every plugin's
    /// `UpdateMeshInstance`.
    pub fn update_instance(&mut self, services: &mut S, instance: Instance, arg: u32) -> i32 {
        for plugin in &mut self.plugins {
            plugin.update_mesh_instance(services, instance, arg);
        }
        0
    }

    /// `QueryInstance(instance, iid)`: the current level's
    /// `QueryMeshInstance`.
    pub fn query_instance(&mut self, services: &mut S, instance: Instance, iid: MeshIid) -> i32 {
        let Some(archetype) = Self::archetype_of(services, instance) else {
            return -1;
        };
        let archetypes = self.archetypes.borrow();
        let Some(set) = archetypes.get(&archetype) else {
            return -1;
        };
        match Self::current_entry(set) {
            Ok(entry) => match entry.plugin.and_then(|p| self.plugins.get_mut(p)) {
                Some(plugin) => plugin.query_mesh_instance(instance, entry.handle, iid),
                None => -8,
            },
            Err(result) => result,
        }
    }

    /// `Notify(arg)`: every plugin's `UpdateMeshInstances`.
    pub fn notify(&mut self, services: &mut S, arg: u32) -> i32 {
        for plugin in &mut self.plugins {
            plugin.update_mesh_instances(services, arg);
        }
        0
    }

    /// The plugin and handle of the level `lod` picks.
    fn picked(
        &mut self,
        id: Archetype,
        lod: f32,
    ) -> Option<(&mut Box<dyn RenderComponent<S>>, Handle)> {
        let entry = {
            let archetypes = self.archetypes.borrow();
            let set = archetypes.get(&id)?;
            set.entries[set.pick(lod)?]
        };
        let plugin = self.plugins.get_mut(entry.plugin?)?;
        Some((plugin, entry.handle))
    }

    /// `IRenderer` slot 3 (`QueryGeometry`): the picked level's slot 12
    /// with the distance's bits, low byte; 0 without one.
    pub fn query_geometry(
        &mut self,
        id: Archetype,
        lod: f32,
        query: GeometryQuery,
    ) -> (i32, Option<i32>) {
        match self.picked(id, lod) {
            Some((plugin, handle)) => {
                let (result, out) = plugin.query_geometry(handle, lod.to_bits(), query);
                (result & 0xFF, out)
            }
            None => (0, None),
        }
    }

    /// `IRenderer` slot 4 (`GetBounds`).
    pub fn bounds(&mut self, id: Archetype, lod: f32) -> (i32, Option<[f32; 6]>) {
        match self.picked(id, lod) {
            Some((plugin, handle)) => {
                let (result, out) = plugin.bounds(handle, lod);
                (result & 0xFF, out)
            }
            None => (0, None),
        }
    }

    /// `IRenderer` slot 5 (`GetCentroid`).
    pub fn centroid(&mut self, id: Archetype, lod: f32) -> (i32, Option<[f32; 3]>) {
        match self.picked(id, lod) {
            Some((plugin, handle)) => {
                let (result, out) = plugin.centroid(handle, lod);
                (result & 0xFF, out)
            }
            None => (0, None),
        }
    }

    /// `IRenderer` slot 7 (`RenderInstance`): culls the instance's sphere,
    /// picks a level, draws it through its plugin and recurses into the
    /// child instances. The rank: 1 when nothing was drawn for want of a
    /// level, else the camera's classification (4 without culling), and
    /// for children the larger rank, at most 3, where they differ.
    pub fn render_instance(
        &mut self,
        services: &mut S,
        camera: &mut dyn MeshCamera,
        instance: Instance,
        threshold: f32,
        request: &DrawRequest,
    ) -> i32 {
        if threshold < 0.0 {
            return 1;
        }
        let world = transform(services.instance_transform(instance));
        let mut flags = request.flags;
        let mut scale = 1.0f32;
        if let Some(matrix) = &request.matrix {
            let m = matrix.to_words();
            let length = |a: f32, b: f32, c: f32| {
                (f64::from(a) * f64::from(a) + f64::from(b) * f64::from(b))
                    + f64::from(c) * f64::from(c)
            };
            let len2 = length(m[8], m[5], m[2]).sqrt() as f32;
            let len1 = length(m[7], m[4], m[1]).sqrt() as f32;
            let len0_wide = length(m[6], m[3], m[0]).sqrt();
            let len0 = len0_wide as f32;
            let larger = if len0_wide < f64::from(len1) {
                len1
            } else {
                len0
            };
            scale = if larger < len2 { len2 } else { larger };
        }
        let mut draw_matrix = request.matrix;
        if let Some(matrix) = request.matrix.filter(|_| flags & DRAW_MATRIX_READY == 0) {
            let mut composed = matrix;
            if flags & DRAW_ROTATE_OFFSET != 0 {
                composed.translation = rotate(&matrix, matrix.translation);
                flags &= !DRAW_ROTATE_OFFSET;
            }
            if flags & DRAW_CONJUGATE != 0 {
                let offset = transpose_transform(&world.rotation, world.translation);
                let inverse = Transform::new(
                    world.rotation.transpose(),
                    Vector::new(-offset.x, -offset.y, -offset.z),
                );
                let left = mul_transform(&world, &composed);
                composed = mul_transform(&left, &inverse);
                flags &= !DRAW_CONJUGATE;
            }
            flags |= DRAW_MATRIX_READY;
            draw_matrix = Some(composed);
        }
        let (mut radius, centre) = services
            .instance_extent(instance)
            .unwrap_or((0.0, [0.0; 3]));
        if f64::from(radius) < 0.001 {
            radius = 1.0;
        }
        let world_centre = transform_xform(&world, Vector::from_words(centre));
        let view = transform(camera.view_transform());
        let view_centre = transform_xform(&view, world_centre);
        let depth = if view_centre.z < 0.0 {
            -view_centre.z
        } else {
            view_centre.z
        };
        let mut rank = 4;
        let mut draw = true;
        if flags & DRAW_NO_CULL == 0 {
            rank = camera.classify_sphere(view_centre.to_words(), radius * scale);
            draw = rank >= 3;
        }
        let child_request = DrawRequest {
            flags,
            matrix: draw_matrix,
            extra: request.extra,
        };
        if draw && let Some(archetype) = Self::archetype_of(services, instance) {
            let mut chosen = None;
            let mut archetypes = self.archetypes.borrow_mut();
            if let Some(set) = archetypes.get_mut(&archetype) {
                let mut submit_flags = flags;
                let count = set.entries.len();
                let entry = if flags & DRAW_KEEP_CURRENT != 0 {
                    usize::try_from(set.current).ok().filter(|&c| c < count)
                } else if flags & DRAW_PICK_SWITCH == 0 {
                    (count > 0).then_some(0)
                } else {
                    if set.root_selection {
                        services.instance_root(instance);
                    }
                    camera.half_fov_x_deg();
                    let viewport = camera.viewport();
                    let half_width =
                        (viewport[2].wrapping_sub(viewport[0]).wrapping_add(1) / 2) as f32;
                    let x_scale = camera.x_scale_over_near() as f32;
                    let z_near = f64::from(camera.z_near());
                    let distance =
                        (f64::from(half_width) / (z_near * f64::from(x_scale)) / f64::from(scale)
                            * f64::from(depth)
                            * f64::from(threshold)) as f32;
                    submit_flags &= !DRAW_PICK_SWITCH;
                    set.pick(distance)
                };
                match entry {
                    Some(index) => {
                        set.current = i32::try_from(index).unwrap_or(i32::MAX);
                        chosen = Some((set.entries[index], submit_flags));
                    }
                    None => {
                        set.current = set.count();
                        rank = 1;
                    }
                }
            }
            drop(archetypes);
            if let Some((LodEntry { handle, plugin }, submit_flags)) = chosen
                && let Some(plugin) = plugin.and_then(|p| self.plugins.get_mut(p))
            {
                plugin.bind_mesh_instance(instance, handle);
                let request = DrawRequest {
                    flags: submit_flags,
                    matrix: draw_matrix,
                    extra: request.extra,
                };
                plugin.render_instance(services, camera, handle, instance, &request);
            }
        }
        if flags & DRAW_NO_CHILDREN == 0 {
            let mut child = services.related_instance(instance, None);
            while let Some(current) = child {
                let child_rank =
                    self.render_instance(services, camera, current, threshold, &child_request);
                if rank != child_rank {
                    rank = rank.max(child_rank).min(3);
                }
                child = services.related_instance(instance, Some(current));
            }
        }
        rank
    }

    /// `IRenderer` slot 8: the instance archetype's level count.
    pub fn instance_lod_count(&mut self, services: &mut S, instance: Instance) -> Result<i32, i32> {
        let archetype = Self::archetype_of(services, instance).ok_or(-1)?;
        self.archetype_lod_count(archetype)
    }

    /// `IRenderer` slot 9: the instance archetype's current level.
    pub fn instance_current_lod(
        &mut self,
        services: &mut S,
        instance: Instance,
    ) -> Result<i32, i32> {
        let archetype = Self::archetype_of(services, instance).ok_or(-1)?;
        self.archetype_current_lod(archetype)
    }

    /// `IRenderer` slot 10: binds level `index` to the instance (the level
    /// count itself selects no level).
    pub fn set_instance_lod(&mut self, services: &mut S, instance: Instance, index: i32) -> i32 {
        let Some(archetype) = Self::archetype_of(services, instance) else {
            return -1;
        };
        let mut archetypes = self.archetypes.borrow_mut();
        let Some(set) = archetypes.get_mut(&archetype) else {
            return -1;
        };
        let count = set.count();
        if index < count {
            let Some(entry) = usize::try_from(index).ok().map(|i| set.entries[i]) else {
                return -1;
            };
            let Some(plugin) = entry.plugin.and_then(|p| self.plugins.get_mut(p)) else {
                return -1;
            };
            let result = plugin.bind_mesh_instance(instance, entry.handle);
            if result == 0 {
                set.current = index;
            }
            result
        } else {
            if index == count {
                set.current = index;
            }
            -1
        }
    }

    /// `IRenderer` slot 11.
    ///
    /// # Errors
    ///
    /// -1 for an unknown archetype.
    pub fn archetype_lod_count(&self, id: Archetype) -> Result<i32, i32> {
        self.archetypes
            .borrow()
            .get(&id)
            .map(LodSet::count)
            .ok_or(-1)
    }

    /// `IRenderer` slot 12.
    ///
    /// # Errors
    ///
    /// -1 for an unknown archetype.
    pub fn archetype_current_lod(&self, id: Archetype) -> Result<i32, i32> {
        self.archetypes
            .borrow()
            .get(&id)
            .map(|set| set.current)
            .ok_or(-1)
    }

    /// `IRenderer` slot 13: any index up to the level count.
    pub fn set_archetype_lod(&mut self, id: Archetype, index: i32) -> i32 {
        match self.archetypes.borrow_mut().get_mut(&id) {
            Some(set) if index <= set.count() => {
                set.current = index;
                0
            }
            _ => -1,
        }
    }

    /// `IRenderer` slot 14: level `index`'s near and far distances.
    ///
    /// # Errors
    ///
    /// -1 for an unknown archetype or an index outside the levels.
    pub fn archetype_lod_range(&self, id: Archetype, index: i32) -> Result<(f32, f32), i32> {
        let archetypes = self.archetypes.borrow();
        let set = archetypes.get(&id).ok_or(-1)?;
        let index = usize::try_from(index)
            .ok()
            .filter(|&i| i < set.entries.len())
            .ok_or(-1)?;
        Ok((set.switch[index], set.switch[index + 1]))
    }

    /// `IRenderer` slot 15: `a <= b` (ordered) or -1.
    pub fn set_archetype_lod_range(&mut self, id: Archetype, index: i32, a: f32, b: f32) -> i32 {
        if a.partial_cmp(&b)
            .is_none_or(|o| o == std::cmp::Ordering::Greater)
        {
            return -1;
        }
        let mut archetypes = self.archetypes.borrow_mut();
        let Some(set) = archetypes.get_mut(&id) else {
            return -1;
        };
        let Some(index) = usize::try_from(index)
            .ok()
            .filter(|&i| i < set.entries.len())
        else {
            return -1;
        };
        set.switch[index] = a;
        set.switch[index + 1] = b;
        0
    }

    /// `IRenderer` slot 16.
    ///
    /// # Errors
    ///
    /// -1 as slot 14, or for an instance without an archetype.
    pub fn instance_lod_range(
        &mut self,
        services: &mut S,
        instance: Instance,
        index: i32,
    ) -> Result<(f32, f32), i32> {
        let archetype = Self::archetype_of(services, instance).ok_or(-1)?;
        self.archetype_lod_range(archetype, index)
    }

    /// `IRenderer` slot 17.
    pub fn set_instance_lod_range(
        &mut self,
        services: &mut S,
        instance: Instance,
        index: i32,
        a: f32,
        b: f32,
    ) -> i32 {
        match Self::archetype_of(services, instance) {
            Some(archetype) => self.set_archetype_lod_range(archetype, index, a, b),
            None => -1,
        }
    }

    /// `IRenderer` slot 18: level `lod`'s slot 12 with the bits of 1.0.
    pub fn query_geometry_at_lod(
        &mut self,
        id: Archetype,
        lod: i32,
        query: GeometryQuery,
    ) -> (i32, Option<i32>) {
        let entry = {
            let archetypes = self.archetypes.borrow();
            let Some(set) = archetypes.get(&id) else {
                return (0, None);
            };
            if lod >= set.count() {
                return (0, None);
            }
            let Some(entry) = usize::try_from(lod).ok().map(|i| set.entries[i]) else {
                return (0, None);
            };
            entry
        };
        match entry.plugin.and_then(|p| self.plugins.get_mut(p)) {
            Some(plugin) => {
                let (result, out) = plugin.query_geometry(entry.handle, 0x3F80_0000, query);
                (result & 0xFF, out)
            }
            None => (0, None),
        }
    }

    /// `IRenderer` slot 19: the root selection flag from bit 0.
    pub fn set_archetype_root_selection(&mut self, id: Archetype, flags: u32) -> i32 {
        match self.archetypes.borrow_mut().get_mut(&id) {
            Some(set) => {
                set.root_selection = flags & 1 != 0;
                0
            }
            None => -1,
        }
    }

    /// `IRenderer` slot 20.
    pub fn set_instance_root_selection(
        &mut self,
        services: &mut S,
        instance: Instance,
        flags: u32,
    ) -> i32 {
        match Self::archetype_of(services, instance) {
            Some(archetype) => self.set_archetype_root_selection(archetype, flags),
            None => -1,
        }
    }

    /// `IRenderer` slot 21: the result and the flags written (0 first).
    #[must_use]
    pub fn archetype_root_selection(&self, id: Archetype) -> (i32, u32) {
        match self.archetypes.borrow().get(&id) {
            Some(set) => (0, u32::from(set.root_selection)),
            None => (-1, 0),
        }
    }

    /// `IRenderer` slot 22: the result and the flags written; nothing is
    /// written without an archetype.
    pub fn instance_root_selection(
        &mut self,
        services: &mut S,
        instance: Instance,
    ) -> (i32, Option<u32>) {
        match Self::archetype_of(services, instance) {
            Some(archetype) => {
                let (result, flags) = self.archetype_root_selection(archetype);
                (result, Some(flags))
            }
            None => (-1, None),
        }
    }

    /// The first plugin's sphere instance of `instance` (the
    /// `ISphereMesh` face, without the engine's archetype lookup).
    pub fn find_sphere_instance(
        &mut self,
        instance: Instance,
    ) -> Option<&mut super::sphere::SphereInstance> {
        self.plugins
            .iter_mut()
            .find_map(|p| p.sphere_mesh(instance))
    }

    /// The destructor: a notice for every archetype left, then every
    /// plugin released.
    pub fn destroy(mut self, services: &mut S) {
        for &id in self.archetypes.borrow().keys() {
            let message = format!("RenderMgr: dtor: Archetype {id} has dangling render data\n");
            services.dump(dump::NOTICE, file::RENDER_MGR, 0xBF, &message);
        }
        for plugin in &mut self.plugins {
            plugin.destroy(services);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{GeometryQuery, plugin_names};

    #[test]
    fn plugin_lines_are_cut_at_blanks_and_comments() {
        let lines: [&[u8]; 4] = [
            b"VMesh",
            b"; TriMesh",
            b"TriMesh\t; strips",
            b"SphereMesh;x",
        ];
        assert_eq!(
            plugin_names(&lines),
            [
                b"VMesh".to_vec(),
                b"TriMesh".to_vec(),
                b"SphereMesh".to_vec()
            ]
        );
    }

    #[test]
    fn geometry_queries_need_mode_flags_and_output() {
        let q = |mode, flags, out| GeometryQuery { mode, flags, out }.can_write();
        assert!(q(0, 0x4003, true));
        assert!(q(3, 0x5003, true));
        assert!(!q(1, 0x4003, true));
        assert!(!q(2, 0x0003, true));
        assert!(!q(2, 0x4003, false));
    }
}
