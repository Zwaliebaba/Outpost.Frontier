//! Programmable passes (decision record 0005, work package P4.9): render
//! targets sized by the back buffer, volume textures, the scene depth as a
//! texture, programs compiled ahead of time, and a pass value that puts
//! everything back when it drops. The volumetric nebula (P6.4) draws with
//! it.
//!
//! The C++ to follow is `IRenderPipelineEx` and `IRPDrawEx`
//! (`CppSource/include/render/IRenderPipelineEx.h`) as
//! `DX9RENDERER/DX9RENDEREREx.cpp` implements them on Direct3D 9. This
//! module is platform-free: the types, the resource model ([`model`]), the
//! pass and the device calls ([`device`]), and the recording side of
//! [`ExBackend`] ([`recording`]). The Direct3D 12 side is
//! `platform::d3d12::backend::ex`.
//!
//! What changes against the C++ (divergences D410 to D419): passes cannot
//! nest and no ordinary draw can happen inside one, because the pass
//! borrows the device; render states set in a pass live in the pass and
//! never reach the device's state model; programs are compiled blobs (a
//! development feature, `shader-compile`, compiles source files at run
//! time); the full-screen quad needs no half-pixel shift.

pub mod device;
pub mod model;
pub mod recording;

use std::fmt;

use super::ff::arena::Handle;
use super::ff::draw::PipelineKey;
use super::ff::fvf::Fvf;
use super::ff::geometry::Primitive;
use super::ff::textures::TextureHandle;

pub use device::Pass;
pub use model::{ExModel, ExProgramEntry, ExTextureEntry, ExTextureKind};

/// Texture slots a pass can bind (`t0` to `t15` with `s0` to `s15`), as
/// Direct3D 9's sixteen pixel samplers.
pub const TEXTURE_SLOTS: usize = 16;
/// Vertex constant registers (`vs_3_0` has 256).
pub const VERTEX_REGISTERS: usize = 256;
/// Pixel constant registers (`ps_3_0` has 224).
pub const PIXEL_REGISTERS: usize = 224;

/// A shader program: a vertex and a pixel shader.
pub type ExProgram = Handle<ExProgramEntry>;
/// A render target, a volume texture or the scene depth.
pub type ExTexture = Handle<ExTextureEntry>;

/// `ERPExFormat`: the formats a render target or volume texture has.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ExFormat {
    /// `A8R8G8B8`: bytes blue, green, red, alpha.
    Bgra8,
    /// `L8`: one byte, sampled as `(l, l, l, 1)`.
    L8,
    /// `A16B16G16R16F`: four half floats, red first.
    Rgba16Float,
    /// `R16F`, sampled as `(r, 1, 1, 1)`.
    R16Float,
    /// `R32F`, sampled as `(r, 1, 1, 1)`.
    R32Float,
    /// `G16R16F`, sampled as `(r, g, 1, 1)`.
    Rg16Float,
}

impl ExFormat {
    /// The format of the C++ number (1 to 6).
    #[must_use]
    pub fn from_raw(raw: u32) -> Option<ExFormat> {
        Some(match raw {
            1 => ExFormat::Bgra8,
            2 => ExFormat::L8,
            3 => ExFormat::Rgba16Float,
            4 => ExFormat::R16Float,
            5 => ExFormat::R32Float,
            6 => ExFormat::Rg16Float,
            _ => return None,
        })
    }

    #[must_use]
    pub fn to_raw(self) -> u32 {
        match self {
            ExFormat::Bgra8 => 1,
            ExFormat::L8 => 2,
            ExFormat::Rgba16Float => 3,
            ExFormat::R16Float => 4,
            ExFormat::R32Float => 5,
            ExFormat::Rg16Float => 6,
        }
    }

    /// Bytes per texel of tightly packed data (`ExFormatToD3d`).
    #[must_use]
    pub fn bytes_per_texel(self) -> u32 {
        match self {
            ExFormat::L8 => 1,
            ExFormat::R16Float => 2,
            ExFormat::Bgra8 | ExFormat::R32Float | ExFormat::Rg16Float => 4,
            ExFormat::Rgba16Float => 8,
        }
    }
}

