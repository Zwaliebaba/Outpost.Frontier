//! The client's side of the particle effects (work package P4.8, third
//! increment): the mesh factory the mesh appearances ask for their meshes
//! (`FLFxMeshFactory` and `FLFxMesh` in `fl_fx_mesh.cpp`, bound by
//! `WarmUpEffectShapes` in `texture_shapes.cpp`) and the services the
//! effects' calls run against, the render stack with that factory bound.
//!
//! An `FLFxMesh` is the client's mesh holder: the archetype of
//! `fx\misc\<name>.3db` loaded through `Archetype::Load` (the archive's
//! libraries first, as the client loads an archetype's files), an engine
//! instance of it placed at the particle's position, posed, updated
//! (`UpdateInstance`) and drawn through the render manager
//! (`RenderInstance` with flag `0x100`, depth writes off and `LESSEQUAL`
//! around it).
//!
//! The scene engine reaches the render manager while it makes and destroys
//! archetypes and instances, and the particle walks run with the render
//! stack borrowed, so a mesh's archetype and instance are made, and its
//! instance destroyed, by [`super::Engine`] once the walk is over and
//! before the frame's draw (D493).

use std::collections::{BTreeMap, BTreeSet};

use neuron_core::math::{Transform, Vector};
use neuron_core::scene::InstanceId;
use neuron_core::vfs::Host;

use crate::render::appearance::{AppearanceServices, Camera};
use crate::render::ff::device::Backend;
use crate::render::mesh::RenderManager;
use crate::render::particles::services::{FrameOut, TextureRecord};
use crate::render::particles::{FxMesh, FxMeshFactory, ParticleDraw, ParticleServices};
use crate::render::stack::{RenderStack, StackAppearance};

use super::SharedScene;

/// `D3DRS_ZWRITEENABLE`.
const RS_ZWRITEENABLE: u32 = 14;
/// `D3DRS_ZFUNC`.
const RS_ZFUNC: u32 = 23;
/// `D3DCMP_LESSEQUAL`.
const CMP_LESSEQUAL: u32 = 4;
/// The render flag `FLFxMesh::Slot6` draws with.
const DRAW_FLAGS: u32 = 0x100;
/// `FFxMeshPath`'s 32 characters, the terminator included.
const PATH_CHARS: usize = 0x1F;
/// `s_fx\misc\%s.3db`.
const PATH_PREFIX: &[u8] = b"fx\\misc\\";

/// The identity transform.
const IDENTITY: [f32; 12] = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0];

/// One `FLFxMesh`.
#[derive(Clone, Debug)]
struct MeshEntry {
    /// The archetype's path.
    path: Vec<u8>,
    /// `ClientMeshHolderSetPosition`'s position.
    position: [f32; 3],
    /// The engine instance, once made.
    instance: Option<InstanceId>,
    /// A pose given before the instance was made.
    pose: Option<[f32; 12]>,
}

/// The client's mesh factory and its meshes.
#[derive(Debug, Default)]
pub struct FxMeshes {
    meshes: Vec<Option<MeshEntry>>,
    found: BTreeMap<Vec<u8>, bool>,
    loaded: BTreeSet<Vec<u8>>,
    pending: Vec<FxMesh>,
    doomed: Vec<InstanceId>,
}

/// The path `FLFxMeshFactory::Slot3` loads: `fx\misc\<name>.3db`, cut to 31
/// characters as `FxMeshPathFormat` cuts it. The reference formats the
/// empty path object itself, so it always names `(null)` (D490).
#[must_use]
pub fn mesh_path(name: &[u8]) -> Vec<u8> {
    let mut path = PATH_PREFIX.to_vec();
    path.extend(name.iter().take_while(|&&c| c != 0));
    path.extend_from_slice(b".3db");
    path.truncate(PATH_CHARS);
    path
}

impl FxMeshes {
    /// The meshes still held.
    #[must_use]
    pub fn live(&self) -> usize {
        self.meshes.iter().flatten().count()
    }

