//! `FMaterialImpl` (`CppSource/surfacematerials/MaterialImpl.cpp`): one
//! type for the 15 kinds of the 19 names `surfacematerials.dll`
//! registers, whose kind selects the schema, the field map, `Bind`,
//! `Teardown`, `Submit`, `ApplyVerify` and the render flags, as in the
//! reference.
//!
//! The 17 words of the material block (`D3DMATERIAL8`: diffuse, ambient,
//! specular, emissive, power) are kept as `f32` bit patterns; every
//! comparison and product the reference makes in `float` or `double` is
//! made in `f64` and rounded where the reference stores a `float`
//! (decision T11).
//!
//! Every renderer, texture-library, engine and candidate call goes through
//! [`SurfaceServices`] in the reference's order with its arguments. Where
//! the reference reads through null pointers (a kind without texture
//! blocks asking for the first block's handle, a submission without its
//! candidate, the engine `G_FlmEngineOuter` before `FLAppearance` set it)
//! the port reads zeros or skips the call (divergence D314).

use neuron_core::diag::{self, DumpFlags, Severity};
use neuron_core::hash::name_crc32;
use neuron_core::material_data::schema::{FLAG_ADDRESS_U, FLAG_ADDRESS_V, PropertyDef, value_type};
use neuron_core::material_data::{LeafWrite, PropertyValue};
use neuron_core::math::{Transform, Vector, inverse_rotate, mul_transform};
use neuron_core::num::{trunc_to_i32, trunc_to_i64};
use neuron_core::vfs::{FileSystem, FsDesc};

use super::super::batcher::{DrawArgs, SubmitContext, VertexSource, VertexStream};
use super::super::instance::Material;
use super::super::kinds::{
    TextureState, alpha_bits, draw_indices, fvf_color_size, fvf_normal_size, fvf_pos_size,
    fvf_stride, has_alpha, has_vertex_colours, put_bits, stream_f32, submit_kernel,
};
use super::super::{GR_GENERIC, GR_OK};
use super::schema;
use super::services::SurfaceServices;
use super::{SurfaceKind, SurfaceSpec};

const IMPL_FILE: &str = "IMaterialImpl.h";
const WIRE2_FILE: &str = "SurfaceMaterials\\WireMaterial2.cpp";
const SHADOWS_FILE: &str = "SurfaceMaterials\\shadows_inl.h";

// --- Direct3D 8 numbers -------------------------------------------------------------------------

const RS_ZENABLE: u32 = 0x07;
const RS_ZWRITEENABLE: u32 = 0x0E;
const RS_CULLMODE: u32 = 0x16;
const RS_ZFUNC: u32 = 0x17;
const RS_LIGHTING: u32 = 0x89;
const RS_AMBIENT: u32 = 0x8B;
const RS_COLORVERTEX: u32 = 0x8D;
const RS_LOCALVIEWER: u32 = 0x8E;
const RS_NORMALIZENORMALS: u32 = 0x8F;

const TSS_COLOROP: u32 = 0x01;
const TSS_COLORARG1: u32 = 0x02;
const TSS_COLORARG2: u32 = 0x03;
const TSS_ALPHAOP: u32 = 0x04;
const TSS_ALPHAARG1: u32 = 0x05;
const TSS_ALPHAARG2: u32 = 0x06;
const TSS_TEXCOORDINDEX: u32 = 0x0B;
const TSS_ADDRESSU: u32 = 0x0D;
const TSS_ADDRESSV: u32 = 0x0E;
const TSS_TEXTURETRANSFORMFLAGS: u32 = 0x18;

// --- The state tables (pairs of state and value) -----------------------------------------------

/// `kGlassRs`, `kHudIconRsOpaque`, `kCloakRsPlain`, `kEngineGlowRs`,
/// `kPlanetRsPlain`.
const RS_OPAQUE: &[(u32, u32)] = &[(0x89, 1), (0x1D, 0), (0x1B, 0)];
/// `kHudIconRsBlend`, `kCloakRsBlend`, `kExclusionRs`, `kPlanetRsBlend`,
/// `kEnvGlassDrawTailRs`.
const RS_BLEND: &[(u32, u32)] = &[(0x89, 1), (0x1D, 0), (0x1B, 1), (0x13, 5), (0x14, 6)];
/// `kVeilbornRs`, `kWireRsBlend`, `kWire2Rs`.
const RS_UNLIT_BLEND: &[(u32, u32)] = &[(0x89, 0), (0x1D, 0), (0x1B, 1), (0x13, 5), (0x14, 6)];
/// `kWireRsPlain`.
const RS_UNLIT_OPAQUE: &[(u32, u32)] = &[(0x89, 0), (0x1D, 0), (0x1B, 0)];

/// `kGlassTss0` and its copies: texture times diffuse.
const TSS_MODULATE: &[(u32, u32)] = &[(1, 4), (2, 2), (3, 0), (4, 4), (5, 2), (6, 0)];
/// `kGlassTss1` and its copies: the stage disabled.
const TSS_DISABLE: &[(u32, u32)] = &[(1, 1), (2, 2), (3, 1), (4, 1), (5, 2), (6, 1)];
/// `kEnvMapTss0`: texture times the stage before, doubled.
const TSS_MODULATE2X: &[(u32, u32)] = &[(1, 5), (2, 2), (3, 1), (4, 4), (5, 2), (6, 1)];
/// `kShadowTss1`: texture times the stage before.
const TSS_MODULATE_CURRENT: &[(u32, u32)] = &[(1, 4), (2, 2), (3, 1), (4, 4), (5, 2), (6, 1)];

const ONE: u32 = 0x3F80_0000;

/// `kGlassRaiseThreshold` (0.8f).
const GLASS_RAISE: f32 = 0.8;

fn float(bits: u32) -> f32 {
    f32::from_bits(bits)
}

fn wide(bits: u32) -> f64 {
    f64::from(f32::from_bits(bits))
}

/// `FlmWireColorByte`: the component times 255 truncated to 64 bits, the
/// low byte kept.
fn wire_color_byte(component: u32) -> u32 {
    (trunc_to_i64(wide(component) * 255.0).cast_unsigned() & 0xFF) as u32
}

/// `VeilbornLockFvf`.
fn veilborn_lock_fvf(stage: u32) -> u32 {
    match stage.cast_signed() {
        s if s > 1 => 0x242,
        s if s > 0 => 0x142,
        _ => 0x42,
    }
}

/// `VeilbornFadeFromValue`: one less the smooth step of `1 - value`.
fn veilborn_fade(value: f32) -> f64 {
    let wide_delta = 1.0 - f64::from(value);
    let delta = wide_delta as f32;
    let curve = if wide_delta < 0.0 {
        0.0
    } else if delta >= 1.0 {
        1.0
    } else {
        ((3.0 - 2.0 * f64::from(delta)) * f64::from(delta)) * f64::from(delta)
    };
    1.0 - curve
}

/// The words of a stream element at `byte` (zeros past the stream).
fn stream_word(stream: &VertexStream, byte: usize) -> u32 {
    stream_f32(stream, byte).to_bits()
}

fn read_leaf(fs: &dyn FileSystem, name: &[u8], len: Option<usize>) -> Option<Vec<u8>> {
    if name.is_empty() {
        return None;
    }
    let mut file = fs.open_file(&FsDesc::read(name)).ok()?;
    let len = match len {
        Some(len) => len,
        None => file.size().ok()? as usize,
    };
    let mut buf = vec![0u8; len];
    match file.read(&mut buf) {
        Ok(n) if n == len => Some(buf),
        _ => None,
    }
}

fn read_word(fs: &dyn FileSystem, name: &[u8]) -> Option<u32> {
    read_leaf(fs, name, Some(4)).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

/// The field index of an address-mode record: 1 (bits 2 and 3) for a
/// record flagged V and not U, else 0.
fn address_field(property: &PropertyDef) -> u32 {
    u32::from(property.flags & FLAG_ADDRESS_U == 0 && property.flags & FLAG_ADDRESS_V != 0)
}

/// Where a property's value lives (`GetParameterAddress`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Slot {
    Block(usize),
    TexName(usize),
    TexFlags(usize),
    TexU(usize),
    TexV(usize),
    VeilbornDiffuse,
    WireColor,
    ColorizeOptions,
}

/// A texture record of a surface material (`kFlmTextureVerifyEntrySize`
/// bytes): the name, the playback and frame, the wrap and address modes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SurfaceTexture {
    /// The texture name; `None` for a null name.
    pub name: Option<Vec<u8>>,
    /// The playback id, the frame and the single-frame flag.
    pub state: TextureState,
    /// The wrap mode (4 at first).
    pub flags: u32,
    /// The U address mode.
    pub address_u: u32,
    /// The V address mode.
    pub address_v: u32,
}

impl Default for SurfaceTexture {
    fn default() -> Self {
        SurfaceTexture {
            name: None,
            state: TextureState::default(),
            flags: 4,
            address_u: 0,
            address_v: 0,
        }
    }
}

