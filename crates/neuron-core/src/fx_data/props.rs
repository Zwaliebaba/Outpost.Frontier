//! The node classes and their property sets (`particlefx_genericfx.cpp`,
//! `particlefx_emitters.cpp`, `particlefx_fields.cpp`,
//! `particlefx_appearances.cpp`, the `*PropertySet` classes).
//!
//! Every node class has its own set: the base properties (class name,
//! instance name, life span, transform) and the class's own, each with its
//! construction default. The C++ reaches a property through the set's
//! accessor protocol, one getter or setter per kind keyed by the property
//! hash; a hash the class does not own answers -8. That protocol is
//! [`PropertySet`]'s methods here, with the same ownership per class (some
//! classes own hashes their row table does not list, and the collide field
//! owns two hashes per ramp). The row tables are the classes' property
//! lists as `EnumerateProps` walks them: the base rows, then the family's,
//! then the class's.

use super::ramp::{
    AnimatedSingle, AnimatedSingleRamp, AnimatedTransform, ColorRampRamp, NestedRamp, RampColor,
    RampSingle, RampSingleRamp, default_grey,
};
use crate::hash::name_crc32;

/// Property kinds, as the property stream and the row tables give them.
pub mod kind {
    /// A flag; its value is bit 15 of the stream's type word.
    pub const BOOL: u32 = 1;
    /// A 32-bit integer.
    pub const INT: u32 = 2;
    /// A float.
    pub const FLOAT: u32 = 3;
    /// A flat scalar ramp; no set class owns one.
    pub const RAMP_SINGLE: u32 = 0x100;
    /// A flat colour ramp; no set class owns one.
    pub const RAMP_COLOR: u32 = 0x101;
    /// A flat animated value; no set class owns one.
    pub const ANIMATED_SINGLE: u32 = 0x102;
    /// A string.
    pub const NAME: u32 = 0x103;
    /// Two 32-bit words.
    pub const PAIR: u32 = 0x104;
    /// An [`AnimatedTransform`](super::AnimatedTransform).
    pub const TRANSFORM: u32 = 0x105;
    /// A [`RampSingleRamp`](super::RampSingleRamp).
    pub const SINGLE_RAMP_RAMP: u32 = 0x200;
    /// A [`ColorRampRamp`](super::ColorRampRamp).
    pub const COLOR_RAMP_RAMP: u32 = 0x201;
    /// An [`AnimatedSingleRamp`](super::AnimatedSingleRamp).
    pub const ANIMATED_SINGLE_RAMP: u32 = 0x202;
}

/// `Node_ClassName`.
pub const NODE_CLASS_NAME: u32 = 0xF54E_F296;
/// `Node_Name`.
pub const NODE_NAME: u32 = 0xEFA8_CE01;
/// `Node_LifeSpan`.
pub const NODE_LIFE_SPAN: u32 = 0xF27F_DE7D;
/// `Node_Transform`.
pub const NODE_TRANSFORM: u32 = 0xE13A_59A1;
/// `Appearance_LODCurve`, owned by every appearance set.
pub const APPEARANCE_LOD_CURVE: u32 = 0x0C53_B008;
/// `Emitter_LODCurve`.
pub const EMITTER_LOD_CURVE: u32 = 0xFE07_7E05;
/// `Emitter_InitialParticles`.
pub const EMITTER_INITIAL_PARTICLES: u32 = 0x0F9A_9D52;
/// `BasicApp_TexName`: the billboard sets keep only the name's CRC.
pub const BILLBOARD_TEX_NAME: u32 = 0x1BA2_3359;
/// `BasicApp_BlendInfo`.
pub const BILLBOARD_BLEND_INFO: u32 = 0x1506_EB6C;
/// `BasicApp_Color`.
pub const BILLBOARD_COLOR: u32 = 0x0487_67E8;
/// `ParticleApp_LifeName`.
pub const PARTICLE_LIFE_NAME: u32 = 0x0EC7_A290;
/// `ParticleApp_DeathName`.
pub const PARTICLE_DEATH_NAME: u32 = 0x03B7_6E6C;
/// `ParticleApp_UseDynamicRotation` (flag bit 0).
pub const PARTICLE_USE_DYNAMIC_ROTATION: u32 = 0xE34C_3C55;
/// `ParticleApp_SmoothRotation` (flag bit 1).
pub const PARTICLE_SMOOTH_ROTATION: u32 = 0x049A_6DBE;
/// `MeshApp_MeshId`.
pub const MESH_ID: u32 = 0x0B7E_E41C;
/// `MeshApp_MeshName`.
pub const MESH_NAME: u32 = 0x18DA_DCA8;
/// `MeshApp_UseParticleTransform`.
pub const MESH_USE_PARTICLE_TRANSFORM: u32 = 0x001F_C112;
/// `MeshApp_ParticleTransform`; the oriented appearance's local transform
/// answers to it too.
pub const MESH_PARTICLE_TRANSFORM: u32 = 0xF000_4D58;
/// The beam appearance's local transform (no row lists it).
pub const BEAM_LOCAL_TRANSFORM: u32 = 0x0BA0_B3BB;

/// The 19 node classes the component factory creates by name.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum NodeClass {
    /// `FxNode`, a folder.
    Node,
    /// `FxSphereEmitter`.
    SphereEmitter,
    /// `FxConeEmitter`.
    ConeEmitter,
    /// `FxCubeEmitter`.
    CubeEmitter,
    /// `FxAirField`.
    AirField,
    /// `FxGravityField`.
    GravityField,
    /// `FxCollideField` (the stream's `FxColideField` is renamed to it).
    CollideField,
    /// `FxRadialField`.
    RadialField,
    /// `FxTurbulenceField`.
    TurbulenceField,
    /// `FLDustField`.
    DustField,
    /// `FLBeamField`, which has only the base properties.
    BeamField,
    /// `FxBasicAppearance`.
    BasicAppearance,
    /// `FxParticleAppearance`.
    ParticleAppearance,
    /// `FxRectAppearance`.
    RectAppearance,
    /// `FxPerpAppearance`.
    PerpAppearance,
    /// `FxMeshAppearance`.
    MeshAppearance,
    /// `FxOrientedAppearance`.
    OrientedAppearance,
    /// `FLDustAppearance`.
    DustAppearance,
    /// `FLBeamAppearance`.
    BeamAppearance,
}

impl NodeClass {
    /// Every class, in the factory's registration order.
    pub const ALL: [Self; 19] = [
        Self::Node,
        Self::SphereEmitter,
        Self::ConeEmitter,
        Self::CubeEmitter,
        Self::AirField,
        Self::GravityField,
        Self::CollideField,
        Self::RadialField,
        Self::TurbulenceField,
        Self::DustField,
        Self::BeamField,
        Self::BasicAppearance,
        Self::ParticleAppearance,
        Self::RectAppearance,
        Self::PerpAppearance,
        Self::MeshAppearance,
        Self::OrientedAppearance,
        Self::DustAppearance,
        Self::BeamAppearance,
    ];

