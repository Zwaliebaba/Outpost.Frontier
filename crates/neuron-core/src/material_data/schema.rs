//! The property schemas of the material kinds (`Initialize*PropertySchema`
//! in `Material.cpp`): one record per property with its flags, leaf name,
//! description, value type and three typed values. The first value is the
//! initial one; the second and third are the minimum and maximum
//! `ValueWithinDeclaredRange` checks for float and vector properties.

/// Value types of a property (`FShadingPropertyValue::Type`).
pub mod value_type {
    /// A dword (flags, booleans).
    pub const WORD: u32 = 1;
    /// An `f32`.
    pub const FLOAT: u32 = 2;
    /// Three `f32`.
    pub const VEC3: u32 = 3;
    /// Four `f32`.
    pub const VEC4: u32 = 4;
    /// A string.
    pub const STRING: u32 = 5;
    /// A component pointer.
    pub const COMPONENT: u32 = 6;
    /// A texture name (a string that makes a texture block).
    pub const TEXTURE_NAME: u32 = 7;
    /// A wrap mode: bits 4 to 7 of the stored dword.
    pub const WRAP_MODE: u32 = 8;
    /// An address mode: bits 0 and 1 (U) or 2 and 3 (V) of the stored
    /// dword.
    pub const ADDRESS_MODE: u32 = 9;
}

/// Record flag: the property is neither loaded nor saved.
pub const FLAG_SKIP: u32 = 1;
/// Record flag of the U address mode.
pub const FLAG_ADDRESS_U: u32 = 0x2_0000;
/// Record flag of the V address mode.
pub const FLAG_ADDRESS_V: u32 = 0x4_0000;

/// One typed schema value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SchemaValue {
    /// The value type.
    pub kind: u32,
    /// The payload dwords (the first one, three or four are meaningful).
    pub words: [u32; 4],
    /// The text of a string value; `None` for a null string.
    pub text: Option<&'static [u8]>,
}

impl SchemaValue {
    const fn word(kind: u32, value: u32) -> Self {
        Self {
            kind,
            words: [value, 0, 0, 0],
            text: None,
        }
    }

    const fn vec3(bits: u32) -> Self {
        Self {
            kind: value_type::VEC3,
            words: [bits, bits, bits, 0],
            text: None,
        }
    }

    const fn text(text: Option<&'static [u8]>) -> Self {
        Self {
            kind: value_type::TEXTURE_NAME,
            words: [0; 4],
            text,
        }
    }

    /// The number of meaningful payload dwords: 3 or 4 for vectors, 0 for
    /// strings, else 1.
    #[must_use]
    pub const fn word_count(&self) -> usize {
        match self.kind {
            value_type::VEC3 => 3,
            value_type::VEC4 => 4,
            value_type::STRING | value_type::TEXTURE_NAME => 0,
            _ => 1,
        }
    }
}

/// One property record (`FShadingPropertyRecord`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PropertyDef {
    /// Record flags.
    pub flags: u32,
    /// The leaf the value is read from.
    pub name: &'static [u8],
    /// The description.
    pub description: &'static [u8],
    /// The initial value; its type is the property's.
    pub current: SchemaValue,
    /// The minimum (the record's "default" slot).
    pub minimum: SchemaValue,
    /// The maximum (the record's "saved" slot).
    pub maximum: SchemaValue,
}

const ONE: u32 = 0x3F80_0000;
const TEN: u32 = 0x4120_0000;
const FOUR: u32 = 0x4080_0000;
const FLOAT_MIN: u32 = 0xFF7F_FFFF;
const FLOAT_MAX: u32 = 0x7F7F_FFFF;
const NONE_NAME: Option<&[u8]> = Some(b"(None)");

const fn colour(
    name: &'static [u8],
    description: &'static [u8],
    initial: u32,
    max: u32,
) -> PropertyDef {
    PropertyDef {
        flags: 0,
        name,
        description,
        current: SchemaValue::vec3(initial),
        minimum: SchemaValue::vec3(0),
        maximum: SchemaValue::vec3(max),
    }
}

