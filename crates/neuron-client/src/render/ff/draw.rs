//! What a draw takes from the model (record 0004, point 6): the
//! pipeline-state key, which holds only what Direct3D 12 bakes into a
//! pipeline state, and the per-draw constants, which hold everything the
//! fixed-function shaders branch on. The byte layout of the constant buffer
//! belongs to the shaders and is made from [`DrawConstants`] there.

use super::fvf::Fvf;
use super::geometry::Primitive;
use super::lights::{LightData, Lights, Material};
use super::model::StateModel;
use super::state::{
    Blend, BlendOp, Color, Compare, Cull, Fill, FogMode, MaterialSource, RenderState,
    RenderStateId, Shade, StageState, StageStateId, TexCoordIndex, TextureAddress, TextureArg,
    TextureFilter, TextureOp, TextureTransformFlags, VertexBlend, ZBuffer,
};
use super::transforms::{Matrix16, Transforms};

/// The texture stages the shaders emulate (record 0004, owner answer 6).
pub const SHADER_STAGES: usize = 2;

/// `D3D12_PRIMITIVE_TOPOLOGY_TYPE`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum TopologyType {
    Point,
    Line,
    Triangle,
}

impl From<Primitive> for TopologyType {
    fn from(p: Primitive) -> Self {
        match p {
            Primitive::PointList => TopologyType::Point,
            Primitive::LineList | Primitive::LineStrip => TopologyType::Line,
            Primitive::TriangleList | Primitive::TriangleStrip | Primitive::TriangleFan => {
                TopologyType::Triangle
            }
        }
    }
}

/// The key a pipeline state is created and cached by. Fields that a
/// disabled feature makes irrelevant are normalised, so states that draw
/// alike share a pipeline state.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PipelineKey {
    pub fvf: Fvf,
    pub topology: TopologyType,
    pub blend: Option<(Blend, Blend, BlendOp)>,
    /// `COLORWRITEENABLE`: red, green, blue, alpha in bits 0 to 3.
    pub color_write: u8,
    /// Depth function and write switch; `None` while `ZENABLE` is off.
    pub depth: Option<(Compare, bool)>,
    pub depth_bias: i32,
    pub cull: Cull,
    pub fill: Fill,
    /// `SHADEMODE` is `FLAT`: the shaders' flat variant, whose colours
    /// are not interpolated (the first vertex's, as in Direct3D 8).
    pub flat: bool,
}

macro_rules! read {
    ($model:expr, $variant:ident) => {
        match $model.render_state(RenderStateId::$variant) {
            RenderState::$variant(v) => v,
            _ => unreachable!("render_state returns the variant of its id"),
        }
    };
}

macro_rules! read_stage {
    ($model:expr, $stage:expr, $variant:ident) => {
        match $model.stage_state($stage, StageStateId::$variant) {
            StageState::$variant(v) => v,
            _ => unreachable!("stage_state returns the variant of its id"),
        }
    };
}

impl PipelineKey {
    #[must_use]
    pub fn new(model: &StateModel, fvf: Fvf, primitive: Primitive) -> PipelineKey {
        let blend = read!(model, AlphaBlendEnable).then(|| {
            (
                read!(model, SrcBlend),
                read!(model, DestBlend),
                read!(model, BlendOperation),
            )
        });
        let depth = match read!(model, ZEnable) {
            ZBuffer::Off => None,
            _ => Some((read!(model, ZFunc), read!(model, ZWriteEnable))),
        };
        PipelineKey {
            fvf,
            topology: primitive.into(),
            blend,
            color_write: (read!(model, ColorWriteEnable) & 0xf) as u8,
            depth,
            depth_bias: if depth.is_some() {
                read!(model, ZBias)
            } else {
                0
            },
            cull: read!(model, CullMode),
            fill: read!(model, FillMode),
            flat: read!(model, ShadeMode) == Shade::Flat,
        }
    }
}

/// One texture stage's combiner and coordinate state.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct StageOps {
    pub color_op: TextureOp,
    pub color_args: [TextureArg; 3],
    pub alpha_op: TextureOp,
    pub alpha_args: [TextureArg; 3],
    pub result: TextureArg,
    pub coord: TexCoordIndex,
    pub transform_flags: TextureTransformFlags,
    /// The stage's texture transform, when its flags enable it.
    pub transform: Option<Matrix16>,
    pub address_u: TextureAddress,
    pub address_v: TextureAddress,
    pub border: Color,
    pub mag_filter: TextureFilter,
    pub min_filter: TextureFilter,
    pub mip_filter: TextureFilter,
}

