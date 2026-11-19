//! The services that join the render manager, the materials and the
//! appearance to the device model (work package P4.6): [`RenderStack`]
//! answers [`MeshServices`], [`MaterialServices`], [`SurfaceServices`] and
//! [`BatchDevice`] on [`Shading`] (the device model of `render::ff`, the
//! texture library, the global records), the material library and the
//! batcher, the `VMesh` registry, the `VMeshWire` component and the
//! render manager's [`MeshDirectory`]; [`StackAppearance`] adds
//! [`AppearanceServices`] with the render manager itself. It also answers
//! the particle draws' [`ParticleServices`] and [`ParticleDraw`] (P4.8),
//! the texture library through [`Shading`] and the module's vertex buffer
//! and the dynamic index buffer on the device model.
//!
//! A draw goes from [`RenderManager::render_instance`] through the mesh
//! kinds to the materials (`Bind`, `Submit`, `Teardown` on the material
//! library's arena, a batched material through the batcher), and from the
//! materials to the device's draw packets. The override materials reach
//! the material they replace (the candidate) and the wire materials the
//! instance's `IMesh` face or wire through the same services, which
//! answers divergence D315's calls.
//!
//! The reference reaches these through interface pointers and process
//! globals; here the materials live in an arena the services own, so a
//! material is taken out of the arena while a call into it runs and put
//! back after (a call that reaches the same material again finds none and
//! fails), and the batcher is taken out while it flushes or while the
//! library loads, creates or releases (divergence D359). The engine's
//! instances stay behind [`StackEngine`].
//!
//! [`StackDeform`] (work package P4.7) adds the render half of the
//! deformable component's calls: a skinned character draws through its
//! face groups' materials (the `EyeMaterial` among them, which the stack
//! answers as an [`EyeHost`]) into the device's draw packets, its skinned
//! vertices in a device vertex buffer.

use std::rc::Rc;

mod deform;

pub use deform::{StackDeform, draw_deformable};

use crate::render::appearance::{AppearanceServices, Camera, Face, MeshView};
use crate::render::deformable::{EyeHost, UvBinding};
use crate::render::ff::buffers::{BufferError, IndexSource};
use crate::render::ff::device::Backend;
use crate::render::ff::fvf::Fvf;
use crate::render::ff::vertex_copy;
use crate::render::material::shading::copy_source;
use crate::render::material::surface::MeshIndices;
use crate::render::material::{
    BatchDevice, DrawArgs, GR_GENERIC, GR_OK, InstanceId, InstanceValues, Material,
    MaterialBatcher, MaterialId, MaterialServices, Shading, SubmitContext, SubmitInput,
    SurfaceServices, VertexLock, VertexSource,
};
use crate::render::mesh::manager::DrawRequest;
use crate::render::mesh::vmesh::SharedRegistry;
use crate::render::mesh::wire::{SCRATCH_IB, draw_wire};
use crate::render::mesh::{
    Archetype, FIELD20_ENGINE, Instance, MeshDirectory, MeshFace, MeshIid, MeshServices,
    RenderManager, VMeshWire,
};
use crate::render::particles::services::TextureRecord;
use crate::render::particles::{ParticleDraw, ParticleServices};
use crate::render::texture::FrameOut;
use crate::render::texture::library::RecordId;

/// `E_FAIL`, the renderer's result for a refused buffer call.
const E_FAIL: i32 = 0x8000_4005_u32.cast_signed();

/// The engine's instances as the render manager, the mesh kinds and the
/// surface materials ask for them (`IEngine`; P1.8's scene engine in the
/// client).
pub trait StackEngine {
    /// `GetInstanceTransform`: the twelve words.
    fn instance_transform(&mut self, instance: Instance) -> [f32; 12];
    /// `GetInstanceArchetype`: -1 for none.
    fn instance_archetype(&mut self, instance: Instance) -> Archetype;
    /// `ReleaseHandle`.
    fn release_handle(&mut self, archetype: Archetype) {
        let _ = archetype;
    }
    /// `GetInstanceExtent(instance, 1, ...)`: `None` when nothing is
    /// written.
    fn instance_extent(&mut self, instance: Instance) -> Option<(f32, [f32; 3])>;
    /// `SetInstanceExtent(instance, 1, radius, centre)`.
    fn set_instance_extent(&mut self, instance: Instance, radius: f32, centre: [f32; 3]);
    /// `GetRelatedInstance(instance, 1, previous)`: the next child.
    fn related_instance(
        &mut self,
        instance: Instance,
        previous: Option<Instance>,
    ) -> Option<Instance> {
        let _ = (instance, previous);
        None
    }
    /// `GetInstanceRoot`.
    fn instance_root(&mut self, instance: Instance) -> Instance {
        instance
    }
    /// `GetInstanceUserData`: the first five words, `None` for null.
    fn user_data(&mut self, instance: Instance) -> Option<[u32; 5]> {
        let _ = instance;
        None
    }
    /// `GetInstanceParent` (the `EyeMaterial`'s bind): -1 for none.
    fn instance_parent(&mut self, instance: Instance) -> i32 {
        let _ = instance;
        -1
    }
    /// `GetInstanceOrientation`: the nine floats.
    fn instance_orientation(&mut self, instance: Instance) -> [f32; 9] {
        let _ = instance;
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    }
}

/// A `FDUMP` or log event the stack received.
#[derive(Clone, Debug, PartialEq)]
pub struct StackDump {
    /// The flags (`dump::NOTICE` and so on), or 0 for a log event.
    pub flags: i32,
    /// The text.
    pub text: String,
}

