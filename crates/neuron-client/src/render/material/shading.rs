//! What the materials draw and load with: the device model
//! ([`crate::render::ff::Device`]), the texture library
//! ([`crate::render::texture`]) with the renderer's texture numbers, the
//! global material records, material animation and the engine's instance
//! extents, as [`MaterialServices`] and [`BatchDevice`].
//!
//! Each call maps to the device as `render::trace`'s model replay maps the
//! same call of a trace: render and stage states into the state model, the
//! material block into the lights, stage textures through the texture
//! number table, transforms into the transform set, `LockVertexMemory` into
//! the pool ring and `CopyVertexData` through the renderer's converters.
//! Vertex and index buffers have numbers of the renderer interface too
//! ([`Shading::vb_number`], [`Shading::ib_number`]): the materials and the
//! batcher name buffers as the C++ named them, by pointer.

use std::collections::{BTreeMap, HashMap};

use neuron_core::image::PixelFormat;

use super::GR_GENERIC;
use super::animation::{InstanceId, MaterialAnimations};
use super::batcher::{BatchDevice, DrawArgs, SubmitContext, VbListing, VertexSource, VertexStream};
use super::global::{GlobalRecords, UNSET};
use super::instance::MaterialId;
use super::services::{MaterialServices, VertexLock};
use super::surface::{MeshIndices, SurfaceEngine, SurfaceServices};
use crate::render::ff::buffers::{IndexSource, VbHandle};
use crate::render::ff::device::{Backend, Device};
use crate::render::ff::fvf::Fvf;
use crate::render::ff::lights::Material as DeviceMaterial;
use crate::render::ff::state::STAGES;
use crate::render::ff::vertex_copy::{self, Source, Stream};
use crate::render::texture::library::RecordId;
use crate::render::texture::{
    Abilities, DevicePipeline, FormatClasses, FrameOut, HandleTable, TextureLibrary,
    TexturePipeline,
};

/// `E_FAIL`, DX8RENDERER's result for a stream it cannot select.
const E_FAIL: i32 = 0x8000_4005_u32.cast_signed();

/// The engine's answer to `GetInstanceExtent`.
pub trait InstanceExtents {
    /// The radius and centre of `instance` on the engine `field20` names;
    /// `None` when nothing is written.
    fn extent(&mut self, field20: usize, instance: usize) -> Option<(f32, [f32; 3])>;
}

/// The services of the materials on the device model.
pub struct Shading<B: Backend> {
    pub device: Device<B>,
    /// The renderer's texture numbers.
    pub handles: HandleTable,
    pub classes: FormatClasses,
    pub abilities: Abilities,
    pub textures: TextureLibrary,
    pub globals: GlobalRecords,
    /// `QueryInterface(IID_IMatAnimLibrary)` on the renderer finds this.
    pub animations: Option<MaterialAnimations>,
    /// The engine `Field20` names, for the atmosphere.
    pub engine: Option<Box<dyn InstanceExtents>>,
    /// What `FLAppearance` publishes to the surface materials (the engine,
    /// the instance values); `None` before it is initialized.
    pub surface: Option<Box<dyn SurfaceEngine>>,
    /// `G_VisibleCounterA`, `B` and `C`.
    pub visible: [u32; 3],
    vbs: BTreeMap<u32, VbHandle>,
    vb_numbers: HashMap<VbHandle, u32>,
    ibs: BTreeMap<u32, IndexSource>,
    ib_numbers: HashMap<IndexSource, u32>,
    next_number: u32,
}

impl<B: Backend> std::fmt::Debug for Shading<B> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Shading")
            .field("vbs", &self.vbs.len())
            .field("ibs", &self.ibs.len())
            .finish_non_exhaustive()
    }
}

impl<B: Backend> Shading<B> {
    /// The services over `device` and `textures`, with the global records
    /// as `Shading.dll` leaves them registered, no animation library and no
    /// engine.
    #[must_use]
    pub fn new(device: Device<B>, textures: TextureLibrary, classes: FormatClasses) -> Shading<B> {
        Shading {
            device,
            handles: HandleTable::new(),
            classes,
            abilities: Abilities::default(),
            textures,
            globals: GlobalRecords::registered(),
            animations: None,
            engine: None,
            surface: None,
            visible: [0; 3],
            vbs: BTreeMap::new(),
            vb_numbers: HashMap::new(),
            ibs: BTreeMap::new(),
            ib_numbers: HashMap::new(),
            next_number: 1,
        }
    }

