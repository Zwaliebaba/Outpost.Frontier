//! The texture library's pixel format numbers and their tables
//! (`ShadingInternal.h`, `TextureLibrary.cpp`).
//!
//! A [`PixelFormat`] is the engine's own format number, not a Direct3D
//! one: 1 to 8 are the uncompressed formats, 10 to 14 DXT1 to DXT5, and 15
//! to 21 are "format classes" (`DAOP`, `DAOT`, ...) that the renderer
//! resolves to a concrete format later. [`PixelFormat::d3d_format`] is the
//! `kPixelFormatFourCc` table that maps them to Direct3D 8 `D3DFORMAT`
//! values and FourCC codes.

/// One colour channel of a [`FormatDescriptor`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ChannelDesc {
    /// Bits of the channel.
    pub bits: u32,
    /// The channel's mask in a pixel.
    pub mask: u32,
    /// The channel's lowest bit.
    pub shift: u32,
    /// `8 - bits`, the shift that widens the channel to eight bits.
    pub expansion_shift: u32,
}

impl ChannelDesc {
    const fn new(bits: u32, mask: u32, shift: u32, expansion_shift: u32) -> Self {
        Self {
            bits,
            mask,
            shift,
            expansion_shift,
        }
    }

    const NONE: Self = Self::new(0, 0, 0, 0);
}

/// One row of `GTextureFormatDescriptors` (`TextureLibrary.cpp`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FormatDescriptor {
    /// The format number the row describes.
    pub id: u32,
    /// Bits of a pixel; 0 for the block-compressed formats.
    pub bits_per_pixel: u32,
    /// Red.
    pub red: ChannelDesc,
    /// Green.
    pub green: ChannelDesc,
    /// Blue.
    pub blue: ChannelDesc,
    /// Alpha.
    pub alpha: ChannelDesc,
    /// The alpha flag of the row.
    pub has_alpha: bool,
}

const fn row(
    id: u32,
    bits_per_pixel: u32,
    channels: [ChannelDesc; 4],
    has_alpha: bool,
) -> FormatDescriptor {
    FormatDescriptor {
        id,
        bits_per_pixel,
        red: channels[0],
        green: channels[1],
        blue: channels[2],
        alpha: channels[3],
        has_alpha,
    }
}

const N: ChannelDesc = ChannelDesc::NONE;

/// `GTextureFormatDescriptors`: formats 0 to 14.
pub const DESCRIPTORS: [FormatDescriptor; 15] = [
    row(0, 0, [N, N, N, N], false),
    row(1, 8, [N, N, N, N], false),
    row(
        2,
        24,
        [
            ChannelDesc::new(8, 0x00FF_0000, 16, 0),
            ChannelDesc::new(8, 0xFF00, 8, 0),
            ChannelDesc::new(8, 0xFF, 0, 0),
            N,
        ],
        false,
    ),
    row(
        3,
        16,
        [
            ChannelDesc::new(5, 0xF800, 11, 3),
            ChannelDesc::new(6, 0x07E0, 5, 2),
            ChannelDesc::new(5, 0x1F, 0, 3),
            N,
        ],
        false,
    ),
    row(
        4,
        16,
        [
            ChannelDesc::new(5, 0x7C00, 10, 3),
            ChannelDesc::new(5, 0x03E0, 5, 3),
            ChannelDesc::new(5, 0x1F, 0, 3),
            N,
        ],
        false,
    ),
    row(
        5,
        16,
        [
            ChannelDesc::new(4, 0x0F00, 8, 4),
            ChannelDesc::new(4, 0xF0, 4, 4),
            ChannelDesc::new(4, 0x0F, 0, 4),
            ChannelDesc::new(4, 0xF000, 12, 4),
        ],
        true,
    ),
    row(
        6,
        16,
        [
            ChannelDesc::new(5, 0x7C00, 10, 3),
            ChannelDesc::new(5, 0x03E0, 5, 3),
            ChannelDesc::new(5, 0x1F, 0, 3),
            ChannelDesc::new(1, 0x8000, 15, 7),
        ],
        true,
    ),
    row(
        7,
        32,
        [
            ChannelDesc::new(8, 0x00FF_0000, 16, 0),
            ChannelDesc::new(8, 0xFF00, 8, 0),
            ChannelDesc::new(8, 0xFF, 0, 0),
            ChannelDesc::new(8, 0xFF00_0000, 24, 0),
        ],
        true,
    ),
    row(
        8,
        32,
        [
            ChannelDesc::new(8, 0x00FF_0000, 16, 0),
            ChannelDesc::new(8, 0xFF00, 8, 0),
            ChannelDesc::new(8, 0xFF, 0, 0),
            N,
        ],
        false,
    ),
    row(9, 0, [N, N, N, N], false),
    row(10, 0, [N, N, N, ChannelDesc::new(1, 0, 0, 0)], true),
    row(11, 0, [N, N, N, N], true),
    row(12, 0, [N, N, N, N], true),
    row(13, 0, [N, N, N, N], true),
    row(14, 0, [N, N, N, N], true),
];