/// `ERPExFilter`, for minification and magnification alike.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub enum Filter {
    #[default]
    Point,
    Linear,
}

/// `ERPExMipFilter`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub enum MipFilter {
    /// Level 0 only.
    #[default]
    None,
    Point,
    Linear,
}

/// `ERPExAddress`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub enum Address {
    Wrap,
    #[default]
    Clamp,
    Mirror,
}

/// `FRPExSamplerState`: how a slot samples. Always explicit; the default
/// is the C++'s for a null state: point, no mipmaps, clamped.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub struct SamplerState {
    pub filter: Filter,
    pub mip: MipFilter,
    /// U, V and W.
    pub address: [Address; 3],
}

impl SamplerState {
    /// The number of distinct sampler states (the backend makes each
    /// once).
    pub const COUNT: u32 = 2 * 3 * 27;

    /// A state with the same address mode on every axis.
    #[must_use]
    pub fn new(filter: Filter, mip: MipFilter, address: Address) -> SamplerState {
        SamplerState {
            filter,
            mip,
            address: [address; 3],
        }
    }

    /// The state's index below [`SamplerState::COUNT`].
    #[must_use]
    pub fn index(self) -> u32 {
        let a = |x: Address| match x {
            Address::Wrap => 0,
            Address::Clamp => 1,
            Address::Mirror => 2,
        };
        let filter = match self.filter {
            Filter::Point => 0,
            Filter::Linear => 1,
        };
        let mip = match self.mip {
            MipFilter::None => 0,
            MipFilter::Point => 1,
            MipFilter::Linear => 2,
        };
        let address = a(self.address[0]) * 9 + a(self.address[1]) * 3 + a(self.address[2]);
        (filter * 3 + mip) * 27 + address
    }

    /// The state of an index below [`SamplerState::COUNT`].
    #[must_use]
    pub fn from_index(index: u32) -> Option<SamplerState> {
        if index >= Self::COUNT {
            return None;
        }
        let a = |x: u32| match x {
            0 => Address::Wrap,
            1 => Address::Clamp,
            _ => Address::Mirror,
        };
        let address = index % 27;
        let mip = (index / 27) % 3;
        let filter = index / 81;
        Some(SamplerState {
            filter: if filter == 0 {
                Filter::Point
            } else {
                Filter::Linear
            },
            mip: match mip {
                0 => MipFilter::None,
                1 => MipFilter::Point,
                _ => MipFilter::Linear,
            },
            address: [a(address / 9), a((address / 3) % 3), a(address % 3)],
        })
    }
}

/// How big a render target is.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum TargetSize {
    Fixed {
        width: u32,
        height: u32,
    },
    /// A fraction of the back buffer, evaluated again on every resize.
    Scaled(f32),
}

impl TargetSize {
    /// The size for a back buffer of `width` by `height`:
    /// `CreateRenderTargetGpu`'s `(unsigned long)((float)W * scale)`,
    /// at least 1. The product is exact in `f64` (T11) and truncated as
    /// the C++'s conversion through 64 bits does.
    #[must_use]
    pub fn resolve(self, width: u32, height: u32) -> (u32, u32) {
        match self {
            TargetSize::Fixed { width, height } => (width.max(1), height.max(1)),
            TargetSize::Scaled(scale) => {
                let side = |v: u32| {
                    let product = f64::from(v as f32) * f64::from(scale);
                    let truncated = neuron_core::num::trunc_to_i64(product);
                    // `(unsigned long)` keeps the low 32 bits.
                    let [a, b, c, d, ..] = truncated.to_le_bytes();
                    u32::from_le_bytes([a, b, c, d]).max(1)
                };
                (side(width), side(height))
            }
        }
    }

    /// Whether the C++ accepts the size: a fixed size of zero falls back to
    /// the scale there, so it is refused here; a scale must be above 0.
    #[must_use]
    pub fn is_valid(self) -> bool {
        match self {
            TargetSize::Fixed { width, height } => width > 0 && height > 0,
            TargetSize::Scaled(scale) => scale > 0.0,
        }
    }
}

/// `FRPExRenderTargetDesc`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RenderTargetDesc {
    pub size: TargetSize,
    pub format: ExFormat,
}