    /// The texture calls on the device ([`DevicePipeline`]).
    pub fn pipeline(&mut self) -> DevicePipeline<'_, B> {
        DevicePipeline {
            device: &mut self.device,
            handles: &mut self.handles,
            classes: &self.classes,
            abilities: self.abilities,
        }
    }

    /// The texture library with the texture calls on the device, for
    /// loading and collecting texture groups.
    pub fn texture_parts(&mut self) -> (&mut TextureLibrary, DevicePipeline<'_, B>) {
        (
            &mut self.textures,
            DevicePipeline {
                device: &mut self.device,
                handles: &mut self.handles,
                classes: &self.classes,
                abilities: self.abilities,
            },
        )
    }

    fn number(&mut self) -> u32 {
        let n = self.next_number;
        self.next_number = self.next_number.wrapping_add(1).max(1);
        n
    }

    /// The renderer's number of a vertex buffer, given on first use.
    pub fn vb_number(&mut self, buffer: VbHandle) -> u32 {
        if let Some(&n) = self.vb_numbers.get(&buffer) {
            return n;
        }
        let n = self.number();
        self.vbs.insert(n, buffer);
        self.vb_numbers.insert(buffer, n);
        n
    }

    /// The renderer's number of an index buffer, given on first use.
    pub fn ib_number(&mut self, source: IndexSource) -> u32 {
        if let Some(&n) = self.ib_numbers.get(&source) {
            return n;
        }
        let n = self.number();
        self.ibs.insert(n, source);
        self.ib_numbers.insert(source, n);
        n
    }

    /// The vertex buffer a number names; 0 and unknown numbers name none.
    #[must_use]
    pub fn vb(&self, number: u32) -> Option<VbHandle> {
        self.vbs.get(&number).copied()
    }

    /// The index buffer a number names.
    #[must_use]
    pub fn ib(&self, number: u32) -> Option<IndexSource> {
        self.ibs.get(&number).copied()
    }
}

fn copy_stream(stream: &VertexStream) -> Stream<'_> {
    Stream {
        data: stream.data.as_deref().unwrap_or_default(),
        byte_size: stream.byte_size,
        stride: stream.stride,
        index: stream.index.as_deref().unwrap_or_default(),
        count: stream.count,
    }
}

/// A material's vertex source as the renderer's converters read it.
#[must_use]
pub fn copy_source(source: &VertexSource) -> Source<'_> {
    Source {
        flags: source.flags,
        position: copy_stream(&source.position),
        normal: copy_stream(&source.normal),
        diffuse: copy_stream(&source.diffuse),
        tex: [copy_stream(&source.tex[0]), copy_stream(&source.tex[1])],
        fvf: source.fvf,
        count: source.count,
    }
}

impl<B: Backend> BatchDevice for Shading<B> {
    fn select_vb(&mut self, vertex_buffer: u32) -> i32 {
        let h = self.vb(vertex_buffer);
        match self.device.buffers.select_vb(h) {
            Ok(()) => 0,
            Err(_) => E_FAIL,
        }
    }

    fn select_ib(&mut self, index_buffer: u32, base_vertex: u32) -> i32 {
        let source = self.ib(index_buffer);
        match self.device.buffers.select_ib(source, base_vertex) {
            Ok(()) => 0,
            Err(_) => E_FAIL,
        }
    }

    fn pipeline_state(&mut self, id: u32) -> Option<i32> {
        self.device
            .state
            .pipeline_raw(id.cast_signed())
            .map(u32::cast_signed)
    }

    fn set_modelview(&mut self, matrix: &[f32; 12]) {
        self.device.set_modelview(matrix);
    }

    fn set_world(&mut self, matrix: &[f32; 12]) {
        self.device.transforms.set_world(matrix);
    }

    fn set_view(&mut self, matrix: &[f32; 12]) {
        self.device.transforms.set_view(matrix);
    }
}

impl<B: Backend> MaterialServices for Shading<B> {
    type TextureRecord = RecordId;
    type Animation = InstanceId;

    fn set_material(&mut self, block: &[u32; 17]) {
        self.device.lights.set_material(DeviceMaterial(*block));
    }

    fn set_render_state(&mut self, state: u32, value: u32) {
        let _ = self.device.state.set_render_state_raw(state, value);
    }