    /// The component name.
    #[must_use]
    pub const fn factory_name(self) -> &'static [u8] {
        match self {
            Self::Node => b"FxNode",
            Self::SphereEmitter => b"FxSphereEmitter",
            Self::ConeEmitter => b"FxConeEmitter",
            Self::CubeEmitter => b"FxCubeEmitter",
            Self::AirField => b"FxAirField",
            Self::GravityField => b"FxGravityField",
            Self::CollideField => b"FxCollideField",
            Self::RadialField => b"FxRadialField",
            Self::TurbulenceField => b"FxTurbulenceField",
            Self::DustField => b"FLDustField",
            Self::BeamField => b"FLBeamField",
            Self::BasicAppearance => b"FxBasicAppearance",
            Self::ParticleAppearance => b"FxParticleAppearance",
            Self::RectAppearance => b"FxRectAppearance",
            Self::PerpAppearance => b"FxPerpAppearance",
            Self::MeshAppearance => b"FxMeshAppearance",
            Self::OrientedAppearance => b"FxOrientedAppearance",
            Self::DustAppearance => b"FLDustAppearance",
            Self::BeamAppearance => b"FLBeamAppearance",
        }
    }

    /// The class a component name creates. The factories compare names
    /// with `strcmp`, so the match is exact.
    #[must_use]
    pub fn from_factory_name(name: &[u8]) -> Option<Self> {
        Self::ALL.into_iter().find(|c| c.factory_name() == name)
    }

    /// The row tables `EnumerateProps` walks, in order.
    #[must_use]
    pub const fn row_tables(self) -> &'static [&'static [PropertyRow]] {
        match self {
            Self::Node | Self::BeamField => &[BASE_ROWS],
            Self::SphereEmitter => &[BASE_ROWS, EMITTER_ROWS, SPHERE_ROWS],
            Self::ConeEmitter => &[BASE_ROWS, EMITTER_ROWS, CONE_ROWS],
            Self::CubeEmitter => &[BASE_ROWS, EMITTER_ROWS, CUBE_ROWS],
            Self::AirField => &[BASE_ROWS, AIR_ROWS],
            Self::GravityField => &[BASE_ROWS, GRAVITY_ROWS],
            Self::CollideField => &[BASE_ROWS, COLLIDE_ROWS],
            Self::RadialField => &[BASE_ROWS, RADIAL_ROWS],
            Self::TurbulenceField => &[BASE_ROWS, TURBULENCE_ROWS],
            Self::DustField => &[BASE_ROWS, DUST_FIELD_ROWS],
            Self::BasicAppearance => &[BASE_ROWS, BASIC_ROWS],
            Self::ParticleAppearance => &[BASE_ROWS, PARTICLE_ROWS],
            Self::RectAppearance => &[BASE_ROWS, RECT_ROWS],
            Self::PerpAppearance => &[BASE_ROWS, PERP_ROWS],
            Self::MeshAppearance => &[BASE_ROWS, MESH_ROWS],
            Self::OrientedAppearance => &[BASE_ROWS, ORIENTED_ROWS],
            Self::DustAppearance => &[BASE_ROWS, DUST_ROWS],
            Self::BeamAppearance => &[BASE_ROWS, BEAM_ROWS],
        }
    }

    /// The rows `EnumerateProps` hands out, in order.
    pub fn rows(self) -> impl Iterator<Item = &'static PropertyRow> {
        self.row_tables().iter().flat_map(|table| table.iter())
    }
}

/// One row of a class's property table: name, hash, kind and the authoring
/// bounds (not defaults; nothing in loading reads them).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PropertyRow {
    /// The property name; its hash is [`crate::hash::exact_name_crc32`] of it.
    pub name: &'static str,
    /// The property hash.
    pub hash: u32,
    /// The kind ([`kind`]).
    pub kind: u32,
    /// The lower authoring bound.
    pub min: f32,
    /// The upper authoring bound.
    pub max: f32,
}

const fn row(name: &'static str, hash: u32, kind: u32, min: f32, max: f32) -> PropertyRow {
    PropertyRow {
        name,
        hash,
        kind,
        min,
        max,
    }
}

const MAX: f32 = f32::MAX;

/// The base rows (.data 0x062572b0).
pub const BASE_ROWS: &[PropertyRow] = &[
    row("Node_ClassName", NODE_CLASS_NAME, kind::NAME, 0.0, 0.0),
    row("Node_Name", NODE_NAME, kind::NAME, 0.0, 0.0),
    row("Node_LifeSpan", NODE_LIFE_SPAN, kind::FLOAT, 0.0, MAX),
    row("Node_Transform", NODE_TRANSFORM, kind::TRANSFORM, 0.0, 0.0),
];

const ANIM: u32 = kind::ANIMATED_SINGLE_RAMP;
const RAMP: u32 = kind::SINGLE_RAMP_RAMP;

/// The emitter rows.
pub const EMITTER_ROWS: &[PropertyRow] = &[
    row("Emitter_LODCurve", EMITTER_LOD_CURVE, RAMP, 0.0, 1.0),
    row(
        "Emitter_InitialParticles",
        EMITTER_INITIAL_PARTICLES,
        kind::INT,
        0.0,
        1000.0,
    ),
    row("Emitter_Frequency", 0x023C_350C, ANIM, 0.0, MAX),
    row("Emitter_MaxParticles", 0xF504_2852, ANIM, 0.0, MAX),
    row("Emitter_EmitCount", 0xE722_1F95, ANIM, 0.0, 5000.0),
    row("Emitter_InitLifeSpan", 0x0A63_5880, ANIM, 0.0, MAX),
    row("Emitter_Pressure", 0x0AB1_80C5, ANIM, -MAX, MAX),
    row("Emitter_VelocityApproach", 0x0681_233E, ANIM, 0.0, 1.0),
];
/// The cone emitter rows.
pub const CONE_ROWS: &[PropertyRow] = &[
    row("ConeEmitter_MinRadius", 0x1C73_D16B, ANIM, 0.0, MAX),
    row("ConeEmitter_MaxRadius", 0xF636_F07F, ANIM, 0.0, MAX),
    row("ConeEmitter_MinSpread", 0x100E_7F55, ANIM, 0.0, 90.0),
    row("ConeEmitter_MaxSpread", 0xFA4B_5E41, ANIM, 0.0, 90.0),
];
/// The cube emitter rows.
pub const CUBE_ROWS: &[PropertyRow] = &[
    row("CubeEmitter_Width", 0xFD22_A11C, ANIM, 0.0, MAX),
    row("CubeEmitter_Height", 0xEFB0_F8CF, ANIM, 0.0, MAX),
    row("CubeEmitter_Depth", 0x14B7_4EB2, ANIM, 0.0, MAX),
    row("CubeEmitter_MinSpread", 0xE282_006B, ANIM, 0.0, 90.0),
    row("CubeEmitter_MaxSpread", 0x08C7_217F, ANIM, 0.0, 90.0),
];
/// The sphere emitter rows.
pub const SPHERE_ROWS: &[PropertyRow] = &[
    row("SphereEmitter_MinRadius", 0xF231_FF60, ANIM, 0.0, MAX),
    row("SphereEmitter_MaxRadius", 0x1874_DE74, ANIM, 0.0, MAX),
];
/// The gravity field rows.
pub const GRAVITY_ROWS: &[PropertyRow] =
    &[row("GravityField_Gravity", 0xE02B_8BD4, ANIM, 0.0, MAX)];
/// The air field rows.
pub const AIR_ROWS: &[PropertyRow] = &[
    row("AirField_Magnitude", 0xE5E3_524C, ANIM, -MAX, MAX),
    row("AirField_Approach", 0x1042_3CEB, ANIM, 0.0, 2.0),
];
/// The radial field rows.
pub const RADIAL_ROWS: &[PropertyRow] = &[
    row("RadialField_Radius", 0xFF28_F620, ANIM, 0.0, MAX),
    row("RadialField_Attenuation", 0x1A43_3168, RAMP, -1.0, 1.0),
    row("RadialField_Magnitude", 0x0064_B3B9, ANIM, -MAX, MAX),
    row("RadialField_Approach", 0xF3CF_5EA5, ANIM, 0.0, 10.0),
];
/// The turbulence field rows.
pub const TURBULENCE_ROWS: &[PropertyRow] = &[
    row("TurbulenceField_Magnitude", 0x1CEA_C6D1, ANIM, -MAX, MAX),
    row("TurbulenceField_Approach", 0x1B97_0B9A, ANIM, 0.0, 2.0),
];
/// The collide field rows.
pub const COLLIDE_ROWS: &[PropertyRow] = &[
    row("CollideField_Reflectivity", 0xF246_8210, ANIM, 0.0, 1.0),
    row("CollideField_Width", 0x138B_DB51, ANIM, 0.0, MAX),
    row("CollideField_Height", 0x1133_6C98, ANIM, 0.0, MAX),
];
/// The dust field's row: the sphere emitter's name and hash.
pub const DUST_FIELD_ROWS: &[PropertyRow] = &[row(
    "SphereEmitter_MaxRadius",
    0x1874_DE74,
    ANIM,
    f32::from_bits(0x4270_6666),
    MAX,
)];

