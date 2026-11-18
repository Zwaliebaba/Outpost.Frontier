//! The 22 material kinds of `Shading` (`FMaterial` in
//! `CppSource/Shading/Material.cpp`, `kMaterialTypes`): one type whose
//! bind and submit recipes the kind selects, as in the reference.
//!
//! The data side (the kinds, their property schemas, the fields the
//! properties address, loading, storing and saving) is
//! `neuron_core::material_data` (work package P1.7); this module adds what
//! the reference does with it at run time: texture verification
//! (`Setup`), `ApplyVerify` with its fallbacks to fewer stages or passes,
//! `ResolveTextures`, the texture-stage and render-state tables of `Bind`
//! and `Teardown`, and the submissions (the kernel's three draw routes,
//! the alpha-test scope, the back faces of the `Two` kinds, the lightmap's
//! depth test, the detail kinds' two passes and the atmosphere's shell).
//!
//! Every renderer, texture-library and animation call goes through
//! [`MaterialServices`] in the reference's order with the reference's
//! arguments. Arithmetic follows decision T11: the atmosphere's values are
//! computed in `f64` and rounded to `f32` where the reference stores a
//! `float`.
//!
//! Where the reference reads through null or short pointers (a texture
//! block a kind does not have, a submission without a source, indices past
//! the array), the port reads zeros (divergence D310).

use neuron_core::diag::{self, DumpFlags, Severity};
use neuron_core::hash::name_crc32;
use neuron_core::material_data::{
    Bind, LeafWrite, Material as MaterialData, MaterialKind, PropertyValue, Submit,
};
use neuron_core::math::{Matrix, Transform, Vector, inverse_transform, mul_transform};
use neuron_core::vfs::FileSystem;

use super::batcher::{DrawArgs, SubmitContext, VertexSource, VertexStream};
use super::instance::Material;
use super::services::MaterialServices;
use super::{GR_GENERIC, GR_OK};
use crate::render::texture::FrameOut;

const SOURCE: &str = "Shading\\Material.cpp";

// --- Direct3D 8 numbers -------------------------------------------------------------------------

const RS_ZWRITEENABLE: u32 = 0x0E;
const RS_ALPHATESTENABLE: u32 = 0x0F;
const RS_CULLMODE: u32 = 0x16;
const RS_ZFUNC: u32 = 0x17;
const RS_ALPHAREF: u32 = 0x18;
const RS_ALPHAFUNC: u32 = 0x19;
const RS_COLORVERTEX: u32 = 0x8D;

const TSS_TEXCOORDINDEX: u32 = 0x0B;
const TSS_ADDRESSU: u32 = 0x0D;
const TSS_ADDRESSV: u32 = 0x0E;
const TSS_TEXTURETRANSFORMFLAGS: u32 = 0x18;

/// `kCullFlip`: none stays, clockwise and counter-clockwise swap.
const CULL_FLIP: [u32; 4] = [0, 1, 3, 2];
/// `GBtDetailTwoCullFlip`: the same lookup, whose first word aliases the
/// string `"Ec"`.
const BT_DETAIL_CULL_FLIP: [u32; 4] = [0x0000_6345, 1, 3, 2];

/// The epsilon the atmosphere replaces a length or shell of 0 or less
/// with (`0x33D6BF95`, about 1e-7).
const ATMOSPHERE_EPS: u32 = 0x33D6_BF95;

// --- The state tables (pairs of state and value) -----------------------------------------------

/// Lit, no specular, no blending (`06ed188c` and its copies).
const RS_OPAQUE: &[(u32, u32)] = &[(0x89, 1), (0x1D, 0), (0x1B, 0)];
/// Lit, blended by source alpha (`06ed18ac` and its copies).
const RS_BLEND: &[(u32, u32)] = &[(0x89, 1), (0x1D, 0), (0x1B, 1), (0x13, 5), (0x14, 6)];
/// Unlit, blended by source alpha (`06ed1e9c`, the illumination pass).
const RS_UNLIT_BLEND: &[(u32, u32)] = &[(0x89, 0), (0x1D, 0), (0x1B, 1), (0x13, 5), (0x14, 6)];
/// Unlit, the destination modulated by the source (`06ed206c`).
const RS_LIGHTMAP: &[(u32, u32)] = &[(0x89, 0), (0x1D, 0), (0x1B, 1), (0x13, 9), (0x14, 1)];
/// No vertex colours (`06ed209c`).
const RS_NO_COLOR_VERTEX: &[(u32, u32)] = &[(RS_COLORVERTEX, 0)];
/// Lit, additive (`06ed1c54`).
const RS_ADDITIVE: &[(u32, u32)] = &[(0x89, 1), (0x1D, 0), (0x1B, 1), (0x13, 2), (0x14, 2)];
/// Unlit, added by source alpha (`06ed23cc`).
const RS_NEBULA: &[(u32, u32)] = &[(0x89, 0), (0x1D, 0), (0x1B, 1), (0x13, 5), (0x14, 2)];

/// Texture times diffuse, colour and alpha.
const TSS_MODULATE: &[(u32, u32)] = &[(1, 4), (2, 2), (3, 0), (4, 4), (5, 2), (6, 0)];
/// Texture times the stage before, doubled; alpha times the stage before.
const TSS_MODULATE2X: &[(u32, u32)] = &[(1, 5), (2, 2), (3, 1), (4, 4), (5, 2), (6, 1)];
/// The stage disabled.
const TSS_DISABLE: &[(u32, u32)] = &[(1, 1), (2, 2), (3, 1), (4, 1), (5, 2), (6, 1)];
/// The texture alone.
const TSS_SELECT: &[(u32, u32)] = &[(1, 2), (2, 2), (3, 0), (4, 2), (5, 2), (6, 0)];
/// Texture plus the stage before; alpha modulated.
const TSS_ADD: &[(u32, u32)] = &[(1, 7), (2, 2), (3, 1), (4, 4), (5, 2), (6, 1)];

// --- Pixel formats --------------------------------------------------------------------------------

const ALPHA_BITS: [u32; 15] = [0, 0, 0, 0, 0, 4, 1, 8, 0, 0, 1, 0, 0, 0, 0];
const HAS_ALPHA: [bool; 15] = [
    false, false, false, false, false, true, true, true, false, false, true, true, true, true, true,
];

fn alpha_bits(format: u32) -> u32 {
    ALPHA_BITS.get(format as usize).copied().unwrap_or(0)
}

fn has_alpha(format: u32) -> bool {
    (10..=14).contains(&format) || HAS_ALPHA.get(format as usize).copied().unwrap_or(false)
}

// --- Vertex formats (`ShadingInternal.h`'s own helpers) ------------------------------------------

/// `FvfPosSize`: 0x1C when bits 2 and 3 are set, 0x10 for bit 2, 0xC for
/// bit 1, else 0 (the helper's own reading of the position bits).
#[must_use]
pub fn fvf_pos_size(fvf: u32) -> u32 {
    let bits = fvf & 0xFF;
    if bits & 0xC == 0xC {
        0x1C
    } else if bits & 4 == 4 {
        0x10
    } else if bits & 2 == 2 {
        0xC
    } else {
        0
    }
}

/// `FvfNormalSize`.
#[must_use]
pub fn fvf_normal_size(fvf: u32) -> u32 {
    if fvf & 0x10 != 0 { 0xC } else { 0 }
}

