//! The texture calls answered by the device model: `CreateTexture`,
//! `SetTextureLevelData`, `DestroyTexture` and `GetTextureFormat` of
//! DX8RENDERER over [`crate::render::ff::Device`], and `LoadCubemap`.
//!
//! The renderer interface hands out its own numbers as texture handles (the
//! C++ hands out pointers); [`HandleTable`] maps them to the device's
//! [`TextureHandle`]s, so a texture made here is drawn by binding
//! [`HandleTable::get`]'s handle to a stage, and reaches the Direct3D 12
//! backend through the device's resource events like any other.

use std::collections::BTreeMap;

use neuron_core::image::PixelFormat;
use neuron_core::image::dds::{self, ImageInfo};

use super::format_class::{FormatClasses, port_supports};
use super::pipeline::{Abilities, GR_GENERIC, GR_INVALID_PARMS, TexturePipeline};
use crate::render::ff::device::{Backend, Device};
use crate::render::ff::state::PipelineProperty;
use crate::render::ff::textures::{TextureHandle, level_layout};

/// The renderer interface's texture numbers and the device textures they
/// name.
#[derive(Clone, Debug)]
pub struct HandleTable {
    next: u32,
    map: BTreeMap<u32, TextureHandle>,
}

impl Default for HandleTable {
    fn default() -> Self {
        HandleTable {
            next: 1,
            map: BTreeMap::new(),
        }
    }
}

impl HandleTable {
    #[must_use]
    pub fn new() -> HandleTable {
        HandleTable::default()
    }

    /// The device texture of a number.
    #[must_use]
    pub fn get(&self, handle: u32) -> Option<TextureHandle> {
        self.map.get(&handle).copied()
    }

    fn insert(&mut self, texture: TextureHandle) -> u32 {
        let handle = self.next;
        self.next = self.next.wrapping_add(1).max(1);
        self.map.insert(handle, texture);
        handle
    }

    /// The live numbers.
    #[must_use]
    pub fn len(&self) -> usize {
        self.map.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }
}

/// [`TexturePipeline`] on a device.
pub struct DevicePipeline<'a, B: Backend> {
    pub device: &'a mut Device<B>,
    pub handles: &'a mut HandleTable,
    pub classes: &'a FormatClasses,
    pub abilities: Abilities,
}

impl<B: Backend> DevicePipeline<'_, B> {
    /// `CheckTextureFormat` with the port's fixed device support.
    fn supported(&self, format: PixelFormat) -> bool {
        let state = &self.device.state;
        port_supports(
            format,
            state.pipeline(PipelineProperty::TextureAllow8Bit) != 0,
            state.pipeline(PipelineProperty::TextureAllow32Bit) != 0,
            state.pipeline(PipelineProperty::TextureAllowDxt) != 0,
        )
    }
}

fn full_chain(width: u32, height: u32) -> u32 {
    32 - width.max(height).leading_zeros()
}

/// The source of a level with zeros after it where its rows reach past
/// the bytes given: `D3DXLoadSurfaceFromMemory` reads each row at `pitch`
/// from the last, and the texture library's DDS pitch for a level wider
/// than high (the linear size over the width's block count) makes rows
/// overlap and the last reach into the next level's bytes, which the
/// loader does not hand over (divergence D264). `None` when nothing is
/// missing.
fn padded_source(
    format: PixelFormat,
    width: u32,
    height: u32,
    pitch: u32,
    pixels: &[u8],
) -> Option<Vec<u8>> {
    let layout = level_layout(format, width, height)?;
    let rows = u64::from(layout.rows);
    let need = (u64::from(pitch) * rows.saturating_sub(1) + u64::from(layout.pitch))
        .max(u64::from(pitch) * rows);
    let need = usize::try_from(need).ok()?;
    (pixels.len() < need).then(|| {
        let mut padded = pixels.to_vec();
        padded.resize(need, 0);
        padded
    })
}