/// The joined services. `B` is the device model's backend.
pub struct RenderStack<B: Backend + 'static> {
    /// The device model, the texture library and the global records.
    pub shading: Shading<B>,
    batcher: Option<MaterialBatcher<RenderStack<B>>>,
    /// The `VMesh` registry the libraries, parts and wires share.
    pub registry: SharedRegistry,
    /// The `VMeshWire` engine component.
    pub wires: VMeshWire,
    /// The render manager's faces; `None` before [`RenderStack::set_directory`].
    directory: Option<MeshDirectory>,
    /// The engine.
    pub engine: Box<dyn StackEngine>,
    /// `G_FlmEngineOuter` is set (the appearance published the engine).
    pub published: bool,
    /// What the meshes and materials dumped.
    pub dumps: Vec<StackDump>,
    /// The face `QueryInstance(instance, IID_IMesh)` handed out last.
    face: Option<(Instance, MeshFace)>,
    /// The start of each index-buffer lock, by buffer number.
    ib_locks: Vec<(u32, u32)>,
    /// The particle draw's vertex lock: the buffer and the byte offset.
    particle_vb_lock: Option<(u32, u32)>,
    /// The particle draw's index lock on the dynamic buffer: its start.
    particle_ib_lock: Option<u32>,
    /// The UV-deform binding of the deformable face group binding its
    /// material (what the context's `Field20` points to).
    uv_binding: Option<UvBinding>,
}

impl<B: Backend + 'static> std::fmt::Debug for RenderStack<B> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("RenderStack")
            .field("shading", &self.shading)
            .field("dumps", &self.dumps.len())
            .finish_non_exhaustive()
    }
}

impl<B: Backend + 'static> RenderStack<B> {
    /// The services over `shading`, a batcher (with its library) made for
    /// them, a shared `VMesh` registry and the engine.
    #[must_use]
    pub fn new(
        shading: Shading<B>,
        batcher: MaterialBatcher<RenderStack<B>>,
        registry: SharedRegistry,
        engine: Box<dyn StackEngine>,
    ) -> RenderStack<B> {
        RenderStack {
            shading,
            batcher: Some(batcher),
            registry,
            wires: VMeshWire::new(),
            directory: None,
            engine,
            published: false,
            dumps: Vec::new(),
            face: None,
            ib_locks: Vec::new(),
            particle_vb_lock: None,
            particle_ib_lock: None,
            uv_binding: None,
        }
    }

    /// Gives the services the render manager's faces.
    pub fn set_directory(&mut self, directory: MeshDirectory) {
        self.directory = Some(directory);
    }

    /// The batcher and its library.
    ///
    /// # Panics
    ///
    /// While the batcher is out ([`RenderStack::with_batcher`]).
    #[must_use]
    pub fn batcher(&self) -> &MaterialBatcher<RenderStack<B>> {
        self.batcher.as_ref().expect("the batcher is in place")
    }

    /// Runs `f` with the batcher taken out, so that it can call the
    /// materials with these services (loading a library, creating,
    /// releasing, flushing). `None` while it is already out.
    pub fn with_batcher<R>(
        &mut self,
        f: impl FnOnce(&mut MaterialBatcher<RenderStack<B>>, &mut RenderStack<B>) -> R,
    ) -> Option<R> {
        let mut batcher = self.batcher.take()?;
        let result = f(&mut batcher, self);
        self.batcher = Some(batcher);
        Some(result)
    }

    /// `FlushAll(clear)` on the batcher.
    pub fn flush_all(&mut self, clear: u8) -> i32 {
        self.with_batcher(|b, s| b.flush_all(s, clear))
            .unwrap_or(GR_GENERIC)
    }

    /// Runs `f` on a material taken out of the arena; `None` for a handle
    /// that names nothing or a material already out (divergence D359).
    fn with_material<R>(
        &mut self,
        id: MaterialId,
        f: impl FnOnce(&mut dyn Material<RenderStack<B>>, &mut RenderStack<B>) -> R,
    ) -> Option<R> {
        let mut object = self
            .batcher
            .as_mut()?
            .library_mut()
            .materials_mut()
            .take(id)?;
        let result = f(object.as_mut(), self);
        let orphan = match self.batcher.as_mut() {
            Some(batcher) => batcher.library_mut().materials_mut().restore(id, object),
            None => Some(object),
        };
        if let Some(mut orphan) = orphan {
            orphan.destroy(self);
        }
        Some(result)
    }

    /// A material of the arena, for reading.
    fn material_ref(&self, id: MaterialId) -> Option<&dyn Material<RenderStack<B>>> {
        self.batcher.as_ref()?.library().materials().get(id)
    }

    /// `IMaterial::Submit` on a material of the library: a batched
    /// material selects its bin and is queued, or drawn through its
    /// batched face in a bin that draws at once; any other material draws.
    fn submit_material(
        &mut self,
        id: MaterialId,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        let route = self.with_material(id, |object, services| {
            match object.batch_route(context, args) {
                None => Err(object.submit(services, context, args)),
                Some(Err(result)) => Err(result),
                Some(Ok(route)) => Ok(route),
            }
        });
        let route = match route {
            None => return GR_GENERIC,
            Some(Err(result)) => return result,
            Some(Ok(route)) => route,
        };
        let Some(batcher) = self.batcher.as_mut() else {
            return GR_GENERIC;
        };
        batcher.select_bin(route.bin);
        let args = DrawArgs {
            flags: route.flags,
            ..*args
        };
        if !batcher.selected_is_immediate() {
            let input = SubmitInput {
                instance: context.instance,
                world: *context.world,
                view: *context.view,
                vertices: Rc::new(context.vertices.cloned().unwrap_or_default()),
                indices: Rc::from(context.indices),
                vb: context.vb.copied().unwrap_or_default(),
                ib: context.ib.copied().unwrap_or_default(),
                replaced: context.replaced,
                field20: context.field20,
            };
            batcher.queue(id, &input, &args, route.depth_bits);
            return GR_OK;
        }
        self.with_material(id, |object, services| {
            object.batch_bind(services, context);
            object.batch_submit(services, context, &args);
            object.batch_teardown(services, context)
        })
        .unwrap_or(GR_GENERIC)
    }

    /// The face of `instance`: the one `query_mesh` found for it, else the
    /// directory's.
    fn face_of(&mut self, instance: Instance) -> Option<MeshFace> {
        if let Some((cached, face)) = &self.face
            && *cached == instance
        {
            return Some(face.clone());
        }
        let face = self.lookup_face(instance)?;
        self.face = Some((instance, face.clone()));
        Some(face)
    }

    /// `QueryInstance(instance, IID_IMesh)`: the engine names the
    /// instance's archetype (`GetInstanceArchetype`, `ReleaseHandle`), the
    /// render manager its current level's face.
    fn lookup_face(&mut self, instance: Instance) -> Option<MeshFace> {
        let archetype = self.engine.instance_archetype(instance);
        if archetype == -1 {
            return None;
        }
        self.engine.release_handle(archetype);
        self.directory
            .as_ref()?
            .instance_face(archetype, instance)
            .ok()
    }

    /// The instance's wire (`QueryInstance(instance, IID_IVMeshWire)` on
    /// the `VMeshWire` component).
    fn wire_of(
        &mut self,
        instance: Instance,
    ) -> Option<Rc<neuron_core::mesh_data::vmesh::VWireData>> {
        let wires = std::mem::take(&mut self.wires);
        let wire = wires.query_instance(self, instance, MeshIid::VMeshWire);
        self.wires = wires;
        wire
    }

    fn ib_source(&self, number: u32) -> Option<IndexSource> {
        if number == SCRATCH_IB {
            Some(IndexSource::Scratch)
        } else {
            self.shading.ib(number)
        }
    }
}