/// `FvfColorSize`: four bytes each for diffuse and specular.
#[must_use]
pub fn fvf_color_size(fvf: u32) -> u32 {
    u32::from(fvf & 0x40 != 0) * 4 + u32::from(fvf & 0x80 != 0) * 4
}

/// `FvfTexSize`: each texture coordinate set by its size bits.
#[must_use]
pub fn fvf_tex_size(fvf: u32) -> u32 {
    let mut size = 0;
    for set in 0..((fvf >> 8) & 0xF) {
        size += match (fvf >> ((0x10 + 2 * set) & 0x1F)) & 3 {
            0 => 8,
            1 => 0xC,
            2 => 0x10,
            _ => 4,
        };
    }
    size
}

/// `FvfStride`.
#[must_use]
pub fn fvf_stride(fvf: u32) -> u32 {
    fvf_pos_size(fvf) + fvf_normal_size(fvf) + fvf_color_size(fvf) + fvf_tex_size(fvf)
}

fn stream_f32(stream: &VertexStream, byte: usize) -> f32 {
    stream
        .data
        .as_deref()
        .and_then(|d| d.get(byte..byte + 4))
        .map_or(0.0, |b| f32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

fn stream_bits(stream: &VertexStream, byte: usize) -> u32 {
    stream_f32(stream, byte).to_bits()
}

fn put_bits(bytes: &mut [u8], at: usize, bits: u32) {
    if let Some(slot) = bytes.get_mut(at..at + 4) {
        slot.copy_from_slice(&bits.to_le_bytes());
    }
}

/// The indices a draw reads: `span` from `ib_start` on, zeros past the
/// array (divergence D310).
fn draw_indices(context: &SubmitContext<'_>, ib_start: u32, span: u32) -> Vec<u16> {
    (0..span as usize)
        .map(|i| {
            context
                .indices
                .get(ib_start as usize + i)
                .copied()
                .unwrap_or(0)
        })
        .collect()
}

fn has_vertex_colours(context: Option<&SubmitContext<'_>>) -> bool {
    context
        .and_then(|c| c.vertices)
        .is_some_and(|v| v.diffuse.data.is_some())
}

/// What a texture block holds at run time (`FTexBlock` from `+4`): the
/// playback id, the frame `GetFrame` wrote and whether resolving skips the
/// block (a single frame).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TextureState {
    /// `TextureId`, -1 for none.
    pub id: i32,
    /// `Handle` and `Uv0..3`.
    pub frame: FrameOut,
    /// `bSkipResolve`.
    pub skip_resolve: bool,
}

impl Default for TextureState {
    fn default() -> Self {
        TextureState {
            id: -1,
            frame: FrameOut {
                handle: 0,
                uv: [0; 4],
            },
            skip_resolve: false,
        }
    }
}

/// The run-time state of a material, for tests and diagnostics.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MaterialState {
    /// `bAlphablend`.
    pub alpha_blend: bool,
    /// `bAlphatest`.
    pub alpha_test: bool,
    /// `bAtmosphereReady`.
    pub atmosphere_ready: bool,
    /// `DetailPassCount`.
    pub detail_pass_count: u32,
    /// `bDetailSecondStageVerified`.
    pub detail_second_stage_verified: bool,
    /// `bDetailMultiPassEnabled`.
    pub detail_multi_pass_enabled: bool,
    /// `bSecondaryTextureEnabled`.
    pub secondary_enabled: bool,
    /// `bSecondaryTextureVerified`.
    pub secondary_verified: bool,
    /// One per texture block.
    pub textures: Vec<TextureState>,
    /// An animation instance is held.
    pub animated: bool,
}

/// A material of one of the 22 kinds. `A` is the animation instance of the
/// services it is used with.
#[derive(Clone, Debug)]
pub struct ShadingMaterial<A> {
    data: MaterialData,
    name: Option<Vec<u8>>,
    textures: Vec<TextureState>,
    alpha_test: bool,
    animation: Option<A>,
    atmosphere_ready: bool,
    detail_pass_count: u32,
    detail_second_stage_verified: bool,
    detail_multi_pass_enabled: bool,
    secondary_enabled: bool,
    secondary_verified: bool,
}

impl<A: Copy> ShadingMaterial<A> {
    /// `CreateMaterialInstance` and `InitializeFromDescriptor`: the
    /// constructor's values (an atmosphere starts ready). The texture
    /// blocks come with [`Material::initialize`].
    #[must_use]
    pub fn new(kind: MaterialKind) -> ShadingMaterial<A> {
        ShadingMaterial {
            data: MaterialData::new(kind),
            name: None,
            textures: Vec::new(),
            alpha_test: false,
            animation: None,
            atmosphere_ready: kind.bind == Bind::Atmosphere,
            detail_pass_count: 2,
            detail_second_stage_verified: true,
            detail_multi_pass_enabled: true,
            secondary_enabled: true,
            secondary_verified: true,
        }
    }

    /// The kind.
    #[must_use]
    pub fn kind(&self) -> MaterialKind {
        self.data.kind
    }

    /// The fields the properties address.
    #[must_use]
    pub fn data(&self) -> &MaterialData {
        &self.data
    }

    /// The run-time state.
    #[must_use]
    pub fn state(&self) -> MaterialState {
        MaterialState {
            alpha_blend: self.data.fields.alpha_blend,
            alpha_test: self.alpha_test,
            atmosphere_ready: self.atmosphere_ready,
            detail_pass_count: self.detail_pass_count,
            detail_second_stage_verified: self.detail_second_stage_verified,
            detail_multi_pass_enabled: self.detail_multi_pass_enabled,
            secondary_enabled: self.secondary_enabled,
            secondary_verified: self.secondary_verified,
            textures: self.textures.clone(),
            animated: self.animation.is_some(),
        }
    }

    fn bind_kind(&self) -> Bind {
        self.data.kind.bind
    }

    fn submit_kind(&self) -> Submit {
        self.data.kind.submit
    }

    fn single_pass(&self) -> bool {
        self.data.kind.is_single_pass_family()
    }

    fn opacity(&self) -> f32 {
        f32::from_bits(self.data.fields.diffuse[3])
    }

    fn float(bits: u32) -> f32 {
        f32::from_bits(bits)
    }

    /// The 17 words of `SetMaterialBlock(&DiffuseR)`.
    fn material_block(&self) -> [u32; 17] {
        let f = &self.data.fields;
        let mut block = [0u32; 17];
        block[0..4].copy_from_slice(&f.diffuse);
        block[4..8].copy_from_slice(&f.ambient);
        block[8..12].copy_from_slice(&f.specular);
        block[12..16].copy_from_slice(&f.emissive);
        block[16] = f.specular_power;
        block
    }

    fn handle(&self, block: usize) -> u32 {
        self.textures.get(block).map_or(0, |t| t.frame.handle)
    }

    fn id(&self, block: usize) -> i32 {
        self.textures.get(block).map_or(-1, |t| t.id)
    }

    /// The flags and address modes of block `index` (the schema's).
    fn modes(&self, index: usize) -> (u32, u32, u32) {
        self.data
            .fields
            .textures
            .get(index)
            .map_or((4, 0, 0), |b| (b.flags, b.address_u, b.address_v))
    }

