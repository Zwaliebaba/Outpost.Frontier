//! What the simulation calls outside the particle module: the texture
//! library (`ITextureLibrary2`) through the appearances' texture wrapper
//! (`FFxTextureWrapper` in `particlefx_genericfx.cpp`, the reference's
//! `ImpFxTexInfo`).
//!
//! Each method stands for one call in the reference's argument order. The
//! renderer's texture library ([`crate::render::texture`]) is one answer;
//! the oracle suite answers with the recording contract of
//! `CppSource/Oracle/oracle_particles.cpp`.

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