/// Fog as the shaders compute it.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Fog {
    /// Per-pixel (table) fog, if not `None`; else vertex fog.
    pub table: FogMode,
    pub vertex: FogMode,
    pub range: bool,
    pub start: f32,
    pub end: f32,
    pub density: f32,
    pub color: Color,
}

/// Lighting inputs.
#[derive(Clone, Debug, PartialEq)]
pub struct Lighting {
    pub ambient: Color,
    pub material: Material,
    pub diffuse_source: MaterialSource,
    pub specular_source: MaterialSource,
    pub ambient_source: MaterialSource,
    pub emissive_source: MaterialSource,
    pub specular: bool,
    pub local_viewer: bool,
    pub normalize_normals: bool,
    /// The enabled lights, at most sixteen, by Direct3D index.
    pub lights: Vec<(u32, LightData)>,
}

/// Everything the fixed-function shaders branch on for one draw.
#[derive(Clone, Debug, PartialEq)]
pub struct DrawConstants {
    pub world: Matrix16,
    /// World matrices 1 to 3 (`D3DTS_WORLDMATRIX(n)`, identity when never
    /// set), for vertex blending.
    pub blend_worlds: [Matrix16; 3],
    pub view: Matrix16,
    pub projection: Matrix16,
    /// Pre-transformed vertices (`XYZRHW`) skip the transforms and lighting.
    pub pretransformed: bool,
    /// `None` while `LIGHTING` is off or the vertices are pre-transformed.
    pub lighting: Option<Lighting>,
    pub color_vertex: bool,
    /// `SPECULARENABLE`: the specular colour is added after the stages,
    /// whether or not the vertices are lit.
    pub specular_enable: bool,
    pub vertex_blend: VertexBlend,
    pub indexed_vertex_blend: bool,
    pub fog: Option<Fog>,
    /// The alpha test function and reference; `None` while it is off.
    pub alpha_test: Option<(Compare, u8)>,
    pub texture_factor: Color,
    pub stages: [StageOps; SHADER_STAGES],
}

impl DrawConstants {
    #[must_use]
    pub fn new(
        model: &StateModel,
        transforms: &Transforms,
        lights: &Lights,
        fvf: Fvf,
    ) -> DrawConstants {
        let pretransformed = fvf.position() == super::fvf::Position::XyzRhw;
        let lighting = (read!(model, Lighting) && !pretransformed).then(|| Lighting {
            ambient: read!(model, Ambient),
            material: lights.material(),
            diffuse_source: read!(model, DiffuseMaterialSource),
            specular_source: read!(model, SpecularMaterialSource),
            ambient_source: read!(model, AmbientMaterialSource),
            emissive_source: read!(model, EmissiveMaterialSource),
            specular: read!(model, SpecularEnable),
            local_viewer: read!(model, LocalViewer),
            normalize_normals: read!(model, NormalizeNormals),
            lights: lights.draw_lights(),
        });
        let fog = read!(model, FogEnable).then(|| Fog {
            table: read!(model, FogTableMode),
            vertex: read!(model, FogVertexMode),
            range: read!(model, RangeFogEnable),
            start: read!(model, FogStart),
            end: read!(model, FogEnd),
            density: read!(model, FogDensity),
            color: read!(model, FogColor),
        });
        let alpha_test = read!(model, AlphaTestEnable).then(|| {
            (
                read!(model, AlphaFunc),
                (read!(model, AlphaRef) & 0xff) as u8,
            )
        });
        let stages = std::array::from_fn(|stage| {
            let transform_flags = read_stage!(model, stage, TransformFlags);
            StageOps {
                color_op: read_stage!(model, stage, ColorOp),
                color_args: [
                    read_stage!(model, stage, ColorArg0),
                    read_stage!(model, stage, ColorArg1),
                    read_stage!(model, stage, ColorArg2),
                ],
                alpha_op: read_stage!(model, stage, AlphaOp),
                alpha_args: [
                    read_stage!(model, stage, AlphaArg0),
                    read_stage!(model, stage, AlphaArg1),
                    read_stage!(model, stage, AlphaArg2),
                ],
                result: read_stage!(model, stage, ResultArg),
                coord: read_stage!(model, stage, TexCoordIdx),
                transform_flags,
                transform: (transform_flags.count != 0).then(|| *transforms.texture_matrix(stage)),
                address_u: read_stage!(model, stage, AddressU),
                address_v: read_stage!(model, stage, AddressV),
                border: read_stage!(model, stage, BorderColor),
                mag_filter: read_stage!(model, stage, MagFilter),
                min_filter: read_stage!(model, stage, MinFilter),
                mip_filter: read_stage!(model, stage, MipFilter),
            }
        });
        DrawConstants {
            world: *transforms.world_matrix(),
            blend_worlds: std::array::from_fn(|i| {
                transforms
                    .palette_matrix(i as u32 + 1)
                    .copied()
                    .unwrap_or(super::transforms::IDENTITY)
            }),
            view: *transforms.view_matrix(),
            projection: *transforms.projection_matrix(),
            pretransformed,
            lighting,
            color_vertex: read!(model, ColorVertex),
            specular_enable: read!(model, SpecularEnable),
            vertex_blend: read!(model, VertexBlendMode),
            indexed_vertex_blend: read!(model, IndexedVertexBlendEnable),
            fog,
            alpha_test,
            texture_factor: read!(model, TextureFactor),
            stages,
        }
    }
}