/// The basic appearance rows.
pub const BASIC_ROWS: &[PropertyRow] = &[
    row(
        "Appearance_LODCurve",
        APPEARANCE_LOD_CURVE,
        RAMP,
        0.0,
        100.0,
    ),
    row("BasicApp_TriTexture", 0xF863_872E, kind::BOOL, 0.0, 0.0),
    row("BasicApp_QuadTexture", 0xF87B_5FD5, kind::BOOL, 0.0, 0.0),
    row("BasicApp_MotionBlur", 0xE914_67F1, kind::BOOL, 0.0, 0.0),
    row(
        "BasicApp_Color",
        BILLBOARD_COLOR,
        kind::COLOR_RAMP_RAMP,
        0.0,
        1.0,
    ),
    row("BasicApp_Alpha", 0x04FC_9016, RAMP, 0.0, 1.0),
    row("BasicApp_Size", 0xF7C2_EBA9, RAMP, 0.0, MAX),
    row("BasicApp_HToVAspect", 0xE0E9_7650, RAMP, 0.0, MAX),
    row("BasicApp_Rotate", 0x05DB_630E, RAMP, -MAX, MAX),
    row("BasicApp_TexName", BILLBOARD_TEX_NAME, kind::NAME, 0.0, 0.0),
    row(
        "BasicApp_BlendInfo",
        BILLBOARD_BLEND_INFO,
        kind::PAIR,
        0.0,
        0.0,
    ),
    row(
        "BasicApp_UseCommonTexFrame",
        0xE2F6_0EEB,
        kind::BOOL,
        0.0,
        0.0,
    ),
    row("BasicApp_TexFrame", 0x015A_6F47, RAMP, 0.0, 1.0),
    row("BasicApp_CommonTexFrame", 0xF52B_8DD5, ANIM, 0.0, 1.0),
    row("BasicApp_FlipTexU", 0xF3D5_2EE4, kind::BOOL, 0.0, 0.0),
    row("BasicApp_FlipTexV", 0xE8DC_7F5E, kind::BOOL, 0.0, 0.0),
];
/// The rect appearance rows.
pub const RECT_ROWS: &[PropertyRow] = &[
    row(
        "Appearance_LODCurve",
        APPEARANCE_LOD_CURVE,
        RAMP,
        0.0,
        100.0,
    ),
    row("BasicApp_MotionBlur", 0xE914_67F1, kind::BOOL, 0.0, 0.0),
    row("RectApp_CenterOnPos", 0xFB79_58C1, kind::BOOL, 0.0, 0.0),
    row(
        "RectApp_ViewingAngleFade",
        0xF9FE_BF0D,
        kind::BOOL,
        0.0,
        0.0,
    ),
    row(
        "BasicApp_Color",
        BILLBOARD_COLOR,
        kind::COLOR_RAMP_RAMP,
        0.0,
        1.0,
    ),
    row("BasicApp_Alpha", 0x04FC_9016, RAMP, 0.0, 1.0),
    row("RectApp_Scale", 0xFBA2_03B8, RAMP, 0.0, MAX),
    row("RectApp_Length", 0xE48D_DAE2, RAMP, 0.0, MAX),
    row("RectApp_Width", 0x1266_735F, RAMP, 0.0, MAX),
    row("BasicApp_TexName", BILLBOARD_TEX_NAME, kind::NAME, 0.0, 0.0),
    row(
        "BasicApp_BlendInfo",
        BILLBOARD_BLEND_INFO,
        kind::PAIR,
        0.0,
        0.0,
    ),
    row(
        "BasicApp_UseCommonTexFrame",
        0xE2F6_0EEB,
        kind::BOOL,
        0.0,
        0.0,
    ),
    row("BasicApp_TexFrame", 0x015A_6F47, RAMP, 0.0, 1.0),
    row("BasicApp_CommonTexFrame", 0xF52B_8DD5, ANIM, 0.0, 1.0),
    row("BasicApp_FlipTexU", 0xF3D5_2EE4, kind::BOOL, 0.0, 0.0),
    row("BasicApp_FlipTexV", 0xE8DC_7F5E, kind::BOOL, 0.0, 0.0),
];
/// The perp appearance rows.
pub const PERP_ROWS: &[PropertyRow] = &[
    row(
        "Appearance_LODCurve",
        APPEARANCE_LOD_CURVE,
        RAMP,
        0.0,
        100.0,
    ),
    row(
        "RectApp_ViewingAngleFade",
        0xF9FE_BF0D,
        kind::BOOL,
        0.0,
        0.0,
    ),
    row(
        "BasicApp_Color",
        BILLBOARD_COLOR,
        kind::COLOR_RAMP_RAMP,
        0.0,
        1.0,
    ),
    row("BasicApp_Alpha", 0x04FC_9016, RAMP, 0.0, 1.0),
    row("BasicApp_Size", 0xF7C2_EBA9, RAMP, 0.0, MAX),
    row("BasicApp_TexName", BILLBOARD_TEX_NAME, kind::NAME, 0.0, 0.0),
    row(
        "BasicApp_BlendInfo",
        BILLBOARD_BLEND_INFO,
        kind::PAIR,
        0.0,
        0.0,
    ),
    row(
        "BasicApp_UseCommonTexFrame",
        0xE2F6_0EEB,
        kind::BOOL,
        0.0,
        0.0,
    ),
    row("BasicApp_TexFrame", 0x015A_6F47, RAMP, 0.0, 1.0),
    row("BasicApp_CommonTexFrame", 0xF52B_8DD5, ANIM, 0.0, 1.0),
    row("BasicApp_FlipTexU", 0xF3D5_2EE4, kind::BOOL, 0.0, 0.0),
    row("BasicApp_FlipTexV", 0xE8DC_7F5E, kind::BOOL, 0.0, 0.0),
];
/// The oriented appearance rows.
pub const ORIENTED_ROWS: &[PropertyRow] = &[
    row(
        "Appearance_LODCurve",
        APPEARANCE_LOD_CURVE,
        RAMP,
        0.0,
        100.0,
    ),
    row(
        "RectApp_ViewingAngleFade",
        0xF9FE_BF0D,
        kind::BOOL,
        0.0,
        0.0,
    ),
    row(
        "MeshApp_ParticleTransform",
        MESH_PARTICLE_TRANSFORM,
        kind::TRANSFORM,
        0.0,
        0.0,
    ),
    row(
        "BasicApp_Color",
        BILLBOARD_COLOR,
        kind::COLOR_RAMP_RAMP,
        0.0,
        1.0,
    ),
    row("BasicApp_Alpha", 0x04FC_9016, RAMP, 0.0, 1.0),
    row("RectApp_Scale", 0xFBA2_03B8, RAMP, 0.0, MAX),
    row("OrientedApp_Height", 0xF267_B8E1, RAMP, 0.0, MAX),
    row("OrientedApp_Width", 0xFA03_992F, RAMP, 0.0, MAX),
    row("BasicApp_TexName", BILLBOARD_TEX_NAME, kind::NAME, 0.0, 0.0),
    row(
        "BasicApp_BlendInfo",
        BILLBOARD_BLEND_INFO,
        kind::PAIR,
        0.0,
        0.0,
    ),
    row(
        "BasicApp_UseCommonTexFrame",
        0xE2F6_0EEB,
        kind::BOOL,
        0.0,
        0.0,
    ),
    row("BasicApp_TexFrame", 0x015A_6F47, RAMP, 0.0, 1.0),
    row("BasicApp_CommonTexFrame", 0xF52B_8DD5, ANIM, 0.0, 1.0),
    row("BasicApp_FlipTexU", 0xF3D5_2EE4, kind::BOOL, 0.0, 0.0),
    row("BasicApp_FlipTexV", 0xE8DC_7F5E, kind::BOOL, 0.0, 0.0),
];
/// The dust appearance rows.
pub const DUST_ROWS: &[PropertyRow] = &[
    row(
        "BasicApp_Color",
        BILLBOARD_COLOR,
        kind::COLOR_RAMP_RAMP,
        0.0,
        1.0,
    ),
    row("BasicApp_Alpha", 0x04FC_9016, RAMP, 0.0, 1.0),
    row("BasicApp_Size", 0xF7C2_EBA9, RAMP, 0.0, MAX),
    row("BasicApp_TexName", BILLBOARD_TEX_NAME, kind::NAME, 0.0, 0.0),
    row(
        "BasicApp_BlendInfo",
        BILLBOARD_BLEND_INFO,
        kind::PAIR,
        0.0,
        0.0,
    ),
    row(
        "BasicApp_UseCommonTexFrame",
        0xE2F6_0EEB,
        kind::BOOL,
        0.0,
        0.0,
    ),
    row("BasicApp_TexFrame", 0x015A_6F47, RAMP, 0.0, 1.0),
    row("BasicApp_CommonTexFrame", 0xF52B_8DD5, ANIM, 0.0, 1.0),
    row("BasicApp_FlipTexU", 0xF3D5_2EE4, kind::BOOL, 0.0, 0.0),
    row("BasicApp_FlipTexV", 0xE8DC_7F5E, kind::BOOL, 0.0, 0.0),
];
/// The beam appearance rows.
pub const BEAM_ROWS: &[PropertyRow] = &[
    row(
        "Appearance_LODCurve",
        APPEARANCE_LOD_CURVE,
        RAMP,
        0.0,
        100.0,
    ),
    row(
        "BasicApp_Color",
        BILLBOARD_COLOR,
        kind::COLOR_RAMP_RAMP,
        0.0,
        1.0,
    ),
    row("BasicApp_Alpha", 0x04FC_9016, RAMP, 0.0, 1.0),
    row("RectApp_Scale", 0xFBA2_03B8, RAMP, 0.0, 1000.0),
    row("RectApp_Width", 0x1266_735F, RAMP, 0.0, 1001.0),
    row("BasicApp_TexName", BILLBOARD_TEX_NAME, kind::NAME, 0.0, 0.0),
    row(
        "BasicApp_BlendInfo",
        BILLBOARD_BLEND_INFO,
        kind::PAIR,
        0.0,
        0.0,
    ),
    row("BasicApp_TexFrame", 0x015A_6F47, RAMP, 0.0, 1.0),
    row("BasicApp_QuadTexture", 0xF87B_5FD5, kind::BOOL, 0.0, 0.0),
    row(
        "BeamApp_DisablePlaceHolder",
        0x0850_8C35,
        kind::BOOL,
        0.0,
        0.0,
    ),
    row(
        "BeamApp_DupeFirstParticle",
        0xE8B8_CDE7,
        kind::BOOL,
        0.0,
        0.0,
    ),
    row("BeamApp_LineAppearance", 0x1C65_B7B9, kind::BOOL, 0.0, 0.0),
];
/// The particle appearance rows.
pub const PARTICLE_ROWS: &[PropertyRow] = &[
    row(
        "ParticleApp_LifeName",
        PARTICLE_LIFE_NAME,
        kind::NAME,
        0.0,
        0.0,
    ),
    row(
        "ParticleApp_DeathName",
        PARTICLE_DEATH_NAME,
        kind::NAME,
        0.0,
        0.0,
    ),
    row(
        "ParticleApp_UseDynamicRotation",
        PARTICLE_USE_DYNAMIC_ROTATION,
        kind::BOOL,
        0.0,
        0.0,
    ),
    row(
        "ParticleApp_SmoothRotation",
        PARTICLE_SMOOTH_ROTATION,
        kind::BOOL,
        0.0,
        0.0,
    ),
];
/// The mesh appearance rows.
pub const MESH_ROWS: &[PropertyRow] = &[
    row("MeshApp_MeshId", MESH_ID, kind::INT, 0.0, 0.0),
    row("MeshApp_MeshName", MESH_NAME, kind::NAME, 0.0, 0.0),
    row(
        "MeshApp_UseParticleTransform",
        MESH_USE_PARTICLE_TRANSFORM,
        kind::BOOL,
        0.0,
        0.0,
    ),
    row(
        "MeshApp_ParticleTransform",
        MESH_PARTICLE_TRANSFORM,
        kind::TRANSFORM,
        0.0,
        0.0,
    ),
];