    fn render_state(&mut self, state: u32) -> u32 {
        self.device.state.render_state_raw(state).unwrap_or(0)
    }

    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32) {
        let _ = self.device.state.set_stage_state_raw(stage, state, value);
    }

    fn set_stage_texture(&mut self, stage: u32, handle: u32) {
        let texture = self.handles.get(handle);
        self.device.set_stage_texture(stage as usize, texture);
    }

    fn set_texture_transform(&mut self, stage: u32, matrix: &[f32; 16]) {
        if (stage as usize) < STAGES {
            self.device.transforms.set_texture(stage as usize, matrix);
        }
    }

    fn texture_format(&mut self, handle: u32) -> Result<u32, i32> {
        self.pipeline()
            .texture_format(handle)
            .map(|f: PixelFormat| f.0)
    }

    fn verify_state(&mut self) -> i32 {
        self.device.verify_state()
    }

    fn draw_indexed_vb(
        &mut self,
        prim: u32,
        vertex_buffer: u32,
        base_vertex: u32,
        vertex_count: u32,
        indices: &[u16],
    ) -> i32 {
        let vb = self.vb(vertex_buffer);
        match self
            .device
            .draw_indexed_vb(prim, vb, base_vertex, vertex_count, indices)
        {
            Ok(()) => 0,
            Err(e) => e.code().cast_signed(),
        }
    }

    fn draw_indexed(
        &mut self,
        prim: u32,
        min_index: u32,
        vertex_count: u32,
        start_index: u32,
        count: u32,
    ) -> i32 {
        match self
            .device
            .draw_indexed(prim, min_index, vertex_count, start_index, count)
        {
            Ok(()) => 0,
            Err(e) => e.code().cast_signed(),
        }
    }

    fn lock_vertex_memory(&mut self, fvf: u32, count: u32) -> Result<VertexLock, i32> {
        let (listing, lock) = self
            .device
            .buffers
            .lock_vertex_memory(Fvf(fvf), count)
            .map_err(|e| e.code().cast_signed())?;
        let number = self.vb_number(listing.buffer);
        Ok(VertexLock {
            listing: VbListing {
                vertex_buffer: number,
                fvf: listing.fvf.0,
                base_vertex: listing.base_vertex,
            },
            bytes: vec![0; lock.len as usize],
            offset: lock.offset,
        })
    }

    fn copy_vertex_data(
        &mut self,
        dest: &mut [u8],
        fvf: u32,
        source: &VertexSource,
        start: u32,
        count: u32,
    ) {
        let stride = Fvf(fvf).stride() as usize;
        let mut scratch = vec![0u8; start as usize * stride];
        scratch.extend_from_slice(dest);
        if vertex_copy::copy_vertex_data(&mut scratch, fvf, &copy_source(source), start, count)
            .is_ok()
        {
            dest.copy_from_slice(&scratch[start as usize * stride..]);
        }
    }

    fn unlock_vertex_memory(&mut self, lock: VertexLock) {
        if let Some(h) = self.vb(lock.listing.vertex_buffer) {
            let _ = self.device.buffers.write_vb(h, lock.offset, &lock.bytes);
            let _ = self.device.buffers.unlock_vb(h);
        }
    }

    fn find_texture(&mut self, name: &[u8], crc: u32) -> Option<RecordId> {
        let _ = name;
        self.textures.find_by_crc(crc, true).ok().flatten()
    }

    fn texture_frame_count(&mut self, record: RecordId) -> Option<i32> {
        match self.textures.frame_count(Some(record)) {
            (0, count) => Some(count),
            _ => None,
        }
    }

    fn create_playback(&mut self, record: RecordId) -> Result<i32, i32> {
        self.textures
            .create_playback(Some(record), true)
            .map(|id| id.unwrap_or(-1))
    }

    fn texture_frame(&mut self, id: i32, frame: i32) -> Option<FrameOut> {
        self.textures.frame(id, frame).ok()
    }

    fn release_texture_record(&mut self, record: RecordId) {
        let mut pipeline = DevicePipeline {
            device: &mut self.device,
            handles: &mut self.handles,
            classes: &self.classes,
            abilities: self.abilities,
        };
        self.textures.release_record(Some(record), &mut pipeline);
    }

    fn dec_ref_texture(&mut self, id: i32) {
        let mut pipeline = DevicePipeline {
            device: &mut self.device,
            handles: &mut self.handles,
            classes: &self.classes,
            abilities: self.abilities,
        };
        self.textures.dec_ref_id(id, &mut pipeline);
    }

    fn has_animation_library(&mut self) -> bool {
        self.animations.is_some()
    }

    fn create_animation_instance(&mut self, name: Option<&[u8]>, crc: u32) -> Option<InstanceId> {
        let _ = name;
        self.animations.as_mut()?.create_instance(crc).ok()
    }

    fn release_animation_instance(&mut self, instance: InstanceId) {
        if let Some(animations) = self.animations.as_mut() {
            animations.release(instance);
        }
    }

    fn animation_uv_matrix(&mut self, instance: InstanceId) -> Option<[f32; 16]> {
        self.animations.as_ref()?.instance(instance)?.uv_matrix()
    }

    fn instance_extent(&mut self, field20: usize, instance: usize) -> Option<(f32, [f32; 3])> {
        self.engine.as_mut()?.extent(field20, instance)
    }

    fn global_record(&mut self, index: u32) -> u32 {
        self.globals.value(index).unwrap_or(UNSET)
    }
}

