//! Render states, texture-stage states and pipeline properties as typed
//! values (decision record 0004, point 3), with the Direct3D 8 numbering
//! the C++ callers and the trace use.
//!
//! The tables are DX8RENDERER's (`kRenderStateProperties`,
//! `kStageStateProperties`, `kStageOps*`, `kPipelineProperties` in
//! `CppSource/DX8RENDERER/DX8RENDERER.cpp`): the states it knows, the names
//! the engine INI uses for them and their built-in defaults. A state's value
//! is kept as the raw 32 bits a caller set, so that a getter returns exactly
//! what was stored; the typed view is read from those bits. A value outside
//! a type's range is kept as `Other`.

/// A value of a state, read from and written to its raw 32 bits.
pub trait RawValue: Copy {
    fn from_raw(raw: u32) -> Self;
    fn to_raw(self) -> u32;
}

impl RawValue for u32 {
    fn from_raw(raw: u32) -> Self {
        raw
    }
    fn to_raw(self) -> u32 {
        self
    }
}

impl RawValue for i32 {
    fn from_raw(raw: u32) -> Self {
        raw.cast_signed()
    }
    fn to_raw(self) -> u32 {
        self.cast_unsigned()
    }
}

impl RawValue for f32 {
    fn from_raw(raw: u32) -> Self {
        f32::from_bits(raw)
    }
    fn to_raw(self) -> u32 {
        self.to_bits()
    }
}

/// Direct3D 8 reads a `BOOL` as any non-zero value.
impl RawValue for bool {
    fn from_raw(raw: u32) -> Self {
        raw != 0
    }
    fn to_raw(self) -> u32 {
        u32::from(self)
    }
}

/// A `D3DCOLOR`: 8-bit channels packed as `0xAARRGGBB`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub struct Color(pub u32);

impl Color {
    #[must_use]
    pub fn a(self) -> u8 {
        (self.0 >> 24) as u8
    }
    #[must_use]
    pub fn r(self) -> u8 {
        (self.0 >> 16) as u8
    }
    #[must_use]
    pub fn g(self) -> u8 {
        (self.0 >> 8) as u8
    }
    #[must_use]
    pub fn b(self) -> u8 {
        self.0 as u8
    }
}

impl RawValue for Color {
    fn from_raw(raw: u32) -> Self {
        Color(raw)
    }
    fn to_raw(self) -> u32 {
        self.0
    }
}