/// The render states the shaders and the pipeline state honour (record
/// 0004, point 6). `DITHERENABLE`, `CLIPPING`, `SOFTWAREVERTEXPROCESSING`,
/// `LASTPIXEL` and `MULTISAMPLEANTIALIAS` have no effect on Direct3D 12 and
/// are stored only.
pub const HONOURED: &[RenderStateId] = &[
    RenderStateId::ZEnable,
    RenderStateId::ZWriteEnable,
    RenderStateId::ZFunc,
    RenderStateId::ZBias,
    RenderStateId::FillMode,
    RenderStateId::CullMode,
    RenderStateId::AlphaBlendEnable,
    RenderStateId::SrcBlend,
    RenderStateId::DestBlend,
    RenderStateId::BlendOperation,
    RenderStateId::ColorWriteEnable,
    RenderStateId::AlphaTestEnable,
    RenderStateId::AlphaRef,
    RenderStateId::AlphaFunc,
    RenderStateId::Lighting,
    RenderStateId::Ambient,
    RenderStateId::ColorVertex,
    RenderStateId::SpecularEnable,
    RenderStateId::LocalViewer,
    RenderStateId::NormalizeNormals,
    RenderStateId::DiffuseMaterialSource,
    RenderStateId::SpecularMaterialSource,
    RenderStateId::AmbientMaterialSource,
    RenderStateId::EmissiveMaterialSource,
    RenderStateId::FogEnable,
    RenderStateId::FogColor,
    RenderStateId::FogTableMode,
    RenderStateId::FogVertexMode,
    RenderStateId::FogStart,
    RenderStateId::FogEnd,
    RenderStateId::FogDensity,
    RenderStateId::RangeFogEnable,
    RenderStateId::TextureFactor,
    RenderStateId::VertexBlendMode,
    RenderStateId::IndexedVertexBlendEnable,
    RenderStateId::ShadeMode,
];

/// States with no Direct3D 12 effect, stored only.
pub const NO_EFFECT: &[RenderStateId] = &[
    RenderStateId::DitherEnable,
    RenderStateId::Clipping,
    RenderStateId::SoftwareVertexProcessing,
    RenderStateId::LastPixel,
    RenderStateId::MultisampleAntialias,
];

/// The render states a draw relies on that the shaders do not emulate:
/// those outside [`HONOURED`] and [`NO_EFFECT`] whose value differs from
/// the default. The device logs each once (record 0004, point 3).
#[must_use]
pub fn unemulated_states(model: &StateModel) -> Vec<RenderStateId> {
    RenderStateId::ALL
        .iter()
        .copied()
        .filter(|id| !HONOURED.contains(id) && !NO_EFFECT.contains(id))
        .filter(|id| model.render_state_raw(id.raw()) != Ok(model.defaults().render_state(*id)))
        .collect()
}

/// Texture stages above [`SHADER_STAGES`] that a draw enables (a colour
/// operation other than `DISABLE`); the shaders do not emulate them.
#[must_use]
pub fn unemulated_stages(model: &StateModel) -> Vec<usize> {
    (SHADER_STAGES..super::state::STAGES)
        .filter(|&s| !matches!(read_stage!(model, s, ColorOp), TextureOp::Disable))
        .collect()
}