/// The surface materials' calls on the model. The engine and the instance
/// values come from [`Shading::surface`]; the model has no meshes, so the
/// mesh queries fail, and no material arena, so the candidate calls do
/// nothing and fail (divergence D315).
impl<B: Backend> SurfaceServices for Shading<B> {
    fn material(&mut self) -> Option<[u32; 17]> {
        Some(self.device.lights.material().0)
    }

    fn stage_state(&mut self, stage: u32, state: u32) -> Option<u32> {
        self.device.state.stage_state_raw(stage, state).ok()
    }

    fn draw_vb(&mut self, prim: u32, vertex_buffer: u32, start: u32, count: u32) -> i32 {
        let vb = self.vb(vertex_buffer);
        match self.device.draw_vb(prim, vb, start, count) {
            Ok(()) => 0,
            Err(e) => e.code().cast_signed(),
        }
    }

    fn record_frame(&mut self, record: RecordId, frame: i32) -> Option<FrameOut> {
        match self.textures.texture_frame(Some(record), frame) {
            (0, out) => Some(out),
            _ => None,
        }
    }

    fn has_engine(&mut self) -> bool {
        self.surface.as_ref().is_some_and(|s| s.present())
    }

    fn instance_user_data(&mut self, instance: usize) -> Option<[u32; 5]> {
        self.surface.as_mut()?.user_data(instance)
    }

    fn instance_root(&mut self, instance: usize) -> usize {
        self.surface.as_mut().map_or(instance, |s| s.root(instance))
    }

    fn instance_relative(&mut self, instance: usize) {
        let _ = instance;
    }

    fn query_wire(&mut self, instance: usize) -> bool {
        let _ = instance;
        false
    }

    fn wire_draw(&mut self, instance: usize) {
        let _ = instance;
    }

    fn query_mesh(&mut self, instance: usize) -> bool {
        let _ = instance;
        false
    }

    fn mesh_open_face_group(&mut self, instance: usize, index: i32, mode: i32) -> i32 {
        let _ = (instance, index, mode);
        GR_GENERIC
    }

    fn mesh_close_face_group(&mut self, instance: usize) {
        let _ = instance;
    }

    fn mesh_open_channels(&mut self, instance: usize, flags: u32) -> i32 {
        let _ = (instance, flags);
        GR_GENERIC
    }

    fn mesh_lock_channels(&mut self, instance: usize, mask: u32) {
        let _ = (instance, mask);
    }

    fn mesh_unlock_channels(&mut self, instance: usize) {
        let _ = instance;
    }

    fn mesh_close_channels(&mut self, instance: usize) {
        let _ = instance;
    }

    fn mesh_data(&mut self, instance: usize, channel: u32) -> Option<Vec<u32>> {
        let _ = (instance, channel);
        None
    }

    fn mesh_indices(&mut self, instance: usize, channel: u32) -> MeshIndices {
        let _ = (instance, channel);
        MeshIndices::default()
    }

    fn instance_value(&mut self, id: i32) -> Option<u32> {
        if id == -1 {
            return None;
        }
        self.surface.as_mut()?.value(id)
    }

    fn instance_value_name(&mut self, id: i32) -> Option<Vec<u8>> {
        if id == -1 {
            return None;
        }
        self.surface.as_mut()?.value_name(id)
    }

    fn count_visible(&mut self, index_span: u32, count: u32) {
        self.visible[0] = self.visible[0].wrapping_add(1);
        self.visible[1] = self.visible[1].wrapping_add(index_span);
        self.visible[2] = self.visible[2].wrapping_add(count);
    }