/// Why an accessor refused a property.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PropertyError {
    /// The class does not own the hash for this kind (-8).
    Miss,
    /// `Node_ClassName` cannot be set (-1; the reference logs "You can't
    /// set class name!!").
    ReadOnly,
}

impl PropertyError {
    /// The reference's result code.
    #[must_use]
    pub const fn code(self) -> i32 {
        match self {
            Self::Miss => -8,
            Self::ReadOnly => -1,
        }
    }
}

/// The emitter shapes' own ramps (all kind 0x202).
#[derive(Clone, Debug, PartialEq)]
pub enum EmitterShape {
    /// `FxConeEmitter`; defaults 0, 1, 0 and 0 (spreads in degrees).
    Cone {
        /// `ConeEmitter_MinRadius`.
        min_radius: AnimatedSingleRamp,
        /// `ConeEmitter_MaxRadius`.
        max_radius: AnimatedSingleRamp,
        /// `ConeEmitter_MinSpread`.
        min_spread: AnimatedSingleRamp,
        /// `ConeEmitter_MaxSpread`.
        max_spread: AnimatedSingleRamp,
    },
    /// `FxCubeEmitter`; defaults 5, 5, 5, 0 and 15.
    Cube {
        /// `CubeEmitter_Width`.
        width: AnimatedSingleRamp,
        /// `CubeEmitter_Height`.
        height: AnimatedSingleRamp,
        /// `CubeEmitter_Depth`.
        depth: AnimatedSingleRamp,
        /// `CubeEmitter_MinSpread`.
        min_spread: AnimatedSingleRamp,
        /// `CubeEmitter_MaxSpread`.
        max_spread: AnimatedSingleRamp,
    },
    /// `FxSphereEmitter`; defaults 0 and 1.
    Sphere {
        /// `SphereEmitter_MinRadius`.
        min_radius: AnimatedSingleRamp,
        /// `SphereEmitter_MaxRadius`.
        max_radius: AnimatedSingleRamp,
    },
}

/// The emitter set (`FFxEmitterPropertySet` and its three shapes).
#[derive(Clone, Debug, PartialEq)]
pub struct Emitter {
    /// `Emitter_Frequency`, default 100.
    pub frequency: AnimatedSingleRamp,
    /// `Emitter_MaxParticles`, default 10000.
    pub max_particles: AnimatedSingleRamp,
    /// `Emitter_InitLifeSpan`, default 2.
    pub init_life_span: AnimatedSingleRamp,
    /// `Emitter_Pressure`, default 10.
    pub pressure: AnimatedSingleRamp,
    /// `Emitter_EmitCount`, default 0.
    pub emit_count: AnimatedSingleRamp,
    /// `Emitter_VelocityApproach`, default 0.
    pub velocity_approach: AnimatedSingleRamp,
    /// `Emitter_InitialParticles`, default 0.
    pub initial_particles: u32,
    /// `Emitter_LODCurve`, default 1.
    pub lod_curve: RampSingleRamp,
    /// The shape's ramps.
    pub shape: EmitterShape,
}

/// The field sets (all ramps kind 0x202 but the radial attenuation).
#[derive(Clone, Debug, PartialEq)]
pub enum Field {
    /// `FxGravityField`.
    Gravity {
        /// `GravityField_Gravity`, default 9.8.
        gravity: AnimatedSingleRamp,
    },
    /// `FxAirField`.
    Air {
        /// `AirField_Approach`, default 1.
        approach: AnimatedSingleRamp,
        /// `AirField_Magnitude`, default 1.
        magnitude: AnimatedSingleRamp,
    },
    /// `FxRadialField`.
    Radial {
        /// `RadialField_Magnitude`, default 10.
        magnitude: AnimatedSingleRamp,
        /// `RadialField_Approach`, default 0.3.
        approach: AnimatedSingleRamp,
        /// `RadialField_Radius`, default 50.
        radius: AnimatedSingleRamp,
        /// `RadialField_Attenuation` (kind 0x200): by default one ramp in
        /// mode 3 from 1 at time 0 to 0.001 at time 1.
        attenuation: RampSingleRamp,
    },
    /// `FxTurbulenceField`.
    Turbulence {
        /// `TurbulenceField_Approach`, default 1.
        approach: AnimatedSingleRamp,
        /// `TurbulenceField_Magnitude`, default 10.
        magnitude: AnimatedSingleRamp,
    },
    /// `FxCollideField`; each ramp also answers to a second hash
    /// (0xE462B789, 0x061AAF28, 0x16FF3474).
    Collide {
        /// `CollideField_Reflectivity`, default 0.7.
        reflectivity: AnimatedSingleRamp,
        /// `CollideField_Width`, default 10.
        width: AnimatedSingleRamp,
        /// `CollideField_Height`, default 10.
        height: AnimatedSingleRamp,
    },
    /// `FLDustField`.
    Dust {
        /// Under the hash of `SphereEmitter_MaxRadius`, default 60.1.
        radius: AnimatedSingleRamp,
    },
}

/// A billboard ramp slot: the reference stores either kind of nested
/// scalar ramp in the same fields.
#[derive(Clone, Debug, PartialEq)]
pub enum NestedSingle {
    /// A [`RampSingleRamp`] (handed out for kind 0x200).
    Ramp(RampSingleRamp),
    /// An [`AnimatedSingleRamp`] (handed out for kind 0x202).
    Animated(AnimatedSingleRamp),
}