const fn float(name: &'static [u8], description: &'static [u8], values: [u32; 3]) -> PropertyDef {
    PropertyDef {
        flags: 0,
        name,
        description,
        current: SchemaValue::word(value_type::FLOAT, values[0]),
        minimum: SchemaValue::word(value_type::FLOAT, values[1]),
        maximum: SchemaValue::word(value_type::FLOAT, values[2]),
    }
}

const fn texture(name: &'static [u8], description: &'static [u8]) -> PropertyDef {
    PropertyDef {
        flags: 0,
        name,
        description,
        current: SchemaValue::text(NONE_NAME),
        minimum: SchemaValue::text(None),
        maximum: SchemaValue::text(None),
    }
}

const fn wrap(name: &'static [u8], description: &'static [u8]) -> PropertyDef {
    PropertyDef {
        flags: 0,
        name,
        description,
        current: SchemaValue::word(value_type::WRAP_MODE, 4),
        minimum: SchemaValue::word(value_type::WRAP_MODE, 0),
        maximum: SchemaValue::word(value_type::WRAP_MODE, 0),
    }
}

const fn address(
    flags: u32,
    name: &'static [u8],
    description: &'static [u8],
    initial: u32,
) -> PropertyDef {
    PropertyDef {
        flags,
        name,
        description,
        current: SchemaValue::word(value_type::ADDRESS_MODE, initial),
        minimum: SchemaValue::word(value_type::ADDRESS_MODE, 0),
        maximum: SchemaValue::word(value_type::ADDRESS_MODE, 0),
    }
}

const fn flip(name: &'static [u8], description: &'static [u8]) -> PropertyDef {
    PropertyDef {
        flags: 0,
        name,
        description,
        current: SchemaValue::word(value_type::WORD, 0),
        minimum: SchemaValue::word(value_type::WORD, 0),
        maximum: SchemaValue::word(value_type::WORD, 1),
    }
}

/// `GAtmospherePropertySchema`.
pub const ATMOSPHERE: [PropertyDef; 9] = [
    colour(b"Ac", b"Ambient Color", ONE, ONE),
    colour(b"Dc", b"Diffuse Color", ONE, TEN),
    texture(b"Dt_name", b"Fade 1D Texture Name"),
    wrap(b"Dt_flags", b"Fade 1D Texture Wrap Mode"),
    address(
        FLAG_ADDRESS_U,
        b"Dt_flags",
        b"Fade 1D Texture U Address Mode",
        2,
    ),
    address(
        FLAG_ADDRESS_V,
        b"Dt_flags",
        b"Fade 1D Texture V Address Mode",
        2,
    ),
    float(b"Fade", b"Fade Off", [ONE, FLOAT_MIN, FLOAT_MAX]),
    float(b"Alpha", b"Alpha", [0x3F40_0000, FLOAT_MIN, FLOAT_MAX]),
    float(b"Scale", b"Scale", [0x3F83_3333, FLOAT_MIN, FLOAT_MAX]),
];

/// `GSinglePassPropertySchema`.
pub const SINGLE_PASS: [PropertyDef; 10] = [
    colour(b"Ac", b"Ambient Color", ONE, ONE),
    colour(b"Dc", b"Diffuse Color", ONE, ONE),
    colour(b"Ec", b"Emissive Color", 0, ONE),
    colour(b"Sc", b"Specular Color", 0, ONE),
    float(b"Sp", b"Specular Power", [0, 0, ONE]),
    float(b"Oc", b"Opacity ", [ONE, 0, ONE]),
    texture(b"Dt_name", b"Texture Name"),
    wrap(b"Dt_flags", b"Texture Wrap Mode"),
    address(FLAG_ADDRESS_U, b"Dt_flags", b"Texture U Address Mode", 0),
    address(FLAG_ADDRESS_V, b"Dt_flags", b"Texture V Address Mode", 0),
];

