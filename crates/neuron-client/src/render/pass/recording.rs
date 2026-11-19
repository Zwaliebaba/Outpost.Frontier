//! The programmable side of [`RecordingBackend`]: it keeps what it is
//! given, for tests and model replay on every host.

use super::super::ff::device::RecordingBackend;
use super::{ExBackend, ExCaps, ExCreate, ExDraw, ExFormat, ExRelease, PassTarget};

/// A resource creation as [`RecordingBackend`] keeps it, without the
/// bytes.
#[derive(Clone, Debug, PartialEq)]
pub enum ExCreateRecord {
    Program {
        program: super::ExProgram,
        vs_len: usize,
        ps_len: usize,
    },
    RenderTarget {
        texture: super::ExTexture,
        format: ExFormat,
        width: u32,
        height: u32,
    },
    Volume {
        texture: super::ExTexture,
        format: ExFormat,
        size: [u32; 3],
        texels: usize,
    },
}

/// What the recording backend saw of the programmable calls.
#[derive(Clone, Debug, Default)]
pub struct ExRecord {
    pub created: Vec<ExCreateRecord>,
    pub released: Vec<ExRelease>,
    pub clears: Vec<(PassTarget, u32)>,
    pub draws: Vec<ExDraw>,
    pub passes_ended: u32,
    pub resizes: Vec<(u32, u32)>,
    /// Render-target formats to refuse, as a device without them would.
    pub refuse_targets: Vec<ExFormat>,
}

impl ExBackend for RecordingBackend {
    fn ex_caps(&self) -> ExCaps {
        ExCaps::feature_level_11_0("recording".to_string())
    }

    fn ex_create(&mut self, create: ExCreate<'_>) -> Result<(), String> {
        let record = match create {
            ExCreate::Program { program, vs, ps } => ExCreateRecord::Program {
                program,
                vs_len: vs.len(),
                ps_len: ps.len(),
            },
            ExCreate::RenderTarget {
                texture,
                desc,
                width,
                height,
            } => {
                if self.ex.refuse_targets.contains(&desc.format) {
                    return Err(format!("{:?} render targets refused", desc.format));
                }
                ExCreateRecord::RenderTarget {
                    texture,
                    format: desc.format,
                    width,
                    height,
                }
            }
            ExCreate::Volume {
                texture,
                format,
                width,
                height,
                depth,
                texels,
            } => ExCreateRecord::Volume {
                texture,
                format,
                size: [width, height, depth],
                texels: texels.len(),
            },
        };
        self.ex.created.push(record);
        Ok(())
    }

    fn ex_release(&mut self, release: ExRelease) {
        self.ex.released.push(release);
    }

    fn ex_clear(&mut self, target: PassTarget, color: u32) {
        self.ex.clears.push((target, color));
    }

    fn ex_draw(&mut self, draw: ExDraw) {
        self.ex.draws.push(draw);
    }

    fn ex_end_pass(&mut self) {
        self.ex.passes_ended += 1;
    }

    fn back_buffer_resized(&mut self, width: u32, height: u32) {
        self.ex.resizes.push((width, height));
    }
}
