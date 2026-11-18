//! The renderer calls the texture library and the large-image tiler make
//! (the texture part of `IRenderPipeline` and `IRPTexture`), as a trait:
//! [`super::device::DevicePipeline`] answers them from the device model,
//! [`ContractPipeline`] from the fixed contract the oracle harness's
//! recording renderer implements.

use std::collections::BTreeMap;

use neuron_core::image::dds::{self, ImageInfo};
use neuron_core::image::{LevelUpload, PixelFormat, TextureRenderer};

/// `GR_GENERIC`.
pub const GR_GENERIC: i32 = -1;
/// `GR_INVALID_PARMS`, also DX8RENDERER's result for a null handle.
pub const GR_INVALID_PARMS: i32 = -2;

/// The device abilities the texture code reads (`QueryDeviceAbility`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Abilities {
    /// Ability 5, `TEXTURE_SQUARE_ONLY`.
    pub square_only: u32,
    /// Ability 6, `TEXTURE_MAX_WIDTH`.
    pub max_width: u32,
    /// Ability 7, `TEXTURE_MAX_HEIGHT`.
    pub max_height: u32,
}

impl Default for Abilities {
    /// What a Direct3D 12 adapter of feature level 11 reports through
    /// Direct3D 9's caps: any texture shape, 16384 texels a side. The C++
    /// asks the Direct3D 8 driver (divergence D261).
    fn default() -> Self {
        Abilities {
            square_only: 0,
            max_width: 16384,
            max_height: 16384,
        }
    }
}

/// The texture calls of the renderer interface. Handles are never 0.
pub trait TexturePipeline {
    /// `QueryDeviceAbility` 5, 6 and 7.
    fn abilities(&self) -> Abilities;
    /// `CreateTexture` with no flags.
    ///
    /// # Errors
    ///
    /// The renderer's result.
    fn create_texture(
        &mut self,
        width: u32,
        height: u32,
        format: PixelFormat,
        levels: u32,
    ) -> Result<u32, i32>;
    /// `SetTextureLevelData`: `height` rows of `pitch` bytes (block rows
    /// for DXT) of `format` from `pixels`; the result.
    #[allow(clippy::too_many_arguments)]
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
    ) -> i32;
    /// `LoadCubemap` on the bytes of a `CUBE` file that
    /// [`dds::image_info`] describes as `info`.
    ///
    /// # Errors
    ///
    /// The renderer's result.
    fn load_cubemap(&mut self, data: &[u8], info: Option<&ImageInfo>) -> Result<u32, i32>;
    /// `DestroyTexture`; the result.
    fn destroy_texture(&mut self, handle: u32) -> i32;
    /// `GetTextureFormat`.
    ///
    /// # Errors
    ///
    /// The renderer's result.
    fn texture_format(&self, handle: u32) -> Result<PixelFormat, i32>;
}

/// A [`TexturePipeline`] as the loader of `neuron-core` calls it.
pub struct AsRenderer<'a, P: TexturePipeline + ?Sized>(pub &'a mut P);

impl<P: TexturePipeline + ?Sized> TextureRenderer for AsRenderer<'_, P> {
    fn create_texture(
        &mut self,
        width: u32,
        height: u32,
        format: PixelFormat,
        levels: u32,
    ) -> Result<u32, i32> {
        self.0.create_texture(width, height, format, levels)
    }

    fn set_level_data(&mut self, upload: &LevelUpload) -> i32 {
        self.0.set_level_data(
            upload.handle,
            upload.level,
            upload.width,
            upload.height,
            upload.pitch,
            upload.format,
            upload.source.bytes(),
            upload.palette.as_deref(),
        )
    }

    fn load_cubemap(&mut self, data: &[u8], info: Option<&ImageInfo>) -> Result<u32, i32> {
        self.0.load_cubemap(data, info)
    }

    fn destroy_texture(&mut self, handle: u32) {
        let _ = self.0.destroy_texture(handle);
    }
}

