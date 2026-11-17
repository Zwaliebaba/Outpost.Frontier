//! Material data (work package P1.7): the material kinds, their property
//! schemas and how a material node's leaves become property values, the
//! enumeration of a `Material library` node, and the `MaterialAnim`
//! key data. Ported from `CppSource/Shading/Material.cpp`
//! (`kMaterialTypes`, the schemas, `InitializeOwnedSchema`,
//! `LoadDeclaredSchema`, `StoreParameterValue`, `GetParameterAddress`,
//! `GetParameterValue`), `MaterialLibrary.cpp` (`LoadFromFileSystem`) and
//! `MaterialAnimation.cpp` (`LoadAnimationGroup`, `ParseMaKeys`).
//!
//! Only data: binding, submission, batching, texture resolution and the
//! animation clock are work package P4.5. The `[MaterialMap]` rules of
//! the engine INI, which can replace a material's type by its name or
//! type before it is created, are part of the material library and are
//! not applied here: a node is parsed as the kind its `Type` leaf names.

pub mod anim;
pub mod schema;

use crate::hash::name_crc32;
use crate::image::library::x87_float_copy;
use crate::vfs::{FileSystem, FsDesc};

pub use anim::{MaDelta, MaKey, MaterialAnimation, load_material_animations};
use schema::{PropertyDef, value_type};

/// The bind recipe of a kind (`EMaterialBind`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Bind {
    /// `BindDcDt`.
    DcDt,
    /// `BindLightMap`.
    LightMap,
    /// `BindNebula`.
    Nebula,
    /// `BindAtmosphere`.
    Atmosphere,
    /// `BindDcDtEt`.
    DcDtEt,
    /// `BindEcEt`.
    EcEt,
    /// `BindDetailMap`.
    DetailMap,
    /// `BindBtDetail`.
    BtDetail,
    /// `BindNull`.
    Null,
    /// `BindStub`.
    Stub,
}

/// The submit recipe of a kind (`EMaterialSubmit`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Submit {
    /// `SubmitDcDt`.
    DcDt,
    /// `SubmitDcDtTwo`.
    DcDtTwo,
    /// `SubmitKernel`.
    Kernel,
    /// `SubmitLightMap`.
    LightMap,
    /// `SubmitDetail2Pass`.
    Detail2Pass,
    /// `SubmitIllumDetail2Pass`.
    IllumDetail2Pass,
    /// `SubmitMaskedDetail2Pass`.
    MaskedDetail2Pass,
    /// `SubmitAtmosphere`.
    Atmosphere,
    /// `SubmitNull`.
    Null,
    /// `SubmitBtDetailTwo`.
    BtDetailTwo,
}

/// One of the 22 material kinds of `Shading` (`kMaterialTypes`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MaterialKind {
    /// The factory name a `Type` leaf names.
    pub name: &'static str,
    /// The class name the material reports.
    pub class_name: &'static str,
    /// The bind recipe.
    pub bind: Bind,
    /// The submit recipe.
    pub submit: Submit,
}

const fn kind(
    name: &'static str,
    class_name: &'static str,
    bind: Bind,
    submit: Submit,
) -> MaterialKind {
    MaterialKind {
        name,
        class_name,
        bind,
        submit,
    }
}

