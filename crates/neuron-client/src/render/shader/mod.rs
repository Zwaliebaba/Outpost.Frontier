//! What the fixed-function shaders take, built on every host (decision
//! record 0004, points 6 to 8): the constant buffer ([`constants`]), the
//! vertex input layout ([`input`]) and the static sampler table
//! ([`samplers`]). The HLSL is `shaders/fixed_function.hlsl`; the
//! Direct3D 12 backend turns these descriptions into its objects.

pub mod constants;
pub mod input;
pub mod samplers;

/// The fixed-function HLSL, for the layout checks.
#[cfg(test)]
const HLSL: &str = include_str!("../../../shaders/fixed_function.hlsl");

#[cfg(test)]
mod tests {
    use super::constants::{CONSTANT_BYTES, reg};
    use super::*;

    /// The register of `field` in the HLSL's `cbuffer Draw`.
    fn packoffset(field: &str) -> u32 {
        let line = HLSL
            .lines()
            .find(|l| {
                l.contains(": packoffset(c")
                    && l.split(':')
                        .next()
                        .unwrap()
                        .split_whitespace()
                        .last()
                        .map(|n| n.split('[').next().unwrap() == field)
                        == Some(true)
            })
            .unwrap_or_else(|| panic!("{field} is not in cbuffer Draw"));
        let at = line.find("packoffset(c").unwrap() + "packoffset(c".len();
        line[at..].split(')').next().unwrap().parse().unwrap()
    }

    #[test]
    fn the_hlsl_cbuffer_matches_the_packer() {
        let fields = [
            ("worldview", reg::WORLDVIEW),
            ("normal_matrix", reg::NORMAL),
            ("projection", reg::PROJECTION),
            ("tex_transform", reg::TEX_TRANSFORM),
            ("material_diffuse", reg::MATERIAL_DIFFUSE),
            ("material_ambient", reg::MATERIAL_AMBIENT),
            ("material_specular", reg::MATERIAL_SPECULAR),
            ("material_emissive", reg::MATERIAL_EMISSIVE),
            ("global_ambient", reg::GLOBAL_AMBIENT),
            ("scalars", reg::SCALARS),
            ("fog_color", reg::FOG_COLOR),
            ("texture_factor", reg::TEXTURE_FACTOR),
            ("viewport", reg::VIEWPORT),
            ("offsets", reg::OFFSETS),
            ("flags", reg::FLAGS),
            ("sources", reg::SOURCES),
            ("counts", reg::COUNTS),
            ("stage", reg::STAGES),
            ("lights", reg::LIGHTS),
        ];
        for (field, register) in fields {
            assert_eq!(packoffset(field), register, "{field}");
        }
        // The Light struct has as many registers as the packer gives it.
        let light = HLSL
            .split("struct Light")
            .nth(1)
            .unwrap()
            .split("};")
            .next()
            .unwrap();
        assert_eq!(
            light.matches("float4 ").count() as u32,
            reg::LIGHT_REGISTERS
        );
        assert!(CONSTANT_BYTES.is_multiple_of(256));
        assert!(CONSTANT_BYTES >= reg::END as usize * 16);
    }

    #[test]
    fn the_hlsl_shares_the_flag_and_sampler_numbers() {
        use constants::vertex_flags as v;
        let define = |name: &str| -> String {
            HLSL.lines()
                .find(|l| l.starts_with("#define ") && l.split_whitespace().nth(1) == Some(name))
                .unwrap_or_else(|| panic!("#define {name}"))
                .split_whitespace()
                .skip(2)
                .collect::<Vec<_>>()
                .join(" ")
        };
        let shift = |bit: u32| format!("(1u << {})", bit.trailing_zeros());
        for (name, bit) in [
            ("PRETRANSFORMED", v::PRETRANSFORMED),
            ("HAS_NORMAL", v::HAS_NORMAL),
            ("HAS_DIFFUSE", v::HAS_DIFFUSE),
            ("HAS_SPECULAR", v::HAS_SPECULAR),
            ("LIGHTING", v::LIGHTING),
            ("COLOR_VERTEX", v::COLOR_VERTEX),
            ("SPECULAR_ENABLE", v::SPECULAR_ENABLE),
            ("LOCAL_VIEWER", v::LOCAL_VIEWER),
            ("NORMALIZE_NORMALS", v::NORMALIZE_NORMALS),
            ("TEXTURE0", v::TEXTURE0),
            ("FOG_ENABLE", constants::fog_bits::ENABLE),
            ("FOG_RANGE", constants::fog_bits::RANGE),
        ] {
            assert_eq!(define(name), shift(bit), "{name}");
        }
        assert_eq!(v::TEXTURE1, v::TEXTURE0 << 1);
        assert_eq!(define("SAMPLERS"), samplers::COUNT.to_string());
    }
}