    fn candidate_is_batched(&mut self, candidate: MaterialId) -> bool {
        let _ = candidate;
        false
    }

    fn candidate_bind(
        &mut self,
        candidate: MaterialId,
        batched: bool,
        context: &SubmitContext<'_>,
    ) -> i32 {
        let _ = (candidate, batched, context);
        GR_GENERIC
    }

    fn candidate_teardown(
        &mut self,
        candidate: MaterialId,
        batched: bool,
        context: &SubmitContext<'_>,
    ) -> i32 {
        let _ = (candidate, batched, context);
        GR_GENERIC
    }

    fn candidate_submit(
        &mut self,
        candidate: MaterialId,
        batched: bool,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        let _ = (candidate, batched, context, args);
        GR_GENERIC
    }

    fn candidate_pass_count(&mut self, candidate: MaterialId) -> Option<i32> {
        let _ = candidate;
        None
    }

    fn candidate_class_name(&mut self, candidate: MaterialId) -> Vec<u8> {
        let _ = candidate;
        Vec::new()
    }

    fn candidate_parameter_tag(
        &mut self,
        candidate: MaterialId,
        index: u32,
    ) -> Result<Option<Vec<u8>>, i32> {
        let _ = (candidate, index);
        Err(GR_GENERIC)
    }

    fn candidate_parameter_word(&mut self, candidate: MaterialId, index: u32) -> u32 {
        let _ = (candidate, index);
        0
    }
}

#[cfg(test)]
mod tests {
    use std::rc::Rc;

    use neuron_core::material_data::PropertyValue;
    use neuron_core::material_data::schema::value_type;

    use super::*;
    use crate::render::ff::defaults::Defaults;
    use crate::render::ff::device::{IndexData, RecordingBackend, VertexData};
    use crate::render::ff::state::{RenderStateId, StageStateId};
    use crate::render::material::batcher::{DrawArgs, IbScratch, SubmitContext, SubmitInput};
    use crate::render::material::{MaterialBatcher, MaterialFactory, ShadingFactory, VertexSource};
    use crate::render::texture::LibraryConfig;

    type Services = Shading<RecordingBackend>;

    fn services() -> Services {
        let device = Device::new(Defaults::builtin(), 64, 64, RecordingBackend::default());
        Shading::new(
            device,
            TextureLibrary::new(LibraryConfig::default()),
            FormatClasses::builtin(),
        )
    }

    /// A device texture of `format` under `name`, its record retained as a
    /// texture library holds a loaded one; the renderer's number.
    fn texture(s: &mut Services, name: &[u8], format: PixelFormat) -> u32 {
        let number = s.pipeline().create_texture(4, 4, format, 1).unwrap();
        s.textures.create_record(number, name, true);
        number
    }

    fn name_value(name: &[u8]) -> PropertyValue {
        PropertyValue {
            kind: value_type::TEXTURE_NAME,
            words: [0; 4],
            text: Some(name.to_vec()),
        }
    }

    fn source() -> VertexSource {
        let floats = |v: &[f32]| Some(v.iter().flat_map(|f| f.to_le_bytes()).collect());
        let stream = |data, size, count| VertexStream {
            data,
            byte_size: size,
            stride: size,
            index: None,
            count,
        };
        VertexSource {
            flags: 0,
            position: stream(floats(&[0., 0., 0., 1., 0., 0., 0., 1., 0.]), 12, 3),
            normal: stream(floats(&[0., 0., 1., 0., 0., 1., 0., 0., 1.]), 12, 3),
            diffuse: VertexStream::default(),
            tex: [
                stream(floats(&[0., 0., 1., 0., 0., 1.]), 8, 3),
                VertexStream::default(),
            ],
            fvf: 0x112,
            count: 3,
        }
    }

    const IDENTITY: [f32; 12] = [1., 0., 0., 0., 1., 0., 0., 0., 1., 0., 0., 0.];