/// `kMaterialTypes`, in order.
pub const KINDS: [MaterialKind; 22] = [
    kind("DcDt", "SinglePassMaterial", Bind::DcDt, Submit::DcDt),
    kind("DcDtEc", "SinglePassMaterial", Bind::DcDt, Submit::DcDt),
    kind("DcDtOcOt", "SinglePassMaterial", Bind::DcDt, Submit::DcDt),
    kind("DcDtEcOcOt", "SinglePassMaterial", Bind::DcDt, Submit::DcDt),
    kind(
        "DcDtTwo",
        "SinglePassTwoMaterial",
        Bind::DcDt,
        Submit::DcDtTwo,
    ),
    kind(
        "DcDtEcTwo",
        "SinglePassTwoMaterial",
        Bind::DcDt,
        Submit::DcDtTwo,
    ),
    kind(
        "DcDtOcOtTwo",
        "SinglePassTwoMaterial",
        Bind::DcDt,
        Submit::DcDtTwo,
    ),
    kind(
        "DcDtEcOcOtTwo",
        "SinglePassTwoMaterial",
        Bind::DcDt,
        Submit::DcDtTwo,
    ),
    kind("DcDtEt", "DcDtEt", Bind::DcDtEt, Submit::Kernel),
    kind("EcEt", "EcEt", Bind::EcEt, Submit::Kernel),
    kind("EcEtTwo", "EcEtTwo", Bind::EcEt, Submit::Kernel),
    kind(
        "AtmosphereMaterial",
        "AtmosphereMaterial",
        Bind::Atmosphere,
        Submit::Atmosphere,
    ),
    kind("Nebula", "Nebula", Bind::Nebula, Submit::Kernel),
    kind("NebulaTwo", "NebulaTwo", Bind::Nebula, Submit::Kernel),
    kind("NullMaterial", "NullMaterial", Bind::Null, Submit::Null),
    kind("LightMap", "LightMap", Bind::LightMap, Submit::LightMap),
    kind(
        "DetailMapMaterial",
        "DetailMapMaterial",
        Bind::DetailMap,
        Submit::Kernel,
    ),
    kind(
        "DetailMap2Dm1Msk2PassMaterial",
        "DetailMap2Dm1Msk2PassMaterial",
        Bind::Stub,
        Submit::Detail2Pass,
    ),
    kind(
        "IllumDetailMapMaterial",
        "IllumDetailMapMaterial",
        Bind::Stub,
        Submit::IllumDetail2Pass,
    ),
    kind(
        "Masked2DetailMapMaterial",
        "Masked2DetailMapMaterial",
        Bind::Stub,
        Submit::MaskedDetail2Pass,
    ),
    kind(
        "BtDetailMapMaterial",
        "BtDetailMapMaterial",
        Bind::BtDetail,
        Submit::Kernel,
    ),
    kind(
        "BtDetailMapTwoMaterial",
        "BtDetailMapTwoMaterial",
        Bind::BtDetail,
        Submit::BtDetailTwo,
    ),
];

/// Where a property's value lives in the material (`GetParameterAddress`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Target {
    Ambient,
    Diffuse,
    Emissive,
    Specular,
    SpecularPower,
    Opacity,
    TexName(usize),
    TexFlags(usize),
    TexAddressU(usize),
    TexAddressV(usize),
    FadeOff,
    Alpha,
    Scale,
    FlipU,
    FlipV,
    TileRate0,
    TileRate1,
}

impl MaterialKind {
    /// `CreateMaterialInstance`: the kind whose factory name is exactly
    /// `name`.
    #[must_use]
    pub fn by_name(name: &[u8]) -> Option<Self> {
        KINDS.iter().copied().find(|k| k.name.as_bytes() == name)
    }

    fn is_single_pass_family(self) -> bool {
        KINDS[..8].iter().any(|k| k.name == self.name)
    }

    fn is_detail_family(self) -> bool {
        matches!(
            self.submit,
            Submit::Detail2Pass | Submit::IllumDetail2Pass | Submit::MaskedDetail2Pass
        ) || self.bind == Bind::DetailMap
    }