    /// The meshes whose engine instance exists.
    #[must_use]
    pub fn instanced(&self) -> usize {
        self.meshes
            .iter()
            .flatten()
            .filter(|m| m.instance.is_some())
            .count()
    }

    fn entry(&mut self, mesh: FxMesh) -> Option<&mut MeshEntry> {
        let index = usize::try_from(mesh).ok()?.checked_sub(1)?;
        self.meshes.get_mut(index)?.as_mut()
    }

    /// The meshes to make once the walk is over: (mesh, path).
    pub(super) fn take_pending(&mut self) -> Vec<(FxMesh, Vec<u8>)> {
        let pending = std::mem::take(&mut self.pending);
        pending
            .into_iter()
            .filter_map(|m| self.entry(m).map(|e| (m, e.path.clone())))
            .collect()
    }

    /// Whether an archive's libraries are still to load (the first time
    /// it is asked for).
    pub(super) fn first_load(&mut self, path: &[u8]) -> bool {
        self.loaded.insert(path.to_ascii_lowercase())
    }

    /// The instances to destroy once the walk is over.
    pub(super) fn take_doomed(&mut self) -> Vec<InstanceId> {
        std::mem::take(&mut self.doomed)
    }

    /// A made instance for a pending mesh: its position, then the pose it
    /// was given meanwhile. `None` (no instance) leaves the mesh without
    /// one, as a failed `BindInstance` does.
    pub(super) fn bind(&mut self, mesh: FxMesh, instance: Option<InstanceId>, scene: &SharedScene) {
        let Some(entry) = self.entry(mesh) else {
            if let Some(instance) = instance {
                self.doomed.push(instance);
            }
            return;
        };
        let Some(instance) = instance else {
            return;
        };
        entry.instance = Some(instance);
        let mut scene = scene.borrow_mut();
        let [x, y, z] = entry.position;
        scene.set_instance_position(Some(instance), Vector::new(x, y, z));
        if let Some(pose) = entry.pose.take() {
            scene.set_instance_matrix(Some(instance), &Transform::from_words(pose));
        }
    }
}

/// The services of the effects' calls: the render stack (textures and the
/// draws), the render manager, the scene engine and the data for the mesh
/// factory, which is bound when `meshes` is given.
pub struct EffectServices<'a, B: Backend + 'static> {
    /// The render stack.
    pub stack: &'a mut RenderStack<B>,
    /// The render manager, for drawing meshes.
    pub manager: Option<&'a mut RenderManager<RenderStack<B>>>,
    /// The scene engine.
    pub scene: &'a SharedScene,
    /// The data, where the meshes' archives are.
    pub data: Option<&'a Host>,
    /// The mesh factory's meshes; `None` binds no factory.
    pub meshes: Option<&'a mut FxMeshes>,
}

impl<B: Backend + 'static> EffectServices<'_, B> {
    fn meshes(&mut self) -> Option<&mut FxMeshes> {
        self.meshes.as_deref_mut()
    }
}

impl<B: Backend + 'static> FxMeshFactory for EffectServices<'_, B> {
    /// `FLFxMeshFactory::Slot3`: a mesh of the archive at [`mesh_path`],
    /// placed at the particle's position; nothing (and success) when the
    /// archive cannot be opened (`Archetype::Load` failing).
    fn create_mesh(
        &mut self,
        _mesh_id: u32,
        mesh_name: &[u8],
        six: &[f32; 6],
    ) -> (i32, Option<FxMesh>) {
        let path = mesh_path(mesh_name);
        let data = self.data;
        let Some(meshes) = self.meshes() else {
            return (0, None);
        };
        let found = *meshes
            .found
            .entry(path.to_ascii_lowercase())
            .or_insert_with(|| data.is_some_and(|d| d.read_file(&path).is_ok()));
        if !found {
            return (0, None);
        }
        meshes.meshes.push(Some(MeshEntry {
            path,
            position: [six[0], six[1], six[2]],
            instance: None,
            pose: None,
        }));
        let mesh = FxMesh::try_from(meshes.meshes.len()).unwrap_or(FxMesh::MAX);
        meshes.pending.push(mesh);
        (0, Some(mesh))
    }

