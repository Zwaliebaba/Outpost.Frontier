//! The vertex input of the fixed-function vertex shader: the input layout
//! for a vertex format, and the defaults vertex that supplies what the
//! format lacks.
//!
//! The shader reads every element a vertex can have. Elements the format
//! has come from slot 0 at their Direct3D 8 offsets ([`Fvf::layout`]);
//! the others come from slot 1, a [`DEFAULTS`] buffer bound with stride 0,
//! so every vertex reads the same values: Direct3D 8's defaults of opaque
//! white diffuse, zero specular and zero normal, weights and coordinates.
//! The point size is in the stride but not read.

use crate::render::ff::fvf::{Element, Fvf};

/// `DXGI_FORMAT` of an input element.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Format {
    R32Float,
    R32G32Float,
    R32G32B32Float,
    R32G32B32A32Float,
    /// `D3DCOLOR` (`0xAARRGGBB` stored little-endian: blue first).
    B8G8R8A8Unorm,
}

impl Format {
    fn floats(n: u8) -> Format {
        match n {
            1 => Format::R32Float,
            2 => Format::R32G32Float,
            3 => Format::R32G32B32Float,
            _ => Format::R32G32B32A32Float,
        }
    }
}

/// One `D3D12_INPUT_ELEMENT_DESC`, per-vertex.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct InputElement {
    pub semantic: &'static str,
    pub index: u32,
    pub format: Format,
    pub slot: u32,
    pub offset: u32,
}

/// Offsets in [`DEFAULTS`].
mod at {
    pub const POSITION: u32 = 0;
    pub const BLEND: u32 = 16;
    pub const NORMAL: u32 = 32;
    pub const DIFFUSE: u32 = 44;
    pub const SPECULAR: u32 = 48;
    pub const TEXCOORD: u32 = 52;
}

/// The size of [`DEFAULTS`].
pub const DEFAULTS_BYTES: usize = 68;

/// The defaults vertex: position (0, 0, 0, 1), weights 0, normal 0, diffuse
/// opaque white, specular 0, coordinates 0.
pub const DEFAULTS: [u8; DEFAULTS_BYTES] = {
    let mut d = [0u8; DEFAULTS_BYTES];
    let one = 1.0_f32.to_bits().to_le_bytes();
    d[12] = one[0];
    d[13] = one[1];
    d[14] = one[2];
    d[15] = one[3];
    d[at::DIFFUSE as usize] = 0xff;
    d[at::DIFFUSE as usize + 1] = 0xff;
    d[at::DIFFUSE as usize + 2] = 0xff;
    d[at::DIFFUSE as usize + 3] = 0xff;
    d
};

/// The texture-coordinate sets the shader reads.
pub const TEXCOORD_SETS: u32 = 8;

/// The input layout for vertices of `fvf`.
#[must_use]
pub fn layout(fvf: Fvf) -> Vec<InputElement> {
    let elements = fvf.layout();
    let find = |pred: &dyn Fn(&Element) -> bool| {
        elements
            .iter()
            .find(|(e, _)| pred(e))
            .map(|&(e, offset)| (e, offset))
    };
    let mut out = Vec::new();
    let mut push = |semantic, index, found: Option<(Format, u32)>, fallback: (Format, u32)| {
        let (format, slot, offset) = match found {
            Some((format, offset)) => (format, 0, offset),
            None => (fallback.0, 1, fallback.1),
        };
        out.push(InputElement {
            semantic,
            index,
            format,
            slot,
            offset,
        });
    };
    let position = find(&|e| matches!(e, Element::Position | Element::PositionRhw)).map(
        |(e, offset)| match e {
            Element::PositionRhw => (Format::R32G32B32A32Float, offset),
            _ => (Format::R32G32B32Float, offset),
        },
    );
    push(
        "POSITION",
        0,
        position,
        (Format::R32G32B32A32Float, at::POSITION),
    );
    let blend = find(&|e| matches!(e, Element::BlendWeights(n) if *n > 0)).map(|(e, offset)| {
        let Element::BlendWeights(n) = e else {
            unreachable!("found by its kind")
        };
        (Format::floats(n.min(4)), offset)
    });
    push(
        "BLENDWEIGHT",
        0,
        blend,
        (Format::R32G32B32A32Float, at::BLEND),
    );
    let normal = find(&|e| *e == Element::Normal).map(|(_, o)| (Format::R32G32B32Float, o));
    push("NORMAL", 0, normal, (Format::R32G32B32Float, at::NORMAL));
    let diffuse = find(&|e| *e == Element::Diffuse).map(|(_, o)| (Format::B8G8R8A8Unorm, o));
    push("COLOR", 0, diffuse, (Format::B8G8R8A8Unorm, at::DIFFUSE));
    let specular = find(&|e| *e == Element::Specular).map(|(_, o)| (Format::B8G8R8A8Unorm, o));
    push("COLOR", 1, specular, (Format::B8G8R8A8Unorm, at::SPECULAR));
    for set in 0..TEXCOORD_SETS {
        let coord = find(
            &|e| matches!(e, Element::TexCoord { set: s, .. } if u32::from(*s) == set),
        )
        .map(|(e, offset)| {
            let Element::TexCoord { floats, .. } = e else {
                unreachable!("found by its kind")
            };
            (Format::floats(floats), offset)
        });
        push(
            "TEXCOORD",
            set,
            coord,
            (Format::R32G32B32A32Float, at::TEXCOORD),
        );
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn present_elements_come_from_the_vertex() {
        // XYZ | NORMAL | DIFFUSE | TEX1.
        let l = layout(Fvf(0x152));
        assert_eq!(l.len(), 5 + TEXCOORD_SETS as usize);
        assert_eq!(
            l[0],
            InputElement {
                semantic: "POSITION",
                index: 0,
                format: Format::R32G32B32Float,
                slot: 0,
                offset: 0
            }
        );
        assert_eq!((l[1].slot, l[1].offset), (1, 16));
        assert_eq!((l[2].slot, l[2].offset), (0, 12));
        assert_eq!((l[3].slot, l[3].offset, l[3].index), (0, 24, 0));
        assert_eq!((l[4].slot, l[4].offset, l[4].index), (1, 48, 1));
        assert_eq!(
            (l[5].slot, l[5].offset, l[5].format),
            (0, 28, Format::R32G32Float)
        );
        assert!(l[6..].iter().all(|e| e.slot == 1 && e.offset == 52));
    }

    #[test]
    fn pretransformed_positions_have_four_floats() {
        let l = layout(Fvf(0x144));
        assert_eq!(l[0].format, Format::R32G32B32A32Float);
        assert_eq!(l[0].slot, 0);
    }

    #[test]
    fn defaults_are_direct3d_8s() {
        assert_eq!(&DEFAULTS[12..16], &1.0_f32.to_le_bytes());
        assert_eq!(&DEFAULTS[44..48], &[0xff; 4]);
        assert_eq!(&DEFAULTS[48..52], &[0; 4]);
    }
}