/// The run-time state of a surface material, for tests and diagnostics.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SurfaceState {
    /// The material block (`+0x20`).
    pub block: [u32; 17],
    /// `Flag64_`: blended (opacity below 1, or a texture with more than one
    /// bit of alpha).
    pub flag64: bool,
    /// `Flag65_`: a texture with one bit of alpha.
    pub flag65: bool,
    /// The texture records.
    pub textures: Vec<SurfaceTexture>,
    /// An animation instance is held.
    pub animated: bool,
    /// `HudAnimFlag_`.
    pub hud_anim: bool,
    /// `bSecondTexture_`.
    pub second_texture: bool,
    /// `StageCounter_`.
    pub stage_counter: u32,
    /// `VeilbornDead68_` and `VeilbornLive6C_` as bits.
    pub veilborn_time: [u32; 2],
    /// `EnvMapFlag_`.
    pub env_map_glass: bool,
    /// `EnvMapResolvedHandle_`.
    pub env_map_handle: u32,
    /// `ShadowResolvedHandle_`.
    pub shadow_handle: u32,
    /// `ShadowLayerCount_`.
    pub shadow_layers: u32,
    /// `WireField78_` and `WireField7C_`.
    pub wire_colors: [u32; 2],
    /// `WireGroupIndex_`.
    pub wire_group: u32,
}

/// A material of one of the surface kinds. `A` is the animation instance
/// and `R` the texture record of the services it is used with.
#[derive(Clone, Debug)]
pub struct SurfaceMaterial<A, R> {
    spec: SurfaceSpec,
    block: [u32; 17],
    textures: Vec<SurfaceTexture>,
    flag64: bool,
    flag65: bool,
    backdrop: bool,
    veilborn_regular: bool,
    second_texture: bool,
    veilborn_dead: u32,
    veilborn_live: u32,
    veilborn_diffuse: [u32; 3],
    hud_anim_flag: bool,
    animation: Option<A>,
    env_map_glass: bool,
    env_map_handle: u32,
    env_record: Option<R>,
    env_crc: u32,
    shadow_record: Option<R>,
    shadow_handle: u32,
    shadow_layers: u32,
    shadow_initialized: bool,
    stage_counter: u32,
    wire_color: [u32; 4],
    wire_argb: u32,
    wire_reduced: u32,
    wire_group: u32,
    colorize_alpha: u32,
    colorize_options: u32,
    name: Option<Vec<u8>>,
}

impl<A: Copy, R: Copy> SurfaceMaterial<A, R> {
    /// `FlmCreateMaterial`, then `InitializeFromDescriptor` with the
    /// factory's name: the constructor's material block (white diffuse and
    /// ambient for the colorize, wire, shadow, engine-glow and HUD-animation
    /// kinds, else the three words of an identity diagonal), `Veilborn`'s
    /// regular flag (not for `VeilbornMaterialNoBendy`), `Backdrop`'s name
    /// flag and the wire kinds' white colour. The texture records come with
    /// [`Material::initialize`].
    #[must_use]
    pub fn new(spec: SurfaceSpec) -> SurfaceMaterial<A, R> {
        let mut block = [0u32; 17];
        if matches!(
            spec.kind,
            SurfaceKind::Colorize
                | SurfaceKind::Wire
                | SurfaceKind::Wire2
                | SurfaceKind::Shadow
                | SurfaceKind::EngineGlow
                | SurfaceKind::HudAnim
        ) {
            for word in &mut block[..8] {
                *word = ONE;
            }
        } else {
            block[0] = ONE;
            block[5] = ONE;
            block[10] = ONE;
        }
        let mut material = SurfaceMaterial {
            spec,
            block,
            textures: Vec::new(),
            flag64: false,
            flag65: false,
            backdrop: false,
            veilborn_regular: false,
            second_texture: false,
            veilborn_dead: 0,
            veilborn_live: 0,
            veilborn_diffuse: [0; 3],
            hud_anim_flag: false,
            animation: None,
            env_map_glass: false,
            env_map_handle: 0,
            env_record: None,
            env_crc: 0,
            shadow_record: None,
            shadow_handle: 0,
            shadow_layers: 0,
            shadow_initialized: false,
            stage_counter: 0,
            wire_color: [0; 4],
            wire_argb: 0,
            wire_reduced: 0,
            wire_group: 0,
            colorize_alpha: 0,
            colorize_options: 0,
            name: None,
        };
        match spec.kind {
            SurfaceKind::Veilborn => {
                material.veilborn_regular = spec.name != "VeilbornMaterialNoBendy";
                material.second_texture = true;
            }
            SurfaceKind::Backdrop => material.backdrop = spec.name == "BackdropMaterial",
            SurfaceKind::Wire | SurfaceKind::Wire2 => {
                material.wire_color = [ONE; 4];
                material.wire_argb = 0xFFFF_FFFF;
                if spec.kind == SurfaceKind::Wire2 {
                    material.wire_reduced = 0xFFFF_FFFF;
                }
            }
            _ => {}
        }
        material
    }

    /// The kind and the factory name.
    #[must_use]
    pub fn spec(&self) -> SurfaceSpec {
        self.spec
    }

    /// The run-time state.
    #[must_use]
    pub fn state(&self) -> SurfaceState {
        SurfaceState {
            block: self.block,
            flag64: self.flag64,
            flag65: self.flag65,
            textures: self.textures.clone(),
            animated: self.animation.is_some(),
            hud_anim: self.hud_anim_flag,
            second_texture: self.second_texture,
            stage_counter: self.stage_counter,
            veilborn_time: [self.veilborn_dead, self.veilborn_live],
            env_map_glass: self.env_map_glass,
            env_map_handle: self.env_map_handle,
            shadow_handle: self.shadow_handle,
            shadow_layers: self.shadow_layers,
            wire_colors: [self.wire_argb, self.wire_reduced],
            wire_group: self.wire_group,
        }
    }

    fn kind(&self) -> SurfaceKind {
        self.spec.kind
    }

