//! The fixed set of static samplers (decision record 0004, point 8) and the
//! index a texture stage's filter and address states select from it.
//!
//! A sampler is a filter class (point or linear minification and
//! magnification with no, point or linear mipmapping; or anisotropic,
//! with or without mipmaps), an address mode per axis and, where an axis
//! uses `BORDER`, one of the three border colours a static sampler can
//! have. Every combination is in the table, so the index of a stage is
//! arithmetic on its states: [`index`]. The backend creates the table
//! once in the root signature; the shader indexes `samplers[]` with the
//! value the constant buffer carries.
//!
//! What a static sampler cannot hold is approximated: a border colour
//! other than transparent black, opaque black or opaque white takes the
//! nearest of the three, and `MIPMAPLODBIAS`, `MAXMIPLEVEL` and
//! `MAXANISOTROPY` keep their defaults (bias 0, every level, anisotropy
//! [`MAX_ANISOTROPY`]).

use crate::render::ff::draw::StageOps;
use crate::render::ff::state::{Color, TextureAddress, TextureFilter};

/// Minification and magnification filters with a mipmap mode, or
/// anisotropic filtering.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FilterClass {
    /// Point or linear minification and magnification.
    MinMag {
        min_linear: bool,
        mag_linear: bool,
        mip: Mip,
    },
    /// Anisotropic; `mipmaps` false clamps to the top level.
    Anisotropic { mipmaps: bool },
}

/// The mipmap mode: `None` samples the top level only.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Mip {
    None,
    Point,
    Linear,
}

/// A static sampler's address mode.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Address {
    Wrap,
    Mirror,
    Clamp,
    Border,
    MirrorOnce,
}

/// The border colours a static sampler can have.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Border {
    TransparentBlack,
    OpaqueBlack,
    OpaqueWhite,
}

/// One static sampler.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct SamplerDesc {
    pub filter: FilterClass,
    pub address_u: Address,
    pub address_v: Address,
    pub border: Border,
}

/// The anisotropy of the anisotropic samplers.
pub const MAX_ANISOTROPY: u32 = 16;

const FILTER_CLASSES: u32 = 14;
const ADDRESSES: u32 = 5;
const BORDERS: u32 = 3;

/// The number of static samplers.
pub const COUNT: u32 = FILTER_CLASSES * ADDRESSES * ADDRESSES * BORDERS;

fn filter_class(c: u32) -> FilterClass {
    match c {
        12 => FilterClass::Anisotropic { mipmaps: false },
        13 => FilterClass::Anisotropic { mipmaps: true },
        _ => FilterClass::MinMag {
            min_linear: c / 6 == 1,
            mag_linear: (c / 3) % 2 == 1,
            mip: [Mip::None, Mip::Point, Mip::Linear][(c % 3) as usize],
        },
    }
}

fn filter_number(f: FilterClass) -> u32 {
    match f {
        FilterClass::Anisotropic { mipmaps } => 12 + u32::from(mipmaps),
        FilterClass::MinMag {
            min_linear,
            mag_linear,
            mip,
        } => {
            u32::from(min_linear) * 6
                + u32::from(mag_linear) * 3
                + match mip {
                    Mip::None => 0,
                    Mip::Point => 1,
                    Mip::Linear => 2,
                }
        }
    }
}

const ADDRESS_ORDER: [Address; 5] = [
    Address::Wrap,
    Address::Mirror,
    Address::Clamp,
    Address::Border,
    Address::MirrorOnce,
];
const BORDER_ORDER: [Border; 3] = [
    Border::TransparentBlack,
    Border::OpaqueBlack,
    Border::OpaqueWhite,
];

/// The sampler at `index`, `0..COUNT`.
#[must_use]
pub fn desc(index: u32) -> SamplerDesc {
    let border = index % BORDERS;
    let v = (index / BORDERS) % ADDRESSES;
    let u = (index / (BORDERS * ADDRESSES)) % ADDRESSES;
    let f = index / (BORDERS * ADDRESSES * ADDRESSES);
    SamplerDesc {
        filter: filter_class(f),
        address_u: ADDRESS_ORDER[u as usize],
        address_v: ADDRESS_ORDER[v as usize],
        border: BORDER_ORDER[border as usize],
    }
}

