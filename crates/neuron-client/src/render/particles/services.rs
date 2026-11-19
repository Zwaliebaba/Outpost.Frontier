//! What the simulation calls outside the particle module: the texture
//! library (`ITextureLibrary2`) through the appearances' texture wrapper
//! (`FFxTextureWrapper` in `particlefx_genericfx.cpp`, the reference's
//! `ImpFxTexInfo`).
//!
//! Each method stands for one call in the reference's argument order. The
//! renderer's texture library ([`crate::render::texture`]) is one answer;
//! the oracle suite answers with the recording contract of
//! `CppSource/Oracle/oracle_particles.cpp`.

pub use crate::render::appearance::Camera;
pub use crate::render::texture::FrameOut;

/// A texture record `FindTextureByCrc` hands out with a reference.
pub type TextureRecord = u32;

/// The texture library calls of the particle appearances.
pub trait ParticleServices {
    /// `FindTextureByCrc({name, crc}, &record)`; `None` when it fails.
    fn find_texture(&mut self, name: &[u8], crc: u32) -> Option<TextureRecord>;
    /// `CreatePlaybackGroup(record, &id)` with `id` set to -1 first: the id
    /// the call wrote, or -1.
    fn create_playback(&mut self, record: TextureRecord) -> i32;
    /// `GetFrameCount(record, &count)`: the count written, `None` when the
    /// call writes nothing.
    fn frame_count(&mut self, record: TextureRecord) -> Option<i32>;
    /// `ReleaseTextureRecord(record)`.
    fn release_texture_record(&mut self, record: TextureRecord);
    /// `SetWrapMode(id, mode)`.
    fn set_wrap_mode(&mut self, id: i32, mode: i32);
    /// `GetFrame(id, frame, out)`: the five words written, `None` when the
    /// call writes nothing.
    fn texture_frame(&mut self, id: i32, frame: i32) -> Option<FrameOut>;
    /// `AdvanceTime(id, seconds)`.
    fn advance_time(&mut self, id: i32, seconds: f32);
    /// `DecRefById(id)`.
    fn dec_ref_texture(&mut self, id: i32);
}

/// Services that answer nothing: no texture is found, so no call after
/// `FindTextureByCrc` is made. The data tests step effects with it.
#[derive(Clone, Copy, Debug, Default)]
pub struct NoTextures;

impl ParticleServices for NoTextures {
    fn find_texture(&mut self, _name: &[u8], _crc: u32) -> Option<TextureRecord> {
        None
    }
    fn create_playback(&mut self, _record: TextureRecord) -> i32 {
        -1
    }
    fn frame_count(&mut self, _record: TextureRecord) -> Option<i32> {
        None
    }
    fn release_texture_record(&mut self, _record: TextureRecord) {}
    fn set_wrap_mode(&mut self, _id: i32, _mode: i32) {}
    fn texture_frame(&mut self, _id: i32, _frame: i32) -> Option<FrameOut> {
        None
    }
    fn advance_time(&mut self, _id: i32, _seconds: f32) {}
    fn dec_ref_texture(&mut self, _id: i32) {}
}

/// The calls the draws make outside the particle module: the renderer's
/// pipeline (`IRenderPipeline`), its vertex and index buffers
/// (`IRPVertexBuffer`, `IRPIndexBuffer`) and its draw (`IRPDraw`), as
/// `FParticleFx::Initialize` acquires them, and the texture library of
/// [`ParticleServices`] for the frame lookups.
///
/// Each method stands for one call in the reference's argument order. A
/// vertex buffer is a handle of the renderer interface, 0 for none (the
/// null `G_VbHandle`); the index buffer is always the renderer's dynamic
/// one (handle -1). [`crate::render::stack::RenderStack`] answers these on
/// the device model; the oracle suite answers with the recording contract
/// of `CppSource/Oracle/oracle_particles.cpp`.
pub trait ParticleDraw: ParticleServices {
    /// `GetRenderState(state, &value)`: the value written.
    fn render_state(&mut self, state: u32) -> u32;
    /// `SetRenderState(state, value)`.
    fn set_render_state(&mut self, state: u32, value: u32);
    /// `SetTextureStageState(stage, state, value)`.
    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32);
    /// `SetTextureStageTexture(stage, handle)`: the frame's texture word, 0
    /// for none.
    fn set_stage_texture(&mut self, stage: u32, handle: u32);
    /// `SetModelview(matrix)`.
    fn set_modelview(&mut self, matrix: &[f32; 12]);
    /// `SelectVb(handle)`.
    fn select_vb(&mut self, vertex_buffer: u32) -> i32;
    /// `CreateVb(fvf, count, &handle, flags)`: the result and the handle
    /// written (0 when nothing is written).
    fn create_vb(&mut self, fvf: u32, count: u32, flags: u32) -> (i32, u32);
    /// `DestroyVb(&handle)` (the handle is cleared after it).
    fn destroy_vb(&mut self, vertex_buffer: u32);
    /// `LockVb(handle, &offset, &memory, count)`: the offset the renderer
    /// writes (the first vertex of the range, which the draw hands to
    /// `SelectIb` as the base vertex).
    ///
    /// # Errors
    ///
    /// The renderer's result.
    fn lock_vb(&mut self, vertex_buffer: u32, count: u32) -> Result<u32, i32>;
    /// `UnlockVb(handle)`, with the vertices written into the lock since it
    /// was taken (the six words of each, 24 bytes, little-endian).
    fn unlock_vb(&mut self, vertex_buffer: u32, written: &[u8]);
    /// `LockIb(-1, &offset, &memory, count)`: the index offset written,
    /// which the draw hands to `DrawIndexedPrimitive` as the start index.
    ///
    /// # Errors
    ///
    /// The renderer's result.
    fn lock_ib(&mut self, count: u32) -> Result<u32, i32>;
    /// `UnlockIb(-1)`, with the indices written into the lock.
    fn unlock_ib(&mut self, written: &[u16]);
    /// `SelectIb(-1, base_vertex, 0, 0)`.
    fn select_ib(&mut self, base_vertex: u32) -> i32;
    /// `IRPDraw::DrawIndexedPrimitive(prim, min_index, vertex_count,
    /// start_index, index_count)`.
    fn draw_indexed(
        &mut self,
        prim: u32,
        min_index: u32,
        vertex_count: u32,
        start_index: u32,
        index_count: u32,
    ) -> i32;
}