/// What a pass draws into: the scene (the C++'s back buffer, here the
/// scene target before the gamma blit) or a render target.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PassTarget {
    Scene,
    Texture(ExTexture),
}

/// `FRPExPassDesc`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PassDesc {
    pub target: PassTarget,
    /// `RPExPassKeepDepth`: the scene depth stays bound, tested and written
    /// as the render states say; without it the pass has no depth and
    /// `ZENABLE` and `ZWRITEENABLE` are off.
    pub keep_depth: bool,
    /// `RPExPassClearColor`: clear the target to this `A8R8G8B8` colour.
    pub clear: Option<u32>,
}

/// What a texture slot holds.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ExBinding {
    /// A render target, a volume texture or the scene depth
    /// (`BindExTexture`).
    Ex(ExTexture),
    /// A texture of the fixed-function device (`BindExPipelineTexture`).
    Device(TextureHandle),
    /// The scene depth, as an [`ExDraw`] names it: the pass turns
    /// `Ex(scene_depth)` into this, so the backend needs no handle for it.
    SceneDepth,
}

/// `FRPExCaps`, for Direct3D 12 at feature level 11_0.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ExCaps {
    /// Major and minor shader model of the programs (6.1).
    pub shader_model: (u8, u8),
    pub max_texture_extent: u32,
    pub max_volume_extent: u32,
    /// The scene depth can be sampled (always, record 0005).
    pub scene_depth: bool,
    pub float16_target: bool,
    pub float16_blending: bool,
    pub float32_target: bool,
    pub volume_textures: bool,
    /// The adapter's description.
    pub adapter: String,
}

impl ExCaps {
    /// What feature level 11_0 guarantees.
    #[must_use]
    pub fn feature_level_11_0(adapter: String) -> ExCaps {
        ExCaps {
            shader_model: (6, 1),
            max_texture_extent: 16384,
            max_volume_extent: 2048,
            scene_depth: true,
            float16_target: true,
            float16_blending: true,
            float32_target: true,
            volume_textures: true,
            adapter,
        }
    }
}

/// A program compiled ahead of time: the two blobs `cargo xtask shaders`
/// wrote into `compiled_shaders/` (record 0005: `<stem>_vs.cso` and
/// `<stem>[_<variant>]_ps.cso`), embedded with [`program_blobs!`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ProgramBlobs {
    /// For diagnostics: the source stem and variant.
    pub name: &'static str,
    pub vs: &'static [u8],
    pub ps: &'static [u8],
}

/// [`ProgramBlobs`] from two blob names in `neuron-client/compiled_shaders`
/// (without `.cso`).
#[macro_export]
macro_rules! program_blobs {
    ($name:literal, $vs:literal, $ps:literal) => {
        $crate::render::pass::ProgramBlobs {
            name: $name,
            vs: include_bytes!(concat!(
                env!("CARGO_MANIFEST_DIR"),
                "/compiled_shaders/",
                $vs,
                ".cso"
            )),
            ps: include_bytes!(concat!(
                env!("CARGO_MANIFEST_DIR"),
                "/compiled_shaders/",
                $ps,
                ".cso"
            )),
        }
    };
}

/// Compiles HLSL at run time (feature `shader-compile`, development only;
/// record 0005, point 11). The Direct3D 12 implementation is
/// `platform::d3d12::dxc::Dxc`.
#[cfg(feature = "shader-compile")]
pub trait ShaderCompiler {
    /// The blob of `entry` in the file at `path` for `profile`
    /// (`vs_6_1`, `ps_6_1`), or the compiler's messages.
    fn compile(
        &mut self,
        path: &std::path::Path,
        entry: &str,
        profile: &str,
    ) -> Result<Vec<u8>, String>;
}

/// Why a call failed.
#[derive(Clone, Debug, PartialEq)]
pub enum PassError {
    /// `GR_INVALID_PARMS`: a format, size, slot, register range, primitive
    /// or data length the call does not take.
    InvalidParams,
    /// The handle names nothing.
    Stale,
    /// A pass target that is not a render target.
    NotRenderTarget,
    /// Sampling the texture the pass draws into.
    TargetBound,
    /// A draw without a program bound.
    NoProgram,
    /// A program whose source never compiled (`shader-compile`).
    ProgramUnusable,
    /// The scene depth belongs to the device and cannot be destroyed.
    SceneDepth,
    /// The backend could not create the resource.
    Backend(String),
    /// A program that did not compile (`shader-compile`): the handle is
    /// made all the same, unusable until a reload compiles it, as
    /// `CreateExProgram` returns one.
    Compile { program: ExProgram, log: String },
}

