//! Flexible vertex format codes as RendComp and Deformable2 read them
//! (`VMeshFvfStride`, `VMeshChannelPresent`, `VMeshChannelByteOffset` in
//! `VMeshLibrary.cpp`; `ComputeFvfStride` in `Deformable2.cpp`, which is
//! the same function).
//!
//! The codes are Direct3D 8 FVF values, but only as numbers: the stride
//! follows the Direct3D sizes, while the channel offsets follow the
//! reference's own table, in which the diffuse colour comes after the
//! texture coordinates and an `XYZB*` position adds a 0x30-byte channel.
//! Both are reproduced as the reference computes them.

/// `D3DFVF_XYZ`.
pub const XYZ: u32 = 0x002;
/// `D3DFVF_NORMAL`.
pub const NORMAL: u32 = 0x010;
/// `D3DFVF_DIFFUSE`.
pub const DIFFUSE: u32 = 0x040;
/// `D3DFVF_SPECULAR`.
pub const SPECULAR: u32 = 0x080;
/// `D3DFVF_TEX1`.
pub const TEX1: u32 = 0x100;
/// `D3DFVF_TEX2`.
pub const TEX2: u32 = 0x200;

/// A vertex channel of the reference's table, by its channel id.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Channel {
    /// Id 1: the position (FVF mask 0xE, 0xC bytes).
    Position,
    /// Id 0x800: the blend weights of an `XYZB*` position (mask 0xC, 0x30
    /// bytes).
    Blend,
    /// Id 8: the normal (mask 0x10, 0xC bytes).
    Normal,
    /// Id 2: texture coordinate set 0 (present with at least one set, 8
    /// bytes).
    Tex0,
    /// Id 4: texture coordinate set 1 (present with at least two sets, 8
    /// bytes).
    Tex1,
    /// Id 0x10: the diffuse colour (mask 0x40, 4 bytes).
    Diffuse,
}

impl Channel {
    /// The table order, which is also the order of the byte offsets.
    pub const ALL: [Self; 6] = [
        Self::Position,
        Self::Blend,
        Self::Normal,
        Self::Tex0,
        Self::Tex1,
        Self::Diffuse,
    ];

    /// The channel id the reference passes around.
    #[must_use]
    pub const fn id(self) -> u32 {
        match self {
            Self::Position => 1,
            Self::Blend => 0x800,
            Self::Normal => 8,
            Self::Tex0 => 2,
            Self::Tex1 => 4,
            Self::Diffuse => 0x10,
        }
    }

    /// The channel with id `id`, if the table has one.
    #[must_use]
    pub fn from_id(id: u32) -> Option<Self> {
        Self::ALL.into_iter().find(|c| c.id() == id)
    }

    /// The table's FVF mask.
    #[must_use]
    pub const fn mask(self) -> u32 {
        match self {
            Self::Position => 0xE,
            Self::Blend => 0xC,
            Self::Normal => 0x10,
            Self::Tex0 => 0x100,
            Self::Tex1 => 0x200,
            Self::Diffuse => 0x40,
        }
    }

    /// The table's element size in bytes.
    #[must_use]
    pub const fn element_bytes(self) -> u32 {
        match self {
            Self::Position | Self::Normal => 0xC,
            Self::Blend => 0x30,
            Self::Tex0 | Self::Tex1 => 8,
            Self::Diffuse => 4,
        }
    }
}

/// An FVF code.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Fvf(pub u32);

impl Fvf {
    /// The format of every Deformable2 mesh vertex buffer (`ParseTag`
    /// 0x112: position, normal, one texture set).
    pub const DEFORMABLE: Self = Self(0x112);

    /// Bytes of the position: 0x1C when both bits of 0xC are set, else
    /// 0x10 with bit 4 (`XYZRHW`), else 0xC with bit 2 (`XYZ`), else 0.
    #[must_use]
    pub const fn position_bytes(self) -> u32 {
        if self.0 & 0xC == 0xC {
            0x1C
        } else if self.0 & 4 == 4 {
            0x10
        } else if self.0 & 2 == 2 {
            0xC
        } else {
            0
        }
    }