    /// `GetParameterTable`: the schema the kind walks.
    #[must_use]
    pub fn schema(self) -> &'static [PropertyDef] {
        match (self.bind, self.submit) {
            (Bind::Nebula | Bind::LightMap, _) => &schema::TEXTURE_ONLY,
            (Bind::Atmosphere, _) => &schema::ATMOSPHERE,
            _ if self.is_single_pass_family() => &schema::SINGLE_PASS,
            (Bind::EcEt, _) => &schema::EC_ET,
            (Bind::DcDtEt, _) => &schema::DC_DT_ET,
            (Bind::BtDetail, _) => &schema::BT_DETAIL,
            (Bind::DetailMap, _) => &schema::DETAIL_MAP,
            (_, Submit::Detail2Pass) => &schema::DETAIL_2PASS,
            (_, Submit::IllumDetail2Pass | Submit::MaskedDetail2Pass) => &schema::ILLUM_DETAIL,
            _ => &[],
        }
    }

    fn target(self, index: usize) -> Option<Target> {
        use Target::{
            Alpha, Ambient, Diffuse, Emissive, FadeOff, FlipU, FlipV, Opacity, Scale, Specular,
            SpecularPower, TexAddressU, TexAddressV, TexFlags, TexName, TileRate0, TileRate1,
        };
        let tex = |block: usize, part: usize| match part {
            0 => TexName(block),
            1 => TexFlags(block),
            2 => TexAddressU(block),
            _ => TexAddressV(block),
        };
        let table: &[Target] = match self.bind {
            Bind::Null => return None,
            Bind::Nebula | Bind::LightMap => &[tex(0, 0), tex(0, 1), tex(0, 2), tex(0, 3)],
            Bind::Atmosphere => &[
                Ambient,
                Diffuse,
                tex(0, 0),
                tex(0, 1),
                tex(0, 2),
                tex(0, 3),
                FadeOff,
                Alpha,
                Scale,
            ],
            Bind::EcEt => &[Emissive, tex(0, 0), tex(0, 1), tex(0, 2), tex(0, 3)],
            Bind::DcDtEt => &[
                Diffuse,
                tex(0, 0),
                tex(0, 1),
                tex(0, 2),
                tex(0, 3),
                tex(1, 0),
                tex(1, 1),
                tex(1, 2),
                tex(1, 3),
            ],
            Bind::BtDetail => &[
                Ambient,
                Diffuse,
                Emissive,
                Specular,
                SpecularPower,
                Opacity,
                tex(0, 0),
                tex(0, 1),
                tex(0, 2),
                tex(0, 3),
                tex(1, 0),
                tex(1, 1),
                tex(1, 2),
                tex(1, 3),
            ],
            _ if self.submit == Submit::Detail2Pass || self.bind == Bind::DetailMap => &[
                Ambient,
                Diffuse,
                tex(0, 0),
                tex(0, 1),
                tex(0, 2),
                tex(0, 3),
                FlipU,
                FlipV,
                tex(1, 0),
                tex(1, 1),
                tex(1, 2),
                tex(1, 3),
                TileRate0,
            ],
            _ if matches!(
                self.submit,
                Submit::IllumDetail2Pass | Submit::MaskedDetail2Pass
            ) =>
            {
                &[
                    Ambient,
                    Diffuse,
                    tex(0, 0),
                    tex(0, 1),
                    tex(0, 2),
                    tex(0, 3),
                    FlipU,
                    FlipV,
                    tex(1, 0),
                    tex(1, 1),
                    tex(1, 2),
                    tex(1, 3),
                    TileRate0,
                    tex(2, 0),
                    tex(2, 1),
                    tex(2, 2),
                    tex(2, 3),
                    TileRate1,
                ]
            }
            _ => &[
                Ambient,
                Diffuse,
                Emissive,
                Specular,
                SpecularPower,
                Opacity,
                tex(0, 0),
                tex(0, 1),
                tex(0, 2),
                tex(0, 3),
            ],
        };
        table.get(index).copied()
    }
}

/// A texture block of a material (`FTexBlock`): the fields the schema
/// writes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TexBlock {
    /// The texture name; `None` for a null name.
    pub name: Option<Vec<u8>>,
    /// The wrap mode (initially 4).
    pub flags: u32,
    /// The U address mode.
    pub address_u: u32,
    /// The V address mode.
    pub address_v: u32,
}

/// The material fields the schemas address, floats as bit patterns.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MaterialFields {
    /// Diffuse red, green and blue, then the opacity (contiguous in the
    /// reference, so a four-dword store reaches the opacity).
    pub diffuse: [u32; 4],
    /// Ambient. Storing `Dc` copies `diffuse` here, opacity included.
    pub ambient: [u32; 4],
    /// Specular.
    pub specular: [u32; 4],
    /// Emissive.
    pub emissive: [u32; 4],
    /// Specular power.
    pub specular_power: u32,
    /// One block per texture-name property.
    pub textures: Vec<TexBlock>,
    /// Atmosphere fade (initially 1.0).
    pub fade_off: u32,
    /// Atmosphere alpha (initially 0.75).
    pub alpha: u32,
    /// Atmosphere scale (initially 1.025).
    pub scale: u32,
    /// Detail flip U, 0 or 1 after a store.
    pub flip_u: u32,
    /// Detail flip V, 0 or 1 after a store.
    pub flip_v: u32,
    /// First detail tile rate (initially 4.0).
    pub tile_rate0: u32,
    /// Second detail tile rate (initially 4.0).
    pub tile_rate1: u32,
    /// The blend flag storing `Oc` sets: opacity below 1.
    pub alpha_blend: bool,
}

impl Default for MaterialFields {
    /// The `FMaterial` constructor's values.
    fn default() -> Self {
        let one = 1.0f32.to_bits();
        Self {
            diffuse: [one; 4],
            ambient: [one; 4],
            specular: [0; 4],
            emissive: [0; 4],
            specular_power: 0,
            textures: Vec::new(),
            fade_off: one,
            alpha: 0.75f32.to_bits(),
            scale: 0x3F83_3333,
            flip_u: 0,
            flip_v: 0,
            tile_rate0: 4.0f32.to_bits(),
            tile_rate1: 4.0f32.to_bits(),
            alpha_blend: false,
        }
    }
}

