//! What the material kinds call: the parts of the renderer interface
//! (`IRenderPipeline`, `IRPDraw`, `IVertexBufferManager`), of the texture
//! library (`ITextureLibrary2`), of material animation (`IMatAnimLibrary`,
//! `IImaInstance`) and of the engine (`IEngine::GetInstanceExtent`) that
//! `FMaterial` in `CppSource/Shading/Material.cpp` uses, and the global
//! material records it reads, as one trait.
//!
//! The reference's materials hold those interfaces as pointers they ask the
//! aggregate for in `Initialize`; here they are the `S` every [`Material`]
//! call receives. [`super::shading::Shading`] answers them from the device
//! model ([`crate::render::ff`]), the texture library
//! ([`crate::render::texture`]), [`super::MaterialAnimations`] and
//! [`super::GlobalRecords`]; the oracle suite answers them with the
//! recording contract `CppSource/Oracle/oracle_material_kinds.cpp` states.
//!
//! Each method stands for one call, in the reference's argument order, so
//! that the calls a material makes are the calls the reference made.
//!
//! [`Material`]: super::Material

use super::batcher::{BatchDevice, VbListing, VertexSource};
use crate::render::texture::FrameOut;

/// A `LockVertexMemory` that succeeded (`FVertexMemoryLock`): where the
/// vertices go and their bytes, which reach the buffer at
/// [`MaterialServices::unlock_vertex_memory`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VertexLock {
    /// The buffer, its format and the first vertex the lock gives.
    pub listing: VbListing,
    /// The locked vertices, the first at index 0.
    pub bytes: Vec<u8>,
    /// Where the lock starts in the buffer, in bytes (the device's).
    pub offset: u32,
}

/// The calls the material kinds make. `TextureRecord` is the texture
/// library's record (`FTextureEntry*`), `Animation` an animation instance
/// (`IImaInstance*`).
pub trait MaterialServices: BatchDevice {
    /// A texture record `FindTextureByCrc` hands out with a reference.
    type TextureRecord: Copy;
    /// An animation instance `CreateAnimationInstance` hands out.
    type Animation: Copy + 'static;

    // --- IRenderPipeline ---------------------------------------------------------------------

    /// `SetMaterial` with the 17 words of a `D3DMATERIAL8` (diffuse,
    /// ambient, specular, emissive, power), as the material's fields from
    /// `+0x20` lie.
    fn set_material(&mut self, block: &[u32; 17]);

    /// `SetRenderState`.
    fn set_render_state(&mut self, state: u32, value: u32);

    /// `GetRenderState`: the value written (0 where the call writes
    /// nothing).
    fn render_state(&mut self, state: u32) -> u32;

    /// `SetTextureStageState`.
    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32);

    /// `SetTextureStageTexture` with a texture handle of the renderer
    /// interface (0 for none).
    fn set_stage_texture(&mut self, stage: u32, handle: u32);

    /// `SetTextureStageTransform`.
    fn set_texture_transform(&mut self, stage: u32, matrix: &[f32; 16]);

    /// `GetTextureFormat`: the format number (`PfIndex`).
    ///
    /// # Errors
    ///
    /// The renderer's result; the output is left alone.
    fn texture_format(&mut self, handle: u32) -> Result<u32, i32>;

    /// `VerifyState`.
    fn verify_state(&mut self) -> i32;

    /// `DrawIndexedPrimitiveVb(prim, vb, base_vertex, vertex_count,
    /// indices, indices.len(), 0)`: `vertex_buffer` is a handle of the
    /// renderer interface, as in [`VbListing`].
    fn draw_indexed_vb(
        &mut self,
        prim: u32,
        vertex_buffer: u32,
        base_vertex: u32,
        vertex_count: u32,
        indices: &[u16],
    ) -> i32;

    // --- IRPDraw -------------------------------------------------------------------------------

    /// `IRPDraw::DrawIndexedPrimitive` on the selected streams.
    fn draw_indexed(
        &mut self,
        prim: u32,
        min_index: u32,
        vertex_count: u32,
        start_index: u32,
        count: u32,
    ) -> i32;

    // --- IVertexBufferManager ------------------------------------------------------------------

    /// `LockVertexMemory`: room for `count` vertices of `fvf`.
    ///
    /// # Errors
    ///
    /// The manager's result.
    fn lock_vertex_memory(&mut self, fvf: u32, count: u32) -> Result<VertexLock, i32>;

    /// `CopyVertexData` into a lock: `count` vertices from vertex `start` of
    /// `source` converted to `fvf`, vertex `start` landing at the first
    /// byte of `dest` (the reference passes the lock's address less `start`
    /// vertices).
    fn copy_vertex_data(
        &mut self,
        dest: &mut [u8],
        fvf: u32,
        source: &VertexSource,
        start: u32,
        count: u32,
    );

    /// `UnlockVertexMemory`.
    fn unlock_vertex_memory(&mut self, lock: VertexLock);

    // --- ITextureLibrary2 ----------------------------------------------------------------------

    /// `FindTextureByCrc` with the query `{name, crc}` and an output: the
    /// record with a reference, `None` on a miss.
    fn find_texture(&mut self, name: &[u8], crc: u32) -> Option<Self::TextureRecord>;

    /// `GetFrameCount`; `None` when the call fails.
    fn texture_frame_count(&mut self, record: Self::TextureRecord) -> Option<i32>;

    /// `CreatePlaybackGroup(record, &id)`.
    ///
    /// # Errors
    ///
    /// The library's result (the id stays -1).
    fn create_playback(&mut self, record: Self::TextureRecord) -> Result<i32, i32>;

    /// `GetFrame(id, frame, out)`: the five words written; `None` when the
    /// call fails and writes nothing.
    fn texture_frame(&mut self, id: i32, frame: i32) -> Option<FrameOut>;

    /// `ReleaseTextureRecord`.
    fn release_texture_record(&mut self, record: Self::TextureRecord);

    /// `DecRefById`.
    fn dec_ref_texture(&mut self, id: i32);

    // --- IMatAnimLibrary and IImaInstance ------------------------------------------------------

    /// `QueryInterface(IID_IMatAnimLibrary)` on the renderer succeeds.
    fn has_animation_library(&mut self) -> bool;

    /// `CreateAnimationInstance(-1, {name, crc}, &out)`: the instance, or
    /// `None` when the call fails and leaves the output alone.
    fn create_animation_instance(
        &mut self,
        name: Option<&[u8]>,
        crc: u32,
    ) -> Option<Self::Animation>;

    /// `IImaInstance::Release`.
    fn release_animation_instance(&mut self, instance: Self::Animation);

    /// `IImaInstance::FillUvMatrix`: the matrix when it returns non-zero.
    fn animation_uv_matrix(&mut self, instance: Self::Animation) -> Option<[f32; 16]>;

    // --- IEngine -------------------------------------------------------------------------------

    /// `IEngine::GetInstanceExtent(instance, 0, &radius, &centre)` on the
    /// engine a submission's `Field20` names (non-zero): the radius and the
    /// centre written, `None` when nothing is written.
    fn instance_extent(&mut self, field20: usize, instance: usize) -> Option<(f32, [f32; 3])>;

    // --- The global material records ------------------------------------------------------------

    /// `GetMaterialGlobalRecordValue(index)`.
    fn global_record(&mut self, index: u32) -> u32;
}