    #[test]
    fn a_single_pass_material_binds_and_draws_on_the_device() {
        let mut s = services();
        let number = texture(&mut s, b"hull.tga", PixelFormat::R5G6B5);
        let mut factory = ShadingFactory::<Services>::new();
        let mut m = factory.create(&mut s, b"DcDt").unwrap();
        assert_eq!(m.initialize(&mut s), 0);
        m.set_name(b"Hull");
        assert_eq!(m.set_parameter(&mut s, 6, &name_value(b"hull.tga")), 0);
        // Verified on the model: two emulated stages, the third disabled.
        assert_eq!(m.apply_verify(&mut s), 0);
        assert_eq!(m.flags(&mut s), Ok(8));

        let vertices = source();
        let indices = [0u16, 1, 2];
        let context = SubmitContext {
            instance: 0,
            world: &IDENTITY,
            view: &IDENTITY,
            vertices: Some(&vertices),
            indices: &indices,
            vb: None,
            ib: None,
            replaced: None,
            field20: 0,
        };
        assert_eq!(m.bind(&mut s, Some(&context)), 0);
        let model = &s.device.state;
        assert_eq!(
            model.stage_state_raw(0, StageStateId::ColorOp.raw()),
            Ok(4),
            "stage 0 modulates"
        );
        assert_eq!(model.stage_state_raw(1, StageStateId::ColorOp.raw()), Ok(1));
        assert_eq!(
            model.render_state_raw(RenderStateId::AlphaBlendEnable.raw()),
            Ok(0)
        );
        assert_eq!(s.device.stage_texture(0), s.handles.get(number));

        // No vertex buffer of the caller's: the source is copied into the
        // pool ring and drawn from there.
        let args = DrawArgs {
            prim_type: 4,
            start: 0,
            count: 3,
            ib_start: 0,
            index_span: 3,
            flags: 0,
        };
        assert_eq!(m.submit(&mut s, &context, &args), 0);
        m.teardown(&mut s, Some(&context));
        let draws = &s.device.backend.draws;
        assert_eq!(draws.len(), 1);
        assert_eq!(draws[0].textures[0], s.handles.get(number));
        assert!(matches!(draws[0].vertices, VertexData::Buffer { .. }));
        assert!(matches!(
            draws[0].indices,
            IndexData::Buffer { count: 3, .. }
        ));
        let VertexData::Buffer { buffer, .. } = draws[0].vertices else {
            unreachable!()
        };
        // The copy reached the buffer: the second vertex's x is 1.
        let bytes = s.device.buffers.vb(buffer).unwrap().bytes();
        assert_eq!(&bytes[32..36], &1.0f32.to_le_bytes());

        m.destroy(&mut s);
        // The playback is gone; the record keeps the reference it was made
        // with.
        assert_eq!(s.textures.playbacks().count(), 0);
    }

    #[test]
    fn an_alpha_texture_blends_and_a_one_bit_alpha_tests() {
        let mut s = services();
        texture(&mut s, b"glass.tga", PixelFormat::A8R8G8B8);
        texture(&mut s, b"fence.tga", PixelFormat::A1R5G5B5);
        let mut factory = ShadingFactory::<Services>::new();
        for (name, flags) in [(&b"glass.tga"[..], 0xA), (b"fence.tga", 8)] {
            let mut m = factory.create(&mut s, b"DcDtOcOt").unwrap();
            m.initialize(&mut s);
            m.set_parameter(&mut s, 6, &name_value(name));
            m.apply_verify(&mut s);
            assert_eq!(
                m.flags(&mut s),
                Ok(flags),
                "{}",
                String::from_utf8_lossy(name)
            );
            m.destroy(&mut s);
        }
    }

    #[test]
    fn detail_kinds_keep_two_passes_on_the_model() {
        let mut s = services();
        texture(&mut s, b"rock.tga", PixelFormat::R5G6B5);
        texture(&mut s, b"grain.tga", PixelFormat::R5G6B5);
        let mut factory = ShadingFactory::<Services>::new();
        let mut m = factory
            .create(&mut s, b"DetailMap2Dm1Msk2PassMaterial")
            .unwrap();
        m.initialize(&mut s);
        m.set_parameter(&mut s, 2, &name_value(b"rock.tga"));
        m.set_parameter(&mut s, 8, &name_value(b"grain.tga"));
        assert_eq!(m.apply_verify(&mut s), 0);
        assert_eq!(m.pass_count(), Ok(2));
        // Without record 0 the second stage goes.
        s.globals.set_value(0, 0);
        assert_eq!(m.apply_verify(&mut s), 0);
        assert_eq!(m.pass_count(), Ok(1));
        m.destroy(&mut s);
    }