/// The ramp slots of a billboard set, named as the reference's members.
/// Which property each slot holds depends on the class (see the tables in
/// [`Billboard`]).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BillboardSlot {
    /// `SizeRamp` (+0x70).
    Size,
    /// `PrimarySizeRamp` (+0x78).
    PrimarySize,
    /// `TexFrameRamp` (+0x7c).
    TexFrame,
    /// `AxisOverrideRamp` (+0x80).
    AxisOverride,
    /// `FrameRamp` (+0x84).
    Frame,
    /// `CommonTexFrameRamp` (+0x88).
    CommonTexFrame,
}

/// The six billboard appearance sets (`FAppearanceRampDescriptor`): basic,
/// rect, perp, oriented, dust and beam. The slots are the reference's
/// fields; each class maps its own hashes onto them.
#[derive(Clone, Debug, PartialEq)]
pub struct Billboard {
    /// `Appearance_LODCurve`, default 1.
    pub lod_curve: RampSingleRamp,
    /// The flag bits the bool properties map to.
    pub flags: u32,
    /// `BasicApp_TexName`: only `NEURONCOMPONENTS_CRC::GetCRC32` of the
    /// name is kept (its getter answers `"???"`).
    pub texture_crc: u32,
    /// `BasicApp_BlendInfo`, default `{5, 2}` (source alpha, one).
    pub blend_info: [u32; 2],
    /// `BasicApp_Color`, default grey.
    pub color: ColorRampRamp,
    /// `SizeRamp`.
    pub size: Option<NestedSingle>,
    /// `PrimarySizeRamp`.
    pub primary_size: Option<NestedSingle>,
    /// `TexFrameRamp`.
    pub tex_frame: Option<NestedSingle>,
    /// `AxisOverrideRamp`.
    pub axis_override: Option<NestedSingle>,
    /// `FrameRamp`.
    pub frame: Option<NestedSingle>,
    /// `CommonTexFrameRamp`.
    pub common_tex_frame: Option<NestedSingle>,
    /// `LocalTransform` (oriented and beam).
    pub local_transform: Option<AnimatedTransform>,
}

impl Billboard {
    /// The slot's value.
    #[must_use]
    pub const fn slot(&self, slot: BillboardSlot) -> Option<&NestedSingle> {
        match slot {
            BillboardSlot::Size => self.size.as_ref(),
            BillboardSlot::PrimarySize => self.primary_size.as_ref(),
            BillboardSlot::TexFrame => self.tex_frame.as_ref(),
            BillboardSlot::AxisOverride => self.axis_override.as_ref(),
            BillboardSlot::Frame => self.frame.as_ref(),
            BillboardSlot::CommonTexFrame => self.common_tex_frame.as_ref(),
        }
    }

    const fn slot_mut(&mut self, slot: BillboardSlot) -> Option<&mut NestedSingle> {
        match slot {
            BillboardSlot::Size => self.size.as_mut(),
            BillboardSlot::PrimarySize => self.primary_size.as_mut(),
            BillboardSlot::TexFrame => self.tex_frame.as_mut(),
            BillboardSlot::AxisOverride => self.axis_override.as_mut(),
            BillboardSlot::Frame => self.frame.as_mut(),
            BillboardSlot::CommonTexFrame => self.common_tex_frame.as_mut(),
        }
    }
}

/// What one billboard class knows about its properties
/// (`FBillboardPropertyClass`).
#[derive(Clone, Copy, Debug)]
pub struct BillboardTables {
    /// Bool hashes and their flag masks; first match wins. A mask of 0
    /// reads 0 and ignores a set.
    pub flags: &'static [(u32, u32)],
    /// Kind 0x200 hashes and their slots; first match wins, then
    /// `Appearance_LODCurve`.
    pub single_ramps: &'static [(u32, BillboardSlot)],
    /// Kind 0x202 hashes and their slots.
    pub animated_ramps: &'static [(u32, BillboardSlot)],
    /// The hash of the local transform, or 0.
    pub transform: u32,
}

use BillboardSlot::{AxisOverride, CommonTexFrame, Frame, PrimarySize, Size, TexFrame};

const BASIC_TABLES: BillboardTables = BillboardTables {
    flags: &[
        (0xF863_872E, 0x1),
        (0xF87B_5FD5, 0x2),
        (0xE914_67F1, 0x4),
        (0xE2F6_0EEB, 0x8),
        (0xF3D5_2EE4, 0x1000),
        (0xE8DC_7F5E, 0x2000),
    ],
    single_ramps: &[
        (0x04FC_9016, Size),
        (0xF7C2_EBA9, PrimarySize),
        (0x05DB_630E, TexFrame),
        (0x015A_6F47, AxisOverride),
        (0xE0E9_7650, CommonTexFrame),
    ],
    animated_ramps: &[(0xF52B_8DD5, Frame)],
    transform: 0,
};
const RECT_SINGLE: &[(u32, BillboardSlot)] = &[
    (0x04FC_9016, Size),
    (0xFBA2_03B8, PrimarySize),
    (0xE48D_DAE2, TexFrame),
    (0x1266_735F, AxisOverride),
    (0x015A_6F47, Frame),
];
const RECT_TABLES: BillboardTables = BillboardTables {
    flags: &[
        (0xFB79_58C1, 0x1),
        (0xE914_67F1, 0x4),
        (0xF9FE_BF0D, 0x10),
        (0xE2F6_0EEB, 0x8),
        (0xF3D5_2EE4, 0x1000),
        (0xE8DC_7F5E, 0x2000),
    ],
    single_ramps: RECT_SINGLE,
    animated_ramps: &[(0xF52B_8DD5, CommonTexFrame)],
    transform: 0,
};
const PERP_SINGLE: &[(u32, BillboardSlot)] = &[
    (0x04FC_9016, Size),
    (0xF7C2_EBA9, PrimarySize),
    (0x015A_6F47, TexFrame),
];
const PERP_TABLES: BillboardTables = BillboardTables {
    flags: &[
        (0xF9FE_BF0D, 0x10),
        (0xE2F6_0EEB, 0x8),
        (0xF3D5_2EE4, 0x1000),
        (0xE8DC_7F5E, 0x2000),
    ],
    single_ramps: PERP_SINGLE,
    animated_ramps: &[(0xF52B_8DD5, AxisOverride)],
    transform: 0,
};
const ORIENTED_TABLES: BillboardTables = BillboardTables {
    flags: &[
        (0xF9FE_BF0D, 0x40),
        (0xE2F6_0EEB, 0x8),
        (0xF3D5_2EE4, 0x1000),
        (0xE8DC_7F5E, 0x2000),
    ],
    single_ramps: &[
        (0x04FC_9016, Size),
        (0xFBA2_03B8, PrimarySize),
        (0xF267_B8E1, TexFrame),
        (0xFA03_992F, AxisOverride),
        (0x015A_6F47, Frame),
    ],
    animated_ramps: &[(0xF52B_8DD5, CommonTexFrame)],
    transform: MESH_PARTICLE_TRANSFORM,
};
const DUST_TABLES: BillboardTables = BillboardTables {
    flags: &[
        (0xE2F6_0EEB, 0x8),
        (0xF3D5_2EE4, 0x1000),
        (0xE8DC_7F5E, 0x2000),
    ],
    single_ramps: PERP_SINGLE,
    animated_ramps: &[(0xF52B_8DD5, AxisOverride)],
    transform: 0,
};
const BEAM_TABLES: BillboardTables = BillboardTables {
    flags: &[
        (0x0850_8C35, 0x200),
        (0xE8B8_CDE7, 0x100),
        (0x1C65_B7B9, 0x400),
        (0xF87B_5FD5, 0x2),
        (0x0350_3B61, 0x0),
        (0xE2F6_0EEB, 0x0),
        (0x0ABE_0402, 0x0),
    ],
    single_ramps: &[
        (0x04FC_9016, Size),
        (0xFBA2_03B8, PrimarySize),
        (0xE48D_DAE2, CommonTexFrame),
        (0x1266_735F, TexFrame),
        (0x015A_6F47, Frame),
    ],
    animated_ramps: &[(0xF52B_8DD5, AxisOverride)],
    transform: BEAM_LOCAL_TRANSFORM,
};

/// The billboard tables of a class, if it is one of the six.
#[must_use]
pub const fn billboard_tables(class: NodeClass) -> Option<&'static BillboardTables> {
    match class {
        NodeClass::BasicAppearance => Some(&BASIC_TABLES),
        NodeClass::RectAppearance => Some(&RECT_TABLES),
        NodeClass::PerpAppearance => Some(&PERP_TABLES),
        NodeClass::OrientedAppearance => Some(&ORIENTED_TABLES),
        NodeClass::DustAppearance => Some(&DUST_TABLES),
        NodeClass::BeamAppearance => Some(&BEAM_TABLES),
        _ => None,
    }
}