fn buffer_code<T>(result: Result<T, BufferError>) -> i32 {
    match result {
        Ok(_) => GR_OK,
        Err(e) => e.code().cast_signed(),
    }
}

impl<B: Backend + 'static> BatchDevice for RenderStack<B> {
    fn select_vb(&mut self, vertex_buffer: u32) -> i32 {
        BatchDevice::select_vb(&mut self.shading, vertex_buffer)
    }

    fn select_ib(&mut self, index_buffer: u32, base_vertex: u32) -> i32 {
        let source = self.ib_source(index_buffer);
        match self.shading.device.buffers.select_ib(source, base_vertex) {
            Ok(()) => GR_OK,
            Err(_) => E_FAIL,
        }
    }

    fn pipeline_state(&mut self, id: u32) -> Option<i32> {
        BatchDevice::pipeline_state(&mut self.shading, id)
    }

    fn set_modelview(&mut self, matrix: &[f32; 12]) {
        BatchDevice::set_modelview(&mut self.shading, matrix);
    }

    fn set_world(&mut self, matrix: &[f32; 12]) {
        BatchDevice::set_world(&mut self.shading, matrix);
    }

    fn set_view(&mut self, matrix: &[f32; 12]) {
        BatchDevice::set_view(&mut self.shading, matrix);
    }
}

impl<B: Backend + 'static> MaterialServices for RenderStack<B> {
    type TextureRecord = RecordId;
    type Animation = InstanceId;

    fn set_material(&mut self, block: &[u32; 17]) {
        self.shading.set_material(block);
    }

    fn set_render_state(&mut self, state: u32, value: u32) {
        MaterialServices::set_render_state(&mut self.shading, state, value);
    }

    fn render_state(&mut self, state: u32) -> u32 {
        MaterialServices::render_state(&mut self.shading, state)
    }

    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32) {
        MaterialServices::set_stage_state(&mut self.shading, stage, state, value);
    }

    fn set_stage_texture(&mut self, stage: u32, handle: u32) {
        MaterialServices::set_stage_texture(&mut self.shading, stage, handle);
    }

    fn set_texture_transform(&mut self, stage: u32, matrix: &[f32; 16]) {
        self.shading.set_texture_transform(stage, matrix);
    }

    fn texture_format(&mut self, handle: u32) -> Result<u32, i32> {
        self.shading.texture_format(handle)
    }

    fn verify_state(&mut self) -> i32 {
        self.shading.verify_state()
    }

    fn draw_indexed_vb(
        &mut self,
        prim: u32,
        vertex_buffer: u32,
        base_vertex: u32,
        vertex_count: u32,
        indices: &[u16],
    ) -> i32 {
        self.shading
            .draw_indexed_vb(prim, vertex_buffer, base_vertex, vertex_count, indices)
    }

    fn draw_indexed(
        &mut self,
        prim: u32,
        min_index: u32,
        vertex_count: u32,
        start_index: u32,
        count: u32,
    ) -> i32 {
        MaterialServices::draw_indexed(
            &mut self.shading,
            prim,
            min_index,
            vertex_count,
            start_index,
            count,
        )
    }

    fn lock_vertex_memory(&mut self, fvf: u32, count: u32) -> Result<VertexLock, i32> {
        MaterialServices::lock_vertex_memory(&mut self.shading, fvf, count)
    }

    fn copy_vertex_data(
        &mut self,
        dest: &mut [u8],
        fvf: u32,
        source: &VertexSource,
        start: u32,
        count: u32,
    ) {
        MaterialServices::copy_vertex_data(&mut self.shading, dest, fvf, source, start, count);
    }

    fn unlock_vertex_memory(&mut self, lock: VertexLock) {
        MaterialServices::unlock_vertex_memory(&mut self.shading, lock);
    }

    fn find_texture(&mut self, name: &[u8], crc: u32) -> Option<RecordId> {
        self.shading.find_texture(name, crc)
    }

    fn texture_frame_count(&mut self, record: RecordId) -> Option<i32> {
        self.shading.texture_frame_count(record)
    }

    fn create_playback(&mut self, record: RecordId) -> Result<i32, i32> {
        self.shading.create_playback(record)
    }

    fn texture_frame(&mut self, id: i32, frame: i32) -> Option<FrameOut> {
        self.shading.texture_frame(id, frame)
    }

    fn release_texture_record(&mut self, record: RecordId) {
        self.shading.release_texture_record(record);
    }

    fn dec_ref_texture(&mut self, id: i32) {
        self.shading.dec_ref_texture(id);
    }

    fn has_animation_library(&mut self) -> bool {
        self.shading.has_animation_library()
    }

    fn create_animation_instance(&mut self, name: Option<&[u8]>, crc: u32) -> Option<InstanceId> {
        self.shading.create_animation_instance(name, crc)
    }

    fn release_animation_instance(&mut self, instance: InstanceId) {
        self.shading.release_animation_instance(instance);
    }

    fn animation_uv_matrix(&mut self, instance: InstanceId) -> Option<[f32; 16]> {
        self.shading.animation_uv_matrix(instance)
    }

    /// The atmosphere's `GetInstanceExtent` on the engine `Field20` names:
    /// the stack's engine for the sphere's [`FIELD20_ENGINE`], else
    /// [`Shading`]'s.
    fn instance_extent(&mut self, field20: usize, instance: usize) -> Option<(f32, [f32; 3])> {
        if field20 == FIELD20_ENGINE {
            return self.engine.instance_extent(instance);
        }
        MaterialServices::instance_extent(&mut self.shading, field20, instance)
    }

    fn global_record(&mut self, index: u32) -> u32 {
        self.shading.global_record(index)
    }
}