    /// Drops the playback of block `index` (`DecRefById`, id -1).
    fn drop_texture<S: MaterialServices<Animation = A> + ?Sized>(
        &mut self,
        services: &mut S,
        index: usize,
    ) {
        if let Some(t) = self.textures.get_mut(index)
            && t.id != -1
        {
            services.dec_ref_texture(t.id);
            t.id = -1;
        }
    }

    fn global<S: MaterialServices<Animation = A> + ?Sized>(services: &mut S, index: u32) -> bool {
        services.global_record(index) >= 1
    }

    // --- Shared pieces --------------------------------------------------------------------------

    fn apply_table<S: MaterialServices<Animation = A> + ?Sized>(
        services: &mut S,
        table: &[(u32, u32)],
        stage: Option<u32>,
    ) {
        for &(state, value) in table {
            match stage {
                Some(stage) => services.set_stage_state(stage, state, value),
                None => services.set_render_state(state, value),
            }
        }
    }

    /// `BindDetailTexture`: the block's texture, coordinate set and address
    /// modes on `stage`.
    fn bind_texture<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        block: usize,
        stage: u32,
    ) {
        let (flags, u, v) = self.modes(block);
        services.set_stage_texture(stage, self.handle(block));
        services.set_stage_state(stage, TSS_TEXCOORDINDEX, flags.saturating_sub(4));
        services.set_stage_state(stage, TSS_ADDRESSU, u.wrapping_add(1));
        services.set_stage_state(stage, TSS_ADDRESSV, v.wrapping_add(1));
    }

    /// `BindMaterialAndContext`: the material block, then vertex colours
    /// when the submission's source has them.
    fn bind_material_and_context<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) {
        services.set_material(&self.material_block());
        if has_vertex_colours(context) {
            services.set_render_state(RS_COLORVERTEX, 1);
        }
    }

    /// `SetDetailTileTransform`: stage 1 scaled by the tile rate.
    fn tile_transform<S: MaterialServices<Animation = A> + ?Sized>(services: &mut S, rate: u32) {
        let mut matrix = [0.0f32; 16];
        matrix[0] = Self::float(rate);
        matrix[5] = Self::float(rate);
        services.set_texture_transform(1, &matrix);
    }

    /// `ApplyOcFlag` and the end of `Setup`: blending for an opacity below
    /// 1, else from the first texture's format (a one-bit alpha tests, any
    /// other alpha blends).
    fn apply_alpha_flags<S: MaterialServices<Animation = A> + ?Sized>(&mut self, services: &mut S) {
        let blend = self.opacity() < 1.0;
        self.data.fields.alpha_blend = blend;
        self.alpha_test = false;
        if blend {
            return;
        }
        let handle = self.handle(0);
        if handle == 0 {
            return;
        }
        let Ok(format) = services.texture_format(handle) else {
            return;
        };
        self.alpha_test = alpha_bits(format) == 1;
        self.data.fields.alpha_blend = has_alpha(format) && !self.alpha_test;
    }

    /// `Setup`: each texture block looked up by name, its playback made and,
    /// for a single frame, its frame read; the previous playback released;
    /// then the alpha flags.
    fn setup<S: MaterialServices<Animation = A> + ?Sized>(&mut self, services: &mut S) {
        for index in 0..self.textures.len() {
            let previous = self.textures[index].id;
            {
                let t = &mut self.textures[index];
                t.frame.handle = 0;
                t.id = -1;
                t.skip_resolve = true;
            }
            let mut suppress = true;
            let name = self
                .data
                .fields
                .textures
                .get(index)
                .and_then(|b| b.name.clone());
            if let Some(name) = &name {
                if let Some(record) = services.find_texture(name, name_crc32(name)) {
                    let frames = services.texture_frame_count(record);
                    let skip = frames == Some(1);
                    self.textures[index].skip_resolve = skip;
                    let played = services.create_playback(record);
                    self.textures[index].id = *played.as_ref().unwrap_or(&-1);
                    if let Ok(id) = played
                        && skip
                        && let Some(frame) = services.texture_frame(id, 0)
                    {
                        self.textures[index].frame = frame;
                    }
                    services.release_texture_record(record);
                } else {
                    suppress = name.as_slice() == b"(None)";
                }
            }
            if previous != -1 {
                services.dec_ref_texture(previous);
            }
            if !suppress {
                let message = format!(
                    "texture verify failed: {}: {}",
                    String::from_utf8_lossy(self.name.as_deref().unwrap_or_default()),
                    String::from_utf8_lossy(name.as_deref().unwrap_or_default()),
                );
                diag::dump(
                    DumpFlags::NOTICE,
                    &diag::general_line(SOURCE, 2793, Severity::Notice, &message),
                );
            }
        }
        self.apply_alpha_flags(services);
    }

    // --- Pass states of the detail kinds --------------------------------------------------------

    fn detail_pass_state<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        pass: u32,
    ) {
        if self.detail_pass_count == 1 {
            services.set_material(&self.material_block());
            self.bind_texture(services, 0, 0);
            Self::apply_table(services, TSS_MODULATE, Some(0));
            Self::apply_table(services, TSS_DISABLE, Some(1));
            Self::apply_table(services, RS_OPAQUE, None);
            return;
        }
        if pass == 0 {
            services.set_material(&self.material_block());
            self.bind_texture(services, 0, 0);
            Self::apply_table(services, TSS_MODULATE, Some(0));
            let mut disabled = 1;
            if self.detail_second_stage_verified {
                self.bind_texture(services, 1, 1);
                Self::apply_table(services, TSS_MODULATE2X, Some(1));
                services.set_stage_state(1, TSS_TEXCOORDINDEX, 0);
                services.set_stage_state(1, TSS_TEXTURETRANSFORMFLAGS, 2);
                Self::tile_transform(services, self.data.fields.tile_rate0);
                disabled = 2;
            }
            Self::apply_table(services, TSS_DISABLE, Some(disabled));
            Self::apply_table(services, RS_OPAQUE, None);
        } else if pass == 1 && self.detail_second_stage_verified {
            Self::apply_table(services, TSS_DISABLE, Some(1));
            Self::apply_table(services, RS_BLEND, None);
            services.set_stage_state(1, TSS_TEXTURETRANSFORMFLAGS, 0);
        }
    }

    fn illum_pass_state<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        pass: u32,
    ) {
        if pass == 0 {
            services.set_material(&self.material_block());
            self.bind_texture(services, 0, 0);
            Self::apply_table(services, TSS_MODULATE, Some(0));
            let mut disabled = 1;
            if self.detail_second_stage_verified {
                self.bind_texture(services, 1, 1);
                Self::apply_table(services, TSS_MODULATE2X, Some(1));
                services.set_stage_state(1, TSS_TEXCOORDINDEX, 0);
                services.set_stage_state(1, TSS_TEXTURETRANSFORMFLAGS, 2);
                Self::tile_transform(services, self.data.fields.tile_rate0);
                disabled = 2;
            }
            Self::apply_table(services, TSS_DISABLE, Some(disabled));
            Self::apply_table(services, RS_OPAQUE, None);
        } else if pass == 1 {
            Self::apply_table(services, TSS_SELECT, Some(0));
            Self::apply_table(services, TSS_MODULATE2X, Some(1));
            Self::apply_table(services, TSS_DISABLE, Some(2));
            Self::apply_table(services, RS_UNLIT_BLEND, None);
            self.bind_texture(services, 2, 1);
            services.set_stage_state(1, TSS_TEXTURETRANSFORMFLAGS, 2);
            Self::tile_transform(services, self.data.fields.tile_rate1);
        }
    }

    fn masked_pass_state<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        pass: u32,
    ) {
        if self.detail_pass_count == 1 {
            services.set_material(&self.material_block());
            self.bind_texture(services, 0, 0);
            Self::apply_table(services, TSS_MODULATE, Some(0));
            Self::apply_table(services, TSS_DISABLE, Some(1));
            Self::apply_table(services, RS_OPAQUE, None);
            return;
        }
        if pass == 0 {
            services.set_material(&self.material_block());
            self.bind_texture(services, 0, 0);
            self.bind_texture(services, 1, 1);
            Self::apply_table(services, TSS_MODULATE, Some(0));
            Self::apply_table(services, TSS_MODULATE2X, Some(1));
            Self::apply_table(services, TSS_DISABLE, Some(2));
            Self::apply_table(services, RS_OPAQUE, None);
            services.set_stage_state(1, TSS_TEXCOORDINDEX, 0);
            services.set_stage_state(1, TSS_TEXTURETRANSFORMFLAGS, 2);
            Self::tile_transform(services, self.data.fields.tile_rate0);
        } else if pass == 1 {
            Self::apply_table(services, TSS_MODULATE2X, Some(1));
            Self::apply_table(services, TSS_DISABLE, Some(2));
            Self::apply_table(services, RS_BLEND, None);
            self.bind_texture(services, 2, 1);
            services.set_stage_state(1, TSS_TEXCOORDINDEX, 0);
            services.set_stage_state(1, TSS_TEXTURETRANSFORMFLAGS, 2);
            Self::tile_transform(services, self.data.fields.tile_rate1);
        }
    }

    /// The pass state of `ApplyVerifyDetailFamily`'s kind.
    fn family_pass_state<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        pass: u32,
    ) {
        if self.submit_kind() == Submit::Detail2Pass {
            self.detail_pass_state(services, pass);
        } else {
            self.masked_pass_state(services, pass);
        }
    }

    // --- Bind -----------------------------------------------------------------------------------

    fn bind_single_pass<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) {
        self.bind_material_and_context(services, context);
        self.bind_texture(services, 0, 0);
        Self::apply_table(services, TSS_MODULATE, Some(0));
        Self::apply_table(services, TSS_DISABLE, Some(1));
        let table = if self.data.fields.alpha_blend {
            RS_BLEND
        } else {
            RS_OPAQUE
        };
        Self::apply_table(services, table, None);
        if let Some(instance) = self.animation
            && let Some(matrix) = services.animation_uv_matrix(instance)
        {
            services.set_stage_state(0, TSS_TEXTURETRANSFORMFLAGS, 2);
            services.set_texture_transform(0, &matrix);
        }
    }

    fn bind_light_map<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) {
        self.bind_material_and_context(services, context);
        Self::apply_table(services, RS_NO_COLOR_VERTEX, None);
        self.bind_texture(services, 0, 0);
        Self::apply_table(services, TSS_SELECT, Some(0));
        Self::apply_table(services, TSS_DISABLE, Some(1));
        Self::apply_table(services, RS_LIGHTMAP, None);
    }

    fn bind_nebula<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) {
        self.bind_material_and_context(services, context);
        self.bind_texture(services, 0, 0);
        Self::apply_table(services, TSS_MODULATE, Some(0));
        Self::apply_table(services, TSS_DISABLE, Some(1));
        Self::apply_table(services, RS_NEBULA, None);
    }

    fn bind_atmosphere<S: MaterialServices<Animation = A> + ?Sized>(&self, services: &mut S) {
        services.set_material(&self.material_block());
        self.bind_texture(services, 0, 0);
        Self::apply_table(services, TSS_MODULATE, Some(0));
        Self::apply_table(services, TSS_DISABLE, Some(1));
        Self::apply_table(services, RS_BLEND, None);
    }

    fn bind_dc_dt_et<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) {
        self.bind_material_and_context(services, context);
        Self::apply_table(services, RS_OPAQUE, None);
        self.bind_texture(services, 0, 0);
        Self::apply_table(services, TSS_MODULATE, Some(0));
        let mut disabled = 1;
        if self.secondary_enabled && self.id(1) != -1 {
            services.set_stage_state(1, TSS_TEXCOORDINDEX, 0);
            self.bind_texture(services, 1, 1);
            Self::apply_table(services, TSS_ADD, Some(1));
            disabled = 2;
        }
        Self::apply_table(services, TSS_DISABLE, Some(disabled));
    }

    fn bind_ec_et<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) {
        self.bind_material_and_context(services, context);
        self.bind_texture(services, 0, 0);
        Self::apply_table(services, TSS_MODULATE, Some(0));
        Self::apply_table(services, TSS_DISABLE, Some(1));
        Self::apply_table(services, RS_ADDITIVE, None);
    }

    fn bind_detail_map<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) {
        self.bind_material_and_context(services, context);
        self.bind_texture(services, 0, 0);
        Self::apply_table(services, TSS_MODULATE, Some(0));
        let mut disabled = 1;
        if self.detail_second_stage_verified {
            self.bind_texture(services, 1, 1);
            Self::apply_table(services, TSS_MODULATE2X, Some(1));
            services.set_stage_state(1, TSS_TEXCOORDINDEX, 0);
            services.set_stage_state(1, TSS_TEXTURETRANSFORMFLAGS, 2);
            Self::tile_transform(services, self.data.fields.tile_rate0);
            disabled = 2;
        }
        Self::apply_table(services, TSS_DISABLE, Some(disabled));
        Self::apply_table(services, RS_OPAQUE, None);
    }

    fn bind_bt_detail<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) {
        self.bind_material_and_context(services, context);
        Self::apply_table(services, RS_OPAQUE, None);
        self.bind_texture(services, 0, 0);
        Self::apply_table(services, TSS_MODULATE, Some(0));
        let mut disabled = 1;
        if self.secondary_verified {
            self.bind_texture(services, 1, 1);
            Self::apply_table(services, TSS_MODULATE2X, Some(1));
            disabled = 2;
        }
        Self::apply_table(services, TSS_DISABLE, Some(disabled));
    }

    fn do_bind<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) -> i32 {
        match self.bind_kind() {
            Bind::Null | Bind::Stub => {}
            Bind::Atmosphere => {
                if self.atmosphere_ready {
                    self.bind_atmosphere(services);
                }
            }
            Bind::Nebula => self.bind_nebula(services, context),
            Bind::DetailMap => self.bind_detail_map(services, context),
            _ if self.single_pass() => self.bind_single_pass(services, context),
            Bind::LightMap => self.bind_light_map(services, context),
            Bind::BtDetail => self.bind_bt_detail(services, context),
            Bind::DcDtEt => self.bind_dc_dt_et(services, context),
            Bind::EcEt => self.bind_ec_et(services, context),
            Bind::DcDt => {}
        }
        GR_OK
    }

    fn do_teardown<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) -> i32 {
        if self.bind_kind() == Bind::Null {
            return GR_OK;
        }
        if matches!(
            self.submit_kind(),
            Submit::Detail2Pass | Submit::IllumDetail2Pass | Submit::MaskedDetail2Pass
        ) {
            Self::apply_table(services, TSS_DISABLE, Some(1));
            services.set_stage_state(0, TSS_TEXTURETRANSFORMFLAGS, 0);
            services.set_stage_state(1, TSS_TEXTURETRANSFORMFLAGS, 0);
            services.set_stage_state(1, TSS_TEXCOORDINDEX, 1);
            return GR_OK;
        }
        if self.bind_kind() == Bind::BtDetail {
            Self::apply_table(services, TSS_DISABLE, Some(1));
        }
        if self.single_pass() && self.animation.is_some() {
            services.set_stage_state(0, TSS_TEXTURETRANSFORMFLAGS, 0);
        }
        if has_vertex_colours(context) {
            services.set_render_state(RS_COLORVERTEX, 0);
        }
        GR_OK
    }

    // --- ApplyVerify ----------------------------------------------------------------------------

    fn verify_animated<S: MaterialServices<Animation = A> + ?Sized>(
        &mut self,
        services: &mut S,
    ) -> i32 {
        if services.has_animation_library() {
            let crc = self.name.as_deref().map_or(0, name_crc32);
            if let Some(old) = self.animation.take() {
                services.release_animation_instance(old);
            }
            self.animation = services.create_animation_instance(self.name.as_deref(), crc);
        }
        self.setup(services);
        self.do_bind(services, None);
        let result = services.verify_state();
        self.do_teardown(services, None);
        result
    }

    fn verify_detail_family<S: MaterialServices<Animation = A> + ?Sized>(
        &mut self,
        services: &mut S,
    ) -> i32 {
        self.detail_pass_count = 2;
        self.detail_multi_pass_enabled = Self::global(services, 0);
        self.setup(services);
        self.detail_second_stage_verified = true;
        self.family_pass_state(services, 0);
        let first = services.verify_state() >= 0;
        self.family_pass_state(services, 1);
        let mut result = services.verify_state();
        let second = result >= 0;
        self.detail_second_stage_verified = first && second;
        if !first || !second || !self.detail_multi_pass_enabled {
            self.drop_texture(services, 1);
            self.detail_second_stage_verified = false;
            self.detail_multi_pass_enabled = false;
            self.detail_pass_count = 1;
            self.family_pass_state(services, 0);
            result = services.verify_state();
        }
        self.do_teardown(services, None);
        result
    }

    fn verify_illum<S: MaterialServices<Animation = A> + ?Sized>(
        &mut self,
        services: &mut S,
    ) -> i32 {
        self.detail_pass_count = 2;
        self.detail_multi_pass_enabled = Self::global(services, 0);
        self.setup(services);
        let mut first = false;
        let mut second = false;
        self.detail_second_stage_verified = true;
        if self.detail_multi_pass_enabled {
            self.illum_pass_state(services, 0);
            first = services.verify_state() >= 0;
            self.illum_pass_state(services, 1);
            second = services.verify_state() >= 0;
        }
        self.detail_second_stage_verified = first && second;
        let mut result = GR_OK;
        if !first || !second || !self.detail_multi_pass_enabled {
            self.detail_pass_count = 1;
            self.detail_second_stage_verified = first;
            if !first || !self.detail_multi_pass_enabled {
                self.drop_texture(services, 1);
                self.detail_pass_count = 1;
                self.detail_second_stage_verified = false;
                self.detail_multi_pass_enabled = false;
                self.illum_pass_state(services, 0);
                result = services.verify_state();
            }
        }
        self.do_teardown(services, None);
        result
    }

    fn verify_bt_detail<S: MaterialServices<Animation = A> + ?Sized>(
        &mut self,
        services: &mut S,
    ) -> i32 {
        self.secondary_enabled = Self::global(services, 0);
        self.setup(services);
        self.secondary_verified = true;
        self.do_bind(services, None);
        let mut result = services.verify_state();
        self.secondary_verified = result >= 0;
        if !self.secondary_verified || !self.secondary_enabled {
            self.drop_texture(services, 1);
            self.do_bind(services, None);
            self.secondary_verified = false;
            self.secondary_enabled = false;
            result = services.verify_state();
        }
        self.do_teardown(services, None);
        result
    }

    fn verify_dc_dt_et<S: MaterialServices<Animation = A> + ?Sized>(
        &mut self,
        services: &mut S,
    ) -> i32 {
        self.secondary_enabled = Self::global(services, 2);
        self.setup(services);
        self.do_bind(services, None);
        let mut result = services.verify_state();
        if result < 0 || !self.secondary_enabled {
            self.drop_texture(services, 1);
            if self.secondary_enabled {
                self.do_bind(services, None);
                result = services.verify_state();
            }
        }
        self.do_teardown(services, None);
        result
    }

    fn verify_detail_map<S: MaterialServices<Animation = A> + ?Sized>(
        &mut self,
        services: &mut S,
    ) -> i32 {
        self.detail_multi_pass_enabled = Self::global(services, 0);
        self.setup(services);
        self.detail_second_stage_verified = true;
        self.do_bind(services, None);
        let mut result = services.verify_state();
        self.detail_second_stage_verified = result >= 0;
        if !self.detail_second_stage_verified || !self.detail_multi_pass_enabled {
            self.drop_texture(services, 1);
            self.do_bind(services, None);
            self.detail_second_stage_verified = false;
            self.detail_multi_pass_enabled = false;
            result = services.verify_state();
        }
        self.do_teardown(services, None);
        result
    }

    fn verify_atmosphere<S: MaterialServices<Animation = A> + ?Sized>(
        &mut self,
        services: &mut S,
    ) -> i32 {
        self.setup(services);
        let saved = self.atmosphere_ready;
        self.atmosphere_ready = true;
        self.do_bind(services, None);
        self.atmosphere_ready = saved;
        let result = services.verify_state();
        if result != GR_OK {
            let message = format!(
                "failed to verify {}",
                String::from_utf8_lossy(self.name.as_deref().unwrap_or_default())
            );
            diag::dump(
                DumpFlags::NOTICE,
                &diag::general_line(
                    "Shading\\Materials\\AtmosphereMaterial.cpp",
                    0x97,
                    Severity::Notice,
                    &message,
                ),
            );
        }
        self.do_teardown(services, None);
        result
    }

    // --- Submit ---------------------------------------------------------------------------------

    /// `SubmitIndexedListing`: the caller's vertex buffer, if it gave one.
    fn indexed_listing<S: MaterialServices<Animation = A> + ?Sized>(
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        let Some(vb) = context.vb else {
            return -1;
        };
        let indices = draw_indices(context, args.ib_start, args.index_span);
        services.draw_indexed_vb(
            args.prim_type,
            vb.vertex_buffer,
            vb.base_vertex.wrapping_add(args.start),
            args.count,
            &indices,
        )
    }

    /// `SubmitCopiedVertices`: the source copied into vertex memory.
    fn copied_vertices<S: MaterialServices<Animation = A> + ?Sized>(
        services: &mut S,
        context: &SubmitContext<'_>,
        source: &VertexSource,
        args: &DrawArgs,
    ) -> i32 {
        let mut lock = match services.lock_vertex_memory(source.fvf, args.count) {
            Ok(lock) => lock,
            Err(result) => return result,
        };
        let fvf = lock.listing.fvf;
        services.copy_vertex_data(&mut lock.bytes, fvf, source, args.start, args.count);
        let listing = lock.listing;
        services.unlock_vertex_memory(lock);
        let indices = draw_indices(context, args.ib_start, args.index_span);
        services.draw_indexed_vb(
            args.prim_type,
            listing.vertex_buffer,
            listing.base_vertex,
            args.count,
            &indices,
        )
    }

    /// `SubmitKernel`: the device's own buffers (flag 0x100), the caller's
    /// vertex buffer, then a copy of the source.
    fn kernel<S: MaterialServices<Animation = A> + ?Sized>(
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        if args.flags & 0x100 != 0 {
            let bias = context.ib.map_or(0, |ib| ib.bias);
            let result = services.draw_indexed(
                args.prim_type,
                args.start,
                args.count,
                bias.wrapping_add(args.ib_start),
                args.index_span,
            );
            if result >= 0 {
                return result;
            }
        }
        let mut result = Self::indexed_listing(services, context, args);
        if result < 0
            && let Some(source) = context.vertices
            && args.count > 0
            && args.index_span > 0
        {
            result = Self::copied_vertices(services, context, source, args);
        }
        result
    }

    /// `SubmitAlphaTest`: the kernel inside an alpha test at 0x80 for a
    /// material whose texture has a one-bit alpha (unless flag 0x400).
    fn alpha_test_submit<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        let scope = self.alpha_test && args.flags & 0x400 == 0;
        let mut saved = (0, 0, 0);
        if scope {
            saved.0 = services.render_state(RS_ALPHAREF);
            services.set_render_state(RS_ALPHAREF, 0x80);
            saved.1 = services.render_state(RS_ALPHAFUNC);
            services.set_render_state(RS_ALPHAFUNC, 5);
            saved.2 = services.render_state(RS_ALPHATESTENABLE);
            services.set_render_state(RS_ALPHATESTENABLE, 1);
        }
        let result = Self::kernel(services, context, args);
        if scope {
            services.set_render_state(RS_ALPHAREF, saved.0);
            services.set_render_state(RS_ALPHAFUNC, saved.1);
            services.set_render_state(RS_ALPHATESTENABLE, saved.2);
        }
        result
    }

    /// `NegateTwoPassNormals`: the source's normals, negated, into the
    /// copied vertices.
    fn negate_normals(
        source: &VertexSource,
        start: u32,
        count: u32,
        bytes: &mut [u8],
        dest_stride: u32,
        dest_pos_size: u32,
    ) {
        let normal = &source.normal;
        let source_stride = fvf_stride(source.fvf) as usize;
        for i in 0..count as usize {
            let at = if source.flags & 4 != 0 {
                let index = normal
                    .index
                    .as_deref()
                    .and_then(|x| x.get(start as usize + i))
                    .copied()
                    .unwrap_or(0);
                index as usize * normal.stride as usize
            } else {
                source_stride * (start as usize + i)
            };
            let out = dest_pos_size as usize + i * dest_stride as usize;
            for k in 0..3 {
                let bits = stream_bits(normal, at + 4 * k) ^ 0x8000_0000;
                put_bits(bytes, out + 4 * k, bits);
            }
        }
    }

    /// The back faces of the `Two` kinds: the source copied with negated
    /// normals and drawn with the cull mode flipped through `flip`.
    fn back_faces<S: MaterialServices<Animation = A> + ?Sized>(
        services: &mut S,
        context: &SubmitContext<'_>,
        source: &VertexSource,
        args: &DrawArgs,
        flip: &[u32; 4],
    ) -> i32 {
        let mut lock = match services.lock_vertex_memory(source.fvf, args.count) {
            Ok(lock) => lock,
            Err(result) => return result,
        };
        let fvf = lock.listing.fvf;
        let pos_size = fvf_pos_size(fvf);
        let stride = fvf_stride(fvf);
        services.copy_vertex_data(&mut lock.bytes, fvf, source, args.start, args.count);
        if source.fvf & 0x10 != 0 {
            Self::negate_normals(
                source,
                args.start,
                args.count,
                &mut lock.bytes,
                stride,
                pos_size,
            );
        }
        let listing = lock.listing;
        services.unlock_vertex_memory(lock);
        let saved = services.render_state(RS_CULLMODE);
        let flipped = flip.get(saved as usize).copied().unwrap_or(0);
        services.set_render_state(RS_CULLMODE, flipped);
        let indices = draw_indices(context, args.ib_start, args.index_span);
        let result = services.draw_indexed_vb(
            args.prim_type,
            listing.vertex_buffer,
            listing.base_vertex,
            args.count,
            &indices,
        );
        services.set_render_state(RS_CULLMODE, saved);
        result
    }

    /// `SubmitAtmosphereLock`: the source copied, the world scaled by
    /// `Scale`, and each vertex's first texture coordinate set from the
    /// angle between its normal and the direction to the camera, faded
    /// with the camera's distance from the shell; drawn without depth
    /// writes.
    #[allow(clippy::too_many_lines)]
    fn atmosphere<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        if !self.atmosphere_ready {
            return GR_OK;
        }
        let Some(source) = context.vertices else {
            return GR_GENERIC;
        };
        let mut lock = match services.lock_vertex_memory(source.fvf, args.count) {
            Ok(lock) => lock,
            Err(result) => return result,
        };
        let fvf = lock.listing.fvf;
        let dest_stride = fvf_stride(fvf) as usize;
        services.copy_vertex_data(&mut lock.bytes, fvf, source, args.start, args.count);
        let tex_offset = (fvf_color_size(fvf) + fvf_normal_size(fvf) + fvf_pos_size(fvf)) as usize;

        let f = &self.data.fields;
        let scale = Self::float(f.scale);
        let scale_transform = Transform::new(
            Matrix {
                m: [[scale, 0.0, 0.0], [0.0, scale, 0.0], [0.0, 0.0, scale]],
            },
            Vector::ZERO,
        );
        let world = Transform::from_words(*context.world);
        let view = Transform::from_words(*context.view);
        let scaled_world = mul_transform(&world, &scale_transform);
        let state = services.pipeline_state(7).unwrap_or(1);
        if state != 0 {
            let modelview = mul_transform(&view, &scaled_world);
            services.set_modelview(&modelview.to_words());
        } else {
            services.set_world(&scaled_world.to_words());
        }

        let mut radius = 1.0f32;
        if context.field20 != 0
            && let Some((r, _centre)) = services.instance_extent(context.field20, context.instance)
        {
            radius = r;
        }
        let radius_over_scale = (f64::from(radius) / f64::from(scale)) as f32;
        let view_world = mul_transform(&view, &world);
        let camera = inverse_transform(&view_world, Vector::ZERO);

        let (cx, cy, cz) = (
            f64::from(camera.x),
            f64::from(camera.y),
            f64::from(camera.z),
        );
        let squared_xy = cx * cx + cy * cy;
        let length_wide = (squared_xy + cz * cz).sqrt();
        let mut length = length_wide as f32;
        if length_wide <= 0.0 {
            length = Self::float(ATMOSPHERE_EPS);
        }
        let inverse_length = 1.0 / f64::from(length);
        let direction = [
            (cx * inverse_length) as f32,
            (cy * inverse_length) as f32,
            (cz * inverse_length) as f32,
        ];
        let threshold = (inverse_length * f64::from(radius)) as f32;
        let mut shell = (f64::from(scale) - 1.0) * f64::from(radius_over_scale);
        if shell <= 0.0 {
            shell = f64::from(Self::float(ATMOSPHERE_EPS));
        }
        let distance = f64::from(length) - f64::from(radius);
        let factor = distance / (shell + distance);
        let factor_cubed = ((factor * factor) * factor) as f32;
        let mut fade = 1.0 - f64::from(Self::float(f.alpha));
        if f64::from(factor_cubed) < 0.5 {
            fade *= f64::from(factor_cubed);
            fade += fade;
        }
        let fade_off = f64::from(Self::float(f.fade_off));
        if source.flags & 4 == 0 {
            for i in 0..args.count as usize {
                let out = tex_offset + i * dest_stride;
                put_bits(&mut lock.bytes, out + 4, (fade as f32).to_bits());
                let at = (args.start as usize + i) * 12;
                let n = [
                    f64::from(stream_f32(&source.normal, at)),
                    f64::from(stream_f32(&source.normal, at + 4)),
                    f64::from(stream_f32(&source.normal, at + 8)),
                ];
                let dot_zy = f64::from(direction[2]) * n[2] + f64::from(direction[1]) * n[1];
                let dot = dot_zy + f64::from(direction[0]) * n[0];
                let threshold = f64::from(threshold);
                let u = if dot < threshold {
                    0.0f32
                } else {
                    (((dot - threshold) / (1.0 - threshold)) * fade_off * f64::from(factor_cubed))
                        as f32
                };
                put_bits(&mut lock.bytes, out, u.to_bits());
            }
        }
        let listing = lock.listing;
        services.unlock_vertex_memory(lock);
        let saved_zfunc = services.render_state(RS_ZFUNC);
        services.set_render_state(RS_ZFUNC, 4);
        let saved_zwrite = services.render_state(RS_ZWRITEENABLE);
        services.set_render_state(RS_ZWRITEENABLE, 0);
        let indices = draw_indices(context, args.ib_start, args.index_span);
        services.draw_indexed_vb(
            args.prim_type,
            listing.vertex_buffer,
            listing.base_vertex,
            args.count,
            &indices,
        );
        services.set_render_state(RS_ZFUNC, saved_zfunc);
        services.set_render_state(RS_ZWRITEENABLE, saved_zwrite);
        services.set_render_state(RS_COLORVERTEX, 0);
        GR_OK
    }

    fn submit_illum<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        let saved_test = services.render_state(RS_ALPHATESTENABLE);
        services.set_render_state(RS_ALPHATESTENABLE, 0);
        self.illum_pass_state(services, 0);
        Self::kernel(services, context, args);
        if self.detail_pass_count == 1 {
            services.set_render_state(RS_ALPHATESTENABLE, saved_test);
            self.do_teardown(services, None);
            return GR_OK;
        }
        services.set_render_state(RS_ALPHATESTENABLE, 1);
        let saved_ref = services.render_state(RS_ALPHAREF);
        services.set_render_state(RS_ALPHAREF, 1);
        let saved_func = services.render_state(RS_ALPHAFUNC);
        services.set_render_state(RS_ALPHAFUNC, 5);
        let saved_zfunc = services.render_state(RS_ZFUNC);
        services.set_render_state(RS_ZFUNC, 4);
        let saved_zwrite = services.render_state(RS_ZWRITEENABLE);
        services.set_render_state(RS_ZWRITEENABLE, 0);
        self.illum_pass_state(services, 1);
        Self::kernel(services, context, args);
        services.set_render_state(RS_ZWRITEENABLE, saved_zwrite);
        services.set_render_state(RS_ZFUNC, saved_zfunc);
        services.set_render_state(RS_ALPHAFUNC, saved_func);
        services.set_render_state(RS_ALPHAREF, saved_ref);
        services.set_render_state(RS_ALPHATESTENABLE, saved_test);
        self.do_teardown(services, None);
        GR_OK
    }

    /// The detail and masked kinds: one pass, or two with the second
    /// alpha-tested on top.
    fn submit_two_pass<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        let saved_test = services.render_state(RS_ALPHATESTENABLE);
        let single = self.detail_pass_count == 1;
        services.set_render_state(RS_ALPHATESTENABLE, u32::from(!single));
        if single {
            self.family_pass_state(services, 0);
            Self::kernel(services, context, args);
            services.set_render_state(RS_ALPHATESTENABLE, saved_test);
            self.do_teardown(services, None);
            return GR_OK;
        }
        let saved_ref = services.render_state(RS_ALPHAREF);
        services.set_render_state(RS_ALPHAREF, 0xFE);
        let saved_func = services.render_state(RS_ALPHAFUNC);
        services.set_render_state(RS_ALPHAFUNC, 2);
        self.family_pass_state(services, 0);
        Self::kernel(services, context, args);
        services.set_render_state(RS_ALPHAREF, 1);
        services.set_render_state(RS_ALPHAFUNC, 5);
        let saved_zfunc = services.render_state(RS_ZFUNC);
        services.set_render_state(RS_ZFUNC, 4);
        self.family_pass_state(services, 1);
        Self::kernel(services, context, args);
        services.set_render_state(RS_ZFUNC, saved_zfunc);
        services.set_render_state(RS_ALPHAFUNC, saved_func);
        services.set_render_state(RS_ALPHAREF, saved_ref);
        services.set_render_state(RS_ALPHATESTENABLE, saved_test);
        self.do_teardown(services, None);
        GR_OK
    }

    fn do_submit<S: MaterialServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        match self.submit_kind() {
            Submit::Null => GR_OK,
            Submit::Atmosphere => self.atmosphere(services, context, args),
            Submit::LightMap => {
                let saved = services.render_state(RS_ZFUNC);
                services.set_render_state(RS_ZFUNC, 4);
                Self::kernel(services, context, args);
                services.set_render_state(RS_ZFUNC, saved);
                GR_OK
            }
            Submit::IllumDetail2Pass => self.submit_illum(services, context, args),
            Submit::MaskedDetail2Pass | Submit::Detail2Pass => {
                self.submit_two_pass(services, context, args)
            }
            Submit::BtDetailTwo => {
                let result = Self::kernel(services, context, args);
                match context.vertices {
                    Some(source) if result >= 0 => {
                        Self::back_faces(services, context, source, args, &BT_DETAIL_CULL_FLIP)
                    }
                    _ => result,
                }
            }
            Submit::DcDt => self.alpha_test_submit(services, context, args),
            Submit::DcDtTwo => {
                let result = self.alpha_test_submit(services, context, args);
                match context.vertices {
                    Some(source) if result >= 0 => {
                        Self::back_faces(services, context, source, args, &CULL_FLIP)
                    }
                    _ => result,
                }
            }
            Submit::Kernel => Self::kernel(services, context, args),
        }
    }

    /// `GetRenderFlags`.
    fn render_flag_bits(&self) -> u32 {
        let name = self.data.kind.name;
        let blend = self.data.fields.alpha_blend;
        match self.bind_kind() {
            Bind::Atmosphere => 0xA2,
            Bind::Stub => 0x48,
            Bind::Null => 8,
            _ if self.submit_kind() == Submit::DcDtTwo || name == "EcEtTwo" => {
                if blend {
                    3
                } else {
                    1
                }
            }
            Bind::DcDtEt => {
                if blend {
                    0x4A
                } else {
                    0x48
                }
            }
            Bind::Nebula => {
                if name == "NebulaTwo" {
                    3
                } else {
                    0xA
                }
            }
            Bind::BtDetail => {
                let base = if name == "BtDetailMapTwoMaterial" {
                    0x41
                } else {
                    0x48
                };
                base | if blend { 2 } else { 0 }
            }
            Bind::LightMap => 0xA,
            _ => {
                if blend {
                    0xA
                } else {
                    8
                }
            }
        }
    }
}