    /// `FLFxMesh::Slot3`: `SetInstanceMatrix`.
    fn set_mesh_transform(&mut self, mesh: FxMesh, transform: &[f32; 12]) {
        let scene = self.scene;
        let Some(entry) = self.meshes().and_then(|m| m.entry(mesh)) else {
            return;
        };
        match entry.instance {
            Some(instance) => scene
                .borrow_mut()
                .set_instance_matrix(Some(instance), &Transform::from_words(*transform)),
            None => entry.pose = Some(*transform),
        }
    }

    /// `FLFxMesh::Slot4`: `GetInstanceTransform`.
    fn mesh_transform(&mut self, mesh: FxMesh) -> [f32; 12] {
        let scene = self.scene;
        let Some(entry) = self.meshes().and_then(|m| m.entry(mesh)) else {
            return IDENTITY;
        };
        match entry.instance {
            Some(instance) => scene.borrow().instance_transform(Some(instance)).to_words(),
            None => entry.pose.unwrap_or_else(|| {
                let mut out = IDENTITY;
                out[9..12].copy_from_slice(&entry.position);
                out
            }),
        }
    }

    /// `FLFxMesh::Slot5`: `UpdateInstance(instance, 0)`.
    fn update_mesh(&mut self, mesh: FxMesh, _dt: f32) {
        let scene = self.scene;
        if let Some(instance) = self
            .meshes()
            .and_then(|m| m.entry(mesh))
            .and_then(|e| e.instance)
        {
            scene.borrow_mut().update_instance(Some(instance), 0);
        }
    }

    /// `FLFxMesh::Slot6`: depth writes off and `LESSEQUAL`, the instance
    /// through the render manager, the two states restored.
    fn draw_mesh(&mut self, mesh: FxMesh, camera: &mut dyn Camera) {
        let Some(instance) = self
            .meshes()
            .and_then(|m| m.entry(mesh))
            .and_then(|e| e.instance)
        else {
            return;
        };
        let Some(manager) = self.manager.as_deref_mut() else {
            return;
        };
        let stack = &mut *self.stack;
        let z_write = ParticleDraw::render_state(stack, RS_ZWRITEENABLE);
        ParticleDraw::set_render_state(stack, RS_ZWRITEENABLE, 0);
        let z_func = ParticleDraw::render_state(stack, RS_ZFUNC);
        ParticleDraw::set_render_state(stack, RS_ZFUNC, CMP_LESSEQUAL);
        let mut services = StackAppearance { stack, manager };
        services.render_instance(camera, instance.index() as usize, 1.0, DRAW_FLAGS, None);
        ParticleDraw::set_render_state(stack, RS_ZFUNC, z_func);
        ParticleDraw::set_render_state(stack, RS_ZWRITEENABLE, z_write);
    }

    /// `FLFxMesh::Slot7`: the instance destroyed (once the walk is over).
    fn drop_mesh(&mut self, mesh: FxMesh) {
        let Some(meshes) = self.meshes() else {
            return;
        };
        let Some(entry) = meshes.entry(mesh) else {
            return;
        };
        if let Some(instance) = entry.instance.take() {
            meshes.doomed.push(instance);
        }
    }

    /// The last `Release`: the destructor's `Slot7` again, then the mesh is
    /// gone.
    fn release_mesh(&mut self, mesh: FxMesh) {
        self.drop_mesh(mesh);
        if let Some(meshes) = self.meshes()
            && let Some(slot) = usize::try_from(mesh)
                .ok()
                .and_then(|m| m.checked_sub(1))
                .and_then(|i| meshes.meshes.get_mut(i))
        {
            *slot = None;
        }
    }
}