    /// `GetParameterTable`.
    fn schema(&self) -> &'static [PropertyDef] {
        match self.kind() {
            SurfaceKind::Wire | SurfaceKind::Wire2 => &schema::WIRE,
            SurfaceKind::Colorize => &schema::COLORIZE,
            SurfaceKind::Cloak => &schema::COLORIZE[..10],
            SurfaceKind::Glass
            | SurfaceKind::HudIcon
            | SurfaceKind::Exclusion
            | SurfaceKind::PlanetWater
            | SurfaceKind::HudAnim
            | SurfaceKind::EngineGlow => schema::GLASS,
            SurfaceKind::Veilborn => &schema::VEILBORN,
            _ => &[],
        }
    }

    /// `GetParameterAddress`.
    fn slot(&self, index: u32) -> Option<Slot> {
        let tex = |block: usize, part: u32| match part {
            0 => Slot::TexName(block),
            1 => Slot::TexFlags(block),
            2 => Slot::TexU(block),
            _ => Slot::TexV(block),
        };
        let generic = |index: u32| match index {
            0 => Some(Slot::Block(4)),
            1 => Some(Slot::Block(0)),
            2 => Some(Slot::Block(12)),
            3 => Some(Slot::Block(8)),
            4 => Some(Slot::Block(16)),
            5 => Some(Slot::Block(3)),
            6..=9 => Some(tex(0, index - 6)),
            _ => None,
        };
        match self.kind() {
            SurfaceKind::Wire | SurfaceKind::Wire2 => (index == 0).then_some(Slot::WireColor),
            SurfaceKind::Colorize => {
                if index == 10 {
                    Some(Slot::ColorizeOptions)
                } else {
                    generic(index)
                }
            }
            SurfaceKind::Veilborn => match index {
                0 => Some(Slot::VeilbornDiffuse),
                1..=4 => Some(tex(0, index - 1)),
                5..=8 => Some(tex(1, index - 5)),
                _ => None,
            },
            SurfaceKind::Cloak
            | SurfaceKind::EngineGlow
            | SurfaceKind::Exclusion
            | SurfaceKind::Glass
            | SurfaceKind::HudIcon
            | SurfaceKind::PlanetWater
            | SurfaceKind::HudAnim => generic(index),
            _ => None,
        }
    }

    /// The words a slot holds from its first, `count` of them (zeros past
    /// the fields).
    fn read_words(&self, slot: Slot, count: usize) -> [u32; 4] {
        let mut out = [0u32; 4];
        let source: Vec<u32> = match slot {
            Slot::Block(at) => self.block[at..].to_vec(),
            Slot::VeilbornDiffuse => self.veilborn_diffuse.to_vec(),
            Slot::WireColor => self.wire_color.to_vec(),
            Slot::ColorizeOptions => vec![self.colorize_options],
            Slot::TexFlags(b) => self.textures.get(b).map(|t| t.flags).into_iter().collect(),
            Slot::TexU(b) => self
                .textures
                .get(b)
                .map(|t| t.address_u)
                .into_iter()
                .collect(),
            Slot::TexV(b) => self
                .textures
                .get(b)
                .map(|t| t.address_v)
                .into_iter()
                .collect(),
            Slot::TexName(_) => Vec::new(),
        };
        for (slot, word) in out.iter_mut().zip(source.into_iter().take(count)) {
            *slot = word;
        }
        out
    }

    /// The copy of `StoreParameterValue` into the property's field: by the
    /// value's type, one, three or four words, or the string.
    fn write_slot(&mut self, index: u32, value: &PropertyValue) {
        let Some(slot) = self.slot(index) else {
            return;
        };
        let count = match value.kind {
            value_type::VEC3 => 3,
            value_type::VEC4 => 4,
            value_type::WORD
            | value_type::FLOAT
            | value_type::WRAP_MODE
            | value_type::ADDRESS_MODE
            | 10 => 1,
            value_type::STRING | value_type::TEXTURE_NAME => {
                if let Slot::TexName(b) = slot
                    && let Some(t) = self.textures.get_mut(b)
                {
                    t.name.clone_from(&value.text);
                }
                return;
            }
            _ => return,
        };
        let words = &value.words[..count];
        let put = |slots: &mut [u32]| {
            for (slot, &word) in slots.iter_mut().zip(words) {
                *slot = word;
            }
        };
        match slot {
            Slot::Block(at) => put(&mut self.block[at..]),
            Slot::VeilbornDiffuse => put(&mut self.veilborn_diffuse),
            Slot::WireColor => put(&mut self.wire_color),
            Slot::ColorizeOptions => put(core::slice::from_mut(&mut self.colorize_options)),
            Slot::TexFlags(b) => {
                if let Some(t) = self.textures.get_mut(b) {
                    put(core::slice::from_mut(&mut t.flags));
                }
            }
            Slot::TexU(b) => {
                if let Some(t) = self.textures.get_mut(b) {
                    put(core::slice::from_mut(&mut t.address_u));
                }
            }
            Slot::TexV(b) => {
                if let Some(t) = self.textures.get_mut(b) {
                    put(core::slice::from_mut(&mut t.address_v));
                }
            }
            Slot::TexName(_) => {}
        }
    }

    /// The handle of texture record `block` (0 without one: divergence
    /// D314).
    fn handle(&self, block: usize) -> u32 {
        self.textures.get(block).map_or(0, |t| t.state.frame.handle)
    }

    fn modes(&self, block: usize) -> (u32, u32, u32) {
        self.textures
            .get(block)
            .map_or((4, 0, 0), |t| (t.flags, t.address_u, t.address_v))
    }

    fn instance_name(&self) -> String {
        self.name.as_deref().map_or_else(
            || "(null)".to_owned(),
            |n| String::from_utf8_lossy(n).into_owned(),
        )
    }

    // --- Shared pieces --------------------------------------------------------------------------

    fn apply_rs<S: SurfaceServices + ?Sized>(services: &mut S, table: &[(u32, u32)]) {
        for &(state, value) in table {
            services.set_render_state(state, value);
        }
    }

    fn apply_tss<S: SurfaceServices + ?Sized>(services: &mut S, stage: u32, table: &[(u32, u32)]) {
        for &(state, value) in table {
            services.set_stage_state(stage, state, value);
        }
    }

    /// `ApplyTssFromTextureBlock`: coordinate set and address modes.
    fn apply_block_tss<S: SurfaceServices + ?Sized>(
        &self,
        services: &mut S,
        stage: u32,
        block: usize,
    ) {
        let (flags, u, v) = self.modes(block);
        services.set_stage_state(stage, TSS_TEXCOORDINDEX, flags.saturating_sub(4));
        services.set_stage_state(stage, TSS_ADDRESSU, u.wrapping_add(1));
        services.set_stage_state(stage, TSS_ADDRESSV, v.wrapping_add(1));
    }

    /// The texture of record `block` on `stage` with its stage states.
    fn bind_block<S: SurfaceServices + ?Sized>(&self, services: &mut S, stage: u32, block: usize) {
        services.set_stage_texture(stage, self.handle(block));
        self.apply_block_tss(services, stage, block);
    }

    /// `RecomputeOpacityFlags`: blended for an opacity below 1, else from
    /// the first texture's format (one bit of alpha tests, more blends).
    fn recompute_opacity<S: SurfaceServices + ?Sized>(&mut self, services: &mut S) {
        self.flag64 = float(self.block[3]) < 1.0;
        self.flag65 = false;
        if self.flag64 {
            return;
        }
        let handle = self.handle(0);
        if handle == 0 {
            return;
        }
        let Ok(format) = services.texture_format(handle) else {
            return;
        };
        self.flag65 = alpha_bits(format) == 1;
        self.flag64 = has_alpha(format) && !self.flag65;
    }

    /// `StoreParameterValue`.
    fn store<S: SurfaceServices + ?Sized>(
        &mut self,
        services: &mut S,
        index: u32,
        value: &PropertyValue,
    ) {
        let tag = self
            .schema()
            .get(index as usize)
            .map_or(&b""[..], |p| p.name);
        match self.kind() {
            SurfaceKind::Colorize if index == 0 => {
                self.block[4..7].copy_from_slice(&value.words[..3]);
                self.block[7] = ONE;
                if value.kind == value_type::VEC4 {
                    self.colorize_alpha = value.words[3];
                }
                return;
            }
            SurfaceKind::Colorize if index == 10 => {
                self.colorize_options = value.words[0];
                return;
            }
            _ => {}
        }
        self.write_slot(index, value);
        if tag == b"Dc" {
            let diffuse: [u32; 4] = [self.block[0], self.block[1], self.block[2], self.block[3]];
            self.block[4..8].copy_from_slice(&diffuse);
            if !matches!(self.kind(), SurfaceKind::Wire | SurfaceKind::Wire2) {
                return;
            }
        } else if tag == b"Oc" {
            self.recompute_opacity(services);
        }
        if matches!(self.kind(), SurfaceKind::Wire | SurfaceKind::Wire2) && index == 0 {
            let c = value.words;
            if self.kind() == SurfaceKind::Wire {
                self.wire_color = c;
                let alpha = wire_color_byte(c[3]);
                let red = wire_color_byte(c[0]);
                let green = wire_color_byte(c[1]);
                let blue = wire_color_byte(c[2]);
                self.wire_argb = (alpha << 24) | (red << 16) | (green << 8) | blue;
                self.block[4] = c[0];
                self.block[5] = c[1];
                self.block[6] = c[2];
                self.block[3] = c[3];
            } else {
                let red = wire_color_byte(c[0]);
                let green = wire_color_byte(c[1]);
                let blue = wire_color_byte(c[2]);
                let alpha = wire_color_byte(c[3]);
                self.wire_argb = (alpha << 24) | (red << 16) | (green << 8) | blue;
                self.wire_reduced = ((red & 0xF8) << 13) | ((green & 0xF8) << 5) | (blue >> 3);
            }
            self.wire_group = 0;
        }
    }

    /// `Setup`: each texture record looked up by name, its playback made
    /// and, for a single frame, its frame read; the previous playback
    /// released; then the opacity flags.
    fn setup<S: SurfaceServices<TextureRecord = R> + ?Sized>(&mut self, services: &mut S) {
        for index in 0..self.textures.len() {
            let previous = self.textures[index].state.id;
            {
                let t = &mut self.textures[index].state;
                t.frame.handle = 0;
                t.id = -1;
                t.skip_resolve = true;
            }
            let mut verified = true;
            let name = self.textures[index].name.clone();
            if let Some(name) = &name {
                if let Some(record) = services.find_texture(name, name_crc32(name)) {
                    let frames = services.texture_frame_count(record);
                    let single = frames == Some(1);
                    self.textures[index].state.skip_resolve = single;
                    let played = services.create_playback(record);
                    self.textures[index].state.id = *played.as_ref().unwrap_or(&-1);
                    if let Ok(id) = played
                        && single
                        && let Some(frame) = services.texture_frame(id, 0)
                    {
                        self.textures[index].state.frame = frame;
                    }
                    services.release_texture_record(record);
                } else {
                    verified = name.as_slice() == b"(None)";
                }
            }
            if previous != -1 {
                services.dec_ref_texture(previous);
            }
            if !verified {
                let message = format!(
                    "texture verify failed: {}: {}",
                    self.instance_name(),
                    String::from_utf8_lossy(name.as_deref().unwrap_or_default()),
                );
                diag::dump(
                    DumpFlags::NOTICE,
                    &diag::general_line(IMPL_FILE, 0x23F, Severity::Notice, &message),
                );
            }
        }
        self.recompute_opacity(services);
    }

    /// `FlmRootInstanceValue`: the value `FLAppearance` keeps for the
    /// root of the context's instance, as a float; `default` without a
    /// context or a value.
    fn root_instance_value<S: SurfaceServices + ?Sized>(
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
        default: f32,
    ) -> f32 {
        let Some(context) = context else {
            return default;
        };
        let mut root = context.instance;
        if services.has_engine() {
            root = services.instance_root(context.instance);
        }
        services
            .instance_value(root as i32)
            .map_or(default, f32::from_bits)
    }

    // --- Candidate calls ------------------------------------------------------------------------

    fn candidate_bind<S: SurfaceServices + ?Sized>(
        services: &mut S,
        context: &SubmitContext<'_>,
    ) -> Option<i32> {
        let candidate = context.replaced?;
        let batched = services.candidate_is_batched(candidate);
        Some(services.candidate_bind(candidate, batched, context))
    }

    fn candidate_teardown<S: SurfaceServices + ?Sized>(
        services: &mut S,
        context: &SubmitContext<'_>,
    ) -> Option<i32> {
        let candidate = context.replaced?;
        let batched = services.candidate_is_batched(candidate);
        Some(services.candidate_teardown(candidate, batched, context))
    }

    fn candidate_submit<S: SurfaceServices + ?Sized>(
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> Option<i32> {
        let candidate = context.replaced?;
        let batched = services.candidate_is_batched(candidate);
        Some(services.candidate_submit(candidate, batched, context, args))
    }

    // --- Bind -----------------------------------------------------------------------------------

    fn bind_glass<S: SurfaceServices + ?Sized>(
        &mut self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) {
        if float(self.block[3]) < GLASS_RAISE {
            self.block[3] = GLASS_RAISE.to_bits();
            self.block[7] = GLASS_RAISE.to_bits();
        }
        services.set_material(&self.block);
        if has_vertex_colours(context) {
            services.set_render_state(RS_COLORVERTEX, 1);
        }
        self.bind_block(services, 0, 0);
        Self::apply_tss(services, 0, TSS_MODULATE);
        Self::apply_tss(services, 1, TSS_DISABLE);
        Self::apply_rs(services, RS_OPAQUE);
    }

    fn bind_veilborn<S: SurfaceServices + ?Sized>(&mut self, services: &mut S) {
        self.stage_counter = 0;
        Self::apply_rs(services, RS_UNLIT_BLEND);
        services.set_render_state(RS_COLORVERTEX, 1);
        self.bind_block(services, self.stage_counter, 0);
        Self::apply_tss(services, self.stage_counter, TSS_MODULATE);
        self.stage_counter += 1;
        if self.second_texture {
            let stage = self.stage_counter;
            self.bind_block(services, stage, 1);
            services.set_stage_state(stage, TSS_COLOROP, 7);
            services.set_stage_state(stage, TSS_COLORARG1, 2);
            services.set_stage_state(stage, TSS_COLORARG2, 1);
            services.set_stage_state(stage, TSS_ALPHAOP, 4);
            services.set_stage_state(stage, TSS_ALPHAARG1, 2);
            services.set_stage_state(stage, TSS_ALPHAARG2, 1);
            self.stage_counter += 1;
        }
        Self::apply_tss(services, self.stage_counter, TSS_DISABLE);
    }

    fn bind_hud_icon<S: SurfaceServices + ?Sized>(
        &mut self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) {
        self.bind_block(services, 0, 0);
        Self::apply_tss(services, 0, TSS_MODULATE);
        Self::apply_tss(services, 1, TSS_DISABLE);
        if let Some(context) = context
            && services.has_engine()
            && let Some(values) = services.instance_user_data(context.instance)
        {
            self.block[4..8].copy_from_slice(&values[..4]);
            self.block[3] = values[4];
        }
        services.set_material(&self.block);
        // Blended below 1 or with flag 64: a NaN opacity draws opaque.
        if wide(self.block[3]) < 1.0 || self.flag64 {
            Self::apply_rs(services, RS_BLEND);
        } else {
            Self::apply_rs(services, RS_OPAQUE);
        }
    }

    fn bind_hud_anim<S: SurfaceServices<Animation = A> + ?Sized>(
        &mut self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) {
        if !self.hud_anim_flag {
            return;
        }
        services.set_material(&self.block);
        if has_vertex_colours(context) {
            services.set_render_state(RS_COLORVERTEX, 1);
        }
        self.bind_block(services, 0, 0);
        Self::apply_tss(services, 0, TSS_MODULATE);
        Self::apply_tss(services, 1, TSS_DISABLE);
        Self::apply_rs(services, if self.flag64 { RS_BLEND } else { RS_OPAQUE });
        self.animated_transform(services);
    }

    /// The animation's UV matrix on stage 0 when it fills one.
    fn animated_transform<S: SurfaceServices<Animation = A> + ?Sized>(&self, services: &mut S) {
        if let Some(instance) = self.animation
            && let Some(matrix) = services.animation_uv_matrix(instance)
        {
            services.set_stage_state(0, TSS_TEXTURETRANSFORMFLAGS, 2);
            services.set_texture_transform(0, &matrix);
        }
    }

    fn bind_colorize<S: SurfaceServices + ?Sized>(
        &mut self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) -> i32 {
        let mut opacity = self.colorize_alpha;
        if self.colorize_options & 1 != 0 {
            self.bind_block(services, 0, 0);
            Self::apply_tss(services, 0, TSS_DISABLE);
            Self::apply_tss(services, 1, TSS_DISABLE);
        } else {
            let Some(context) = context else {
                return GR_GENERIC;
            };
            let Some(candidate) = context.replaced else {
                return GR_GENERIC;
            };
            let batched = services.candidate_is_batched(candidate);
            services.candidate_bind(candidate, batched, context);
            if let Ok(Some(tag)) = services.candidate_parameter_tag(candidate, 5)
                && tag.starts_with(b"Oc")
            {
                let candidate_opacity = services.candidate_parameter_word(candidate, 5);
                opacity = ((wide(candidate_opacity) * wide(opacity)) as f32).to_bits();
            }
        }
        self.block[3] = opacity;
        services.set_material(&self.block);
        Self::apply_rs(
            services,
            if wide(opacity) < 1.0 {
                RS_BLEND
            } else {
                RS_OPAQUE
            },
        );
        GR_OK
    }

    fn bind_cloak<S: SurfaceServices + ?Sized>(
        &mut self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) {
        let mut base = Self::root_instance_value(services, context, 1.0);
        if let Some(context) = context
            && Self::candidate_bind(services, context).is_some()
        {
            if let Some(block) = services.material() {
                self.block = block;
            }
            base = (f64::from(base) * wide(self.block[3])) as f32;
        } else {
            self.bind_block(services, 0, 0);
            Self::apply_tss(services, 0, TSS_DISABLE);
            Self::apply_tss(services, 1, TSS_DISABLE);
        }
        self.block[3] = base.to_bits();
        services.set_material(&self.block);
        if f64::from(base) < 1.0 {
            self.flag64 = true;
            Self::apply_rs(services, RS_BLEND);
        } else {
            self.flag64 = false;
            Self::apply_rs(services, RS_OPAQUE);
        }
    }

    fn bind_wire<S: SurfaceServices + ?Sized>(&self, services: &mut S) {
        services.set_material(&self.block);
        let table = if wide(self.wire_color[3]) < 0.99 {
            RS_UNLIT_BLEND
        } else {
            RS_UNLIT_OPAQUE
        };
        Self::apply_rs(services, table);
        services.set_stage_texture(0, 0);
        Self::apply_tss(services, 0, TSS_DISABLE);
    }

    fn bind_exclusion<S: SurfaceServices + ?Sized>(&self, services: &mut S) {
        self.bind_block(services, 0, 0);
        Self::apply_tss(services, 0, TSS_MODULATE);
        Self::apply_tss(services, 1, TSS_DISABLE);
        Self::apply_rs(services, RS_BLEND);
    }

    fn release_env_record<S: SurfaceServices<TextureRecord = R> + ?Sized>(
        &mut self,
        services: &mut S,
    ) {
        if let Some(record) = self.env_record.take() {
            services.release_texture_record(record);
        }
    }

    fn bind_env_map<S: SurfaceServices<TextureRecord = R> + ?Sized>(
        &mut self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) {
        self.env_map_glass = false;
        let mut name: Option<Vec<u8>> = None;
        if let Some(context) = context
            && let Some(candidate) = context.replaced
        {
            if services.candidate_is_batched(candidate) {
                services.candidate_bind(candidate, true, context);
            } else {
                let class = services.candidate_class_name(candidate);
                self.env_map_glass = name_crc32(&class) == name_crc32(b"GlassMaterial");
                services.candidate_bind(candidate, false, context);
            }
            if self.env_map_glass {
                name = Some(b"envmapglass".to_vec());
            } else {
                let mut root = context.instance;
                if services.has_engine() {
                    root = services.instance_root(root);
                }
                name = services.instance_value_name(root as i32);
            }
        }
        self.env_map_handle = 0;
        match &name {
            None => {
                self.release_env_record(services);
                self.env_crc = 0;
            }
            Some(name) => {
                let crc = name_crc32(name);
                if crc != self.env_crc {
                    self.release_env_record(services);
                    self.env_crc = crc;
                    if crc != 0 {
                        self.env_record = services.find_texture(name, name_crc32(name));
                    }
                }
            }
        }
        if let Some(record) = self.env_record
            && let Some(frame) = services.record_frame(record, 0)
        {
            self.env_map_handle = frame.handle;
        }
        if !self.env_map_glass && self.env_map_handle != 0 {
            let mut stage = 0;
            // `PipelineGetTextureStageState` writes 0 when the call fails.
            if services.stage_state(0, TSS_COLOROP).unwrap_or(0) != 1 {
                stage = 1;
                if services.stage_state(1, TSS_COLOROP).unwrap_or(0) != 1 {
                    return;
                }
            }
            self.env_map_stages(services, stage);
        }
    }

    /// The reflection texture on `stage`, the stage after it disabled.
    fn env_map_stages<S: SurfaceServices + ?Sized>(&self, services: &mut S, stage: u32) {
        Self::apply_tss(services, stage, TSS_MODULATE2X);
        Self::apply_tss(services, stage + 1, TSS_DISABLE);
        services.set_stage_texture(stage, self.env_map_handle);
        services.set_stage_state(stage, TSS_TEXCOORDINDEX, 0x30000);
        services.set_render_state(RS_LOCALVIEWER, 1);
        services.set_render_state(RS_NORMALIZENORMALS, 1);
    }

    fn bind_shadow<S: SurfaceServices<TextureRecord = R> + ?Sized>(
        &mut self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) -> i32 {
        if !self.shadow_initialized {
            self.shadow_initialized = true;
            if self.shadow_record.is_none() {
                self.shadow_record = services.find_texture(b"efxshadow", name_crc32(b"efxshadow"));
                match self.shadow_record {
                    None => diag::dump(
                        DumpFlags::WARNING,
                        &diag::general_line(
                            SHADOWS_FILE,
                            0x24,
                            Severity::Warning,
                            "couldnt find texture 'efxshadow' (from efx.txm?)",
                        ),
                    ),
                    Some(record) => {
                        if let Some(frame) = services.record_frame(record, 0) {
                            self.shadow_handle = frame.handle;
                        }
                    }
                }
            }
        }
        if let Some(context) = context
            && let Some(candidate) = context.replaced
        {
            if let Some(layers) = services.candidate_pass_count(candidate) {
                self.shadow_layers = layers.cast_unsigned();
            }
            let batched = services.candidate_is_batched(candidate);
            services.candidate_bind(candidate, batched, context);
        }
        GR_GENERIC
    }

    fn bind_engine_glow<S: SurfaceServices + ?Sized>(
        &self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) {
        services.set_material(&self.block);
        if has_vertex_colours(context) {
            services.set_render_state(RS_COLORVERTEX, 1);
        }
        Self::apply_tss(services, 0, TSS_DISABLE);
        Self::apply_rs(services, RS_OPAQUE);
        services.set_material(&self.block);
    }

    fn bind_planet_water<S: SurfaceServices<Animation = A> + ?Sized>(
        &self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) {
        services.set_material(&self.block);
        if has_vertex_colours(context) {
            services.set_render_state(RS_COLORVERTEX, 1);
        }
        self.bind_block(services, 0, 0);
        Self::apply_tss(services, 0, TSS_MODULATE);
        Self::apply_tss(services, 1, TSS_DISABLE);
        Self::apply_rs(services, if self.flag64 { RS_BLEND } else { RS_OPAQUE });
        self.animated_transform(services);
    }

    fn do_bind<S: SurfaceServices<Animation = A, TextureRecord = R> + ?Sized>(
        &mut self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) -> i32 {
        match self.kind() {
            SurfaceKind::Glass => self.bind_glass(services, context),
            SurfaceKind::Veilborn => self.bind_veilborn(services),
            SurfaceKind::Backdrop => {
                if let Some(context) = context {
                    Self::candidate_bind(services, context);
                }
                return GR_GENERIC;
            }
            SurfaceKind::HudIcon => self.bind_hud_icon(services, context),
            SurfaceKind::HudAnim => self.bind_hud_anim(services, context),
            SurfaceKind::Colorize => return self.bind_colorize(services, context),
            SurfaceKind::Cloak => self.bind_cloak(services, context),
            SurfaceKind::Wire => self.bind_wire(services),
            SurfaceKind::Wire2 => {
                services.set_material(&self.block);
                if has_vertex_colours(context) {
                    services.set_render_state(RS_COLORVERTEX, 1);
                }
            }
            SurfaceKind::Exclusion => self.bind_exclusion(services),
            SurfaceKind::EnvMap => self.bind_env_map(services, context),
            SurfaceKind::Shadow => return self.bind_shadow(services, context),
            SurfaceKind::EngineGlow => self.bind_engine_glow(services, context),
            SurfaceKind::VisibleCounter => {}
            SurfaceKind::PlanetWater => self.bind_planet_water(services, context),
        }
        GR_OK
    }

    fn do_teardown<S: SurfaceServices<Animation = A, TextureRecord = R> + ?Sized>(
        &mut self,
        services: &mut S,
        context: Option<&SubmitContext<'_>>,
    ) -> i32 {
        let kind = self.kind();
        match kind {
            // The reference reads the candidate without a test (D314).
            SurfaceKind::Backdrop => {
                return context
                    .and_then(|c| Self::candidate_teardown(services, c))
                    .unwrap_or(GR_GENERIC);
            }
            SurfaceKind::Shadow => {
                return context
                    .and_then(|c| Self::candidate_teardown(services, c))
                    .unwrap_or(GR_GENERIC);
            }
            SurfaceKind::Cloak | SurfaceKind::Colorize => {
                if let Some(context) = context {
                    Self::candidate_teardown(services, context);
                }
                return GR_OK;
            }
            SurfaceKind::EnvMap => {
                if self.env_record.is_some() {
                    services.set_render_state(RS_NORMALIZENORMALS, 0);
                    services.set_render_state(RS_LOCALVIEWER, 0);
                    services.set_stage_state(0, TSS_TEXCOORDINDEX, 0);
                    services.set_stage_state(1, TSS_TEXCOORDINDEX, 1);
                }
                if let Some(context) = context {
                    Self::candidate_teardown(services, context);
                }
                return GR_OK;
            }
            _ => {}
        }
        if kind == SurfaceKind::Veilborn {
            services.set_render_state(RS_COLORVERTEX, 0);
        }
        if kind == SurfaceKind::PlanetWater && self.animation.is_some() {
            services.set_stage_state(0, TSS_TEXTURETRANSFORMFLAGS, 0);
        }
        if kind == SurfaceKind::HudAnim && self.hud_anim_flag {
            if self.animation.is_some() {
                services.set_stage_state(0, TSS_TEXTURETRANSFORMFLAGS, 0);
            }
            if has_vertex_colours(context) {
                services.set_render_state(RS_COLORVERTEX, 0);
            }
        }
        if matches!(
            kind,
            SurfaceKind::EngineGlow
                | SurfaceKind::Exclusion
                | SurfaceKind::Glass
                | SurfaceKind::HudIcon
                | SurfaceKind::Wire
                | SurfaceKind::Wire2
                | SurfaceKind::PlanetWater
        ) && has_vertex_colours(context)
        {
            services.set_render_state(RS_COLORVERTEX, 0);
        }
        GR_OK
    }

    // --- ApplyVerify ----------------------------------------------------------------------------

    /// `ApplyInitialRenderStates` (`WireMaterial2` only).
    fn initial_render_states<S: SurfaceServices + ?Sized>(services: &mut S) {
        Self::apply_rs(services, RS_UNLIT_BLEND);
        services.set_stage_texture(0, 0);
        Self::apply_tss(services, 0, TSS_DISABLE);
    }

    fn verify<S: SurfaceServices<Animation = A, TextureRecord = R> + ?Sized>(
        &mut self,
        services: &mut S,
    ) -> i32 {
        match self.kind() {
            SurfaceKind::Cloak | SurfaceKind::Colorize => return GR_OK,
            SurfaceKind::Wire2 => {
                Self::initial_render_states(services);
                services.verify_state();
                let verify = services.verify_state();
                if verify != GR_OK {
                    let message = format!("failed to verify material {}", self.instance_name());
                    diag::dump(
                        DumpFlags::NOTICE,
                        &diag::general_line(WIRE2_FILE, 0x78, Severity::Notice, &message),
                    );
                }
                return verify;
            }
            SurfaceKind::PlanetWater => {
                if services.has_animation_library() {
                    let crc = self.name.as_deref().map_or(0, name_crc32);
                    if let Some(old) = self.animation.take() {
                        services.release_animation_instance(old);
                    }
                    self.animation = services.create_animation_instance(self.name.as_deref(), crc);
                }
                self.setup(services);
                self.do_bind(services, None);
                let verify = services.verify_state();
                self.do_teardown(services, None);
                return verify;
            }
            SurfaceKind::Veilborn => {
                self.setup(services);
                self.second_texture = true;
                let bound = self.do_bind(services, None);
                let mut result = bound;
                let mut failed = bound < 0;
                if !failed {
                    result = services.verify_state();
                    failed = result < 0;
                }
                self.second_texture = !failed;
                if failed {
                    self.do_bind(services, None);
                    result = services.verify_state();
                }
                self.do_teardown(services, None);
                return result;
            }
            SurfaceKind::EngineGlow => {
                self.block = [0; 17];
                self.block[3] = ONE;
                self.block[12] = 0.75f32.to_bits();
                self.block[13] = 0.7f32.to_bits();
                self.block[14] = 0.7f32.to_bits();
                self.block[15] = ONE;
            }
            _ => {}
        }
        self.setup(services);
        self.do_bind(services, None);
        let verify = services.verify_state();
        self.do_teardown(services, None);
        verify
    }

    // --- Submit ---------------------------------------------------------------------------------

    /// `SubmitVeilbornGeometry`: the source's positions and, for the
    /// generated stage, a coordinate from the light's angle with each
    /// normal, coloured by the fade of the instance's root; drawn without
    /// depth writes, culling clockwise.
    #[allow(clippy::too_many_lines)]
    fn submit_veilborn<S: SurfaceServices + ?Sized>(
        &self,
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        let has_engine = services.has_engine();
        let value = Self::root_instance_value(services, Some(context), 1.0);
        if has_engine {
            services.instance_relative(context.instance);
        }
        let fade = veilborn_fade(value);
        let stage = self.stage_counter;
        let (colour, phase) = if self.second_texture {
            let byte = trunc_to_i32(fade * 255.0).cast_unsigned();
            let live = wide(self.veilborn_live);
            let phase = (1.0 - (live % 5.0) / 5.0) as f32;
            ((byte << 24) | 0x00FF_FFFF, phase)
        } else {
            let byte = trunc_to_i32((fade * 0.5) * 255.0).cast_unsigned();
            let low = if stage.cast_signed() > 0 {
                0x000A_FFE6
            } else {
                0x000A_EAF5
            };
            ((byte << 24) | low, 1.0f32)
        };
        let Ok(mut lock) = services.lock_vertex_memory(veilborn_lock_fvf(stage), args.count) else {
            return GR_GENERIC;
        };
        let fvf = lock.listing.fvf;
        let stride = fvf_stride(fvf) as usize;
        let colour_offset = (fvf_pos_size(fvf) + fvf_normal_size(fvf)) as usize;
        let texture_offset = colour_offset + fvf_color_size(fvf) as usize;
        let world = Transform::from_words(*context.world);
        let view = Transform::from_words(*context.view);
        let light = inverse_rotate(&mul_transform(&view, &world), Vector::new(0.0, 0.0, -1.0));
        if let Some(source) = context.vertices {
            Self::veilborn_vertices(
                source,
                args,
                &mut lock.bytes,
                [stride, colour_offset, texture_offset],
                stage,
                colour,
                phase,
                light,
            );
        }
        let listing = lock.listing;
        services.unlock_vertex_memory(lock);
        let saved_zwrite = services.render_state(RS_ZWRITEENABLE);
        services.set_render_state(RS_ZWRITEENABLE, 0);
        let saved_cull = services.render_state(RS_CULLMODE);
        services.set_render_state(RS_CULLMODE, 1);
        let indices = draw_indices(context, args.ib_start, args.index_span);
        services.draw_indexed_vb(
            args.prim_type,
            listing.vertex_buffer,
            listing.base_vertex,
            args.count,
            &indices,
        );
        services.set_render_state(RS_CULLMODE, saved_cull);
        services.set_render_state(RS_ZWRITEENABLE, saved_zwrite);
        GR_OK
    }

    /// `VeilbornCopyMesh` and `VeilbornWriteVert`.
    #[allow(clippy::too_many_arguments)]
    fn veilborn_vertices(
        source: &VertexSource,
        args: &DrawArgs,
        bytes: &mut [u8],
        [stride, colour_offset, texture_offset]: [usize; 3],
        stage: u32,
        colour: u32,
        phase: f32,
        light: Vector,
    ) {
        let indexed = source.flags & 4 != 0;
        let start = args.start as usize;
        let source_stride = fvf_stride(source.fvf) as usize;
        let element = |stream: &VertexStream, i: usize, stream_stride: usize| -> usize {
            if indexed {
                let index = stream
                    .index
                    .as_deref()
                    .and_then(|x| x.get(start + i))
                    .copied()
                    .unwrap_or(0);
                index as usize * stream_stride
            } else {
                (start + i) * stream_stride
            }
        };
        let generated = stage.cast_signed();
        for i in 0..args.count as usize {
            let (pos_at, normal_at, uv_at) = if indexed {
                (
                    element(&source.position, i, source.position.stride as usize),
                    element(&source.normal, i, source.normal.stride as usize),
                    element(&source.tex[0], i, source.tex[0].stride as usize),
                )
            } else {
                (
                    element(&source.position, i, source_stride),
                    element(&source.normal, i, source_stride),
                    element(&source.tex[0], i, source_stride),
                )
            };
            let out = i * stride;
            for k in 0..3 {
                put_bits(
                    bytes,
                    out + 4 * k,
                    stream_word(&source.position, pos_at + 4 * k),
                );
            }
            let n = [
                f64::from(stream_f32(&source.normal, normal_at)),
                f64::from(stream_f32(&source.normal, normal_at + 4)),
                f64::from(stream_f32(&source.normal, normal_at + 8)),
            ];
            put_bits(bytes, out + colour_offset, colour);
            let zy = f64::from(light.z) * n[2] + f64::from(light.y) * n[1];
            let lit = ((zy + f64::from(light.x) * n[0]) + 1.0) * 0.5;
            if generated > 1 {
                for k in 0..2 {
                    put_bits(
                        bytes,
                        texture_offset + i * stride + 4 * k,
                        stream_word(&source.tex[0], uv_at + 4 * k),
                    );
                }
            }
            if generated > 0 {
                let lane = texture_offset + 8 * (generated as usize - 1) + i * stride;
                put_bits(bytes, lane, (lit as f32).to_bits());
                put_bits(bytes, lane + 4, phase.to_bits());
            }
        }
    }

    /// `SubmitGlassGeometry`: the back faces, then the front faces blended.
    fn submit_glass<S: SurfaceServices + ?Sized>(
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        services.set_render_state(RS_CULLMODE, 3);
        let first = submit_kernel(services, context, args);
        services.set_render_state(RS_CULLMODE, 2);
        if first < 0 {
            return first;
        }
        Self::apply_rs(services, RS_BLEND);
        submit_kernel(services, context, args)
    }

    /// `SubmitExclusionGeometry`: full ambient, the instance's colour and
    /// opacity from its user data.
    fn submit_exclusion<S: SurfaceServices + ?Sized>(
        &mut self,
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        let saved = services.render_state(RS_AMBIENT);
        services.set_render_state(RS_AMBIENT, 0xFFFF_FFFF);
        if services.has_engine()
            && let Some(values) = services.instance_user_data(context.instance)
        {
            self.block[3] = values[4];
            self.block[4..7].copy_from_slice(&values[..3]);
        }
        services.set_material(&self.block);
        submit_kernel(services, context, args);
        services.set_render_state(RS_AMBIENT, saved);
        GR_OK
    }

    /// `SubmitEnvMapGeometry`: the candidate alone, or the glass's two
    /// passes with the reflection on the second; the result is the count.
    fn submit_env_map<S: SurfaceServices + ?Sized>(
        &self,
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        let Some(candidate) = context.replaced else {
            return GR_GENERIC;
        };
        if !self.env_map_glass {
            return services.candidate_submit(candidate, false, context, args);
        }
        services.set_render_state(RS_CULLMODE, 3);
        let first = submit_kernel(services, context, args);
        services.set_render_state(RS_CULLMODE, 2);
        if first < 0 {
            return args.count.cast_signed();
        }
        if self.env_map_handle != 0 {
            let mut stage = 0;
            // A failed query keeps the seed of 1.
            let mut probe = services.stage_state(0, TSS_COLOROP).unwrap_or(1);
            if probe != 1 {
                stage = 1;
                probe = services.stage_state(1, TSS_COLOROP).unwrap_or(probe);
            }
            if probe == 1 {
                self.env_map_stages(services, stage);
            }
        }
        Self::apply_rs(services, RS_BLEND);
        submit_kernel(services, context, args);
        args.count.cast_signed()
    }

    /// `DrawShadow`: the projected `efxshadow` texture on the stage after
    /// the candidate's, drawn with the candidate or, for a candidate of more
    /// than one pass, again on top.
    fn submit_shadow<S: SurfaceServices + ?Sized>(
        &self,
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        let saved_zfunc = services.render_state(RS_ZFUNC);
        let stage = u32::from(self.shadow_layers < 2);
        services.set_stage_texture(stage, self.shadow_handle);
        if stage == 1 {
            Self::apply_tss(services, 1, TSS_MODULATE_CURRENT);
        }
        services.set_stage_state(stage, TSS_ADDRESSU, 3);
        services.set_stage_state(stage, TSS_ADDRESSV, 3);
        services.set_stage_state(stage, TSS_TEXCOORDINDEX, 0x20000);
        services.set_stage_state(stage, TSS_TEXTURETRANSFORMFLAGS, 0x103);
        let mut result = Self::candidate_submit(services, context, args).unwrap_or(GR_GENERIC);
        if self.shadow_layers > 1 {
            services.set_render_state(RS_LIGHTING, 0);
            services.set_render_state(RS_ZFUNC, 4);
            result = submit_kernel(services, context, args);
        }
        services.set_stage_state(stage, TSS_TEXCOORDINDEX, stage);
        services.set_stage_state(stage, TSS_TEXTURETRANSFORMFLAGS, 0);
        services.set_render_state(RS_ZFUNC, saved_zfunc);
        services.set_render_state(RS_LIGHTING, 1);
        result
    }

    /// `DrawBackdrop`: the candidate without depth test or writes.
    fn submit_backdrop<S: SurfaceServices + ?Sized>(
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        if context.replaced.is_none() {
            return GR_GENERIC;
        }
        let saved_z = services.render_state(RS_ZENABLE);
        services.set_render_state(RS_ZENABLE, 0);
        let saved_zwrite = services.render_state(RS_ZWRITEENABLE);
        services.set_render_state(RS_ZWRITEENABLE, 0);
        let saved_zfunc = services.render_state(RS_ZFUNC);
        services.set_render_state(RS_ZFUNC, 8);
        let result = Self::candidate_submit(services, context, args).unwrap_or(GR_GENERIC);
        services.set_render_state(RS_ZFUNC, saved_zfunc);
        services.set_render_state(RS_ZWRITEENABLE, saved_zwrite);
        services.set_render_state(RS_ZENABLE, saved_z);
        result
    }

    /// `SubmitPlanetWaterGeometry`: no depth writes, depth test less or
    /// equal.
    fn submit_planet_water<S: SurfaceServices + ?Sized>(
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        let saved_zwrite = services.render_state(RS_ZWRITEENABLE);
        services.set_render_state(RS_ZWRITEENABLE, 0);
        let saved_zfunc = services.render_state(RS_ZFUNC);
        services.set_render_state(RS_ZFUNC, 4);
        let result = submit_kernel(services, context, args);
        services.set_render_state(RS_ZFUNC, saved_zfunc);
        services.set_render_state(RS_ZWRITEENABLE, saved_zwrite);
        result
    }

    /// `SubmitWireGeometry` and `SubmitWire2Geometry`: the instance's wire
    /// drawn by the mesh's own wire (`WireMaterial`), else its edges.
    fn submit_wire<S: SurfaceServices + ?Sized>(
        &mut self,
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        if context.vertices.is_none()
            || args.count == 0
            || args.index_span == 0
            || !services.has_engine()
        {
            return GR_GENERIC;
        }
        let instance = context.instance;
        let wire2 = self.kind() == SurfaceKind::Wire2;
        if !wire2 && services.query_wire(instance) {
            if args.start == 0 {
                let saved_lighting = services.render_state(RS_LIGHTING);
                services.set_render_state(RS_LIGHTING, 1);
                let saved_ambient = services.render_state(RS_AMBIENT);
                services.set_render_state(RS_AMBIENT, self.wire_argb);
                services.wire_draw(instance);
                services.set_render_state(RS_AMBIENT, saved_ambient);
                services.set_render_state(RS_LIGHTING, saved_lighting);
            }
            return GR_OK;
        }
        if !services.query_mesh(instance) {
            return GR_GENERIC;
        }
        // `MeshSelectAndAdvanceCursor`: the current group, else group 0.
        if services.mesh_open_face_group(instance, self.wire_group.cast_signed(), 1) != 0 {
            self.wire_group = 0;
            if services.mesh_open_face_group(instance, 0, 1) < 0 {
                return GR_OK;
            }
        }
        self.wire_group = self.wire_group.wrapping_add(1);
        let colours = [self.wire_argb, if wire2 { self.wire_reduced } else { 0 }];
        Self::wire_edges(services, instance, colours, wire2);
        GR_OK
    }

    /// `WireDrawEdges`: two vertices per edge (a line list), or twelve
    /// (four triangles with the points shifted by the half-width).
    fn wire_edges<S: SurfaceServices + ?Sized>(
        services: &mut S,
        instance: usize,
        colours: [u32; 2],
        wire2: bool,
    ) {
        if services.mesh_open_channels(instance, 0x180) == -8 {
            services.mesh_close_face_group(instance);
            return;
        }
        services.mesh_lock_channels(instance, 0x180);
        let values = services.mesh_data(instance, 0x100).unwrap_or_default();
        let value_indices = services
            .mesh_indices(instance, 0x100)
            .indices
            .unwrap_or_default();
        let positions = services.mesh_data(instance, 0x80).unwrap_or_default();
        let position_indices = services.mesh_indices(instance, 0x80);
        let count = position_indices.count;
        let position_indices = position_indices.indices.unwrap_or_default();
        let reserve = if wire2 { count.wrapping_mul(6) } else { count };
        if let Ok(mut lock) = services.lock_vertex_memory(0x42, reserve) {
            let fvf = lock.listing.fvf;
            let stride = fvf_stride(fvf) as usize;
            let colour_offset = (fvf_pos_size(fvf) + fvf_normal_size(fvf)) as usize;
            let mut written = 0u32;
            let mut out = 0usize;
            let value_at = |i: usize| values.get(i).copied().unwrap_or(0);
            let position_index = |i: usize| position_indices.get(i).copied().unwrap_or(0);
            for edge in 0..(count >> 1) as usize {
                let value_index = value_indices.get(edge).copied().unwrap_or(0) as usize;
                // `<= 0.1` stops; a NaN goes on.
                if wide(value_at(value_index)) <= 0.1 {
                    break;
                }
                let mut emit = |endpoint: usize, colour: u32, offset: i8, bytes: &mut [u8]| {
                    let index = position_index(edge * 2 + endpoint) as usize;
                    let mut position = [
                        value_or_zero(&positions, index * 3),
                        value_or_zero(&positions, index * 3 + 1),
                        value_or_zero(&positions, index * 3 + 2),
                    ];
                    if offset != 0 {
                        let shift = f64::from(0.005f32);
                        for word in &mut position[..2] {
                            let shifted = if offset < 0 {
                                wide(*word) - shift
                            } else {
                                wide(*word) + shift
                            };
                            *word = (shifted as f32).to_bits();
                        }
                    }
                    for (k, word) in position.iter().enumerate() {
                        put_bits(bytes, out + 4 * k, *word);
                    }
                    put_bits(bytes, out + colour_offset, colour);
                    out += stride;
                    written += 1;
                };
                if wire2 {
                    const ENDPOINTS: [usize; 12] = [0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1];
                    const COLOURS: [usize; 12] = [0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 1];
                    const OFFSETS: [i8; 12] = [0, 0, -1, 0, -1, -1, 0, 0, 1, 0, 1, 1];
                    for slot in 0..12 {
                        emit(
                            ENDPOINTS[slot],
                            colours[COLOURS[slot]],
                            OFFSETS[slot],
                            &mut lock.bytes,
                        );
                    }
                } else {
                    emit(0, colours[0], 0, &mut lock.bytes);
                    emit(1, colours[0], 0, &mut lock.bytes);
                }
            }
            let listing = lock.listing;
            services.unlock_vertex_memory(lock);
            if written > 0 {
                services.draw_vb(
                    if wire2 { 4 } else { 2 },
                    listing.vertex_buffer,
                    listing.base_vertex,
                    written,
                );
            }
        }
        services.mesh_unlock_channels(instance);
        services.mesh_close_channels(instance);
        services.mesh_close_face_group(instance);
    }

    fn do_submit<S: SurfaceServices + ?Sized>(
        &mut self,
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        match self.kind() {
            SurfaceKind::VisibleCounter => {
                services.count_visible(args.index_span, args.count);
                GR_OK
            }
            SurfaceKind::Shadow => self.submit_shadow(services, context, args),
            SurfaceKind::Veilborn => self.submit_veilborn(services, context, args),
            SurfaceKind::HudAnim => {
                if !self.hud_anim_flag || args.flags & 0x100 == 0 {
                    GR_OK
                } else {
                    submit_kernel(services, context, args)
                }
            }
            SurfaceKind::PlanetWater => Self::submit_planet_water(services, context, args),
            SurfaceKind::Wire | SurfaceKind::Wire2 => self.submit_wire(services, context, args),
            SurfaceKind::Backdrop => Self::submit_backdrop(services, context, args),
            SurfaceKind::Cloak => {
                let Some(candidate) = context.replaced else {
                    return GR_OK;
                };
                let mut forwarded = *args;
                if self.flag64 {
                    forwarded.flags |= 0x400;
                }
                let batched = services.candidate_is_batched(candidate);
                services.candidate_submit(candidate, batched, context, &forwarded)
            }
            SurfaceKind::EngineGlow | SurfaceKind::Colorize | SurfaceKind::HudIcon => {
                submit_kernel(services, context, args)
            }
            SurfaceKind::EnvMap => self.submit_env_map(services, context, args),
            SurfaceKind::Exclusion => self.submit_exclusion(services, context, args),
            SurfaceKind::Glass => Self::submit_glass(services, context, args),
        }
    }

    /// `GetRenderFlags`.
    fn render_flag_bits(&self) -> u32 {
        match self.kind() {
            SurfaceKind::Colorize => {
                if self.flag64 || wide(self.colorize_alpha) < 1.0 {
                    0xA
                } else {
                    8
                }
            }
            SurfaceKind::Cloak => 0x2A,
            SurfaceKind::Wire | SurfaceKind::Wire2 => 0xA0,
            SurfaceKind::Veilborn => 0xA2,
            SurfaceKind::Exclusion | SurfaceKind::HudIcon if self.flag64 => 0x2A,
            SurfaceKind::Exclusion | SurfaceKind::HudIcon => 0x28,
            SurfaceKind::EngineGlow
            | SurfaceKind::EnvMap
            | SurfaceKind::VisibleCounter
            | SurfaceKind::HudAnim
                if self.flag64 =>
            {
                0xA
            }
            _ => 8,
        }
    }

    /// `FlmLoadDeclaredParameter` for every record: the leaf of the
    /// record's name read as its type and stored when the read succeeds.
    /// Floats and vectors are copied as they are; a texture name is the
    /// leaf up to its first NUL (the reference reads past a leaf without
    /// one); a wrap mode is bits 4 to 7 of a dword, an address mode bits 0
    /// and 1 or 2 and 3.
    fn load_from<S: SurfaceServices + ?Sized>(&mut self, services: &mut S, fs: &dyn FileSystem) {
        for (index, property) in self.schema().iter().enumerate() {
            if property.flags & 1 != 0 {
                continue;
            }
            let kind = property.current.kind;
            let mut value = PropertyValue {
                kind,
                words: [0; 4],
                text: None,
            };
            let loaded = match kind {
                value_type::ADDRESS_MODE => read_word(fs, property.name).map(|w| {
                    value.words[0] = (w >> (2 * address_field(property))) & 3;
                }),
                value_type::STRING | value_type::TEXTURE_NAME => read_leaf(fs, property.name, None)
                    .map(|leaf| {
                        let end = leaf.iter().position(|&b| b == 0).unwrap_or(leaf.len());
                        value.text = Some(leaf[..end].to_vec());
                    }),
                1..=8 if kind != value_type::COMPONENT => {
                    let count = match kind {
                        value_type::VEC3 => 3,
                        value_type::VEC4 => 4,
                        _ => 1,
                    };
                    read_leaf(fs, property.name, Some(count * 4)).map(|leaf| {
                        for (word, chunk) in value.words.iter_mut().zip(leaf.as_chunks::<4>().0) {
                            *word = u32::from_le_bytes(*chunk);
                        }
                        if kind == value_type::WRAP_MODE {
                            value.words[0] = (value.words[0] >> 4) & 0xF;
                        }
                    })
                }
                _ => Some(()),
            };
            if loaded.is_some() {
                self.store(services, index as u32, &value);
            }
        }
    }

    /// `FlmSaveParameterToFileSystem` for every record.
    fn save_to(&self, fs: &dyn FileSystem) -> Vec<LeafWrite> {
        let mut writes: Vec<LeafWrite> = Vec::new();
        for (index, property) in self.schema().iter().enumerate() {
            if property.flags & 1 != 0 || property.name.is_empty() {
                continue;
            }
            let Ok(value) = self.parameter_value(index as u32) else {
                continue;
            };
            let word = value.words[0];
            let bytes = match value.kind {
                value_type::WRAP_MODE | value_type::ADDRESS_MODE => {
                    let written = writes
                        .iter()
                        .rev()
                        .find(|w| w.name == property.name)
                        .map(|w| {
                            w.bytes
                                .get(..4)
                                .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
                        });
                    let mut packed = written
                        .unwrap_or_else(|| read_word(fs, property.name))
                        .unwrap_or(0);
                    if value.kind == value_type::WRAP_MODE {
                        packed ^= (packed ^ (word << 4)) & 0xF0;
                    } else {
                        if address_field(property) == 0 {
                            packed ^= (packed ^ word) & 3;
                        }
                        packed ^= (packed ^ (word << 2)) & 0xC;
                    }
                    packed.to_le_bytes().to_vec()
                }
                value_type::STRING | value_type::TEXTURE_NAME => match &value.text {
                    Some(text) => {
                        let mut bytes = text.clone();
                        bytes.push(0);
                        bytes
                    }
                    None => continue,
                },
                value_type::VEC3 => value.words[..3]
                    .iter()
                    .flat_map(|w| w.to_le_bytes())
                    .collect(),
                value_type::VEC4 => value.words.iter().flat_map(|w| w.to_le_bytes()).collect(),
                1 | 2 => word.to_le_bytes().to_vec(),
                _ => continue,
            };
            writes.push(LeafWrite {
                name: property.name.to_vec(),
                bytes,
            });
        }
        writes
    }

    /// `GetParameterValue`.
    fn parameter_value(&self, index: u32) -> Result<PropertyValue, i32> {
        let property = self.schema().get(index as usize).ok_or(GR_GENERIC)?;
        let slot = self.slot(index).ok_or(GR_GENERIC)?;
        let kind = property.current.kind;
        if let Slot::TexName(b) = slot {
            return Ok(PropertyValue {
                kind,
                words: [0; 4],
                text: self.textures.get(b).and_then(|t| t.name.clone()),
            });
        }
        let count = match kind {
            value_type::VEC3 => 3,
            value_type::VEC4 => 4,
            value_type::WORD
            | value_type::FLOAT
            | value_type::WRAP_MODE
            | value_type::ADDRESS_MODE
            | 10 => 1,
            _ => 0,
        };
        Ok(PropertyValue {
            kind,
            words: self.read_words(slot, count),
            text: None,
        })
    }

    /// `ValueWithinDeclaredRange`.
    fn in_declared_range(property: &PropertyDef, value: &PropertyValue) -> bool {
        if value.kind != property.current.kind {
            return false;
        }
        let count = match value.kind {
            value_type::FLOAT => 1,
            value_type::VEC3 => 3,
            value_type::VEC4 => 4,
            _ => return true,
        };
        let epsilon = 2f64.powi(-23);
        (0..count).all(|i| {
            let v = wide(value.words[i]);
            let above = v >= wide(property.maximum.words[i]) + epsilon;
            let below = v <= wide(property.minimum.words[i]) - epsilon;
            !above && !below
        })
    }
}