/// Emulated stages whose sampler a static sampler only approximates
/// (divergence D209): a non-default `MIPMAPLODBIAS`, `MAXMIPLEVEL` or
/// `MAXANISOTROPY`, or a `BORDER` address with a border colour other than
/// transparent black, opaque black and opaque white. The device logs each
/// stage once.
#[must_use]
pub fn approximated_samplers(model: &StateModel) -> Vec<usize> {
    (0..SHADER_STAGES)
        .filter(|&s| {
            let differs =
                |id: StageStateId| model.stage_state(s, id).to_raw() != id.builtin_default();
            let border_used = read_stage!(model, s, AddressU) == TextureAddress::Border
                || read_stage!(model, s, AddressV) == TextureAddress::Border;
            let border = read_stage!(model, s, BorderColor).0;
            differs(StageStateId::MipMapLodBias)
                || differs(StageStateId::MaxMipLevel)
                || differs(StageStateId::MaxAnisotropy)
                || (border_used && !matches!(border, 0 | 0xff00_0000 | 0xffff_ffff))
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::super::defaults::Defaults;
    use super::super::state::RawValue;
    use super::*;

    fn model() -> StateModel {
        StateModel::new(Defaults::builtin())
    }

    #[test]
    fn keys_normalise_disabled_features() {
        let mut m = model();
        let fvf = Fvf(0x152);
        let a = PipelineKey::new(&m, fvf, Primitive::TriangleFan);
        assert_eq!(a.topology, TopologyType::Triangle);
        assert_eq!(a.blend, None);
        assert_eq!(a.depth, Some((Compare::Less, true)));
        assert_eq!(a.cull, Cull::None);
        // Blend factors do not matter while blending is off.
        m.set_render_state(RenderState::SrcBlend(Blend::SrcAlpha));
        assert_eq!(PipelineKey::new(&m, fvf, Primitive::TriangleList), a);
        m.set_render_state(RenderState::AlphaBlendEnable(true));
        let b = PipelineKey::new(&m, fvf, Primitive::TriangleList);
        assert_eq!(b.blend, Some((Blend::SrcAlpha, Blend::Zero, BlendOp::Add)));
        // ZENABLE off drops the depth function, write and bias.
        m.set_render_state(RenderState::ZEnable(ZBuffer::Off));
        m.set_render_state(RenderState::ZBias(3));
        assert_eq!(PipelineKey::new(&m, fvf, Primitive::LineStrip).depth, None);
        assert_eq!(
            PipelineKey::new(&m, fvf, Primitive::LineStrip).depth_bias,
            0
        );
    }

    #[test]
    fn constants_follow_the_states() {
        let mut m = model();
        let t = Transforms::new(true);
        let mut l = Lights::new();
        l.add_light(1, LightData([0; 26]));
        l.set_light_enable(1, true);
        l.flush().unwrap();
        let c = DrawConstants::new(&m, &t, &l, Fvf(0x112));
        assert!(c.lighting.is_none());
        assert!(c.fog.is_none());
        assert!(c.alpha_test.is_none());
        assert_eq!(c.stages[0].color_op, TextureOp::Disable);
        m.set_render_state(RenderState::Lighting(true));
        m.set_render_state(RenderState::AlphaTestEnable(true));
        m.set_render_state_raw(RenderStateId::AlphaRef.raw(), 0x180)
            .unwrap();
        m.set_stage_state(0, StageState::ColorOp(TextureOp::Modulate));
        m.set_stage_state(
            0,
            StageState::TransformFlags(<TextureTransformFlags as RawValue>::from_raw(2)),
        );
        let c = DrawConstants::new(&m, &t, &l, Fvf(0x112));
        assert_eq!(c.lighting.as_ref().unwrap().lights.len(), 1);
        assert_eq!(c.alpha_test, Some((Compare::Always, 0x80)));
        assert_eq!(c.stages[0].color_op, TextureOp::Modulate);
        assert!(c.stages[0].transform.is_some());
        // Pre-transformed vertices are not lit.
        let c = DrawConstants::new(&m, &t, &l, Fvf(0x144));
        assert!(c.pretransformed && c.lighting.is_none());
    }

    #[test]
    fn unemulated_states_are_reported() {
        let mut m = model();
        assert!(unemulated_states(&m).is_empty());
        m.set_render_state(RenderState::StencilEnable(true));
        m.set_render_state(RenderState::DitherEnable(true));
        assert_eq!(unemulated_states(&m), vec![RenderStateId::StencilEnable]);
        assert!(unemulated_stages(&m).is_empty());
        m.set_stage_state(2, StageState::ColorOp(TextureOp::Modulate));
        assert_eq!(unemulated_stages(&m), vec![2]);
        assert!(approximated_samplers(&m).is_empty());
        // Border addressing with an opaque white border is exact; grey is
        // approximated, and so is an anisotropy other than the default.
        m.set_stage_state(0, StageState::AddressU(TextureAddress::Border));
        m.set_stage_state(0, StageState::BorderColor(Color(0xffff_ffff)));
        assert!(approximated_samplers(&m).is_empty());
        m.set_stage_state(0, StageState::BorderColor(Color(0xff80_8080)));
        assert_eq!(approximated_samplers(&m), vec![0]);
        m.set_stage_state(1, StageState::MaxAnisotropy(4));
        assert_eq!(approximated_samplers(&m), vec![0, 1]);
    }
}