/// `kPixelFormatFourCc`: the Direct3D format or FourCC of each format
/// number 0 to 21.
pub const D3D_FORMATS: [u32; 22] = [
    0,
    0x29,
    0x14,
    0x17,
    0x18,
    0x1A,
    0x19,
    0x15,
    0x16,
    0,
    fourcc(*b"DXT1"),
    fourcc(*b"DXT2"),
    fourcc(*b"DXT3"),
    fourcc(*b"DXT4"),
    fourcc(*b"DXT5"),
    fourcc(*b"DAOP"),
    fourcc(*b"DAOT"),
    fourcc(*b"DAAA"),
    fourcc(*b"DAAL"),
    fourcc(*b"DAA1"),
    fourcc(*b"DAA4"),
    fourcc(*b"DAA8"),
];

/// `kPixelFormatAlphaBits`.
const ALPHA_BITS: [u32; 15] = [0, 0, 0, 0, 0, 4, 1, 8, 0, 0, 1, 0, 0, 0, 0];
/// `kPixelFormatHasAlpha`.
const HAS_ALPHA: [bool; 15] = [
    false, false, false, false, false, true, true, true, false, false, true, true, true, true, true,
];

/// Four characters as the little-endian dword a FourCC field holds.
#[must_use]
pub const fn fourcc(code: [u8; 4]) -> u32 {
    u32::from_le_bytes(code)
}

/// An engine pixel format number.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct PixelFormat(pub u32);

impl PixelFormat {
    /// No format (`D3DFMT_UNKNOWN`).
    pub const UNKNOWN: Self = Self(0);
    /// 8-bit palette indices.
    pub const P8: Self = Self(1);
    /// 24-bit RGB, blue in the first byte.
    pub const R8G8B8: Self = Self(2);
    /// 16-bit 5-6-5.
    pub const R5G6B5: Self = Self(3);
    /// 16-bit 5-5-5, top bit unused.
    pub const X1R5G5B5: Self = Self(4);
    /// 16-bit 4-4-4-4.
    pub const A4R4G4B4: Self = Self(5);
    /// 16-bit 5-5-5 with a 1-bit alpha.
    pub const A1R5G5B5: Self = Self(6);
    /// 32-bit ARGB, blue in the first byte.
    pub const A8R8G8B8: Self = Self(7);
    /// 32-bit RGB, top byte unused.
    pub const X8R8G8B8: Self = Self(8);
    /// DXT1 blocks.
    pub const DXT1: Self = Self(10);
    /// DXT2 blocks (premultiplied DXT3).
    pub const DXT2: Self = Self(11);
    /// DXT3 blocks.
    pub const DXT3: Self = Self(12);
    /// DXT4 blocks (premultiplied DXT5).
    pub const DXT4: Self = Self(13);
    /// DXT5 blocks.
    pub const DXT5: Self = Self(14);
    /// The `DAOP` format class: opaque palettised sources.
    pub const DAOP: Self = Self(15);
    /// The `DAOT` format class: opaque sources.
    pub const DAOT: Self = Self(16);
    /// The `DAA1` format class: 1-bit alpha.
    pub const DAA1: Self = Self(19);
    /// The `DAA4` format class: 4-bit alpha.
    pub const DAA4: Self = Self(20);
    /// The `DAA8` format class: 8-bit alpha.
    pub const DAA8: Self = Self(21);