impl<S, A> Material<S> for ShadingMaterial<A>
where
    S: MaterialServices<Animation = A> + ?Sized,
    A: Copy + 'static,
{
    /// `Initialize` and `InitializeOwnedSchema`: one texture block per
    /// texture-name property, then every property's initial value stored.
    fn initialize(&mut self, services: &mut S) -> i32 {
        let _ = services;
        self.data = MaterialData::new(self.data.kind);
        self.textures = vec![TextureState::default(); self.data.fields.textures.len()];
        if self.data.kind.schema().iter().any(|p| p.name == b"Oc") {
            self.alpha_test = false;
        }
        GR_OK
    }

    /// `SetName`: a lower-case copy of at most 63 bytes.
    fn set_name(&mut self, name: &[u8]) -> i32 {
        let name = &name[..name.iter().position(|&b| b == 0).unwrap_or(name.len())];
        let keep = name.len().min(0x3F);
        self.name = Some(name[..keep].to_ascii_lowercase());
        GR_OK
    }

    /// `LoadFromFileSystem`: the schema's leaves from the open node
    /// (`neuron_core::material_data::Material::load`); storing `Oc` sets
    /// the alpha flags as at run time.
    fn load(&mut self, services: &mut S, fs: &mut dyn FileSystem) -> i32 {
        let oc = self.data.kind.schema().iter().position(|p| p.name == b"Oc");
        let mut stored_oc = false;
        self.data
            .load_with(&*fs, |index| stored_oc |= Some(index) == oc);
        if stored_oc {
            self.apply_alpha_flags(services);
        }
        GR_OK
    }

    fn apply_verify(&mut self, services: &mut S) -> i32 {
        if self.bind_kind() == Bind::Null {
            return GR_OK;
        }
        if self.single_pass() {
            return self.verify_animated(services);
        }
        match (self.submit_kind(), self.bind_kind()) {
            (Submit::Detail2Pass | Submit::MaskedDetail2Pass, _) => {
                self.verify_detail_family(services)
            }
            (Submit::IllumDetail2Pass, _) => self.verify_illum(services),
            (_, Bind::BtDetail) => self.verify_bt_detail(services),
            (_, Bind::DcDtEt) => self.verify_dc_dt_et(services),
            (_, Bind::DetailMap) => self.verify_detail_map(services),
            (_, Bind::Atmosphere) => self.verify_atmosphere(services),
            _ => {
                self.setup(services);
                self.do_bind(services, None);
                let result = services.verify_state();
                self.do_teardown(services, None);
                result
            }
        }
    }

    /// `ResolveTextures`: the current frame of every animated block; the
    /// atmosphere's readiness and `DcDtEt`'s second texture from the global
    /// records.
    fn resolve_textures(&mut self, services: &mut S, delta: f32) -> i32 {
        let _ = delta;
        for index in 0..self.textures.len() {
            let t = self.textures[index];
            if t.id == -1 {
                self.textures[index].frame.handle = 0;
            } else if !t.skip_resolve
                && let Some(frame) = services.texture_frame(t.id, -1)
            {
                self.textures[index].frame = frame;
            }
        }
        match self.bind_kind() {
            Bind::Atmosphere => self.atmosphere_ready = Self::global(services, 1),
            Bind::DcDtEt => self.secondary_enabled = Self::global(services, 2),
            _ => {}
        }
        GR_OK
    }

    fn render_flags(&mut self, services: &mut S) -> u32 {
        let _ = services;
        self.render_flag_bits()
    }

    fn submit(&mut self, services: &mut S, context: &SubmitContext<'_>, args: &DrawArgs) -> i32 {
        self.do_submit(services, context, args)
    }

    fn bind(&mut self, services: &mut S, context: Option<&SubmitContext<'_>>) -> i32 {
        self.do_bind(services, context)
    }

    fn teardown(&mut self, services: &mut S, context: Option<&SubmitContext<'_>>) -> i32 {
        self.do_teardown(services, context)
    }

    fn name(&self) -> Result<Vec<u8>, i32> {
        self.name.clone().ok_or(GR_GENERIC)
    }

    fn class_name(&self) -> &'static str {
        self.data.kind.class_name
    }

    fn pass_count(&self) -> Result<i32, i32> {
        Ok(
            if matches!(
                self.submit_kind(),
                Submit::Detail2Pass | Submit::IllumDetail2Pass | Submit::MaskedDetail2Pass
            ) {
                self.detail_pass_count.cast_signed()
            } else {
                1
            },
        )
    }

    fn parameter_count(&self) -> Result<u32, i32> {
        Ok(self.data.kind.schema().len() as u32)
    }

    fn parameter(&self, index: u32) -> Result<PropertyValue, i32> {
        self.data.parameter(index as usize).ok_or(GR_GENERIC)
    }

    /// `SetParameterValue`: checked against the schema, stored, then the
    /// alpha flags for `Oc` (with the texture's format once the material
    /// has one).
    fn set_parameter(&mut self, services: &mut S, index: u32, value: &PropertyValue) -> i32 {
        let index = index as usize;
        let Some(property) = self.data.kind.schema().get(index) else {
            return GR_GENERIC;
        };
        if !self.data.in_declared_range(index, value) {
            return GR_GENERIC;
        }
        self.data.store(index, value);
        if property.name == b"Oc" {
            self.apply_alpha_flags(services);
        }
        GR_OK
    }

    fn save(&mut self, fs: &dyn FileSystem) -> Result<Vec<LeafWrite>, i32> {
        Ok(self.data.save_properties(fs))
    }

    /// The destructor: the animation instance (single-pass kinds), then
    /// every block's playback.
    fn destroy(&mut self, services: &mut S) {
        if self.single_pass()
            && let Some(instance) = self.animation.take()
        {
            services.release_animation_instance(instance);
        }
        for index in 0..self.textures.len() {
            self.drop_texture(services, index);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vertex_format_sizes() {
        // Position, normal, one texture set.
        assert_eq!(fvf_stride(0x112), 32);
        // Position, diffuse, two sets.
        assert_eq!(fvf_stride(0x242), 12 + 4 + 16);
        // Pre-transformed with diffuse and specular.
        assert_eq!(fvf_stride(0x1C4), 16 + 8 + 8);
        // The helper reads XYZB5 (0xE) as 0x1C bytes and XYZB3 (0xA) as 0xC.
        assert_eq!(fvf_pos_size(0xE), 0x1C);
        assert_eq!(fvf_pos_size(0xA), 0xC);
        // Size bits: one three-component set, one single.
        assert_eq!(fvf_tex_size(0x200 | (1 << 16) | (3 << 18)), 0xC + 4);
    }

    #[test]
    fn alpha_formats() {
        assert_eq!(alpha_bits(6), 1);
        assert!(has_alpha(7) && has_alpha(12) && !has_alpha(3) && !has_alpha(99));
        assert_eq!(alpha_bits(99), 0);
    }
}