    #[test]
    fn device_buffers_and_the_batcher_draw_through_a_batched_material() {
        let mut s = services();
        texture(&mut s, b"glass.tga", PixelFormat::A8R8G8B8);
        let mut ini = neuron_core::ini::EngineIni::new();
        ini.load_buffer(
            b"[BatchedMaterials]\nType = .*OcOt\n",
            &mut neuron_core::ini::NoIncludes,
        )
        .unwrap();
        let mut batcher: MaterialBatcher<Services> =
            MaterialBatcher::new(Box::new(ShadingFactory::new()), Some(&ini));
        let crc = batcher
            .library_mut()
            .create_after_map(&mut s, b"Glass", b"DcDtOcOt", true)
            .expect("made");
        let id = batcher.library().entry(crc).unwrap().material.unwrap();
        {
            let m = batcher.library_mut().materials_mut().get_mut(id).unwrap();
            assert!(m.is_batched());
            assert_eq!(m.set_parameter(&mut s, 6, &name_value(b"glass.tga")), 0);
            assert_eq!(m.apply_verify(&mut s), 0);
            // The inner flags (blended) with the batched bit.
            assert_eq!(m.flags(&mut s), Ok(0x1A));
        }
        // A vertex buffer and an index buffer of the caller's.
        let fvf = crate::render::ff::fvf::Fvf(0x112);
        let vb = s.device.buffers.create_vb(fvf, 3, 0);
        let vb_number = s.vb_number(vb);
        let ib = s.device.buffers.create_ib(3, 0);
        let ib_number = s.ib_number(IndexSource::Buffer(ib));
        let input = SubmitInput {
            instance: 0,
            world: IDENTITY,
            view: IDENTITY,
            vertices: Rc::new(source()),
            indices: Rc::from(vec![0u16, 1, 2]),
            vb: VbListing {
                vertex_buffer: vb_number,
                fvf: 0x112,
                base_vertex: 0,
            },
            ib: IbScratch {
                index_buffer: ib_number,
                bias: 0,
            },
            replaced: None,
            field20: 0,
        };
        let args = DrawArgs {
            prim_type: 4,
            start: 0,
            count: 3,
            ib_start: 0,
            index_span: 3,
            flags: 0x10C,
        };
        // Bin 1 (blended) queues; the flush selects the buffers, binds and
        // draws through the device's own streams.
        assert_eq!(batcher.submit_material(&mut s, id, &input, &args), 0);
        assert_eq!(batcher.selected_bin(), 1);
        assert!(s.device.backend.draws.is_empty());
        batcher.flush_all(&mut s, 1);
        let draws = &s.device.backend.draws;
        assert_eq!(draws.len(), 1);
        assert!(draws[0].textures[0].is_some());
        assert_eq!(
            s.device
                .state
                .render_state_raw(RenderStateId::AlphaBlendEnable.raw()),
            Ok(1)
        );
        // A number the services never gave selects nothing.
        assert_eq!(s.select_vb(9999), E_FAIL);
    }

    struct Planet(std::rc::Rc<std::cell::Cell<u32>>);

    impl InstanceExtents for Planet {
        fn extent(&mut self, field20: usize, instance: usize) -> Option<(f32, [f32; 3])> {
            assert_eq!((field20, instance), (0x55, 7));
            self.0.set(self.0.get() + 1);
            Some((2.0, [0.0; 3]))
        }
    }

    #[test]
    fn the_atmosphere_asks_the_engine_for_its_radius() {
        let mut s = services();
        texture(&mut s, b"fade.tga", PixelFormat::A8R8G8B8);
        let asked = std::rc::Rc::new(std::cell::Cell::new(0));
        s.engine = Some(Box::new(Planet(std::rc::Rc::clone(&asked))));
        let mut factory = ShadingFactory::<Services>::new();
        let mut m = factory.create(&mut s, b"AtmosphereMaterial").unwrap();
        m.initialize(&mut s);
        m.set_parameter(&mut s, 2, &name_value(b"fade.tga"));
        assert_eq!(m.apply_verify(&mut s), 0);
        m.resolve_textures(&mut s, 0.0);
        assert_eq!(m.flags(&mut s), Ok(0xA2));
        let vertices = source();
        let indices = [0u16, 1, 2];
        let view = [1., 0., 0., 0., 1., 0., 0., 0., 1., 0., 0., -10.];
        let context = SubmitContext {
            instance: 7,
            world: &IDENTITY,
            view: &view,
            vertices: Some(&vertices),
            indices: &indices,
            vb: None,
            ib: None,
            replaced: None,
            field20: 0x55,
        };
        let args = DrawArgs {
            prim_type: 4,
            start: 0,
            count: 3,
            ib_start: 0,
            index_span: 3,
            flags: 0,
        };
        m.bind(&mut s, Some(&context));
        assert_eq!(m.submit(&mut s, &context, &args), 0);
        assert_eq!(asked.get(), 1);
        assert_eq!(s.device.backend.draws.len(), 1);
        // The depth test and writes are restored after the shell.
        assert_eq!(
            s.device
                .state
                .render_state_raw(RenderStateId::ZWriteEnable.raw()),
            Ok(1)
        );
        m.destroy(&mut s);
    }