fn value_or_zero(words: &[u32], index: usize) -> u32 {
    words.get(index).copied().unwrap_or(0)
}

impl<S, A, R> Material<S> for SurfaceMaterial<A, R>
where
    S: SurfaceServices<Animation = A, TextureRecord = R> + ?Sized,
    A: Copy + 'static,
    R: Copy + 'static,
{
    /// `Initialize`: one texture record per texture-name property, then
    /// every property's initial value stored.
    fn initialize(&mut self, services: &mut S) -> i32 {
        let schema = self.schema();
        let count = schema
            .iter()
            .filter(|p| p.current.kind == value_type::TEXTURE_NAME)
            .count();
        self.textures = vec![SurfaceTexture::default(); count];
        for (index, property) in schema.iter().enumerate() {
            let value = PropertyValue {
                kind: property.current.kind,
                words: property.current.words,
                text: property.current.text.map(<[u8]>::to_vec),
            };
            self.store(services, index as u32, &value);
        }
        GR_OK
    }

    /// `SetName`: a lower-case copy of at most 63 bytes.
    fn set_name(&mut self, name: &[u8]) -> i32 {
        let name = &name[..name.iter().position(|&b| b == 0).unwrap_or(name.len())];
        self.name = Some(name[..name.len().min(0x3F)].to_ascii_lowercase());
        GR_OK
    }

    fn load(&mut self, services: &mut S, fs: &mut dyn FileSystem) -> i32 {
        self.load_from(services, &*fs);
        GR_OK
    }

    fn apply_verify(&mut self, services: &mut S) -> i32 {
        self.verify(services)
    }

    /// `ResolveTextures`: `Veilborn`'s clocks, the current frame of every
    /// animated texture and, for `HUDAnimMaterial`, global record 4 and the
    /// animation instance it enables.
    fn resolve_textures(&mut self, services: &mut S, delta: f32) -> i32 {
        if self.kind() == SurfaceKind::Veilborn {
            if self.veilborn_regular {
                self.veilborn_dead =
                    ((f64::from(delta) + wide(self.veilborn_dead)) as f32).to_bits();
            }
            self.veilborn_live = ((f64::from(delta) + wide(self.veilborn_live)) as f32).to_bits();
        }
        for index in 0..self.textures.len() {
            let t = self.textures[index].state;
            if t.id == -1 {
                self.textures[index].state.frame.handle = 0;
            } else if !t.skip_resolve
                && let Some(frame) = services.texture_frame(t.id, -1)
            {
                self.textures[index].state.frame = frame;
            }
        }
        if self.kind() == SurfaceKind::HudAnim {
            self.hud_anim_flag = services.global_record(4) >= 1;
            if services.has_animation_library() {
                if self.hud_anim_flag {
                    if self.animation.is_none() {
                        let crc = self.name.as_deref().map_or(0, name_crc32);
                        self.animation =
                            services.create_animation_instance(self.name.as_deref(), crc);
                    }
                } else if let Some(instance) = self.animation.take() {
                    services.release_animation_instance(instance);
                }
            }
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

    /// `ClassName`: the glass and veilborn kinds answer one name for their
    /// factories, `Backdrop` its own of two, the others their factory's.
    fn class_name(&self) -> &'static str {
        match self.kind() {
            SurfaceKind::Glass => "GlassMaterial",
            SurfaceKind::Veilborn => "VeilbornMaterial",
            SurfaceKind::PlanetWater => "PlanetWaterMaterial",
            SurfaceKind::Shadow => "ShadowMaterial",
            SurfaceKind::VisibleCounter => "VisibleCounterMaterial",
            SurfaceKind::Backdrop => {
                if self.backdrop {
                    "BackdropMaterial"
                } else {
                    "StarsphereMaterial"
                }
            }
            _ => self.spec.name,
        }
    }

    fn parameter_count(&self) -> Result<u32, i32> {
        Ok(self.schema().len() as u32)
    }

    fn parameter(&self, index: u32) -> Result<PropertyValue, i32> {
        self.parameter_value(index)
    }

    /// `SetParameterValue`: checked against the record, then stored.
    fn set_parameter(&mut self, services: &mut S, index: u32, value: &PropertyValue) -> i32 {
        let Some(property) = self.schema().get(index as usize) else {
            return GR_GENERIC;
        };
        if !Self::in_declared_range(property, value) {
            return GR_GENERIC;
        }
        self.store(services, index, value);
        GR_OK
    }

    fn save(&mut self, fs: &dyn FileSystem) -> Result<Vec<LeafWrite>, i32> {
        Ok(self.save_to(fs))
    }

    /// The destructor: the shadow's and the reflection's texture records,
    /// the animation instance, then every record's playback.
    fn destroy(&mut self, services: &mut S) {
        match self.kind() {
            SurfaceKind::Shadow => {
                if let Some(record) = self.shadow_record.take() {
                    services.release_texture_record(record);
                    self.shadow_handle = 0;
                }
            }
            SurfaceKind::EnvMap => {
                self.release_env_record(services);
                self.env_crc = 0;
            }
            _ => {
                if let Some(instance) = self.animation.take() {
                    services.release_animation_instance(instance);
                }
            }
        }
        for t in &mut self.textures {
            if t.state.id != -1 {
                services.dec_ref_texture(t.state.id);
                t.state.id = -1;
            }
        }
    }
}