/// `InitializeDetailPropertyRecords`: `GIllumDetailPropertySchema` and
/// `GMaskedDetailPropertySchema`.
pub const ILLUM_DETAIL: [PropertyDef; 18] = [
    colour(b"Ac", b"Ambient Color", ONE, ONE),
    colour(b"Dc", b"Diffuse Color", ONE, ONE),
    texture(b"Dt_name", b"Diffuse Texture Name"),
    wrap(b"Dt_flags", b"Diffuse Texture Wrap Mode"),
    address(
        FLAG_ADDRESS_U,
        b"Dt_flags",
        b"Diffuse Texture U Address Mode",
        2,
    ),
    address(
        FLAG_ADDRESS_V,
        b"Dt_flags",
        b"Diffuse Texture V Address Mode",
        2,
    ),
    flip(b"flip u", b"Flip U "),
    flip(b"flip v", b"Flip V "),
    texture(b"Dm0_name", b"Detail Texture Alpha0 Name"),
    wrap(b"Dm0_flags", b"Detail Texture Alpha0 Wrap Mode"),
    address(
        FLAG_ADDRESS_U,
        b"Dm0_flags",
        b"Detail Texture Alpha0 U Address Mode",
        0,
    ),
    address(
        FLAG_ADDRESS_V,
        b"Dm0_flags",
        b"Detail Texture Alpha0 V Address Mode",
        0,
    ),
    float(
        b"TileRate0",
        b"Tile Rate Alpha0",
        [FOUR, FLOAT_MIN, FLOAT_MAX],
    ),
    texture(b"Dm1_name", b"Detail Texture Alpha1 Name"),
    wrap(b"Dm1_flags", b"Detail Texture Alpha1 Wrap Mode"),
    address(
        FLAG_ADDRESS_U,
        b"Dm1_flags",
        b"Detail Texture Alpha1 U Address Mode",
        0,
    ),
    address(
        FLAG_ADDRESS_V,
        b"Dm1_flags",
        b"Detail Texture Alpha1 V Address Mode",
        0,
    ),
    float(
        b"TileRate1",
        b"Tile Rate Alpha1",
        [FOUR, FLOAT_MIN, FLOAT_MAX],
    ),
];

/// `InitializeDetail2PassPropertyRecords`: `GDetail2PassPropertySchema`.
pub const DETAIL_2PASS: [PropertyDef; 13] = [
    colour(b"Ac", b"Ambient Color", ONE, ONE),
    colour(b"Dc", b"Diffuse Color", ONE, TEN),
    texture(b"Dt_name", b"Diffuse Texture Name"),
    wrap(b"Dt_flags", b"Diffuse Texture Wrap Mode"),
    address(
        FLAG_ADDRESS_U,
        b"Dt_flags",
        b"Diffuse Texture U Address Mode",
        2,
    ),
    address(
        FLAG_ADDRESS_V,
        b"Dt_flags",
        b"Diffuse Texture V Address Mode",
        2,
    ),
    flip(b"flip u", b"Flip U "),
    flip(b"flip v", b"Flip V "),
    texture(b"Dm1_name", b"Detail Texture Name"),
    wrap(b"Dm1_flags", b"Detail Texture Wrap Mode"),
    address(
        FLAG_ADDRESS_U,
        b"Dm1_flags",
        b"Detail Texture U Address Mode",
        0,
    ),
    address(
        FLAG_ADDRESS_V,
        b"Dm1_flags",
        b"Detail Texture V Address Mode",
        0,
    ),
    float(b"TileRate", b"Tile Rate ", [FOUR, FLOAT_MIN, FLOAT_MAX]),
];

/// `GDetailMapPropertySchema`.
pub const DETAIL_MAP: [PropertyDef; 13] = [
    colour(b"Ac", b"Ambient Color", ONE, ONE),
    colour(b"Dc", b"Diffuse Color", ONE, TEN),
    texture(b"Dt_name", b"Diffuse Texture Name"),
    wrap(b"Dt_flags", b"Diffuse Texture Wrap Mode"),
    address(
        FLAG_ADDRESS_U,
        b"Dt_flags",
        b"Diffuse Texture U Address Mode",
        2,
    ),
    address(
        FLAG_ADDRESS_V,
        b"Dt_flags",
        b"Diffuse Texture V Address Mode",
        2,
    ),
    flip(b"flip u", b"Flip U "),
    flip(b"flip v", b"Flip V "),
    texture(b"Dm_name", b"Detail Texture Name"),
    wrap(b"Dm_flags", b"Detail Texture Wrap Mode"),
    address(
        FLAG_ADDRESS_U,
        b"Dm_flags",
        b"Detail Texture U Address Mode",
        0,
    ),
    address(
        FLAG_ADDRESS_V,
        b"Dm_flags",
        b"Detail Texture V Address Mode",
        0,
    ),
    float(b"TileRate", b"Tile Rate ", [FOUR, FLOAT_MIN, FLOAT_MAX]),
];