    /// What `FLAppearance` would publish: an engine with one instance's
    /// user data.
    struct Appearance;

    impl SurfaceEngine for Appearance {
        fn present(&self) -> bool {
            true
        }

        fn user_data(&mut self, instance: usize) -> Option<[u32; 5]> {
            (instance == 7).then(|| [0.25f32, 0.5, 0.75, 1.0, 0.5].map(f32::to_bits))
        }
    }

    /// Some material id for a context's candidate.
    fn candidate_id(s: &mut Services) -> MaterialId {
        use crate::render::material::{Materials, SurfaceFactory};
        let mut arena: Materials<Services> = Materials::new();
        let m = SurfaceFactory::new().create(s, b"WireMaterial").unwrap();
        arena.insert(m, 1)
    }

    #[test]
    fn surface_materials_bind_and_draw_on_the_device() {
        use crate::render::material::SurfaceFactory;

        let mut s = services();
        let number = texture(&mut s, b"icon.tga", PixelFormat::R5G6B5);
        let mut factory =
            ShadingFactory::<Services>::with_fallback(Box::new(SurfaceFactory::new()));
        assert!(factory.create(&mut s, b"FLAppearance").is_err());
        let mut m = factory.create(&mut s, b"HUDIconMaterial").unwrap();
        assert_eq!(m.class_name(), "HUDIconMaterial");
        assert_eq!(m.initialize(&mut s), 0);
        m.set_name(b"Radar");
        assert_eq!(m.set_parameter(&mut s, 6, &name_value(b"icon.tga")), 0);
        assert_eq!(m.apply_verify(&mut s), 0);
        assert_eq!(m.flags(&mut s), Ok(0x28));

        let vertices = source();
        let indices = [0u16, 1, 2];
        let mut context = SubmitContext {
            instance: 7,
            world: &IDENTITY,
            view: &IDENTITY,
            vertices: Some(&vertices),
            indices: &indices,
            vb: None,
            ib: None,
            replaced: None,
            field20: 0,
        };
        let blend = RenderStateId::AlphaBlendEnable.raw();
        // Without FLAppearance the icon keeps its own colour and draws
        // opaque.
        assert_eq!(m.bind(&mut s, Some(&context)), 0);
        assert_eq!(s.device.state.render_state_raw(blend), Ok(0));
        assert_eq!(s.device.stage_texture(0), s.handles.get(number));
        // With it, the instance's user data colours the icon and its
        // opacity of 0.5 blends.
        s.surface = Some(Box::new(Appearance));
        assert_eq!(m.bind(&mut s, Some(&context)), 0);
        assert_eq!(s.device.state.render_state_raw(blend), Ok(1));
        let block = SurfaceServices::material(&mut s).unwrap();
        assert_eq!(block[3], 0.5f32.to_bits());
        assert_eq!(block[4], 0.25f32.to_bits());
        let args = DrawArgs {
            prim_type: 4,
            start: 0,
            count: 3,
            ib_start: 0,
            index_span: 3,
            flags: 0,
        };
        assert_eq!(m.submit(&mut s, &context, &args), 0);
        m.teardown(&mut s, Some(&context));
        assert_eq!(s.device.backend.draws.len(), 1);
        m.destroy(&mut s);
        assert_eq!(s.textures.playbacks().count(), 0);

        // The visible counter counts on the services.
        let mut counter = factory.create(&mut s, b"VisibleCounterMaterial").unwrap();
        counter.initialize(&mut s);
        counter.submit(&mut s, &context, &args);
        counter.submit(&mut s, &context, &args);
        assert_eq!(s.visible, [2, 6, 6]);

        // The model reaches no candidate: an override material with one
        // draws nothing and fails (D315).
        let mut stars = factory.create(&mut s, b"StarsphereMaterial").unwrap();
        assert_eq!(stars.class_name(), "StarsphereMaterial");
        stars.initialize(&mut s);
        context.replaced = Some(candidate_id(&mut s));
        assert_eq!(stars.submit(&mut s, &context, &args), GR_GENERIC);
        assert_eq!(s.device.backend.draws.len(), 1);
    }
}