/// One call [`ContractPipeline`] answered.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PipelineCall {
    /// `CreateTexture`.
    Create {
        width: u32,
        height: u32,
        format: PixelFormat,
        levels: u32,
        /// 0 on failure.
        handle: u32,
        result: i32,
    },
    /// `SetTextureLevelData`; the bytes read are `pitch` times the rows
    /// (block rows for DXT), cut at the end of `pixels`.
    Level {
        handle: u32,
        level: u32,
        width: u32,
        height: u32,
        pitch: u32,
        format: PixelFormat,
        bytes: Vec<u8>,
        result: i32,
    },
    /// `LoadCubemap`.
    Cube { handle: u32, result: i32 },
    /// `DestroyTexture`.
    Destroy { handle: u32 },
}

/// The fixed renderer contract of `neuron_core::image::library` with the
/// abilities given, recording every call: `CreateTexture` fails for a zero
/// side or level count, or more levels than a full chain, else hands out
/// the next handle from 1; `SetTextureLevelData` returns -2 for handle 0
/// or source format 0 and -1 for a level the texture lacks;
/// `GetTextureFormat` gives the created format, -1 for any other handle.
#[derive(Clone, Debug)]
pub struct ContractPipeline {
    pub abilities: Abilities,
    next_handle: u32,
    created: BTreeMap<u32, (PixelFormat, u32)>,
    pub calls: Vec<PipelineCall>,
}

impl ContractPipeline {
    #[must_use]
    pub fn new(abilities: Abilities) -> ContractPipeline {
        ContractPipeline {
            abilities,
            next_handle: 1,
            created: BTreeMap::new(),
            calls: Vec::new(),
        }
    }
}

fn full_chain(width: u32, height: u32) -> u32 {
    32 - width.max(height).leading_zeros()
}

impl TexturePipeline for ContractPipeline {
    fn abilities(&self) -> Abilities {
        self.abilities
    }

    fn create_texture(
        &mut self,
        width: u32,
        height: u32,
        format: PixelFormat,
        levels: u32,
    ) -> Result<u32, i32> {
        let ok = width != 0 && height != 0 && levels != 0 && levels <= full_chain(width, height);
        let handle = if ok {
            let handle = self.next_handle;
            self.next_handle += 1;
            self.created.insert(handle, (format, levels));
            handle
        } else {
            0
        };
        let result = if ok { 0 } else { GR_GENERIC };
        self.calls.push(PipelineCall::Create {
            width,
            height,
            format,
            levels,
            handle,
            result,
        });
        if ok { Ok(handle) } else { Err(result) }
    }

    fn set_level_data(
        &mut self,
        handle: u32,
        level: u32,
        width: u32,
        height: u32,
        pitch: u32,
        format: PixelFormat,
        pixels: &[u8],
        _palette: Option<&[u8]>,
    ) -> i32 {
        let result = if handle == 0 || format == PixelFormat::UNKNOWN {
            GR_INVALID_PARMS
        } else if self.created.get(&handle).is_none_or(|&(_, n)| level >= n) {
            GR_GENERIC
        } else {
            0
        };
        let rows = if format.is_dxt() {
            height.wrapping_add(3) >> 2
        } else {
            height
        };
        let need = (u64::from(pitch) * u64::from(rows)).min(pixels.len() as u64) as usize;
        self.calls.push(PipelineCall::Level {
            handle,
            level,
            width,
            height,
            pitch,
            format,
            bytes: pixels[..need].to_vec(),
            result,
        });
        result
    }

    fn load_cubemap(&mut self, _data: &[u8], info: Option<&ImageInfo>) -> Result<u32, i32> {
        let ok = info.is_some_and(|i| i.resource_type == dds::RTYPE_CUBETEXTURE);
        let handle = if ok {
            let handle = self.next_handle;
            self.next_handle += 1;
            handle
        } else {
            0
        };
        let result = if ok { 0 } else { GR_GENERIC };
        self.calls.push(PipelineCall::Cube { handle, result });
        if ok { Ok(handle) } else { Err(result) }
    }

    fn destroy_texture(&mut self, handle: u32) -> i32 {
        self.calls.push(PipelineCall::Destroy { handle });
        0
    }

    fn texture_format(&self, handle: u32) -> Result<PixelFormat, i32> {
        self.created
            .get(&handle)
            .map(|&(format, _)| format)
            .ok_or(GR_GENERIC)
    }
}