impl fmt::Display for PassError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidParams => f.write_str("invalid parameters"),
            Self::Stale => f.write_str("the handle names nothing"),
            Self::NotRenderTarget => f.write_str("the pass target is not a render target"),
            Self::TargetBound => f.write_str("the texture is the pass's render target"),
            Self::NoProgram => f.write_str("no program is bound"),
            Self::ProgramUnusable => f.write_str("the program never compiled"),
            Self::SceneDepth => f.write_str("the scene depth belongs to the device"),
            Self::Backend(e) => write!(f, "the backend refused: {e}"),
            Self::Compile { log, .. } => write!(f, "the program did not compile: {log}"),
        }
    }
}

impl std::error::Error for PassError {}

impl From<super::ff::arena::StaleHandle> for PassError {
    fn from(_: super::ff::arena::StaleHandle) -> Self {
        PassError::Stale
    }
}

/// A resource the backend makes.
#[derive(Clone, Copy, Debug)]
pub enum ExCreate<'a> {
    /// A program, or new code for it after a reload (the backend drops what
    /// it made from the old code).
    Program {
        program: ExProgram,
        vs: &'a [u8],
        ps: &'a [u8],
    },
    RenderTarget {
        texture: ExTexture,
        desc: RenderTargetDesc,
        width: u32,
        height: u32,
    },
    /// One level, `texels` tightly packed: rows, then slices.
    Volume {
        texture: ExTexture,
        format: ExFormat,
        width: u32,
        height: u32,
        depth: u32,
        texels: &'a [u8],
    },
}

/// A resource the backend lets go.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ExRelease {
    Program(ExProgram),
    Texture(ExTexture),
}

/// What a programmable draw bakes into a pipeline state: the
/// fixed-function key's states (with the full-screen quad's or the
/// caller's vertex format, never flat), the program and the target.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct ExPipelineKey {
    pub program: ExProgram,
    /// Vertex format, topology, blend, colour writes, depth, bias, cull
    /// and fill; `depth` is `None` when no depth is bound.
    pub base: PipelineKey,
    /// The render target's format ([`ExFormat::Bgra8`] for the scene).
    pub target_format: ExFormat,
    /// Whether the scene depth is bound as the depth buffer.
    pub depth_bound: bool,
}

/// One programmable draw as the backend receives it.
#[derive(Clone, Debug, PartialEq)]
pub struct ExDraw {
    pub key: ExPipelineKey,
    pub target: PassTarget,
    /// The target's size: the viewport covers it.
    pub target_size: (u32, u32),
    /// Every vertex register (`c0` to `c255` of the vertex stage).
    pub vertex_constants: Vec<[f32; 4]>,
    /// Every pixel register (`c0` to `c223` of the pixel stage).
    pub pixel_constants: Vec<[f32; 4]>,
    pub textures: [Option<(ExBinding, SamplerState)>; TEXTURE_SLOTS],
    /// The topology the backend draws; a fan arrives as a list.
    pub primitive: Primitive,
    pub fvf: Fvf,
    pub stride: u32,
    pub vertices: Vec<u8>,
    pub vertex_count: u32,
    /// List indices of a converted fan.
    pub indices: Option<Vec<u16>>,
}