impl<B: Backend + 'static> SurfaceServices for RenderStack<B> {
    fn material(&mut self) -> Option<[u32; 17]> {
        SurfaceServices::material(&mut self.shading)
    }

    fn stage_state(&mut self, stage: u32, state: u32) -> Option<u32> {
        self.shading.stage_state(stage, state)
    }

    fn draw_vb(&mut self, prim: u32, vertex_buffer: u32, start: u32, count: u32) -> i32 {
        SurfaceServices::draw_vb(&mut self.shading, prim, vertex_buffer, start, count)
    }

    fn record_frame(&mut self, record: RecordId, frame: i32) -> Option<FrameOut> {
        self.shading.record_frame(record, frame)
    }

    fn has_engine(&mut self) -> bool {
        self.published
    }

    fn instance_user_data(&mut self, instance: usize) -> Option<[u32; 5]> {
        self.engine.user_data(instance)
    }

    fn instance_root(&mut self, instance: usize) -> usize {
        self.engine.instance_root(instance)
    }

    fn instance_relative(&mut self, instance: usize) {
        let _ = instance;
    }

    fn query_wire(&mut self, instance: usize) -> bool {
        self.wire_of(instance).is_some()
    }

    fn wire_draw(&mut self, instance: usize) {
        if let Some(wire) = self.wire_of(instance) {
            let registry = Rc::clone(&self.registry);
            draw_wire(&wire, &mut registry.borrow_mut(), self);
        }
    }

    fn query_mesh(&mut self, instance: usize) -> bool {
        self.face = None;
        self.face_of(instance).is_some()
    }

    fn mesh_open_face_group(&mut self, instance: usize, index: i32, mode: i32) -> i32 {
        self.face_of(instance)
            .map_or(GR_GENERIC, |f| f.open_face_group(index, mode))
    }

    fn mesh_close_face_group(&mut self, instance: usize) {
        if let Some(face) = self.face_of(instance) {
            let _ = face.close_face_group();
        }
    }

    fn mesh_open_channels(&mut self, instance: usize, flags: u32) -> i32 {
        self.face_of(instance)
            .map_or(GR_GENERIC, |f| f.open_channels(flags))
    }

    fn mesh_lock_channels(&mut self, instance: usize, mask: u32) {
        if let Some(face) = self.face_of(instance) {
            let _ = face.lock_channels(mask);
        }
    }

    fn mesh_unlock_channels(&mut self, instance: usize) {
        if let Some(face) = self.face_of(instance) {
            let _ = face.unlock_channels();
        }
    }

    fn mesh_close_channels(&mut self, instance: usize) {
        if let Some(face) = self.face_of(instance) {
            let _ = face.close_channels();
        }
    }

    fn mesh_data(&mut self, instance: usize, channel: u32) -> Option<Vec<u32>> {
        self.face_of(instance)?
            .face_group_data(channel)
            .ok()
            .and_then(|data| data.words())
    }

    fn mesh_indices(&mut self, instance: usize, channel: u32) -> MeshIndices {
        match self
            .face_of(instance)
            .map(|f| f.face_group_indices(channel))
        {
            Some(Ok(found)) => MeshIndices {
                indices: found.indices,
                count: found.count,
            },
            _ => MeshIndices::default(),
        }
    }

    fn instance_value(&mut self, id: i32) -> Option<u32> {
        self.shading.values.word(id)
    }

    fn instance_value_name(&mut self, id: i32) -> Option<Vec<u8>> {
        self.shading.values.name(id)
    }

    fn count_visible(&mut self, index_span: u32, count: u32) {
        self.shading.count_visible(index_span, count);
    }

    fn candidate_is_batched(&mut self, candidate: MaterialId) -> bool {
        self.material_ref(candidate).is_some_and(|m| m.is_batched())
    }

    fn candidate_bind(
        &mut self,
        candidate: MaterialId,
        batched: bool,
        context: &SubmitContext<'_>,
    ) -> i32 {
        self.with_material(candidate, |object, services| {
            if batched {
                object.batch_bind(services, context)
            } else {
                object.bind(services, Some(context))
            }
        })
        .unwrap_or(GR_GENERIC)
    }

    fn candidate_teardown(
        &mut self,
        candidate: MaterialId,
        batched: bool,
        context: &SubmitContext<'_>,
    ) -> i32 {
        self.with_material(candidate, |object, services| {
            if batched {
                object.batch_teardown(services, context)
            } else {
                object.teardown(services, Some(context))
            }
        })
        .unwrap_or(GR_GENERIC)
    }

    fn candidate_submit(
        &mut self,
        candidate: MaterialId,
        batched: bool,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        if batched {
            return self
                .with_material(candidate, |object, services| {
                    object.batch_submit(services, context, args)
                })
                .unwrap_or(GR_GENERIC);
        }
        self.submit_material(candidate, context, args)
    }

    fn candidate_pass_count(&mut self, candidate: MaterialId) -> Option<i32> {
        self.material_ref(candidate)?.pass_count().ok()
    }

    fn candidate_class_name(&mut self, candidate: MaterialId) -> Vec<u8> {
        self.material_ref(candidate)
            .map(|m| m.class_name().as_bytes().to_vec())
            .unwrap_or_default()
    }

    fn candidate_parameter_tag(
        &mut self,
        candidate: MaterialId,
        index: u32,
    ) -> Result<Option<Vec<u8>>, i32> {
        self.material_ref(candidate)
            .ok_or(GR_GENERIC)?
            .parameter_tag(index)
    }

    fn candidate_parameter_word(&mut self, candidate: MaterialId, index: u32) -> u32 {
        self.material_ref(candidate)
            .and_then(|m| m.parameter(index).ok())
            .map_or(0, |v| v.words[0])
    }
}