/// A typed property value (`FShadingPropertyValue`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PropertyValue {
    /// The value type.
    pub kind: u32,
    /// The payload dwords of a non-string value (the first one, three or
    /// four are meaningful, see [`schema::SchemaValue::word_count`]).
    pub words: [u32; 4],
    /// The text of a string value; `None` for a null string.
    pub text: Option<Vec<u8>>,
}

impl PropertyValue {
    fn from_schema(value: &schema::SchemaValue) -> Self {
        Self {
            kind: value.kind,
            words: value.words,
            text: value.text.map(<[u8]>::to_vec),
        }
    }
}

/// A material of one of the 22 kinds after `Initialize` and
/// `LoadFromFileSystem`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Material {
    /// The kind.
    pub kind: MaterialKind,
    /// The fields.
    pub fields: MaterialFields,
}

/// The text of a string up to its first NUL.
fn until_nul(bytes: &[u8]) -> &[u8] {
    &bytes[..bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len())]
}

fn read_exact(fs: &dyn FileSystem, name: &[u8], len: usize) -> Option<Vec<u8>> {
    let mut file = fs.open_file(&FsDesc::read(name)).ok()?;
    let mut buf = vec![0u8; len];
    match file.read(&mut buf) {
        Ok(n) if n == len => Some(buf),
        _ => None,
    }
}