    /// The number of texture coordinate sets, bits 8 to 11.
    #[must_use]
    pub const fn tex_count(self) -> u32 {
        (self.0 >> 8) & 0xF
    }

    /// Bytes of texture set `set`: its two bits at 16 + 2 * `set` select
    /// 8, 0xC, 0x10 or 4 bytes. For sets 8 and up the shift passes 31;
    /// the x86 shift the reference compiles to uses its low five bits,
    /// and so does this.
    #[must_use]
    pub const fn tex_coord_bytes(self, set: u32) -> u32 {
        let shift = 16 + 2 * set;
        match (self.0 >> (shift & 31)) & 3 {
            0 => 8,
            1 => 0xC,
            2 => 0x10,
            _ => 4,
        }
    }

    /// `VMeshFvfStride` (and `ComputeFvfStride`): position, normal (0xC),
    /// diffuse and specular (4 each) and every texture set.
    #[must_use]
    pub const fn stride(self) -> u32 {
        let normal = if self.0 & NORMAL != 0 { 0xC } else { 0 };
        let mut colours = 0;
        if self.0 & DIFFUSE != 0 {
            colours += 4;
        }
        if self.0 & SPECULAR != 0 {
            colours += 4;
        }
        let mut textures = 0;
        let mut set = 0;
        while set < self.tex_count() {
            textures += self.tex_coord_bytes(set);
            set += 1;
        }
        textures + colours + normal + self.position_bytes()
    }

    /// `VMeshChannelPresent` for a table channel. A mask without bits in
    /// 0xF00 tests `fvf & mask`; the texture channels need a texture count
    /// of at least the channel id minus 2 and a non-zero count.
    #[must_use]
    pub const fn has_channel(self, channel: Channel) -> bool {
        let mask = channel.mask();
        if mask & 0xF00 == 0 {
            self.0 & mask != 0
        } else if self.0 & 0xF00 == 0 {
            false
        } else {
            ((self.0 & 0xF00) >> 8) >= channel.id() - 2
        }
    }

    /// `VMeshChannelPresent(fvf, id)`: false for an id the table lacks.
    #[must_use]
    pub fn has_channel_id(self, id: u32) -> bool {
        Channel::from_id(id).is_some_and(|c| self.has_channel(c))
    }

    /// `VMeshChannelByteOffset`: 0 for an absent channel, else the element
    /// sizes of the present channels before it in the table.
    #[must_use]
    pub fn channel_offset(self, channel: Channel) -> u32 {
        if !self.has_channel(channel) {
            return 0;
        }
        Channel::ALL
            .into_iter()
            .take_while(|&c| c != channel)
            .filter(|&c| self.has_channel(c))
            .map(Channel::element_bytes)
            .sum()
    }
}

#[cfg(test)]
mod tests {
    use super::{Channel, Fvf};

    #[test]
    fn strides_of_the_formats_in_the_data() {
        for (fvf, stride) in [
            (0x112, 0x20),
            (0x212, 0x28),
            (0x102, 0x14),
            (0x142, 0x18),
            (0x12, 0x18),
            (0x152, 0x24),
            (0x252, 0x2C),
            (0x2, 0xC),
        ] {
            assert_eq!(Fvf(fvf).stride(), stride, "{fvf:#x}");
        }
    }

    #[test]
    fn diffuse_follows_the_textures() {
        let fvf = Fvf(0x142);
        assert_eq!(fvf.channel_offset(Channel::Position), 0);
        assert_eq!(fvf.channel_offset(Channel::Tex0), 0xC);
        assert_eq!(fvf.channel_offset(Channel::Diffuse), 0x14);
        assert!(!fvf.has_channel(Channel::Tex1));
        assert_eq!(fvf.channel_offset(Channel::Tex1), 0);
        let blend = Fvf(0x1C);
        assert!(blend.has_channel(Channel::Blend));
        assert_eq!(blend.channel_offset(Channel::Normal), 0x3C);
        assert_eq!(blend.stride(), 0x28);
    }
}