impl<B: Backend + 'static> MeshServices for RenderStack<B> {
    fn pipeline_state(&mut self, id: u32) -> Option<i32> {
        BatchDevice::pipeline_state(&mut self.shading, id)
    }

    fn set_modelview(&mut self, matrix: &[f32; 12]) {
        BatchDevice::set_modelview(&mut self.shading, matrix);
    }

    fn set_world(&mut self, matrix: &[f32; 12]) {
        BatchDevice::set_world(&mut self.shading, matrix);
    }

    fn set_view(&mut self, matrix: &[f32; 12]) {
        BatchDevice::set_view(&mut self.shading, matrix);
    }

    fn lock_vertex_memory(&mut self, fvf: u32, count: u32) -> Result<VertexLock, i32> {
        MaterialServices::lock_vertex_memory(&mut self.shading, fvf, count)
    }

    fn copy_vertex_data(
        &mut self,
        dest: &mut [u8],
        fvf: u32,
        source: &VertexSource,
        start: u32,
        count: u32,
    ) -> i32 {
        let stride = Fvf(fvf).stride() as usize;
        let mut scratch = vec![0u8; start as usize * stride];
        scratch.extend_from_slice(dest);
        match vertex_copy::copy_vertex_data(&mut scratch, fvf, &copy_source(source), start, count) {
            Ok(()) => {
                dest.copy_from_slice(&scratch[start as usize * stride..]);
                GR_OK
            }
            Err(_) => E_FAIL,
        }
    }

    fn unlock_vertex_memory(&mut self, lock: VertexLock) {
        MaterialServices::unlock_vertex_memory(&mut self.shading, lock);
    }

    fn select_vb(&mut self, vertex_buffer: u32) -> i32 {
        BatchDevice::select_vb(&mut self.shading, vertex_buffer)
    }

    fn create_vb(&mut self, fvf: u32, count: u32) -> Result<u32, i32> {
        let handle = self.shading.device.buffers.create_vb(Fvf(fvf), count, 0);
        Ok(self.shading.vb_number(handle))
    }

    fn lock_vb(&mut self, vertex_buffer: u32, count: u32) -> i32 {
        match self.shading.vb(vertex_buffer) {
            Some(h) => buffer_code(self.shading.device.buffers.lock_vb(h, 0, count)),
            None => E_FAIL,
        }
    }

    fn copy_vertices(
        &mut self,
        vertex_buffer: u32,
        base: u32,
        source: &VertexSource,
        start: u32,
        count: u32,
    ) -> (i32, u32) {
        let h = self.shading.vb(vertex_buffer);
        match self
            .shading
            .device
            .buffers
            .copy_vertices(h, base, &copy_source(source), start, count)
        {
            Ok(at) => (GR_OK, at),
            Err(e) => (e.code().cast_signed(), base),
        }
    }

    fn unlock_vb(&mut self, vertex_buffer: u32) {
        if let Some(h) = self.shading.vb(vertex_buffer) {
            let _ = self.shading.device.buffers.unlock_vb(h);
        }
    }

    fn destroy_vb(&mut self, vertex_buffer: u32) {
        if let Some(h) = self.shading.vb(vertex_buffer) {
            let _ = self.shading.device.buffers.destroy_vb(h);
        }
    }

    fn select_ib(&mut self, index_buffer: u32, base_vertex: u32) -> i32 {
        BatchDevice::select_ib(self, index_buffer, base_vertex)
    }

    fn create_ib(&mut self, count: u32) -> Result<u32, i32> {
        let handle = self.shading.device.buffers.create_ib(count, 0);
        Ok(self.shading.ib_number(IndexSource::Buffer(handle)))
    }

    fn lock_ib(&mut self, index_buffer: u32, count: u32) -> i32 {
        let Some(source) = self.ib_source(index_buffer) else {
            return E_FAIL;
        };
        match self.shading.device.buffers.lock_ib(source, 0, count) {
            Ok(lock) => {
                self.ib_locks.retain(|&(n, _)| n != index_buffer);
                self.ib_locks.push((index_buffer, lock.start));
                GR_OK
            }
            Err(e) => e.code().cast_signed(),
        }
    }

    fn unlock_ib(&mut self, index_buffer: u32, written: &[u16]) {
        let Some(source) = self.ib_source(index_buffer) else {
            return;
        };
        let start = self
            .ib_locks
            .iter()
            .find(|&&(n, _)| n == index_buffer)
            .map_or(0, |&(_, s)| s);
        self.ib_locks.retain(|&(n, _)| n != index_buffer);
        let _ = self.shading.device.buffers.write_ib(source, start, written);
        let _ = self.shading.device.buffers.unlock_ib(source);
    }

    fn destroy_ib(&mut self, index_buffer: u32) {
        if let Some(IndexSource::Buffer(h)) = self.ib_source(index_buffer) {
            let _ = self.shading.device.buffers.destroy_ib(h);
        }
    }

    fn copy_scratch_indices(&mut self, indices: &[u16]) -> u32 {
        self.shading
            .device
            .buffers
            .copy_indices(IndexSource::Scratch, 0, indices)
            .unwrap_or(0)
    }

    fn draw_indexed(
        &mut self,
        prim: u32,
        min_index: u32,
        vertex_count: u32,
        start_index: u32,
        count: u32,
    ) -> i32 {
        MaterialServices::draw_indexed(
            &mut self.shading,
            prim,
            min_index,
            vertex_count,
            start_index,
            count,
        )
    }

    fn instance_transform(&mut self, instance: Instance) -> [f32; 12] {
        self.engine.instance_transform(instance)
    }

    fn instance_archetype(&mut self, instance: Instance) -> Archetype {
        self.engine.instance_archetype(instance)
    }

    fn release_handle(&mut self, archetype: Archetype) {
        self.engine.release_handle(archetype);
    }

    fn instance_extent(&mut self, instance: Instance) -> Option<(f32, [f32; 3])> {
        self.engine.instance_extent(instance)
    }

    fn set_instance_extent(&mut self, instance: Instance, radius: f32, centre: [f32; 3]) {
        self.engine.set_instance_extent(instance, radius, centre);
    }

    fn related_instance(
        &mut self,
        instance: Instance,
        previous: Option<Instance>,
    ) -> Option<Instance> {
        self.engine.related_instance(instance, previous)
    }

    fn instance_root(&mut self, instance: Instance) {
        self.engine.instance_root(instance);
    }

    fn find_material(&mut self, crc: u32) -> (i32, Option<MaterialId>) {
        let Some(batcher) = self.batcher.as_mut() else {
            return (GR_GENERIC, None);
        };
        match batcher.library_mut().find_material_by_crc(crc) {
            Ok(material) => (GR_OK, material),
            Err(result) => (result, None),
        }
    }

    fn add_ref_material(&mut self, material: MaterialId) {
        if let Some(batcher) = self.batcher.as_mut() {
            batcher.library_mut().materials_mut().add_ref(material);
        }
    }

    fn release_material(&mut self, material: MaterialId) {
        self.with_batcher(|b, s| b.library_mut().materials_mut().release(material, s));
    }

    fn material_flags(&mut self, material: MaterialId) -> u32 {
        self.with_material(material, |object, services| {
            object.flags(services).unwrap_or(0)
        })
        .unwrap_or(0)
    }

    fn material_name(&mut self, material: MaterialId) -> Option<Vec<u8>> {
        self.material_ref(material)?.name().ok()
    }

    fn material_class(&mut self, material: MaterialId) -> Vec<u8> {
        self.material_ref(material)
            .map(|m| m.class_name().as_bytes().to_vec())
            .unwrap_or_default()
    }

    fn material_parameter_count(&mut self, material: MaterialId) -> u32 {
        self.material_ref(material)
            .and_then(|m| m.parameter_count().ok())
            .unwrap_or(0)
    }

    fn material_parameter(&mut self, material: MaterialId, index: u32) -> (u32, u32) {
        self.material_ref(material)
            .and_then(|m| m.parameter(index).ok())
            .map_or((0, 0), |v| (v.kind, v.words[0]))
    }

    fn bind(&mut self, material: MaterialId, context: &SubmitContext<'_>) {
        self.with_material(material, |object, services| {
            object.bind(services, Some(context))
        });
    }

    fn submit(&mut self, material: MaterialId, context: &SubmitContext<'_>, args: &DrawArgs) {
        self.submit_material(material, context, args);
    }

    fn teardown(&mut self, material: MaterialId, context: &SubmitContext<'_>) {
        self.with_material(material, |object, services| {
            object.teardown(services, Some(context))
        });
    }

    fn dump(&mut self, flags: i32, file: &str, line: u32, message: &str) {
        self.dumps.push(StackDump {
            flags,
            text: format!("{file}({line}) : {message}"),
        });
    }

    fn log_event(&mut self, name: &str, value: f32, flags: u32) {
        let _ = flags;
        self.dumps.push(StackDump {
            flags: 0,
            text: format!("{name} {value}"),
        });
    }
}