impl<B: Backend> TexturePipeline for DevicePipeline<'_, B> {
    fn abilities(&self) -> Abilities {
        self.abilities
    }

    /// `CreateTexture`: square-only devices get the larger side for both;
    /// the format asked for or the first supported candidate of its
    /// class. A level count above a full chain fails, as the Direct3D
    /// runtime fails it.
    fn create_texture(
        &mut self,
        width: u32,
        height: u32,
        format: PixelFormat,
        levels: u32,
    ) -> Result<u32, i32> {
        let (mut w, mut h) = (width, height);
        if w != h && self.abilities.square_only != 0 {
            let side = if w.cast_signed() > h.cast_signed() {
                w
            } else {
                h
            };
            (w, h) = (side, side);
        }
        let Some(format) = self.classes.resolve(format, |f| self.supported(f)) else {
            return Err(GR_GENERIC);
        };
        if w == 0 || h == 0 || levels > full_chain(w, h) {
            return Err(GR_GENERIC);
        }
        let texture = self
            .device
            .textures
            .create(w, h, format, levels, 0)
            .map_err(|e| e.code().cast_signed())?;
        Ok(self.handles.insert(texture))
    }

    /// `SetTextureLevelData`: unbinds the texture from the stages first, as
    /// DX8RENDERER does; levels other than 0 are ignored while
    /// `TEXTURE_LOD` is off.
    fn set_level_data(
        &mut self,
        handle: u32,
        level: u32,
        width: u32,
        height: u32,
        pitch: u32,
        format: PixelFormat,
        pixels: &[u8],
        palette: Option<&[u8]>,
    ) -> i32 {
        let Some(texture) = self.handles.get(handle) else {
            return GR_INVALID_PARMS;
        };
        self.device.unbind_texture(texture);
        let lod = self.device.state.pipeline(PipelineProperty::TextureLod) != 0;
        let padded = padded_source(format, width, height, pitch, pixels);
        let pixels = padded.as_deref().unwrap_or(pixels);
        match self.device.textures.set_level_data(
            texture, level, width, height, pitch, format, pixels, palette, lod,
        ) {
            Ok(()) => 0,
            Err(e) => e.code().cast_signed(),
        }
    }

    /// `LoadCubemap`: a cube texture of the file's format with the top
    /// level of each face (divergence D260).
    fn load_cubemap(&mut self, data: &[u8], info: Option<&ImageInfo>) -> Result<u32, i32> {
        let info = info.ok_or(GR_GENERIC)?;
        if info.resource_type != dds::RTYPE_CUBETEXTURE || info.width != info.height {
            return Err(GR_GENERIC);
        }
        let format = PixelFormat::from_d3d_format(info.format);
        if !self.supported(format) {
            return Err(GR_GENERIC);
        }
        let size = info.width;
        let mut stride = 0usize;
        for level in 0..info.mip_levels.min(32) {
            let side = (size >> level).max(1);
            stride += level_layout(format, side, side).ok_or(GR_GENERIC)?.bytes();
        }
        let top = level_layout(format, size, size).ok_or(GR_GENERIC)?.bytes();
        let face = |i: usize| -> Result<Vec<u8>, i32> {
            let start = dds::DATA_OFFSET + i * stride;
            data.get(start..start + top)
                .map(<[u8]>::to_vec)
                .ok_or(GR_GENERIC)
        };
        let faces = [face(0)?, face(1)?, face(2)?, face(3)?, face(4)?, face(5)?];
        let texture = self
            .device
            .textures
            .create_cube(size, format, faces)
            .map_err(|e| e.code().cast_signed())?;
        Ok(self.handles.insert(texture))
    }

    /// `DestroyTexture`: unbinds and destroys.
    fn destroy_texture(&mut self, handle: u32) -> i32 {
        if let Some(texture) = self.handles.map.remove(&handle) {
            let _ = self.device.destroy_texture(texture);
        }
        0
    }

    /// `GetTextureFormat`: the format the texture was created in.
    fn texture_format(&self, handle: u32) -> Result<PixelFormat, i32> {
        let texture = self.handles.get(handle).ok_or(GR_INVALID_PARMS)?;
        self.device
            .textures
            .get(texture)
            .map(|t| t.format)
            .map_err(|_| GR_GENERIC)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::render::ff::defaults::Defaults;
    use crate::render::ff::device::RecordingBackend;

    #[test]
    fn classes_resolve_and_levels_fill() {
        let mut device = Device::new(Defaults::builtin(), 64, 64, RecordingBackend::default());
        let mut handles = HandleTable::new();
        let classes = FormatClasses::builtin();
        let mut p = DevicePipeline {
            device: &mut device,
            handles: &mut handles,
            classes: &classes,
            abilities: Abilities::default(),
        };
        let h = p.create_texture(2, 2, PixelFormat::DAA8, 2).unwrap();
        assert_eq!(p.texture_format(h), Ok(PixelFormat::A8R8G8B8));
        assert_eq!(
            p.create_texture(2, 2, PixelFormat::DAA8, 3),
            Err(GR_GENERIC)
        );
        let pixels = [0x11u8; 16];
        assert_eq!(
            p.set_level_data(h, 0, 2, 2, 8, PixelFormat::A8R8G8B8, &pixels, None),
            0
        );
        assert_eq!(
            p.set_level_data(9, 0, 2, 2, 8, PixelFormat::A8R8G8B8, &pixels, None),
            GR_INVALID_PARMS
        );
        let o = p.create_texture(4, 2, PixelFormat::DAOT, 1).unwrap();
        assert_eq!(p.texture_format(o), Ok(PixelFormat::R5G6B5));
        assert_eq!(p.destroy_texture(h), 0);
        assert_eq!(p.texture_format(h), Err(GR_INVALID_PARMS));
        assert_eq!(handles.len(), 1);
    }
}
