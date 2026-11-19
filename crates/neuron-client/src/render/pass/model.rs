//! The programmable resources as the device knows them (record 0005,
//! points 2 to 6): programs and Ex textures in arenas with generational
//! handles, the sizes of render targets for the current back buffer, and
//! the constant registers, which are device state and outlive a pass as
//! Direct3D 9's `SetVertexShaderConstantF` registers do.

use super::super::ff::arena::Arena;
use super::{
    ExFormat, ExProgram, ExTexture, PIXEL_REGISTERS, PassError, RenderTargetDesc, VERTEX_REGISTERS,
};

/// Where a program's code comes from.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ProgramOrigin {
    /// Blobs compiled ahead of time.
    Compiled,
    /// A source file compiled at run time (`shader-compile`).
    File {
        path: std::path::PathBuf,
        vs_entry: String,
        ps_entry: String,
    },
}

/// A program.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ExProgramEntry {
    pub name: String,
    pub origin: ProgramOrigin,
    /// False while a source program has never compiled.
    pub usable: bool,
}

/// What an Ex texture is.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum ExTextureKind {
    /// Made again at every back-buffer change when its size is scaled.
    RenderTarget(RenderTargetDesc),
    /// One level of `depth` slices.
    Volume { depth: u32 },
    /// The scene's depth buffer, owned by the device.
    SceneDepth,
}

/// An Ex texture and its current size.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ExTextureEntry {
    pub kind: ExTextureKind,
    /// The scene depth reads as one float (the C++ calls it `R32F`).
    pub format: ExFormat,
    pub width: u32,
    pub height: u32,
}

/// The programmable resources of a device.
pub struct ExModel {
    programs: Arena<ExProgramEntry>,
    textures: Arena<ExTextureEntry>,
    scene_depth: ExTexture,
    back_buffer: (u32, u32),
    vertex_constants: Vec<[f32; 4]>,
    pixel_constants: Vec<[f32; 4]>,
}

impl ExModel {
    /// The resources of a device with a `width` by `height` back buffer:
    /// none but the scene depth.
    #[must_use]
    pub fn new(width: u32, height: u32) -> ExModel {
        let mut textures = Arena::new();
        let scene_depth = textures.insert(ExTextureEntry {
            kind: ExTextureKind::SceneDepth,
            format: ExFormat::R32Float,
            width,
            height,
        });
        ExModel {
            programs: Arena::new(),
            textures,
            scene_depth,
            back_buffer: (width, height),
            vertex_constants: vec![[0.0; 4]; VERTEX_REGISTERS],
            pixel_constants: vec![[0.0; 4]; PIXEL_REGISTERS],
        }
    }

    /// `GetSceneDepth`: the depth buffer as a texture. Direct3D 12 keeps it
    /// in a typeless format, so there always is one.
    #[must_use]
    pub fn scene_depth(&self) -> ExTexture {
        self.scene_depth
    }

    #[must_use]
    pub fn back_buffer(&self) -> (u32, u32) {
        self.back_buffer
    }

    /// A new back-buffer size: the scene depth and every scaled render
    /// target take their new sizes.
    pub fn resize(&mut self, width: u32, height: u32) {
        self.back_buffer = (width, height);
        let handles: Vec<ExTexture> = self.textures.iter().map(|(h, _)| h).collect();
        for h in handles {
            let t = self.textures.get_mut(h).expect("listed above");
            match t.kind {
                ExTextureKind::SceneDepth => (t.width, t.height) = (width, height),
                ExTextureKind::RenderTarget(desc) => {
                    (t.width, t.height) = desc.size.resolve(width, height);
                }
                ExTextureKind::Volume { .. } => {}
            }
        }
    }

    pub fn program(&self, program: ExProgram) -> Result<&ExProgramEntry, PassError> {
        Ok(self.programs.get(program)?)
    }

    pub fn program_mut(&mut self, program: ExProgram) -> Result<&mut ExProgramEntry, PassError> {
        Ok(self.programs.get_mut(program)?)
    }

    pub fn insert_program(&mut self, entry: ExProgramEntry) -> ExProgram {
        self.programs.insert(entry)
    }

    pub fn remove_program(&mut self, program: ExProgram) -> Result<ExProgramEntry, PassError> {
        Ok(self.programs.remove(program)?)
    }

    /// Every program handle, in slot order.
    #[must_use]
    pub fn programs(&self) -> Vec<ExProgram> {
        self.programs.iter().map(|(h, _)| h).collect()
    }