fn read_word(fs: &dyn FileSystem, name: &[u8]) -> Option<u32> {
    read_exact(fs, name, 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

impl Material {
    /// `CreateMaterialInstance` and `Initialize`: the constructor's fields,
    /// one texture block per texture-name property, then every property's
    /// initial value stored (`InitializeOwnedSchema`).
    #[must_use]
    pub fn new(kind: MaterialKind) -> Self {
        let schema = kind.schema();
        let blocks = schema
            .iter()
            .filter(|p| p.current.kind == value_type::TEXTURE_NAME)
            .count();
        let mut material = Self {
            kind,
            fields: MaterialFields {
                textures: vec![
                    TexBlock {
                        name: None,
                        flags: 4,
                        address_u: 0,
                        address_v: 0,
                    };
                    blocks
                ],
                ..MaterialFields::default()
            },
        };
        for (index, property) in schema.iter().enumerate() {
            material.store(index, &PropertyValue::from_schema(&property.current));
        }
        material
    }

    /// `LoadDeclaredSchema`: each property not flagged [`schema::FLAG_SKIP`]
    /// is read from the leaf of its name in `fs` (from the open node) and
    /// stored when the read succeeds:
    ///
    /// * a dword: exactly four bytes, as stored;
    /// * a float: four bytes through the x87 (a signalling NaN comes back
    ///   quiet);
    /// * a vector: exactly 12 or 16 bytes;
    /// * a texture name or string: the whole leaf, which must not be empty,
    ///   up to its first NUL (the reference reads past a leaf without one);
    /// * a wrap mode: bits 4 to 7 of a dword; an address mode: bits 0 and 1,
    ///   or 2 and 3 for a record flagged V and not U.
    ///
    /// A missing or short leaf leaves the initial value.
    pub fn load(&mut self, fs: &dyn FileSystem) {
        for (index, property) in self.kind.schema().iter().enumerate() {
            if property.flags & schema::FLAG_SKIP != 0 {
                continue;
            }
            let kind = property.current.kind;
            let mut value = PropertyValue {
                kind,
                words: [0; 4],
                text: None,
            };
            let ok = match kind {
                value_type::WORD => read_word(fs, property.name).map(|w| value.words[0] = w),
                value_type::FLOAT => {
                    read_word(fs, property.name).map(|w| value.words[0] = x87_float_copy(w))
                }
                value_type::VEC3 | value_type::VEC4 => {
                    let count = if kind == value_type::VEC3 { 3 } else { 4 };
                    read_exact(fs, property.name, count * 4).map(|b| {
                        for (word, chunk) in value.words.iter_mut().zip(b.as_chunks::<4>().0) {
                            *word = u32::from_le_bytes(*chunk);
                        }
                    })
                }
                value_type::STRING | value_type::TEXTURE_NAME => {
                    let leaf = fs.open_file(&FsDesc::read(property.name)).ok();
                    leaf.and_then(|mut file| {
                        let size = file.size().ok()? as usize;
                        let mut buf = vec![0u8; size];
                        match file.read(&mut buf) {
                            Ok(n) if n == size => {
                                value.text = Some(until_nul(&buf).to_vec());
                                Some(())
                            }
                            _ => None,
                        }
                    })
                }
                value_type::WRAP_MODE => {
                    read_word(fs, property.name).map(|w| value.words[0] = (w >> 4) & 0xF)
                }
                value_type::ADDRESS_MODE => read_word(fs, property.name).map(|w| {
                    let v = property.flags & schema::FLAG_ADDRESS_U == 0
                        && property.flags & schema::FLAG_ADDRESS_V != 0;
                    value.words[0] = (w >> if v { 2 } else { 0 }) & 3;
                }),
                _ => Some(()),
            };
            if ok.is_some() {
                self.store(index, &value);
            }
        }
    }

    /// `StoreParameterValue`: the value written to its field, then the
    /// effects keyed by the property name (`Dc` copies the diffuse block
    /// to ambient, `Oc` sets the blend flag) and, for the detail kinds,
    /// the flips at indices 6 and 7 reduced to 0 or 1.
    pub fn store(&mut self, index: usize, value: &PropertyValue) {
        let fields = &mut self.fields;
        if let Some(target) = self.kind.target(index) {
            let count = match value.kind {
                value_type::VEC4 => 4,
                value_type::VEC3 => 3,
                value_type::WORD
                | value_type::FLOAT
                | value_type::WRAP_MODE
                | value_type::ADDRESS_MODE
                | 10 => 1,
                _ => 0,
            };
            let words = &value.words[..count];
            let put = |slots: &mut [u32]| {
                for (slot, &word) in slots.iter_mut().zip(words) {
                    *slot = word;
                }
            };
            match target {
                Target::Ambient => put(&mut fields.ambient),
                Target::Diffuse => put(&mut fields.diffuse),
                Target::Emissive => put(&mut fields.emissive),
                Target::Specular => put(&mut fields.specular),
                Target::SpecularPower => put(core::slice::from_mut(&mut fields.specular_power)),
                Target::Opacity => put(&mut fields.diffuse[3..]),
                Target::FadeOff => put(core::slice::from_mut(&mut fields.fade_off)),
                Target::Alpha => put(core::slice::from_mut(&mut fields.alpha)),
                Target::Scale => put(core::slice::from_mut(&mut fields.scale)),
                Target::FlipU => put(core::slice::from_mut(&mut fields.flip_u)),
                Target::FlipV => put(core::slice::from_mut(&mut fields.flip_v)),
                Target::TileRate0 => put(core::slice::from_mut(&mut fields.tile_rate0)),
                Target::TileRate1 => put(core::slice::from_mut(&mut fields.tile_rate1)),
                Target::TexName(i) => {
                    if matches!(value.kind, value_type::STRING | value_type::TEXTURE_NAME) {
                        fields.textures[i].name.clone_from(&value.text);
                    }
                }
                Target::TexFlags(i) => put(core::slice::from_mut(&mut fields.textures[i].flags)),
                Target::TexAddressU(i) => {
                    put(core::slice::from_mut(&mut fields.textures[i].address_u));
                }
                Target::TexAddressV(i) => {
                    put(core::slice::from_mut(&mut fields.textures[i].address_v));
                }
            }
        }
        let name = self.kind.schema().get(index).map_or(&b""[..], |p| p.name);
        if name == b"Dc" {
            fields.ambient = fields.diffuse;
        } else if name == b"Oc" {
            fields.alpha_blend = f32::from_bits(fields.diffuse[3]) < 1.0;
        }
        if self.kind.is_detail_family() {
            if index == 6 {
                fields.flip_u = u32::from(fields.flip_u.cast_signed() > 0);
            } else if index == 7 {
                fields.flip_v = u32::from(fields.flip_v.cast_signed() > 0);
            }
        }
    }

    /// `GetParameterValue(index)`: the property's type and the value read
    /// back from its field; `None` past the schema or for a kind without
    /// fields.
    #[must_use]
    pub fn parameter(&self, index: usize) -> Option<PropertyValue> {
        let property = self.kind.schema().get(index)?;
        let target = self.kind.target(index)?;
        let kind = property.current.kind;
        let fields = &self.fields;
        let words: &[u32] = match target {
            Target::Ambient => &fields.ambient,
            Target::Diffuse => &fields.diffuse,
            Target::Emissive => &fields.emissive,
            Target::Specular => &fields.specular,
            Target::SpecularPower => core::slice::from_ref(&fields.specular_power),
            Target::Opacity => &fields.diffuse[3..],
            Target::FadeOff => core::slice::from_ref(&fields.fade_off),
            Target::Alpha => core::slice::from_ref(&fields.alpha),
            Target::Scale => core::slice::from_ref(&fields.scale),
            Target::FlipU => core::slice::from_ref(&fields.flip_u),
            Target::FlipV => core::slice::from_ref(&fields.flip_v),
            Target::TileRate0 => core::slice::from_ref(&fields.tile_rate0),
            Target::TileRate1 => core::slice::from_ref(&fields.tile_rate1),
            Target::TexName(i) => {
                return Some(PropertyValue {
                    kind,
                    words: [0; 4],
                    text: fields.textures[i].name.clone(),
                });
            }
            Target::TexFlags(i) => core::slice::from_ref(&fields.textures[i].flags),
            Target::TexAddressU(i) => core::slice::from_ref(&fields.textures[i].address_u),
            Target::TexAddressV(i) => core::slice::from_ref(&fields.textures[i].address_v),
        };
        let count = match kind {
            value_type::VEC3 => 3,
            value_type::VEC4 => 4,
            _ => 1,
        };
        let mut value = PropertyValue {
            kind,
            words: [0; 4],
            text: None,
        };
        for (slot, &word) in value.words.iter_mut().zip(words.iter().take(count)) {
            *slot = word;
        }
        Some(value)
    }

    /// Every property value in schema order.
    #[must_use]
    pub fn parameters(&self) -> Vec<PropertyValue> {
        (0..self.kind.schema().len())
            .filter_map(|i| self.parameter(i))
            .collect()
    }
}

/// What enumerating a `Material library` child produced.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MaterialEntry {
    /// A material of the same name (by CRC) was created before; nothing is
    /// read.
    Duplicate,
    /// The child could not be opened as the current node.
    NotOpened,
    /// No `Type` leaf, or an empty one.
    NoType,
    /// The `Type` leaf (up to 0x103 bytes, cut at its first NUL) and, for a
    /// kind of `Shading`, the material.
    Typed {
        /// The type name.
        type_name: Vec<u8>,
        /// The material, `None` for a type no kind has.
        material: Option<Material>,
    },
}

/// One directory child of `Material library`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MaterialNode {
    /// The child's name.
    pub name: Vec<u8>,
    /// What it produced.
    pub entry: MaterialEntry,
}