impl<B: Backend + 'static> EyeHost for RenderStack<B> {
    fn eye_instance_parent(&mut self, instance: i32) -> i32 {
        usize::try_from(instance).map_or(-1, |i| self.engine.instance_parent(i))
    }

    fn eye_instance_orientation(&mut self, instance: i32) -> [f32; 9] {
        usize::try_from(instance).map_or([1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0], |i| {
            self.engine.instance_orientation(i)
        })
    }

    fn take_uv_binding(&mut self) -> Option<UvBinding> {
        self.uv_binding.take()
    }

    fn restore_uv_binding(&mut self, binding: UvBinding) {
        self.uv_binding = Some(binding);
    }

    fn eye_dump(&mut self, flags: i32, file: &str, line: u32, message: &str) {
        MeshServices::dump(self, flags, file, line, message);
    }
}

/// The appearance's services: the stack and the render manager it draws
/// through (`IRenderer2::RenderInstance` and `GetBounds`).
pub struct StackAppearance<'a, B: Backend + 'static> {
    /// The services.
    pub stack: &'a mut RenderStack<B>,
    /// The render manager.
    pub manager: &'a mut RenderManager<RenderStack<B>>,
}

/// `QueryDeviceAbility(9)`: depth bias (`D3DPRASTERCAPS_ZBIAS`), which
/// every device the port runs on has.
const ABILITY_DEPTH_BIAS: u32 = 9;