/// An enumeration of Direct3D 8 values with a fallback that keeps any
/// other number.
macro_rules! d3d_enum {
    ($(#[$meta:meta])* $name:ident { $($variant:ident = $value:literal,)* }) => {
        $(#[$meta])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
        pub enum $name {
            $($variant,)*
            /// A number Direct3D 8 does not define for this state.
            Other(u32),
        }

        impl RawValue for $name {
            fn from_raw(raw: u32) -> Self {
                match raw {
                    $($value => $name::$variant,)*
                    other => $name::Other(other),
                }
            }
            fn to_raw(self) -> u32 {
                match self {
                    $($name::$variant => $value,)*
                    $name::Other(other) => other,
                }
            }
        }
    };
}

d3d_enum!(
    /// `D3DZBUFFERTYPE`.
    ZBuffer { Off = 0, On = 1, UseW = 2, }
);
d3d_enum!(
    /// `D3DFILLMODE`.
    Fill { Point = 1, Wireframe = 2, Solid = 3, }
);
d3d_enum!(
    /// `D3DSHADEMODE`.
    Shade { Flat = 1, Gouraud = 2, Phong = 3, }
);
d3d_enum!(
    /// `D3DBLEND`.
    Blend {
        Zero = 1,
        One = 2,
        SrcColor = 3,
        InvSrcColor = 4,
        SrcAlpha = 5,
        InvSrcAlpha = 6,
        DestAlpha = 7,
        InvDestAlpha = 8,
        DestColor = 9,
        InvDestColor = 10,
        SrcAlphaSat = 11,
        BothSrcAlpha = 12,
        BothInvSrcAlpha = 13,
    }
);
d3d_enum!(
    /// `D3DBLENDOP`.
    BlendOp { Add = 1, Subtract = 2, RevSubtract = 3, Min = 4, Max = 5, }
);
d3d_enum!(
    /// `D3DCULL`.
    Cull { None = 1, Clockwise = 2, CounterClockwise = 3, }
);
d3d_enum!(
    /// `D3DCMPFUNC`.
    Compare {
        Never = 1,
        Less = 2,
        Equal = 3,
        LessEqual = 4,
        Greater = 5,
        NotEqual = 6,
        GreaterEqual = 7,
        Always = 8,
    }
);
d3d_enum!(
    /// `D3DFOGMODE`.
    FogMode { None = 0, Exp = 1, Exp2 = 2, Linear = 3, }
);
d3d_enum!(
    /// `D3DSTENCILOP`.
    StencilOp {
        Keep = 1,
        Zero = 2,
        Replace = 3,
        IncrementSaturate = 4,
        DecrementSaturate = 5,
        Invert = 6,
        Increment = 7,
        Decrement = 8,
    }
);
d3d_enum!(
    /// `D3DMATERIALCOLORSOURCE`.
    MaterialSource { Material = 0, Color1 = 1, Color2 = 2, }
);
d3d_enum!(
    /// `D3DVERTEXBLENDFLAGS`.
    VertexBlend {
        Disable = 0,
        Weights1 = 1,
        Weights2 = 2,
        Weights3 = 3,
        Tweening = 255,
        Weights0 = 256,
    }
);
d3d_enum!(
    /// `D3DPATCHEDGESTYLE`.
    PatchEdge { Discrete = 0, Continuous = 1, }
);
d3d_enum!(
    /// `D3DDEBUGMONITORTOKENS`.
    DebugMonitor { Enable = 0, Disable = 1, }
);
d3d_enum!(
    /// `D3DORDERTYPE`.
    Order { Linear = 1, Quadratic = 2, Cubic = 3, Quintic = 5, }
);
d3d_enum!(
    /// `D3DTEXTUREOP`.
    TextureOp {
        Disable = 1,
        SelectArg1 = 2,
        SelectArg2 = 3,
        Modulate = 4,
        Modulate2x = 5,
        Modulate4x = 6,
        Add = 7,
        AddSigned = 8,
        AddSigned2x = 9,
        Subtract = 10,
        AddSmooth = 11,
        BlendDiffuseAlpha = 12,
        BlendTextureAlpha = 13,
        BlendFactorAlpha = 14,
        BlendTextureAlphaPm = 15,
        BlendCurrentAlpha = 16,
        PreModulate = 17,
        ModulateAlphaAddColor = 18,
        ModulateColorAddAlpha = 19,
        ModulateInvAlphaAddColor = 20,
        ModulateInvColorAddAlpha = 21,
        BumpEnvMap = 22,
        BumpEnvMapLuminance = 23,
        DotProduct3 = 24,
        MultiplyAdd = 25,
        Lerp = 26,
    }
);
d3d_enum!(
    /// `D3DTEXTUREADDRESS`.
    TextureAddress { Wrap = 1, Mirror = 2, Clamp = 3, Border = 4, MirrorOnce = 5, }
);
d3d_enum!(
    /// `D3DTEXTUREFILTERTYPE`.
    TextureFilter {
        None = 0,
        Point = 1,
        Linear = 2,
        Anisotropic = 3,
        FlatCubic = 4,
        GaussianCubic = 5,
    }
);

/// The source of a texture-stage argument (`D3DTA_*`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ArgSource {
    Diffuse,
    Current,
    Texture,
    Factor,
    Specular,
    Temp,
    Other(u8),
}

/// A texture-stage argument: a source and its two modifiers.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct TextureArg {
    pub source: ArgSource,
    /// `D3DTA_COMPLEMENT`: one minus the value.
    pub complement: bool,
    /// `D3DTA_ALPHAREPLICATE`: the alpha in every channel.
    pub alpha_replicate: bool,
    /// Bits above the modifiers, kept for the raw value.
    pub rest: u32,
}

