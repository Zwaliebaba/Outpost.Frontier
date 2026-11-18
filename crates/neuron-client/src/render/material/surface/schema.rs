//! The property schemas of the surface materials (`FlmGlassParamsBase`,
//! `FlmVeilbornParamsBase`, `FlmColorizeParamsBase` and
//! `FlmWireColorDescBase` in `CppSource/surfacematerials/MaterialImpl.cpp`),
//! as the records `neuron_core::material_data::schema` describes: flags,
//! leaf name, description, value type, then the initial value, the minimum
//! and the maximum `ValueWithinDeclaredRange` checks.
//!
//! The glass family's ten records are the records of `Shading`'s
//! single-pass kinds, word for word and string for string, so that schema
//! is reused.

use neuron_core::material_data::schema::{
    FLAG_ADDRESS_U, FLAG_ADDRESS_V, PropertyDef, SINGLE_PASS, SchemaValue, value_type,
};

const ONE: u32 = 0x3F80_0000;

const fn word(kind: u32, value: u32) -> SchemaValue {
    SchemaValue {
        kind,
        words: [value, 0, 0, 0],
        text: None,
    }
}

const fn vec3(bits: u32) -> SchemaValue {
    SchemaValue {
        kind: value_type::VEC3,
        words: [bits, bits, bits, 0],
        text: None,
    }
}

const fn vec4(bits: u32) -> SchemaValue {
    SchemaValue {
        kind: value_type::VEC4,
        words: [bits; 4],
        text: None,
    }
}

const fn text(text: Option<&'static [u8]>) -> SchemaValue {
    SchemaValue {
        kind: value_type::TEXTURE_NAME,
        words: [0; 4],
        text,
    }
}

const fn texture(
    name: &'static [u8],
    description: &'static [u8],
    initial: &'static [u8],
) -> PropertyDef {
    PropertyDef {
        flags: 0,
        name,
        description,
        current: text(Some(initial)),
        minimum: text(None),
        maximum: text(None),
    }
}

const fn wrap(name: &'static [u8], description: &'static [u8], initial: u32) -> PropertyDef {
    PropertyDef {
        flags: 0,
        name,
        description,
        current: word(value_type::WRAP_MODE, initial),
        minimum: word(value_type::WRAP_MODE, 0),
        maximum: word(value_type::WRAP_MODE, 0),
    }
}

const fn address(flags: u32, name: &'static [u8], description: &'static [u8]) -> PropertyDef {
    PropertyDef {
        flags,
        name,
        description,
        current: word(value_type::ADDRESS_MODE, 0),
        minimum: word(value_type::ADDRESS_MODE, 0),
        maximum: word(value_type::ADDRESS_MODE, 0),
    }
}

/// The glass family's schema (`Glass`, `HUDIcon`, `HUDAnim`,
/// `ExclusionZone`, `EngineGlow`, `PlanetWater`): `Shading`'s single-pass
/// schema.
pub const GLASS: &[PropertyDef] = &SINGLE_PASS;

/// `FlmVeilbornParamsBase`: the diffuse colour and two textures, the
/// second named `NomadRGB1_NomadAlpha1` (the name the ship textures hold,
/// decision O30) and wrapped in mode 5 at first.
pub const VEILBORN: [PropertyDef; 9] = [
    PropertyDef {
        flags: 0,
        name: b"Dc",
        description: b"Diffuse Color",
        current: vec3(ONE),
        minimum: vec3(0),
        maximum: vec3(ONE),
    },
    texture(b"Dt_name", b"Diffuse Name", b"(None)"),
    wrap(b"Dt_flags", b"Diffuse Wrap Mode", 4),
    address(FLAG_ADDRESS_U, b"Dt_flags", b"Diffuse U Address Mode"),
    address(FLAG_ADDRESS_V, b"Dt_flags", b"Diffuse V Address Mode"),
    texture(
        b"nt_name",
        b"VeilbornTexture Name",
        b"NomadRGB1_NomadAlpha1",
    ),
    wrap(b"nt_flags", b"VeilbornTexture Wrap Mode", 5),
    address(
        FLAG_ADDRESS_U,
        b"nt_flags",
        b"VeilbornTexture U Address Mode",
    ),
    address(
        FLAG_ADDRESS_V,
        b"nt_flags",
        b"VeilbornTexture V Address Mode",
    ),
];

/// `FlmColorizeParamsBase(false)`: the single-pass schema with a
/// four-component ambient colour, then `Colorize Options`, a dword whose
/// record has no leaf name (its fourth word holds `"flcopt"`). `Cloak`
/// uses the first ten records.
pub const COLORIZE: [PropertyDef; 11] = [
    PropertyDef {
        flags: 0,
        name: b"Ac",
        description: b"Ambient Color",
        current: vec4(ONE),
        minimum: vec4(0),
        maximum: vec4(ONE),
    },
    SINGLE_PASS[1],
    SINGLE_PASS[2],
    SINGLE_PASS[3],
    SINGLE_PASS[4],
    SINGLE_PASS[5],
    SINGLE_PASS[6],
    SINGLE_PASS[7],
    SINGLE_PASS[8],
    SINGLE_PASS[9],
    PropertyDef {
        flags: 0,
        name: b"",
        description: b"Colorize Options",
        current: word(value_type::WORD, 0),
        minimum: word(value_type::WORD, 0),
        maximum: word(value_type::WORD, 0),
    },
];

/// `FlmWireColorDescBase`: one four-component colour, white at first.
pub const WIRE: [PropertyDef; 1] = [PropertyDef {
    flags: 0,
    name: b"Dc",
    description: b"Color",
    current: vec4(ONE),
    minimum: vec4(0),
    maximum: vec4(ONE),
}];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn colorize_starts_with_a_four_component_ambient() {
        assert_eq!(COLORIZE[0].current.kind, value_type::VEC4);
        assert_eq!(COLORIZE[1].name, b"Dc");
        assert_eq!(COLORIZE[5].name, b"Oc");
        assert_eq!(COLORIZE[10].current.kind, value_type::WORD);
        assert_eq!(GLASS.len(), 10);
        assert_eq!(GLASS[6].current.text, Some(&b"(None)"[..]));
    }
}