/// The particle appearance set.
#[derive(Clone, Debug, PartialEq)]
pub struct ParticleAppearance {
    /// `Appearance_LODCurve` (owned, though no row lists it), default 1.
    pub lod_curve: RampSingleRamp,
    /// `ParticleApp_LifeName`, the effect spawned along a particle's life;
    /// default `""`.
    pub life_name: Option<Vec<u8>>,
    /// `ParticleApp_DeathName`, the effect spawned at its death; default `""`.
    pub death_name: Option<Vec<u8>>,
    /// Bit 0 `ParticleApp_UseDynamicRotation`, bit 1
    /// `ParticleApp_SmoothRotation`.
    pub flags: u32,
}

/// The mesh appearance set.
#[derive(Clone, Debug, PartialEq)]
pub struct MeshAppearance {
    /// `Appearance_LODCurve` (owned, though no row lists it), default 1.
    pub lod_curve: RampSingleRamp,
    /// `MeshApp_MeshId`, default 0.
    pub mesh_id: u32,
    /// `MeshApp_MeshName`, default `""`.
    pub mesh_name: Option<Vec<u8>>,
    /// `MeshApp_UseParticleTransform`, the byte as set.
    pub use_particle_transform: u8,
    /// `MeshApp_ParticleTransform`.
    pub particle_transform: AnimatedTransform,
}

/// A set's class-specific part.
#[derive(Clone, Debug, PartialEq)]
pub enum ClassData {
    /// `FxNode` and `FLBeamField`: the base properties only.
    Base,
    /// The three emitters.
    Emitter(Box<Emitter>),
    /// Six of the seven fields.
    Field(Field),
    /// The six billboard appearances.
    Billboard(Box<Billboard>),
    /// `FxParticleAppearance`.
    Particle(ParticleAppearance),
    /// `FxMeshAppearance`.
    Mesh(Box<MeshAppearance>),
}

/// A node's property set: the base properties and the class's own.
#[derive(Clone, Debug, PartialEq)]
pub struct PropertySet {
    /// The class the set was made for.
    pub class: NodeClass,
    /// `Node_ClassName`: the name the node was created by.
    pub class_name: Option<Vec<u8>>,
    /// `Node_Name`: the instance name, initially none.
    pub name: Option<Vec<u8>>,
    /// `Node_LifeSpan`, default 3.
    pub life_span: f32,
    /// `Node_Transform`.
    pub transform: AnimatedTransform,
    /// The class's own properties.
    pub data: ClassData,
}

fn anim(weight: f32) -> AnimatedSingleRamp {
    NestedRamp::with_key(AnimatedSingle::with_weight(weight))
}

fn ramp(value: f32) -> RampSingleRamp {
    NestedRamp::with_key(RampSingle::with_value(value))
}

fn ramp_slot(value: f32) -> Option<NestedSingle> {
    Some(NestedSingle::Ramp(ramp(value)))
}

fn anim_slot(weight: f32) -> Option<NestedSingle> {
    Some(NestedSingle::Animated(anim(weight)))
}

fn new_emitter(shape: EmitterShape) -> ClassData {
    ClassData::Emitter(Box::new(Emitter {
        frequency: anim(100.0),
        max_particles: anim(10000.0),
        init_life_span: anim(2.0),
        pressure: anim(10.0),
        emit_count: anim(0.0),
        velocity_approach: anim(0.0),
        initial_particles: 0,
        lod_curve: ramp(1.0),
        shape,
    }))
}

fn new_billboard(class: NodeClass) -> Billboard {
    let mut set = Billboard {
        lod_curve: ramp(1.0),
        flags: 0,
        texture_crc: 0,
        blend_info: [5, 2],
        color: NestedRamp::with_key(RampColor::with_packed(default_grey())),
        size: ramp_slot(1.0),
        primary_size: ramp_slot(1.0),
        tex_frame: None,
        axis_override: None,
        frame: None,
        common_tex_frame: None,
        local_transform: None,
    };
    match class {
        NodeClass::BasicAppearance => {
            set.tex_frame = ramp_slot(0.0);
            set.axis_override = ramp_slot(0.0);
            set.frame = anim_slot(0.0);
            set.common_tex_frame = ramp_slot(1.0);
        }
        NodeClass::RectAppearance | NodeClass::OrientedAppearance => {
            set.tex_frame = ramp_slot(1.0);
            set.axis_override = ramp_slot(1.0);
            set.frame = ramp_slot(0.0);
            set.common_tex_frame = anim_slot(0.0);
            if class == NodeClass::OrientedAppearance {
                set.local_transform = Some(AnimatedTransform::new());
            }
        }
        NodeClass::BeamAppearance => {
            set.common_tex_frame = ramp_slot(1.0);
            set.tex_frame = ramp_slot(1.0);
            set.frame = ramp_slot(0.0);
            set.axis_override = anim_slot(0.0);
            set.local_transform = Some(AnimatedTransform::new());
            set.flags = 2;
        }
        _ => {
            // Perp and dust.
            set.tex_frame = ramp_slot(0.0);
            set.axis_override = anim_slot(0.0);
        }
    }
    set
}

fn new_class_data(class: NodeClass) -> ClassData {
    match class {
        NodeClass::Node | NodeClass::BeamField => ClassData::Base,
        NodeClass::ConeEmitter => new_emitter(EmitterShape::Cone {
            min_radius: anim(0.0),
            max_radius: anim(1.0),
            min_spread: anim(0.0),
            max_spread: anim(0.0),
        }),
        NodeClass::CubeEmitter => new_emitter(EmitterShape::Cube {
            width: anim(5.0),
            height: anim(5.0),
            depth: anim(5.0),
            min_spread: anim(0.0),
            max_spread: anim(15.0),
        }),
        NodeClass::SphereEmitter => new_emitter(EmitterShape::Sphere {
            min_radius: anim(0.0),
            max_radius: anim(1.0),
        }),
        NodeClass::GravityField => ClassData::Field(Field::Gravity {
            gravity: anim(f32::from_bits(0x411C_CCCD)),
        }),
        NodeClass::AirField => ClassData::Field(Field::Air {
            approach: anim(1.0),
            magnitude: anim(1.0),
        }),
        NodeClass::RadialField => {
            let mut falloff = RampSingle::with_value(1.0);
            falloff.add_key(1.0, f32::from_bits(0x3A83_126F));
            falloff.mode = 3;
            ClassData::Field(Field::Radial {
                magnitude: anim(10.0),
                approach: anim(f32::from_bits(0x3E99_999A)),
                radius: anim(50.0),
                attenuation: NestedRamp::with_key(falloff),
            })
        }
        NodeClass::TurbulenceField => ClassData::Field(Field::Turbulence {
            approach: anim(1.0),
            magnitude: anim(10.0),
        }),
        NodeClass::CollideField => ClassData::Field(Field::Collide {
            reflectivity: anim(f32::from_bits(0x3F33_3333)),
            width: anim(10.0),
            height: anim(10.0),
        }),
        NodeClass::DustField => ClassData::Field(Field::Dust {
            radius: anim(f32::from_bits(0x4270_6666)),
        }),
        NodeClass::ParticleAppearance => ClassData::Particle(ParticleAppearance {
            lod_curve: ramp(1.0),
            life_name: Some(Vec::new()),
            death_name: Some(Vec::new()),
            flags: 0,
        }),
        NodeClass::MeshAppearance => ClassData::Mesh(Box::new(MeshAppearance {
            lod_curve: ramp(1.0),
            mesh_id: 0,
            mesh_name: Some(Vec::new()),
            use_particle_transform: 0,
            particle_transform: AnimatedTransform::new(),
        })),
        NodeClass::BasicAppearance
        | NodeClass::RectAppearance
        | NodeClass::PerpAppearance
        | NodeClass::OrientedAppearance
        | NodeClass::DustAppearance
        | NodeClass::BeamAppearance => ClassData::Billboard(Box::new(new_billboard(class))),
    }
}

// The ramp and transform getters come in shared and mutable pairs with the
// same ownership rules: each `*_of!` macro is the one body, expanded with
// `&` or `&mut` as its last tokens.

macro_rules! billboard_slot {
    ($b:ident, $slot:expr, &) => {
        $b.slot($slot)
    };
    ($b:ident, $slot:expr, & mut) => {
        $b.slot_mut($slot)
    };
}