impl RawValue for TextureArg {
    fn from_raw(raw: u32) -> Self {
        let source = match raw & 0xf {
            0 => ArgSource::Diffuse,
            1 => ArgSource::Current,
            2 => ArgSource::Texture,
            3 => ArgSource::Factor,
            4 => ArgSource::Specular,
            5 => ArgSource::Temp,
            other => ArgSource::Other(other as u8),
        };
        TextureArg {
            source,
            complement: raw & 0x10 != 0,
            alpha_replicate: raw & 0x20 != 0,
            rest: raw & !0x3f,
        }
    }
    fn to_raw(self) -> u32 {
        let source = match self.source {
            ArgSource::Diffuse => 0,
            ArgSource::Current => 1,
            ArgSource::Texture => 2,
            ArgSource::Factor => 3,
            ArgSource::Specular => 4,
            ArgSource::Temp => 5,
            ArgSource::Other(n) => u32::from(n) & 0xf,
        };
        source
            | if self.complement { 0x10 } else { 0 }
            | if self.alpha_replicate { 0x20 } else { 0 }
            | self.rest
    }
}

/// How a stage's texture coordinates are generated (the high word of
/// `D3DTSS_TEXCOORDINDEX`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum TexCoordGen {
    /// The vertex's coordinate set.
    PassThrough,
    CameraSpaceNormal,
    CameraSpacePosition,
    CameraSpaceReflection,
    Other(u16),
}

/// `D3DTSS_TEXCOORDINDEX`: a coordinate set and a generation mode.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct TexCoordIndex {
    pub index: u16,
    pub generate: TexCoordGen,
}

impl RawValue for TexCoordIndex {
    fn from_raw(raw: u32) -> Self {
        let generate = match (raw >> 16) as u16 {
            0 => TexCoordGen::PassThrough,
            1 => TexCoordGen::CameraSpaceNormal,
            2 => TexCoordGen::CameraSpacePosition,
            3 => TexCoordGen::CameraSpaceReflection,
            other => TexCoordGen::Other(other),
        };
        TexCoordIndex {
            index: raw as u16,
            generate,
        }
    }
    fn to_raw(self) -> u32 {
        let high: u16 = match self.generate {
            TexCoordGen::PassThrough => 0,
            TexCoordGen::CameraSpaceNormal => 1,
            TexCoordGen::CameraSpacePosition => 2,
            TexCoordGen::CameraSpaceReflection => 3,
            TexCoordGen::Other(n) => n,
        };
        (u32::from(high) << 16) | u32::from(self.index)
    }
}

/// `D3DTSS_TEXTURETRANSFORMFLAGS`: the number of coordinates the stage
/// transform produces (0 disables it) and the projective divide.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct TextureTransformFlags {
    pub count: u8,
    pub projected: bool,
    /// Other bits, kept for the raw value.
    pub rest: u32,
}

impl RawValue for TextureTransformFlags {
    fn from_raw(raw: u32) -> Self {
        TextureTransformFlags {
            count: (raw & 0xff) as u8,
            projected: raw & 0x100 != 0,
            rest: raw & !0x1ff,
        }
    }
    fn to_raw(self) -> u32 {
        u32::from(self.count) | if self.projected { 0x100 } else { 0 } | self.rest
    }
}