impl<B: Backend + 'static> AppearanceServices for StackAppearance<'_, B> {
    type TextureRecord = RecordId;

    fn query(&mut self, face: Face) -> bool {
        let _ = face;
        true
    }

    fn has_system(&mut self) -> bool {
        true
    }

    fn publish_engine(&mut self) {
        self.stack.published = true;
    }

    fn find_or_create(&mut self, name: &str, type_name: &str) -> (i32, Option<MaterialId>) {
        let found = self.stack.with_batcher(|b, s| {
            b.library_mut()
                .find_or_create(s, name.as_bytes(), type_name.as_bytes())
        });
        match found {
            Some(Ok(material)) => (GR_OK, material),
            Some(Err(result)) => (result, None),
            None => (GR_GENERIC, None),
        }
    }

    fn remove_material(&mut self, name: &str, crc: u32) {
        let _ = name;
        self.stack
            .with_batcher(|b, s| b.library_mut().remove_material(s, 0, crc));
    }

    fn set_global_record(&mut self, index: u32, value: u32) {
        self.stack.shading.globals.set_record(index, value);
    }

    fn release_material(&mut self, material: MaterialId) {
        MeshServices::release_material(self.stack, material);
    }

    fn instance_values(&mut self) -> &mut InstanceValues {
        &mut self.stack.shading.values
    }

    fn device_ability(&mut self, index: u32) -> Option<u32> {
        let abilities = self.stack.shading.abilities;
        Some(match index {
            5 => abilities.square_only,
            6 => abilities.max_width,
            7 => abilities.max_height,
            ABILITY_DEPTH_BIAS => 1,
            _ => 0,
        })
    }

    fn set_perspective(&mut self, fov_y_deg: f32, aspect: f32, near: f32, far: f32) {
        let _ = self
            .stack
            .shading
            .device
            .transforms
            .set_perspective(fov_y_deg, aspect, near, far);
    }

    fn render_state(&mut self, state: u32) -> u32 {
        MaterialServices::render_state(self.stack, state)
    }

    fn set_render_state(&mut self, state: u32, value: u32) {
        MaterialServices::set_render_state(self.stack, state, value);
    }

    fn set_modelview(&mut self, matrix: &[f32; 12]) {
        BatchDevice::set_modelview(self.stack, matrix);
    }

    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32) {
        MaterialServices::set_stage_state(self.stack, stage, state, value);
    }

    fn set_stage_texture(&mut self, stage: u32, handle: u32) {
        MaterialServices::set_stage_texture(self.stack, stage, handle);
    }

    fn draw_vb(&mut self, prim: u32, vertex_buffer: u32, start: u32, count: u32) {
        SurfaceServices::draw_vb(self.stack, prim, vertex_buffer, start, count);
    }

    fn find_texture(&mut self, name: &str, crc: u32) -> Result<RecordId, i32> {
        let _ = name;
        match self.stack.shading.textures.find_by_crc(crc, true) {
            Ok(Some(record)) => Ok(record),
            Ok(None) => Err(GR_GENERIC),
            Err(result) => Err(result),
        }
    }

    fn record_frame(&mut self, record: &RecordId, frame: i32) -> Option<FrameOut> {
        self.stack.shading.record_frame(*record, frame)
    }

    fn release_texture(&mut self, record: RecordId) {
        self.stack.shading.release_texture_record(record);
    }

    fn lock_vertex_memory(&mut self, fvf: u32, count: u32) -> Result<VertexLock, i32> {
        MaterialServices::lock_vertex_memory(self.stack, fvf, count)
    }

    fn unlock_vertex_memory(&mut self, lock: VertexLock) {
        MaterialServices::unlock_vertex_memory(self.stack, lock);
    }

    fn instance_transform(&mut self, instance: Instance) -> [f32; 12] {
        self.stack.engine.instance_transform(instance)
    }

    fn instance_archetype(&mut self, instance: Instance) -> Archetype {
        self.stack.engine.instance_archetype(instance)
    }

    fn release_handle(&mut self, archetype: Archetype) {
        self.stack.engine.release_handle(archetype);
    }

    fn bounds(&mut self, archetype: Archetype, lod: f32) -> (i32, Option<[f32; 6]>) {
        self.manager.bounds(archetype, lod)
    }

    fn render_instance(
        &mut self,
        camera: &mut dyn Camera,
        instance: Instance,
        threshold: f32,
        flags: u32,
        extra: Option<MaterialId>,
    ) {
        let request = DrawRequest {
            flags,
            matrix: None,
            extra,
        };
        self.manager.render_instance(
            self.stack,
            &mut MeshView(camera),
            instance,
            threshold,
            &request,
        );
    }

    fn reset_visible(&mut self) {
        self.stack.shading.visible = [0; 3];
    }

    fn visible(&mut self) -> [u32; 3] {
        self.stack.shading.visible
    }

    fn dump(&mut self, flags: i32, text: &str) {
        self.stack.dumps.push(StackDump {
            flags,
            text: text.to_owned(),
        });
    }
}