/// The programmable side of a renderer backend.
pub trait ExBackend {
    fn ex_caps(&self) -> ExCaps;
    /// Makes a resource; the error is the backend's reason.
    fn ex_create(&mut self, create: ExCreate<'_>) -> Result<(), String>;
    fn ex_release(&mut self, release: ExRelease);
    /// Clears `target` to an `A8R8G8B8` colour.
    fn ex_clear(&mut self, target: PassTarget, color: u32);
    fn ex_draw(&mut self, draw: ExDraw);
    /// The pass ended: the scene target and depth buffer are what the
    /// fixed-function draws expect again.
    fn ex_end_pass(&mut self);
    /// The back buffer has a new size: the scene, its depth and every
    /// scaled render target are made again behind the same handles.
    fn back_buffer_resized(&mut self, width: u32, height: u32);
}

/// The full-screen quad of `DrawFullscreenQuad`: four `XYZ | TEX1`
/// vertices as a triangle strip, clip-space positions at z 0 and
/// coordinates (0, 0) top left to (1, 1) bottom right. Direct3D 12 samples
/// pixel centres, so pixel (i, j) of a W by H target interpolates
/// ((i + 0.5) / W, (j + 0.5) / H) without the C++'s half-pixel shift
/// (divergence D414).
#[must_use]
pub fn fullscreen_quad() -> Vec<u8> {
    let corners: [[f32; 5]; 4] = [
        [-1.0, 1.0, 0.0, 0.0, 0.0],
        [1.0, 1.0, 0.0, 1.0, 0.0],
        [-1.0, -1.0, 0.0, 0.0, 1.0],
        [1.0, -1.0, 0.0, 1.0, 1.0],
    ];
    corners
        .iter()
        .flatten()
        .flat_map(|f| f.to_le_bytes())
        .collect()
}

/// `D3DFVF_XYZ | D3DFVF_TEX1`.
pub const QUAD_FVF: Fvf = Fvf(0x102);

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn formats_keep_the_cpp_numbers_and_sizes() {
        for raw in 1..=6 {
            let f = ExFormat::from_raw(raw).unwrap();
            assert_eq!(f.to_raw(), raw);
        }
        assert_eq!(ExFormat::from_raw(0), None);
        assert_eq!(ExFormat::from_raw(7), None);
        let sizes: Vec<u32> = (1..=6)
            .map(|r| ExFormat::from_raw(r).unwrap().bytes_per_texel())
            .collect();
        assert_eq!(sizes, [4, 1, 8, 2, 4, 4]);
    }

    #[test]
    fn sampler_states_number_one_to_one() {
        assert_eq!(SamplerState::default().address, [Address::Clamp; 3]);
        assert_eq!(SamplerState::default().filter, Filter::Point);
        let mut seen = std::collections::HashSet::new();
        for i in 0..SamplerState::COUNT {
            let s = SamplerState::from_index(i).unwrap();
            assert_eq!(s.index(), i);
            assert!(seen.insert(s));
        }
        assert_eq!(SamplerState::from_index(SamplerState::COUNT), None);
    }

    #[test]
    fn scaled_sizes_truncate_and_keep_one_texel() {
        assert_eq!(TargetSize::Scaled(0.5).resolve(640, 480), (320, 240));
        assert_eq!(TargetSize::Scaled(0.3).resolve(1024, 768), (307, 230));
        // 0.1f is a little above 0.1: 1000 * 0.1f = 100.0000015 -> 100.
        assert_eq!(TargetSize::Scaled(0.1).resolve(1000, 3), (100, 1));
        // 0.7f is a little below 0.7: 10 * 0.7f = 6.99999988 -> 6.
        assert_eq!(TargetSize::Scaled(0.7).resolve(10, 10), (6, 6));
        assert_eq!(TargetSize::Scaled(0.001).resolve(8, 8), (1, 1));
        assert_eq!(
            TargetSize::Fixed {
                width: 3,
                height: 5
            }
            .resolve(640, 480),
            (3, 5)
        );
        assert!(!TargetSize::Scaled(0.0).is_valid());
        assert!(!TargetSize::Scaled(f32::NAN).is_valid());
        assert!(
            !TargetSize::Fixed {
                width: 0,
                height: 5
            }
            .is_valid()
        );
    }

    #[test]
    fn the_quad_covers_clip_space_with_unshifted_coordinates() {
        let q = fullscreen_quad();
        assert_eq!(q.len() as u32, 4 * QUAD_FVF.stride());
        let f: Vec<f32> = q
            .as_chunks::<4>()
            .0
            .iter()
            .map(|b| f32::from_le_bytes(*b))
            .collect();
        assert_eq!(&f[0..5], &[-1.0, 1.0, 0.0, 0.0, 0.0]);
        assert_eq!(&f[15..20], &[1.0, -1.0, 0.0, 1.0, 1.0]);
    }
}