    pub fn texture(&self, texture: ExTexture) -> Result<&ExTextureEntry, PassError> {
        Ok(self.textures.get(texture)?)
    }

    /// A render target for the current back buffer; its size is checked by
    /// the caller.
    pub fn insert_render_target(&mut self, desc: RenderTargetDesc) -> ExTexture {
        let (width, height) = desc.size.resolve(self.back_buffer.0, self.back_buffer.1);
        self.textures.insert(ExTextureEntry {
            kind: ExTextureKind::RenderTarget(desc),
            format: desc.format,
            width,
            height,
        })
    }

    pub fn insert_volume(
        &mut self,
        format: ExFormat,
        width: u32,
        height: u32,
        depth: u32,
    ) -> ExTexture {
        self.textures.insert(ExTextureEntry {
            kind: ExTextureKind::Volume { depth },
            format,
            width,
            height,
        })
    }

    /// Removes a render target or volume texture; the scene depth stays.
    pub fn remove_texture(&mut self, texture: ExTexture) -> Result<(), PassError> {
        if texture == self.scene_depth {
            return Err(PassError::SceneDepth);
        }
        self.textures.remove(texture)?;
        Ok(())
    }

    /// Writes `data` into the vertex registers from `start` on.
    pub fn set_vertex_constants(&mut self, start: u32, data: &[[f32; 4]]) -> Result<(), PassError> {
        write_registers(&mut self.vertex_constants, start, data)
    }

    /// Writes `data` into the pixel registers from `start` on.
    pub fn set_pixel_constants(&mut self, start: u32, data: &[[f32; 4]]) -> Result<(), PassError> {
        write_registers(&mut self.pixel_constants, start, data)
    }

    #[must_use]
    pub fn vertex_constants(&self) -> &[[f32; 4]] {
        &self.vertex_constants
    }

    #[must_use]
    pub fn pixel_constants(&self) -> &[[f32; 4]] {
        &self.pixel_constants
    }
}

/// `Set*ShaderConstantF`: Direct3D 9 refuses a range past the last
/// register and writes nothing.
fn write_registers(
    registers: &mut [[f32; 4]],
    start: u32,
    data: &[[f32; 4]],
) -> Result<(), PassError> {
    let start = start as usize;
    let end = start
        .checked_add(data.len())
        .ok_or(PassError::InvalidParams)?;
    registers
        .get_mut(start..end)
        .ok_or(PassError::InvalidParams)?
        .copy_from_slice(data);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::super::TargetSize;
    use super::*;

    #[test]
    fn scaled_targets_and_the_depth_follow_the_back_buffer() {
        let mut m = ExModel::new(640, 480);
        let half = m.insert_render_target(RenderTargetDesc {
            size: TargetSize::Scaled(0.5),
            format: ExFormat::Rgba16Float,
        });
        let fixed = m.insert_render_target(RenderTargetDesc {
            size: TargetSize::Fixed {
                width: 7,
                height: 9,
            },
            format: ExFormat::Bgra8,
        });
        let volume = m.insert_volume(ExFormat::L8, 4, 4, 4);
        assert_eq!(m.texture(half).unwrap().width, 320);
        m.resize(1000, 100);
        let size = |h| {
            let t = m.texture(h).unwrap();
            (t.width, t.height)
        };
        assert_eq!(size(half), (500, 50));
        assert_eq!(size(fixed), (7, 9));
        assert_eq!(size(volume), (4, 4));
        assert_eq!(size(m.scene_depth()), (1000, 100));
        assert_eq!(
            m.remove_texture(m.scene_depth()),
            Err(PassError::SceneDepth)
        );
        m.remove_texture(half).unwrap();
        assert_eq!(m.texture(half), Err(PassError::Stale));
    }

    #[test]
    fn constants_refuse_ranges_past_the_last_register() {
        let mut m = ExModel::new(8, 8);
        m.set_pixel_constants(222, &[[1.0; 4], [2.0; 4]]).unwrap();
        assert_eq!(m.pixel_constants()[223], [2.0; 4]);
        assert_eq!(
            m.set_pixel_constants(223, &[[3.0; 4], [3.0; 4]]),
            Err(PassError::InvalidParams)
        );
        assert_eq!(m.pixel_constants()[223], [2.0; 4]);
        m.set_vertex_constants(255, &[[4.0; 4]]).unwrap();
        assert_eq!(
            m.set_vertex_constants(u32::MAX, &[[4.0; 4]]),
            Err(PassError::InvalidParams)
        );
    }
}