impl<B: Backend + 'static> ParticleServices for RenderStack<B> {
    fn find_texture(&mut self, name: &[u8], crc: u32) -> Option<TextureRecord> {
        MaterialServices::find_texture(&mut self.shading, name, crc)
            .and_then(|record| u32::try_from(record).ok())
    }

    fn create_playback(&mut self, record: TextureRecord) -> i32 {
        MaterialServices::create_playback(&mut self.shading, record as RecordId).unwrap_or(-1)
    }

    fn frame_count(&mut self, record: TextureRecord) -> Option<i32> {
        self.shading.texture_frame_count(record as RecordId)
    }

    fn release_texture_record(&mut self, record: TextureRecord) {
        MaterialServices::release_texture_record(&mut self.shading, record as RecordId);
    }

    fn set_wrap_mode(&mut self, id: i32, mode: i32) {
        self.shading.textures.set_wrap(id, mode);
    }

    fn texture_frame(&mut self, id: i32, frame: i32) -> Option<FrameOut> {
        MaterialServices::texture_frame(&mut self.shading, id, frame)
    }

    fn advance_time(&mut self, id: i32, seconds: f32) {
        self.shading.textures.advance(id, seconds);
    }

    fn dec_ref_texture(&mut self, id: i32) {
        MaterialServices::dec_ref_texture(&mut self.shading, id);
    }
}

/// The particle draws on the device model: the render states and stages
/// through [`Shading`], the module's vertex buffer by its renderer number,
/// the dynamic index buffer as the scratch buffer.
impl<B: Backend + 'static> ParticleDraw for RenderStack<B> {
    fn render_state(&mut self, state: u32) -> u32 {
        MaterialServices::render_state(&mut self.shading, state)
    }

    fn set_render_state(&mut self, state: u32, value: u32) {
        MaterialServices::set_render_state(&mut self.shading, state, value);
    }

    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32) {
        MaterialServices::set_stage_state(&mut self.shading, stage, state, value);
    }

    fn set_stage_texture(&mut self, stage: u32, handle: u32) {
        MaterialServices::set_stage_texture(&mut self.shading, stage, handle);
    }

    fn set_modelview(&mut self, matrix: &[f32; 12]) {
        BatchDevice::set_modelview(&mut self.shading, matrix);
    }

    fn select_vb(&mut self, vertex_buffer: u32) -> i32 {
        if vertex_buffer == 0 {
            return E_FAIL;
        }
        BatchDevice::select_vb(&mut self.shading, vertex_buffer)
    }

    fn create_vb(&mut self, fvf: u32, count: u32, flags: u32) -> (i32, u32) {
        let handle = self
            .shading
            .device
            .buffers
            .create_vb(Fvf(fvf), count, flags);
        (GR_OK, self.shading.vb_number(handle))
    }

    fn destroy_vb(&mut self, vertex_buffer: u32) {
        if let Some(h) = self.shading.vb(vertex_buffer) {
            let _ = self.shading.device.buffers.destroy_vb(h);
        }
    }

    fn lock_vb(&mut self, vertex_buffer: u32, count: u32) -> Result<u32, i32> {
        let h = self.shading.vb(vertex_buffer).ok_or(E_FAIL)?;
        let lock = self
            .shading
            .device
            .buffers
            .lock_vb(h, 0, count)
            .map_err(|e| e.code().cast_signed())?;
        self.particle_vb_lock = Some((vertex_buffer, lock.offset));
        Ok(lock.start)
    }

    fn unlock_vb(&mut self, vertex_buffer: u32, written: &[u8]) {
        let Some(h) = self.shading.vb(vertex_buffer) else {
            return;
        };
        if let Some((locked, offset)) = self.particle_vb_lock.take()
            && locked == vertex_buffer
        {
            let _ = self.shading.device.buffers.write_vb(h, offset, written);
        }
        let _ = self.shading.device.buffers.unlock_vb(h);
    }

    fn lock_ib(&mut self, count: u32) -> Result<u32, i32> {
        let lock = self
            .shading
            .device
            .buffers
            .lock_ib(IndexSource::Scratch, 0, count)
            .map_err(|e| e.code().cast_signed())?;
        self.particle_ib_lock = Some(lock.start);
        Ok(lock.start)
    }

    fn unlock_ib(&mut self, written: &[u16]) {
        if let Some(start) = self.particle_ib_lock.take() {
            let _ = self
                .shading
                .device
                .buffers
                .write_ib(IndexSource::Scratch, start, written);
        }
        let _ = self.shading.device.buffers.unlock_ib(IndexSource::Scratch);
    }

    fn select_ib(&mut self, base_vertex: u32) -> i32 {
        BatchDevice::select_ib(self, SCRATCH_IB, base_vertex)
    }

    fn draw_indexed(
        &mut self,
        prim: u32,
        min_index: u32,
        vertex_count: u32,
        start_index: u32,
        index_count: u32,
    ) -> i32 {
        MaterialServices::draw_indexed(
            &mut self.shading,
            prim,
            min_index,
            vertex_count,
            start_index,
            index_count,
        )
    }
}