/// `FMaterialLibrary::LoadFromFileSystem` without the type remapping: the
/// directory children of `Material library` whose names do not start with
/// `.`, in find order. A material is created from the `Type` leaf when one
/// of the 22 kinds has that exact name, then loaded from its node. `None`
/// when `Material library` cannot be opened.
pub fn load_material_library(fs: &mut dyn FileSystem) -> Option<Vec<MaterialNode>> {
    fs.open_node(b"Material library").ok()?;
    let found = fs.find(b"*.*").unwrap_or_default();
    let mut created = std::collections::BTreeSet::new();
    let mut nodes = Vec::new();
    for entry in found {
        let name = entry.data.name.clone();
        if name.first() == Some(&b'.') || !entry.data.is_dir() {
            continue;
        }
        let crc = name_crc32(&name);
        if created.contains(&crc) {
            nodes.push(MaterialNode {
                name,
                entry: MaterialEntry::Duplicate,
            });
            continue;
        }
        if fs.open_node(&name).is_err() {
            nodes.push(MaterialNode {
                name,
                entry: MaterialEntry::NotOpened,
            });
            continue;
        }
        let type_name = fs
            .open_file(&FsDesc::read(b"Type"))
            .ok()
            .and_then(|mut file| {
                let size = (file.size().ok()? as usize).min(0x103);
                let mut buf = vec![0u8; size];
                match file.read(&mut buf) {
                    Ok(n) if n == size => Some(until_nul(&buf).to_vec()),
                    _ => None,
                }
            });
        let entry = match type_name {
            None => MaterialEntry::NoType,
            Some(type_name) => {
                let material = MaterialKind::by_name(&type_name).map(|kind| {
                    let mut material = Material::new(kind);
                    material.load(&*fs);
                    created.insert(crc);
                    material
                });
                MaterialEntry::Typed {
                    type_name,
                    material,
                }
            }
        };
        nodes.push(MaterialNode { name, entry });
        let _ = fs.open_node(b"..");
    }
    let _ = fs.open_node(b"..");
    Some(nodes)
}

#[cfg(test)]
mod tests;