/// The index of `desc` in the table.
#[must_use]
pub fn number(desc: &SamplerDesc) -> u32 {
    let pos = |a: Address| ADDRESS_ORDER.iter().position(|x| *x == a).unwrap() as u32;
    let border = BORDER_ORDER.iter().position(|x| *x == desc.border).unwrap() as u32;
    ((filter_number(desc.filter) * ADDRESSES + pos(desc.address_u)) * ADDRESSES
        + pos(desc.address_v))
        * BORDERS
        + border
}

fn address(a: TextureAddress) -> Address {
    match a {
        TextureAddress::Mirror => Address::Mirror,
        TextureAddress::Clamp => Address::Clamp,
        TextureAddress::Border => Address::Border,
        TextureAddress::MirrorOnce => Address::MirrorOnce,
        _ => Address::Wrap,
    }
}

fn linear(f: TextureFilter) -> bool {
    matches!(
        f,
        TextureFilter::Linear | TextureFilter::FlatCubic | TextureFilter::GaussianCubic
    )
}

/// The nearest border colour a static sampler has.
#[must_use]
pub fn border(c: Color) -> Border {
    if c.a() < 0x80 {
        Border::TransparentBlack
    } else {
        // Rec. 601 luma of the colour.
        let luma = 299 * u32::from(c.r()) + 587 * u32::from(c.g()) + 114 * u32::from(c.b());
        if luma >= 128 * 1000 {
            Border::OpaqueWhite
        } else {
            Border::OpaqueBlack
        }
    }
}

/// The sampler a stage's states select.
#[must_use]
pub fn select(stage: &StageOps) -> SamplerDesc {
    let mip = match stage.mip_filter {
        TextureFilter::None => Mip::None,
        TextureFilter::Point => Mip::Point,
        TextureFilter::Linear
        | TextureFilter::Anisotropic
        | TextureFilter::FlatCubic
        | TextureFilter::GaussianCubic => Mip::Linear,
        _ => Mip::None,
    };
    let anisotropic = matches!(stage.min_filter, TextureFilter::Anisotropic)
        || matches!(stage.mag_filter, TextureFilter::Anisotropic);
    let filter = if anisotropic {
        FilterClass::Anisotropic {
            mipmaps: mip != Mip::None,
        }
    } else {
        FilterClass::MinMag {
            min_linear: linear(stage.min_filter),
            mag_linear: linear(stage.mag_filter),
            mip,
        }
    };
    let address_u = address(stage.address_u);
    let address_v = address(stage.address_v);
    // The colour matters only where an axis uses it; elsewhere one value
    // keeps the samplers in use few.
    let border = if address_u == Address::Border || address_v == Address::Border {
        border(stage.border)
    } else {
        Border::TransparentBlack
    };
    SamplerDesc {
        filter,
        address_u,
        address_v,
        border,
    }
}

/// The table index of the sampler a stage selects.
#[must_use]
pub fn index(stage: &StageOps) -> u32 {
    number(&select(stage))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_table_round_trips() {
        for i in 0..COUNT {
            assert_eq!(number(&desc(i)), i);
        }
        assert_eq!(COUNT, 1050);
    }

    #[test]
    fn borders_take_the_nearest_static_colour() {
        assert_eq!(border(Color(0)), Border::TransparentBlack);
        assert_eq!(border(Color(0xff00_0000)), Border::OpaqueBlack);
        assert_eq!(border(Color(0xffff_ffff)), Border::OpaqueWhite);
        assert_eq!(border(Color(0xff40_4040)), Border::OpaqueBlack);
        assert_eq!(border(Color(0x7fff_ffff)), Border::TransparentBlack);
    }
}