/// `GBtDetailPropertySchema`. The reference writes a fourteenth record
/// (`Bt_flags`, V address mode) past the thirteen it declares, over the
/// first Atmosphere record; the table length the material reports is 13,
/// so the fourteenth is never loaded. The port has the thirteen.
pub const BT_DETAIL: [PropertyDef; 13] = [
    colour(b"Ac", b"Ambient Color", ONE, ONE),
    colour(b"Dc", b"Diffuse Color", ONE, ONE),
    colour(b"Ec", b"Emissive Color", 0, ONE),
    colour(b"Sc", b"Specular Color", 0, ONE),
    float(b"Sp", b"Specular Power", [0, 0, ONE]),
    float(b"Oc", b"Opacity ", [ONE, 0, ONE]),
    texture(b"Dt_name", b"Texture Name"),
    wrap(b"Dt_flags", b"Texture Wrap Mode"),
    address(FLAG_ADDRESS_U, b"Dt_flags", b"Texture U Address Mode", 0),
    address(FLAG_ADDRESS_V, b"Dt_flags", b"Texture V Address Mode", 0),
    texture(b"Bt_name", b"DetailTexture Name"),
    wrap(b"Bt_flags", b"DetailTexture Wrap Mode"),
    address(
        FLAG_ADDRESS_U,
        b"Bt_flags",
        b"DetailTexture U Address Mode",
        0,
    ),
];

/// `InitializeTexturePropertyRecords`: `GNebulaPropertySchema` and
/// `GLightMapPropertySchema`.
pub const TEXTURE_ONLY: [PropertyDef; 4] = [
    texture(b"Dt_name", b"Texture Name"),
    wrap(b"Dt_flags", b"Texture Wrap Mode"),
    address(FLAG_ADDRESS_U, b"Dt_flags", b"Texture U Address Mode", 0),
    address(FLAG_ADDRESS_V, b"Dt_flags", b"Texture V Address Mode", 0),
];

/// `GEcEtPropertySchema`.
pub const EC_ET: [PropertyDef; 5] = [
    PropertyDef {
        flags: 4,
        ..colour(b"Ec", b"Emissive Color", ONE, ONE)
    },
    texture(b"Et_name", b"Emissive Texture Name"),
    wrap(b"Et_flags", b"Emissive Texture Wrap Mode"),
    address(
        FLAG_ADDRESS_U,
        b"Et_flags",
        b"Emissive Texture U Address Mode",
        0,
    ),
    address(
        FLAG_ADDRESS_V,
        b"Et_flags",
        b"Emissive Texture V Address Mode",
        0,
    ),
];

/// `GDcDtEtPropertySchema`.
pub const DC_DT_ET: [PropertyDef; 9] = [
    colour(b"Dc", b"Diffuse Color", ONE, ONE),
    texture(b"Dt_name", b"Diffuse Texture Name"),
    wrap(b"Dt_flags", b"Diffuse Texture Wrap Mode"),
    address(
        FLAG_ADDRESS_U,
        b"Dt_flags",
        b"Diffuse Texture U Address Mode",
        0,
    ),
    address(
        FLAG_ADDRESS_V,
        b"Dt_flags",
        b"Diffuse Texture V Address Mode",
        0,
    ),
    texture(b"Et_name", b"Emissive Texture Name"),
    wrap(b"Et_flags", b"Emissive Texture Wrap Mode"),
    address(
        FLAG_ADDRESS_U,
        b"Et_flags",
        b"Emissive Texture U Address Mode",
        0,
    ),
    address(
        FLAG_ADDRESS_V,
        b"Et_flags",
        b"Emissive Texture V Address Mode",
        0,
    ),
];