    /// `TextureFormatDescriptor(id)`: the descriptor row, row 0 for numbers
    /// above 14.
    #[must_use]
    pub const fn descriptor(self) -> &'static FormatDescriptor {
        &DESCRIPTORS[if self.0 > 14 { 0 } else { self.0 as usize }]
    }

    /// Bits of a pixel; 0 for block-compressed and class formats.
    #[must_use]
    pub const fn bits_per_pixel(self) -> u32 {
        self.descriptor().bits_per_pixel
    }

    /// Whether the format is one of DXT1 to DXT5.
    #[must_use]
    pub const fn is_dxt(self) -> bool {
        self.0 >= 10 && self.0 <= 14
    }

    /// The Direct3D format or FourCC (`kPixelFormatFourCc`), 0 for numbers
    /// past the table.
    #[must_use]
    pub const fn d3d_format(self) -> u32 {
        if self.0 as usize >= D3D_FORMATS.len() {
            0
        } else {
            D3D_FORMATS[self.0 as usize]
        }
    }

    /// `D3dFormatToTextureFormat`: the first format number whose table
    /// entry is `format`, searching all 22 entries; 0 on a miss. Format 0
    /// itself maps to 0.
    #[must_use]
    pub fn from_d3d_format(format: u32) -> Self {
        D3D_FORMATS
            .iter()
            .position(|&f| f == format)
            .map_or(Self::UNKNOWN, |i| Self(i as u32))
    }

    /// `FourCcToFormatId`: the first of entries 9 to 21 equal to `code`; 0
    /// on a miss. Entry 9 is 0, so a FourCC of 0 maps to 9.
    #[must_use]
    pub fn from_fourcc(code: u32) -> Self {
        (9..D3D_FORMATS.len())
            .find(|&i| D3D_FORMATS[i] == code)
            .map_or(Self::UNKNOWN, |i| Self(i as u32))
    }

    /// `PixelFormatAlphaBits`: 0 outside 0 to 14.
    #[must_use]
    pub const fn alpha_bits(self) -> u32 {
        if self.0 > 14 {
            ALPHA_BITS[0]
        } else {
            ALPHA_BITS[self.0 as usize]
        }
    }

    /// `PixelFormatHasAlpha`: false outside 0 to 14.
    #[must_use]
    pub const fn has_alpha(self) -> bool {
        if self.0 > 14 {
            HAS_ALPHA[0]
        } else {
            HAS_ALPHA[self.0 as usize]
        }
    }

    /// `PixelFormatMatchFields`: bits per pixel and the red, green, blue
    /// and alpha bit counts of the descriptor.
    #[must_use]
    pub const fn match_fields(self) -> [u32; 5] {
        let d = self.descriptor();
        [
            d.bits_per_pixel,
            d.red.bits,
            d.green.bits,
            d.blue.bits,
            d.alpha.bits,
        ]
    }
}

/// `MatchSourcePixelFormat`: the first of the nine physical formats 0 to 8
/// whose bits per pixel, alpha flag and channel widths match. The
/// reference compares `blue` with the descriptor's red width, not its blue
/// width; that is kept. `None` stands for the C++ `false` with format 0.
#[must_use]
pub fn match_source_pixel_format(
    bits: u32,
    red: u32,
    green: u32,
    blue: u32,
    alpha: u32,
) -> Option<PixelFormat> {
    (0..9u32).map(PixelFormat).find(|format| {
        let d = format.descriptor();
        bits == d.bits_per_pixel
            && (alpha != 0) == d.has_alpha
            && red == d.red.bits
            && green == d.green.bits
            && blue == d.red.bits
            && alpha == d.alpha.bits
    })
}