impl<B: Backend + 'static> ParticleServices for EffectServices<'_, B> {
    fn find_texture(&mut self, name: &[u8], crc: u32) -> Option<TextureRecord> {
        self.stack.find_texture(name, crc)
    }
    fn create_playback(&mut self, record: TextureRecord) -> i32 {
        self.stack.create_playback(record)
    }
    fn frame_count(&mut self, record: TextureRecord) -> Option<i32> {
        self.stack.frame_count(record)
    }
    fn release_texture_record(&mut self, record: TextureRecord) {
        self.stack.release_texture_record(record);
    }
    fn set_wrap_mode(&mut self, id: i32, mode: i32) {
        self.stack.set_wrap_mode(id, mode);
    }
    fn texture_frame(&mut self, id: i32, frame: i32) -> Option<FrameOut> {
        self.stack.texture_frame(id, frame)
    }
    fn advance_time(&mut self, id: i32, seconds: f32) {
        self.stack.advance_time(id, seconds);
    }
    fn dec_ref_texture(&mut self, id: i32) {
        self.stack.dec_ref_texture(id);
    }
    fn mesh_factory(&mut self) -> Option<&mut dyn FxMeshFactory> {
        if self.meshes.is_some() {
            Some(self)
        } else {
            None
        }
    }
}

impl<B: Backend + 'static> ParticleDraw for EffectServices<'_, B> {
    fn render_state(&mut self, state: u32) -> u32 {
        ParticleDraw::render_state(self.stack, state)
    }
    fn set_render_state(&mut self, state: u32, value: u32) {
        ParticleDraw::set_render_state(self.stack, state, value);
    }
    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32) {
        ParticleDraw::set_stage_state(self.stack, stage, state, value);
    }
    fn set_stage_texture(&mut self, stage: u32, handle: u32) {
        ParticleDraw::set_stage_texture(self.stack, stage, handle);
    }
    fn set_modelview(&mut self, matrix: &[f32; 12]) {
        ParticleDraw::set_modelview(self.stack, matrix);
    }
    fn select_vb(&mut self, vertex_buffer: u32) -> i32 {
        self.stack.select_vb(vertex_buffer)
    }
    fn create_vb(&mut self, fvf: u32, count: u32, flags: u32) -> (i32, u32) {
        self.stack.create_vb(fvf, count, flags)
    }
    fn destroy_vb(&mut self, vertex_buffer: u32) {
        self.stack.destroy_vb(vertex_buffer);
    }
    fn lock_vb(&mut self, vertex_buffer: u32, count: u32) -> Result<u32, i32> {
        self.stack.lock_vb(vertex_buffer, count)
    }
    fn unlock_vb(&mut self, vertex_buffer: u32, written: &[u8]) {
        self.stack.unlock_vb(vertex_buffer, written);
    }
    fn lock_ib(&mut self, count: u32) -> Result<u32, i32> {
        self.stack.lock_ib(count)
    }
    fn unlock_ib(&mut self, written: &[u16]) {
        self.stack.unlock_ib(written);
    }
    fn select_ib(&mut self, base_vertex: u32) -> i32 {
        ParticleDraw::select_ib(self.stack, base_vertex)
    }
    fn draw_indexed(
        &mut self,
        prim: u32,
        min_index: u32,
        vertex_count: u32,
        start_index: u32,
        index_count: u32,
    ) -> i32 {
        self.stack
            .draw_indexed(prim, min_index, vertex_count, start_index, index_count)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_path_names_the_mesh_and_keeps_31_characters() {
        assert_eq!(mesh_path(b"TLRtube"), b"fx\\misc\\TLRtube.3db".to_vec());
        assert_eq!(
            mesh_path(b"beryl_asteroid10"),
            b"fx\\misc\\beryl_asteroid10.3db".to_vec()
        );
        let long = mesh_path(b"a_very_long_mesh_name_indeed_x");
        assert_eq!(long.len(), 31);
        assert!(long.starts_with(b"fx\\misc\\a_very_long"));
    }
}