/// Generates a state id enum from a table, and the typed state enum whose
/// variants carry the value.
macro_rules! state_table {
    (
        $(#[$id_meta:meta])* $id_name:ident,
        $(#[$state_meta:meta])* $state_name:ident,
        $( $variant:ident($ty:ty) = $raw:literal, $ini:literal, $default:literal; )*
    ) => {
        $(#[$id_meta])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
        pub enum $id_name {
            $($variant,)*
        }

        impl $id_name {
            /// Every state in the order of the C++ table.
            pub const ALL: &'static [$id_name] = &[$($id_name::$variant,)*];

            /// The Direct3D 8 number.
            #[must_use]
            pub fn raw(self) -> u32 {
                match self {
                    $($id_name::$variant => $raw,)*
                }
            }

            /// The state with Direct3D 8 number `raw`, if the table has it.
            #[must_use]
            pub fn from_raw(raw: u32) -> Option<Self> {
                match raw {
                    $($raw => Some($id_name::$variant),)*
                    _ => None,
                }
            }

            /// The name the engine INI and the C++ table use.
            #[must_use]
            pub fn name(self) -> &'static str {
                match self {
                    $($id_name::$variant => $ini,)*
                }
            }

            /// The table's built-in default, as raw bits.
            #[must_use]
            pub fn builtin_default(self) -> u32 {
                match self {
                    $($id_name::$variant => $default,)*
                }
            }

            /// The position in [`Self::ALL`].
            #[must_use]
            pub fn index(self) -> usize {
                self as usize
            }
        }

        $(#[$state_meta])*
        #[derive(Clone, Copy, Debug, PartialEq)]
        pub enum $state_name {
            $($variant($ty),)*
        }

        impl $state_name {
            #[must_use]
            pub fn id(self) -> $id_name {
                match self {
                    $($state_name::$variant(_) => $id_name::$variant,)*
                }
            }

            /// The typed view of `raw` bits of state `id`.
            #[must_use]
            pub fn from_raw(id: $id_name, raw: u32) -> Self {
                match id {
                    $($id_name::$variant => $state_name::$variant(<$ty as RawValue>::from_raw(raw)),)*
                }
            }

            /// The raw bits of the value.
            #[must_use]
            pub fn to_raw(self) -> u32 {
                match self {
                    $($state_name::$variant(v) => RawValue::to_raw(v),)*
                }
            }
        }
    };
}

state_table!(
    /// A render state DX8RENDERER knows (`kRenderStateProperties`, 76 rows).
    RenderStateId,
    /// A render state with its value.
    RenderState,
    ZEnable(ZBuffer) = 0x7, "ZENABLE", 0x1;
    FillMode(Fill) = 0x8, "FILLMODE", 0x3;
    ShadeMode(Shade) = 0x9, "SHADEMODE", 0x2;
    LinePattern(u32) = 0xA, "LINEPATTERN", 0x0;
    ZWriteEnable(bool) = 0xE, "ZWRITEENABLE", 0x1;
    AlphaTestEnable(bool) = 0xF, "ALPHATESTENABLE", 0x0;
    LastPixel(bool) = 0x10, "LASTPIXEL", 0x1;
    SrcBlend(Blend) = 0x13, "SRCBLEND", 0x2;
    DestBlend(Blend) = 0x14, "DESTBLEND", 0x1;
    CullMode(Cull) = 0x16, "CULLMODE", 0x1;
    ZFunc(Compare) = 0x17, "ZFUNC", 0x2;
    AlphaRef(u32) = 0x18, "ALPHAREF", 0x0;
    AlphaFunc(Compare) = 0x19, "ALPHAFUNC", 0x8;
    DitherEnable(bool) = 0x1A, "DITHERENABLE", 0x0;
    AlphaBlendEnable(bool) = 0x1B, "ALPHABLENDENABLE", 0x0;
    FogEnable(bool) = 0x1C, "FOGENABLE", 0x0;
    SpecularEnable(bool) = 0x1D, "SPECULARENABLE", 0x0;
    ZVisible(bool) = 0x1E, "ZVISIBLE", 0x1;
    FogColor(Color) = 0x22, "FOGCOLOR", 0x0;
    FogTableMode(FogMode) = 0x23, "FOGTABLEMODE", 0x1;
    FogStart(f32) = 0x24, "FOGSTART", 0x0;
    FogEnd(f32) = 0x25, "FOGEND", 0x0;
    FogDensity(f32) = 0x26, "FOGDENSITY", 0x0;
    EdgeAntialias(bool) = 0x28, "EDGEANTIALIAS", 0x0;
    ZBias(i32) = 0x2F, "ZBIAS", 0x0;
    RangeFogEnable(bool) = 0x30, "RANGEFOGENABLE", 0x0;
    StencilEnable(bool) = 0x34, "STENCILENABLE", 0x0;
    StencilFail(StencilOp) = 0x35, "STENCILFAIL", 0x1;
    StencilZFail(StencilOp) = 0x36, "STENCILZFAIL", 0x1;
    StencilPass(StencilOp) = 0x37, "STENCILPASS", 0x1;
    StencilFunc(Compare) = 0x38, "STENCILFUNC", 0x8;
    StencilRef(u32) = 0x39, "STENCILREF", 0xFFFF_FFFF;
    StencilMask(u32) = 0x3A, "STENCILMASK", 0xFFFF_FFFF;
    StencilWriteMask(u32) = 0x3B, "STENCILWRITEMASK", 0xFFFF_FFFF;
    TextureFactor(Color) = 0x3C, "TEXTUREFACTOR", 0x0;
    Wrap0(u32) = 0x80, "WRAP0", 0x0;
    Wrap1(u32) = 0x81, "WRAP1", 0x0;
    Wrap2(u32) = 0x82, "WRAP2", 0x0;
    Wrap3(u32) = 0x83, "WRAP3", 0x0;
    Wrap4(u32) = 0x84, "WRAP4", 0x0;
    Wrap5(u32) = 0x85, "WRAP5", 0x0;
    Wrap6(u32) = 0x86, "WRAP6", 0x0;
    Wrap7(u32) = 0x87, "WRAP7", 0x0;
    Clipping(bool) = 0x88, "CLIPPING", 0x1;
    Lighting(bool) = 0x89, "LIGHTING", 0x0;
    Ambient(Color) = 0x8B, "AMBIENT", 0x0;
    FogVertexMode(FogMode) = 0x8C, "FOGVERTEXMODE", 0x0;
    ColorVertex(bool) = 0x8D, "COLORVERTEX", 0x0;
    LocalViewer(bool) = 0x8E, "LOCALVIEWER", 0x0;
    NormalizeNormals(bool) = 0x8F, "NORMALIZENORMALS", 0x0;
    DiffuseMaterialSource(MaterialSource) = 0x91, "DIFFUSEMATERIALSOURCE", 0x1;
    SpecularMaterialSource(MaterialSource) = 0x92, "SPECULARMATERIALSOURCE", 0x2;
    AmbientMaterialSource(MaterialSource) = 0x93, "AMBIENTMATERIALSOURCE", 0x2;
    EmissiveMaterialSource(MaterialSource) = 0x94, "EMISSIVEMATERIALSOURCE", 0x0;
    VertexBlendMode(VertexBlend) = 0x97, "VERTEXBLEND", 0x0;
    ClipPlaneEnable(u32) = 0x98, "CLIPPLANEENABLE", 0x0;
    SoftwareVertexProcessing(bool) = 0x99, "SOFTWAREVERTEXPROCESSING", 0x0;
    PointSize(f32) = 0x9A, "POINTSIZE", 0x3F80_0000;
    PointSizeMin(f32) = 0x9B, "POINTSIZE_MIN", 0x3F80_0000;
    PointSpriteEnable(bool) = 0x9C, "POINTSPRITEENABLE", 0x0;
    PointScaleEnable(bool) = 0x9D, "POINTSCALEENABLE", 0x0;
    PointScaleA(f32) = 0x9E, "POINTSCALE_A", 0x3F80_0000;
    PointScaleB(f32) = 0x9F, "POINTSCALE_B", 0x0;
    PointScaleC(f32) = 0xA0, "POINTSCALE_C", 0x0;
    MultisampleAntialias(bool) = 0xA1, "MULTISAMPLEANTIALIAS", 0x1;
    MultisampleMask(u32) = 0xA2, "MULTISAMPLEMASK", 0xFFFF_FFFF;
    PatchEdgeStyle(PatchEdge) = 0xA3, "PATCHEDGESTYLE", 0x0;
    PatchSegments(f32) = 0xA4, "PATCHSEGMENTS", 0x0;
    DebugMonitorToken(DebugMonitor) = 0xA5, "DEBUGMONITORTOKEN", 0x0;
    PointSizeMax(f32) = 0xA6, "POINTSIZE_MAX", 0x3F80_0000;
    IndexedVertexBlendEnable(bool) = 0xA7, "INDEXEDVERTEXBLENDENABLE", 0x0;
    ColorWriteEnable(u32) = 0xA8, "COLORWRITEENABLE", 0xF;
    TweenFactor(f32) = 0xAA, "TWEENFACTOR", 0x0;
    BlendOperation(BlendOp) = 0xAB, "BLENDOP", 0x1;
    PositionOrder(Order) = 0xAC, "POSITIONORDER", 0x1;
    NormalOrder(Order) = 0xAD, "NORMALORDER", 0x1;
);

state_table!(
    /// A texture-stage state (`kStageOps*` and `kStageStateProperties`, 27
    /// rows). The defaults of `COLOROP` and `ALPHAOP` are those of a stage
    /// other than the first: `Initialize` rebuilds stage 0 with them too,
    /// so every stage starts disabled.
    StageStateId,
    /// A texture-stage state with its value.
    StageState,
    ColorOp(TextureOp) = 0x1, "COLOROP", 0x1;
    ColorArg1(TextureArg) = 0x2, "COLORARG1", 0x2;
    ColorArg2(TextureArg) = 0x3, "COLORARG2", 0x1;
    AlphaOp(TextureOp) = 0x4, "ALPHAOP", 0x1;
    AlphaArg1(TextureArg) = 0x5, "ALPHAARG1", 0x2;
    AlphaArg2(TextureArg) = 0x6, "ALPHAARG2", 0x1;
    BumpEnvMat00(f32) = 0x7, "BUMPENVMAT00", 0x0;
    BumpEnvMat01(f32) = 0x8, "BUMPENVMAT01", 0x0;
    BumpEnvMat10(f32) = 0x9, "BUMPENVMAT10", 0x0;
    BumpEnvMat11(f32) = 0xA, "BUMPENVMAT11", 0x0;
    TexCoordIdx(TexCoordIndex) = 0xB, "TEXCOORDINDEX", 0x0;
    AddressU(TextureAddress) = 0xD, "ADDRESSU", 0x1;
    AddressV(TextureAddress) = 0xE, "ADDRESSV", 0x1;
    BorderColor(Color) = 0xF, "BORDERCOLOR", 0x0;
    MagFilter(TextureFilter) = 0x10, "MAGFILTER", 0x2;
    MinFilter(TextureFilter) = 0x11, "MINFILTER", 0x2;
    MipFilter(TextureFilter) = 0x12, "MIPFILTER", 0x1;
    MipMapLodBias(f32) = 0x13, "MIPMAPLODBIAS", 0x0;
    MaxMipLevel(u32) = 0x14, "MAXMIPLEVEL", 0x0;
    MaxAnisotropy(u32) = 0x15, "MAXANISOTROPY", 0x1;
    BumpEnvLScale(f32) = 0x16, "BUMPENVLSCALE", 0x0;
    BumpEnvLOffset(f32) = 0x17, "BUMPENVLOFFSET", 0x0;
    TransformFlags(TextureTransformFlags) = 0x18, "TEXTURETRANSFORMFLAGS", 0x0;
    AddressW(TextureAddress) = 0x19, "ADDRESSW", 0x1;
    ColorArg0(TextureArg) = 0x1A, "COLORARG0", 0x1;
    AlphaArg0(TextureArg) = 0x1B, "ALPHAARG0", 0x1;
    ResultArg(TextureArg) = 0x1C, "RESULTARG", 0x1;
);

/// The number of render states in DX8RENDERER's table.
pub const RENDER_STATES: usize = 76;

/// The number of texture stages the model stores (record 0004, answer 6).
pub const STAGES: usize = 8;

/// One more than the largest Direct3D 8 texture-stage state number.
pub const STAGE_STATE_SLOTS: usize = 0x1D;

/// One more than the largest pipeline property number `GetPipelineState`
/// reads (0 to 0xE).
pub const PIPELINE_SLOTS: usize = 0xF;

/// A pipeline property (`kPipelineProperties`): renderer switches the
/// client sets with `SetPipelineState`, read from the engine INI.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PipelineProperty {
    Texture,
    Lighting,
    TextureLod,
    TextureAllow8Bit,
    TextureAllow32Bit,
    TextureAllowDxt,
    /// Id 6, named `BROKEN_MULTITEXTURE` in the table. Once the device
    /// caps are applied (`ApplyDeviceCaps`), DX8RENDERER keeps the texture
    /// stage count there (`MaxTextureBlendStages`, at most 7), and its
    /// getter returns that; the defaults line carries it. The model only
    /// stores and returns the value.
    BrokenMultitexture,
    /// While on, `set_modelview` flips Z into the world matrix.
    ViewspaceLights,
    ClearColor,
    ClearZDepth,
    ClearStencil,
    StateCache,
    BrokenMod2x,
    BrokenFlip,
}

impl PipelineProperty {
    /// In the order of the C++ table.
    pub const ALL: [PipelineProperty; 14] = [
        PipelineProperty::Texture,
        PipelineProperty::Lighting,
        PipelineProperty::ViewspaceLights,
        PipelineProperty::StateCache,
        PipelineProperty::ClearColor,
        PipelineProperty::ClearZDepth,
        PipelineProperty::ClearStencil,
        PipelineProperty::TextureLod,
        PipelineProperty::TextureAllow8Bit,
        PipelineProperty::TextureAllow32Bit,
        PipelineProperty::TextureAllowDxt,
        PipelineProperty::BrokenMultitexture,
        PipelineProperty::BrokenMod2x,
        PipelineProperty::BrokenFlip,
    ];

    /// The number `SetPipelineState` takes.
    #[must_use]
    pub fn raw(self) -> u32 {
        match self {
            PipelineProperty::Texture => 0x0,
            PipelineProperty::Lighting => 0x1,
            PipelineProperty::TextureLod => 0x2,
            PipelineProperty::TextureAllow8Bit => 0x3,
            PipelineProperty::TextureAllow32Bit => 0x4,
            PipelineProperty::TextureAllowDxt => 0x5,
            PipelineProperty::BrokenMultitexture => 0x6,
            PipelineProperty::ViewspaceLights => 0x7,
            PipelineProperty::ClearColor => 0x8,
            PipelineProperty::ClearZDepth => 0x9,
            PipelineProperty::ClearStencil => 0xA,
            PipelineProperty::StateCache => 0xB,
            PipelineProperty::BrokenMod2x => 0xC,
            PipelineProperty::BrokenFlip => 0xD,
        }
    }

    #[must_use]
    pub fn from_raw(raw: u32) -> Option<Self> {
        Self::ALL.into_iter().find(|p| p.raw() == raw)
    }

    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            PipelineProperty::Texture => "TEXTURE",
            PipelineProperty::Lighting => "LIGHTING",
            PipelineProperty::TextureLod => "TEXTURE_LOD",
            PipelineProperty::TextureAllow8Bit => "TEXTURE_ALLOW_8BIT",
            PipelineProperty::TextureAllow32Bit => "TEXTURE_ALLOW_32BIT",
            PipelineProperty::TextureAllowDxt => "TEXTURE_ALLOW_DXT",
            PipelineProperty::BrokenMultitexture => "BROKEN_MULTITEXTURE",
            PipelineProperty::ViewspaceLights => "VIEWSPACE_LIGHTS",
            PipelineProperty::ClearColor => "CLEAR_COLOR",
            PipelineProperty::ClearZDepth => "CLEAR_ZDEPTH",
            PipelineProperty::ClearStencil => "CLEAR_STENCIL",
            PipelineProperty::StateCache => "STATE_CACHE",
            PipelineProperty::BrokenMod2x => "BROKEN_MOD2X",
            PipelineProperty::BrokenFlip => "BROKEN_FLIP",
        }
    }

    /// The table's built-in default.
    #[must_use]
    pub fn builtin_default(self) -> u32 {
        match self {
            PipelineProperty::Texture
            | PipelineProperty::Lighting
            | PipelineProperty::ViewspaceLights
            | PipelineProperty::StateCache
            | PipelineProperty::TextureLod
            | PipelineProperty::TextureAllow32Bit
            | PipelineProperty::TextureAllowDxt
            | PipelineProperty::BrokenMultitexture => 1,
            PipelineProperty::ClearZDepth => 0x3F80_0000,
            PipelineProperty::ClearStencil => 0xFFFF_FFFF,
            PipelineProperty::ClearColor
            | PipelineProperty::TextureAllow8Bit
            | PipelineProperty::BrokenMod2x
            | PipelineProperty::BrokenFlip => 0,
        }
    }

    /// A hardware-quirk switch, whose INI key the port ignores
    /// (decision T22, divergence D202).
    #[must_use]
    pub fn is_hardware_quirk(self) -> bool {
        matches!(
            self,
            PipelineProperty::TextureAllow8Bit
                | PipelineProperty::TextureAllow32Bit
                | PipelineProperty::TextureAllowDxt
                | PipelineProperty::BrokenMultitexture
                | PipelineProperty::BrokenMod2x
                | PipelineProperty::BrokenFlip
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tables_have_the_reference_sizes() {
        assert_eq!(RenderStateId::ALL.len(), RENDER_STATES);
        assert_eq!(RENDER_STATES, 76);
        assert_eq!(StageStateId::ALL.len(), 27);
        for (i, id) in RenderStateId::ALL.iter().enumerate() {
            assert_eq!(id.index(), i);
            assert_eq!(RenderStateId::from_raw(id.raw()), Some(*id));
        }
        for id in StageStateId::ALL {
            assert!((id.raw() as usize) < STAGE_STATE_SLOTS);
            assert_eq!(StageStateId::from_raw(id.raw()), Some(*id));
        }
        assert_eq!(RenderStateId::from_raw(0x89), Some(RenderStateId::Lighting));
        assert_eq!(RenderStateId::from_raw(0x8A), None);
    }

    #[test]
    fn raw_values_round_trip() {
        for raw in [0, 1, 5, 8, 9, 0x8000_0000, u32::MAX] {
            for id in RenderStateId::ALL {
                let back = RenderState::from_raw(*id, raw).to_raw();
                // A BOOL state reads any non-zero value as true.
                let is_bool = RenderState::from_raw(*id, 2).to_raw() == 1;
                let want = if is_bool { u32::from(raw != 0) } else { raw };
                assert_eq!(back, want, "{id:?}");
            }
            for id in StageStateId::ALL {
                assert_eq!(StageState::from_raw(*id, raw).to_raw(), raw, "{id:?}");
            }
        }
        assert_eq!(
            RenderState::from_raw(RenderStateId::ZFunc, 4),
            RenderState::ZFunc(Compare::LessEqual)
        );
        assert_eq!(
            RenderState::from_raw(RenderStateId::ZFunc, 99),
            RenderState::ZFunc(Compare::Other(99))
        );
        let arg = TextureArg::from_raw(0x32);
        assert_eq!(arg.source, ArgSource::Texture);
        assert!(arg.complement && arg.alpha_replicate);
        let index = TexCoordIndex::from_raw(0x0003_0001);
        assert_eq!(index.index, 1);
        assert_eq!(index.generate, TexCoordGen::CameraSpaceReflection);
    }

    #[test]
    fn pipeline_properties() {
        for p in PipelineProperty::ALL {
            assert_eq!(PipelineProperty::from_raw(p.raw()), Some(p));
            assert!((p.raw() as usize) < PIPELINE_SLOTS);
        }
        assert_eq!(PipelineProperty::from_raw(0xE), None);
    }
}