macro_rules! transform_of {
    ($set:ident, $hash:ident, $($r:tt)+) => {{
        let own = match ($($r)+ $set.data, $set.class) {
            (ClassData::Billboard(b), class) => match billboard_tables(class) {
                Some(t) if t.transform != 0 && $hash == t.transform => {
                    match $($r)+ b.local_transform {
                        Some(transform) => Some(transform),
                        None => None,
                    }
                }
                _ => None,
            },
            (ClassData::Mesh(m), _) if $hash == MESH_PARTICLE_TRANSFORM => {
                Some($($r)+ m.particle_transform)
            }
            _ => None,
        };
        match own {
            Some(transform) => Some(transform),
            None if $hash == NODE_TRANSFORM => Some($($r)+ $set.transform),
            None => None,
        }
    }};
}

macro_rules! single_ramp_ramp_of {
    ($set:ident, $hash:ident, $($r:tt)+) => {
        match ($($r)+ $set.data, $set.class) {
            (ClassData::Emitter(e), _) if $hash == EMITTER_LOD_CURVE => Some($($r)+ e.lod_curve),
            (ClassData::Field(Field::Radial { attenuation, .. }), _)
                if $hash == RADIAL_ATTENUATION =>
            {
                Some(attenuation)
            }
            (ClassData::Billboard(b), class) => {
                let slot = billboard_tables(class)
                    .and_then(|t| t.single_ramps.iter().find(|(h, _)| *h == $hash))
                    .map(|&(_, slot)| slot);
                match slot {
                    Some(slot) => match billboard_slot!(b, slot, $($r)+) {
                        Some(NestedSingle::Ramp(ramp)) => Some(ramp),
                        _ => None,
                    },
                    None if $hash == APPEARANCE_LOD_CURVE => Some($($r)+ b.lod_curve),
                    None => None,
                }
            }
            (ClassData::Particle(p), _) if $hash == APPEARANCE_LOD_CURVE => {
                Some($($r)+ p.lod_curve)
            }
            (ClassData::Mesh(m), _) if $hash == APPEARANCE_LOD_CURVE => Some($($r)+ m.lod_curve),
            _ => None,
        }
    };
}

macro_rules! color_ramp_ramp_of {
    ($set:ident, $hash:ident, $($r:tt)+) => {
        match $($r)+ $set.data {
            ClassData::Billboard(b) if $hash == BILLBOARD_COLOR => Some($($r)+ b.color),
            _ => None,
        }
    };
}

macro_rules! animated_single_ramp_of {
    ($set:ident, $hash:ident, $($r:tt)+) => {
        match ($($r)+ $set.data, $set.class) {
            (ClassData::Emitter(e), _) => {
                let shape = match $($r)+ e.shape {
                    EmitterShape::Cone { min_radius, .. } if $hash == 0x1C73_D16B => {
                        Some(min_radius)
                    }
                    EmitterShape::Cone { max_radius, .. } if $hash == 0xF636_F07F => {
                        Some(max_radius)
                    }
                    EmitterShape::Cone { min_spread, .. } if $hash == 0x100E_7F55 => {
                        Some(min_spread)
                    }
                    EmitterShape::Cone { max_spread, .. } if $hash == 0xFA4B_5E41 => {
                        Some(max_spread)
                    }
                    EmitterShape::Cube { width, .. } if $hash == 0xFD22_A11C => Some(width),
                    EmitterShape::Cube { height, .. } if $hash == 0xEFB0_F8CF => Some(height),
                    EmitterShape::Cube { depth, .. } if $hash == 0x14B7_4EB2 => Some(depth),
                    EmitterShape::Cube { min_spread, .. } if $hash == 0xE282_006B => {
                        Some(min_spread)
                    }
                    EmitterShape::Cube { max_spread, .. } if $hash == 0x08C7_217F => {
                        Some(max_spread)
                    }
                    EmitterShape::Sphere { min_radius, .. } if $hash == 0xF231_FF60 => {
                        Some(min_radius)
                    }
                    EmitterShape::Sphere { max_radius, .. } if $hash == 0x1874_DE74 => {
                        Some(max_radius)
                    }
                    _ => None,
                };
                match shape {
                    Some(ramp) => Some(ramp),
                    None => match $hash {
                        0x023C_350C => Some($($r)+ e.frequency),
                        0xF504_2852 => Some($($r)+ e.max_particles),
                        0xE722_1F95 => Some($($r)+ e.emit_count),
                        0x0AB1_80C5 => Some($($r)+ e.pressure),
                        0x0A63_5880 => Some($($r)+ e.init_life_span),
                        0x0681_233E => Some($($r)+ e.velocity_approach),
                        _ => None,
                    },
                }
            }
            (ClassData::Field(f), _) => match f {
                Field::Gravity { gravity } if $hash == 0xE02B_8BD4 => Some(gravity),
                Field::Air { approach, .. } if $hash == 0x1042_3CEB => Some(approach),
                Field::Air { magnitude, .. } if $hash == 0xE5E3_524C => Some(magnitude),
                Field::Radial { radius, .. } if $hash == 0xFF28_F620 => Some(radius),
                Field::Radial { magnitude, .. } if $hash == 0x0064_B3B9 => Some(magnitude),
                Field::Radial { approach, .. } if $hash == 0xF3CF_5EA5 => Some(approach),
                Field::Turbulence { approach, .. } if $hash == 0x1B97_0B9A => Some(approach),
                Field::Turbulence { magnitude, .. } if $hash == 0x1CEA_C6D1 => Some(magnitude),
                Field::Collide { reflectivity, .. }
                    if $hash == 0xF246_8210 || $hash == 0xE462_B789 =>
                {
                    Some(reflectivity)
                }
                Field::Collide { width, .. } if $hash == 0x138B_DB51 || $hash == 0x061A_AF28 => {
                    Some(width)
                }
                Field::Collide { height, .. } if $hash == 0x1133_6C98 || $hash == 0x16FF_3474 => {
                    Some(height)
                }
                Field::Dust { radius } if $hash == 0x1874_DE74 => Some(radius),
                _ => None,
            },
            (ClassData::Billboard(b), class) => {
                let slot = billboard_tables(class)
                    .and_then(|t| t.animated_ramps.iter().find(|(h, _)| *h == $hash))
                    .map(|&(_, slot)| slot);
                match slot {
                    Some(slot) => match billboard_slot!(b, slot, $($r)+) {
                        Some(NestedSingle::Animated(ramp)) => Some(ramp),
                        _ => None,
                    },
                    None => None,
                }
            }
            _ => None,
        }
    };
}

/// `RadialField_Attenuation`.
pub const RADIAL_ATTENUATION: u32 = 0x1A43_3168;

impl PropertySet {
    /// The set a node of `class` is created with, `Node_ClassName` set to
    /// `class_name` (the name the component was requested by).
    #[must_use]
    pub fn new(class: NodeClass, class_name: &[u8]) -> Self {
        Self {
            class,
            class_name: Some(class_name.to_vec()),
            name: None,
            life_span: 3.0,
            transform: AnimatedTransform::new(),
            data: new_class_data(class),
        }
    }

    /// The instance name's hash (`GetInstanceNameHash`): the exact-case CRC
    /// of the name, 0 for none.
    #[must_use]
    pub fn name_hash(&self) -> u32 {
        crate::hash::exact_name_crc32(self.name.as_deref().unwrap_or_default())
    }

    /// `GetPropBool`.
    ///
    /// # Errors
    ///
    /// [`PropertyError::Miss`] for a hash the class does not own.
    pub fn get_bool(&self, hash: u32) -> Result<u8, PropertyError> {
        match (&self.data, self.class) {
            (ClassData::Billboard(b), class) => billboard_tables(class)
                .and_then(|t| t.flags.iter().find(|(h, _)| *h == hash))
                .map(|&(_, mask)| u8::from(b.flags & mask != 0))
                .ok_or(PropertyError::Miss),
            (ClassData::Particle(p), _) if hash == PARTICLE_USE_DYNAMIC_ROTATION => {
                Ok(u8::from(p.flags & 1 != 0))
            }
            (ClassData::Particle(p), _) if hash == PARTICLE_SMOOTH_ROTATION => {
                Ok(u8::from(p.flags & 2 != 0))
            }
            (ClassData::Mesh(m), _) if hash == MESH_USE_PARTICLE_TRANSFORM => {
                Ok(m.use_particle_transform)
            }
            _ => Err(PropertyError::Miss),
        }
    }

    /// `SetPropBool`: a flag is set for a non-zero value and cleared for 0;
    /// the mesh set keeps the byte.
    ///
    /// # Errors
    ///
    /// [`PropertyError::Miss`] for a hash the class does not own.
    pub fn set_bool(&mut self, hash: u32, value: u8) -> Result<(), PropertyError> {
        let apply = |flags: &mut u32, mask: u32| {
            if value != 0 {
                *flags |= mask;
            } else {
                *flags &= !mask;
            }
        };
        match (&mut self.data, self.class) {
            (ClassData::Billboard(b), class) => {
                let &(_, mask) = billboard_tables(class)
                    .and_then(|t| t.flags.iter().find(|(h, _)| *h == hash))
                    .ok_or(PropertyError::Miss)?;
                apply(&mut b.flags, mask);
                Ok(())
            }
            (ClassData::Particle(p), _) if hash == PARTICLE_USE_DYNAMIC_ROTATION => {
                apply(&mut p.flags, 1);
                Ok(())
            }
            (ClassData::Particle(p), _) if hash == PARTICLE_SMOOTH_ROTATION => {
                apply(&mut p.flags, 2);
                Ok(())
            }
            (ClassData::Mesh(m), _) if hash == MESH_USE_PARTICLE_TRANSFORM => {
                m.use_particle_transform = value;
                Ok(())
            }
            _ => Err(PropertyError::Miss),
        }
    }

    /// `GetPropInt`.
    ///
    /// # Errors
    ///
    /// [`PropertyError::Miss`] for a hash the class does not own.
    pub fn get_int(&self, hash: u32) -> Result<u32, PropertyError> {
        match &self.data {
            ClassData::Emitter(e) if hash == EMITTER_INITIAL_PARTICLES => Ok(e.initial_particles),
            ClassData::Mesh(m) if hash == MESH_ID => Ok(m.mesh_id),
            _ => Err(PropertyError::Miss),
        }
    }

    /// `SetPropInt`.
    ///
    /// # Errors
    ///
    /// [`PropertyError::Miss`] for a hash the class does not own.
    pub fn set_int(&mut self, hash: u32, value: u32) -> Result<(), PropertyError> {
        match &mut self.data {
            ClassData::Emitter(e) if hash == EMITTER_INITIAL_PARTICLES => {
                e.initial_particles = value;
                Ok(())
            }
            ClassData::Mesh(m) if hash == MESH_ID => {
                m.mesh_id = value;
                Ok(())
            }
            _ => Err(PropertyError::Miss),
        }
    }

    /// `GetPropFloat`: `Node_LifeSpan` in every class.
    ///
    /// # Errors
    ///
    /// [`PropertyError::Miss`] for any other hash.
    pub fn get_float(&self, hash: u32) -> Result<f32, PropertyError> {
        if hash == NODE_LIFE_SPAN {
            Ok(self.life_span)
        } else {
            Err(PropertyError::Miss)
        }
    }

    /// `SetPropFloat`.
    ///
    /// # Errors
    ///
    /// [`PropertyError::Miss`] for any hash but `Node_LifeSpan`.
    pub fn set_float(&mut self, hash: u32, value: f32) -> Result<(), PropertyError> {
        if hash == NODE_LIFE_SPAN {
            self.life_span = value;
            Ok(())
        } else {
            Err(PropertyError::Miss)
        }
    }

    /// `GetPropPair`: the billboards' blend pair.
    ///
    /// # Errors
    ///
    /// [`PropertyError::Miss`] for a hash the class does not own.
    pub fn get_pair(&self, hash: u32) -> Result<[u32; 2], PropertyError> {
        match &self.data {
            ClassData::Billboard(b) if hash == BILLBOARD_BLEND_INFO => Ok(b.blend_info),
            _ => Err(PropertyError::Miss),
        }
    }

    /// `SetPropPair`.
    ///
    /// # Errors
    ///
    /// [`PropertyError::Miss`] for a hash the class does not own.
    pub fn set_pair(&mut self, hash: u32, value: [u32; 2]) -> Result<(), PropertyError> {
        match &mut self.data {
            ClassData::Billboard(b) if hash == BILLBOARD_BLEND_INFO => {
                b.blend_info = value;
                Ok(())
            }
            _ => Err(PropertyError::Miss),
        }
    }

    /// `GetPropName`: the string, or `None` for the reference's null
    /// buffer. The billboards answer `"???"` for their texture name.
    ///
    /// # Errors
    ///
    /// [`PropertyError::Miss`] for a hash the class does not own.
    pub fn get_name(&self, hash: u32) -> Result<Option<&[u8]>, PropertyError> {
        match &self.data {
            ClassData::Billboard(_) if hash == BILLBOARD_TEX_NAME => return Ok(Some(b"???")),
            ClassData::Particle(p) if hash == PARTICLE_LIFE_NAME => {
                return Ok(p.life_name.as_deref());
            }
            ClassData::Particle(p) if hash == PARTICLE_DEATH_NAME => {
                return Ok(p.death_name.as_deref());
            }
            ClassData::Mesh(m) if hash == MESH_NAME => return Ok(m.mesh_name.as_deref()),
            _ => {}
        }
        match hash {
            NODE_NAME => Ok(self.name.as_deref()),
            NODE_CLASS_NAME => Ok(self.class_name.as_deref()),
            _ => Err(PropertyError::Miss),
        }
    }

    /// `SetPropName`. The text ends at its first NUL.
    ///
    /// # Errors
    ///
    /// [`PropertyError::ReadOnly`] for `Node_ClassName`,
    /// [`PropertyError::Miss`] for a hash the class does not own.
    pub fn set_name(&mut self, hash: u32, text: &[u8]) -> Result<(), PropertyError> {
        let text = crate::hash::c_str(text);
        match &mut self.data {
            ClassData::Billboard(b) if hash == BILLBOARD_TEX_NAME => {
                b.texture_crc = name_crc32(text);
                return Ok(());
            }
            ClassData::Particle(p) if hash == PARTICLE_LIFE_NAME => {
                p.life_name = Some(text.to_vec());
                return Ok(());
            }
            ClassData::Particle(p) if hash == PARTICLE_DEATH_NAME => {
                p.death_name = Some(text.to_vec());
                return Ok(());
            }
            ClassData::Mesh(m) if hash == MESH_NAME => {
                m.mesh_name = Some(text.to_vec());
                return Ok(());
            }
            _ => {}
        }
        match hash {
            NODE_NAME => {
                self.name = Some(text.to_vec());
                Ok(())
            }
            NODE_CLASS_NAME => Err(PropertyError::ReadOnly),
            _ => Err(PropertyError::Miss),
        }
    }

    /// `GetPropTransform` (kind 0x105).
    #[must_use]
    pub fn transform(&self, hash: u32) -> Option<&AnimatedTransform> {
        transform_of!(self, hash, &)
    }

    /// `GetPropTransform`, for changing the transform in place.
    pub fn transform_mut(&mut self, hash: u32) -> Option<&mut AnimatedTransform> {
        transform_of!(self, hash, &mut)
    }

    /// `GetPropSingleRampRamp` (kind 0x200).
    #[must_use]
    pub fn single_ramp_ramp(&self, hash: u32) -> Option<&RampSingleRamp> {
        single_ramp_ramp_of!(self, hash, &)
    }

    /// `GetPropSingleRampRamp`, for changing the ramp in place.
    pub fn single_ramp_ramp_mut(&mut self, hash: u32) -> Option<&mut RampSingleRamp> {
        single_ramp_ramp_of!(self, hash, &mut)
    }

    /// `GetPropColorRampRamp` (kind 0x201).
    #[must_use]
    pub fn color_ramp_ramp(&self, hash: u32) -> Option<&ColorRampRamp> {
        color_ramp_ramp_of!(self, hash, &)
    }

    /// `GetPropColorRampRamp`, for changing the ramp in place.
    pub fn color_ramp_ramp_mut(&mut self, hash: u32) -> Option<&mut ColorRampRamp> {
        color_ramp_ramp_of!(self, hash, &mut)
    }

    /// `GetPropAnimatedSingleRamp` (kind 0x202).
    #[must_use]
    pub fn animated_single_ramp(&self, hash: u32) -> Option<&AnimatedSingleRamp> {
        animated_single_ramp_of!(self, hash, &)
    }

    /// `GetPropAnimatedSingleRamp`, for changing the ramp in place.
    pub fn animated_single_ramp_mut(&mut self, hash: u32) -> Option<&mut AnimatedSingleRamp> {
        animated_single_ramp_of!(self, hash, &mut)
    }
}
