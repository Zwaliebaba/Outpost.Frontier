//! DDS files (`MIPS` and `CUBE` texture nodes).
//!
//! The reference hands a `MIPS` file to `D3DXGetImageInfoFromFileInMemory`
//! (D3DX 8.1, statically linked) for its size, mip count and format, then
//! walks the mip chain itself (`LoadDdsTexture`, see
//! [`super::library`]). A `CUBE` file goes whole to
//! `D3DXCreateCubeTextureFromFileInMemory`. [`image_info`] is the D3DX
//! part for the DDS files D3DX accepts; the oracle harness checks it
//! against D3DX itself.
//!
//! Layout, little-endian: `"DDS "`, then the 124-byte header: +0x04 size
//! (124), +0x08 flags, +0x0C height, +0x10 width, +0x14 pitch or linear
//! size, +0x18 depth, +0x1C mip count, +0x4C pixel format (size 32,
//! flags, FourCC, bit count, red, green, blue and alpha masks), +0x6C
//! caps, +0x70 caps2. Offsets count from the start of the file, magic
//! included, as the reference's `Header[i]` dwords do.

/// `"DDS "` as a little-endian dword.
pub const MAGIC: u32 = 0x2053_4444;
/// Bytes of the magic and the header; the pixels start here.
pub const DATA_OFFSET: usize = 0x80;

/// `DDSD_PITCH`.
pub const FLAG_PITCH: u32 = 0x8;
/// `DDSD_MIPMAPCOUNT`.
pub const FLAG_MIPMAPCOUNT: u32 = 0x2_0000;
/// `DDSD_DEPTH`.
pub const FLAG_DEPTH: u32 = 0x80_0000;
/// `DDPF_ALPHAPIXELS`.
pub const PF_ALPHAPIXELS: u32 = 0x1;
/// `DDPF_ALPHA`.
pub const PF_ALPHA: u32 = 0x2;
/// `DDPF_FOURCC`.
pub const PF_FOURCC: u32 = 0x4;
/// `DDPF_PALETTEINDEXED8`.
pub const PF_PALETTEINDEXED8: u32 = 0x20;
/// `DDPF_RGB`.
pub const PF_RGB: u32 = 0x40;
/// `DDPF_LUMINANCE`.
pub const PF_LUMINANCE: u32 = 0x2_0000;
/// `DDSCAPS2_CUBEMAP`.
pub const CAPS2_CUBEMAP: u32 = 0x200;
/// `DDSCAPS2_VOLUME`.
pub const CAPS2_VOLUME: u32 = 0x20_0000;

/// `D3DRTYPE_TEXTURE`.
pub const RTYPE_TEXTURE: u32 = 3;
/// `D3DRTYPE_VOLUMETEXTURE`.
pub const RTYPE_VOLUMETEXTURE: u32 = 4;
/// `D3DRTYPE_CUBETEXTURE`.
pub const RTYPE_CUBETEXTURE: u32 = 5;
/// `D3DXIFF_DDS`.
pub const IFF_DDS: u32 = 4;

/// The dword at byte `offset` of `bytes`; 0 past the end.
#[must_use]
pub fn dword(bytes: &[u8], offset: usize) -> u32 {
    bytes
        .get(offset..offset + 4)
        .map_or(0, |b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

/// The header fields the reference and D3DX read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DdsHeader {
    /// +0x04, 124 in a valid file.
    pub size: u32,
    /// +0x08.
    pub flags: u32,
    /// +0x0C.
    pub height: u32,
    /// +0x10.
    pub width: u32,
    /// +0x14: the pitch with [`FLAG_PITCH`], else the top level's size.
    pub pitch_or_linear_size: u32,
    /// +0x18.
    pub depth: u32,
    /// +0x1C.
    pub mip_map_count: u32,
    /// +0x4C, 32 in a valid file.
    pub pf_size: u32,
    /// +0x50.
    pub pf_flags: u32,
    /// +0x54.
    pub fourcc: u32,
    /// +0x58.
    pub rgb_bit_count: u32,
    /// +0x5C.
    pub red_mask: u32,
    /// +0x60.
    pub green_mask: u32,
    /// +0x64.
    pub blue_mask: u32,
    /// +0x68.
    pub alpha_mask: u32,
    /// +0x6C.
    pub caps: u32,
    /// +0x70.
    pub caps2: u32,
}

impl DdsHeader {
    /// The header of a file that starts with `"DDS "` and has all 128
    /// header bytes.
    #[must_use]
    pub fn parse(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < DATA_OFFSET || dword(bytes, 0) != MAGIC {
            return None;
        }
        Some(Self {
            size: dword(bytes, 0x04),
            flags: dword(bytes, 0x08),
            height: dword(bytes, 0x0C),
            width: dword(bytes, 0x10),
            pitch_or_linear_size: dword(bytes, 0x14),
            depth: dword(bytes, 0x18),
            mip_map_count: dword(bytes, 0x1C),
            pf_size: dword(bytes, 0x4C),
            pf_flags: dword(bytes, 0x50),
            fourcc: dword(bytes, 0x54),
            rgb_bit_count: dword(bytes, 0x58),
            red_mask: dword(bytes, 0x5C),
            green_mask: dword(bytes, 0x60),
            blue_mask: dword(bytes, 0x64),
            alpha_mask: dword(bytes, 0x68),
            caps: dword(bytes, 0x6C),
            caps2: dword(bytes, 0x70),
        })
    }

    /// The Direct3D 8 format the pixel format describes, as D3DX names
    /// it; `None` for one D3DX would not load.
    #[must_use]
    pub fn d3d_format(&self) -> Option<u32> {
        let flags = self.pf_flags;
        if flags & PF_FOURCC != 0 {
            return matches!(
                &self.fourcc.to_le_bytes(),
                b"DXT1" | b"DXT2" | b"DXT3" | b"DXT4" | b"DXT5" | b"UYVY" | b"YUY2"
            )
            .then_some(self.fourcc);
        }
        let alpha = if flags & (PF_ALPHAPIXELS | PF_ALPHA) != 0 {
            self.alpha_mask
        } else {
            0
        };
        let masks = (
            self.rgb_bit_count,
            self.red_mask,
            self.green_mask,
            self.blue_mask,
            alpha,
        );
        if flags & PF_PALETTEINDEXED8 != 0 {
            return (self.rgb_bit_count == 8).then_some(0x29);
        }
        if flags & PF_RGB != 0 {
            return RGB_FORMATS
                .iter()
                .find(|(bits, r, g, b, a, _)| (*bits, *r, *g, *b, *a) == masks)
                .map(|row| row.5);
        }
        if flags & PF_LUMINANCE != 0 {
            return LUMINANCE_FORMATS
                .iter()
                .find(|(bits, l, a, _)| (*bits, *l, *a) == (masks.0, masks.1, masks.4))
                .map(|row| row.3);
        }
        if flags & PF_ALPHA != 0 && self.rgb_bit_count == 8 && self.alpha_mask == 0xFF {
            return Some(28);
        }
        None
    }
}

/// Bit count, red, green, blue and alpha masks, and the `D3DFORMAT`.
const RGB_FORMATS: [(u32, u32, u32, u32, u32, u32); 12] = [
    (24, 0x00FF_0000, 0xFF00, 0xFF, 0, 20),
    (32, 0x00FF_0000, 0xFF00, 0xFF, 0xFF00_0000, 21),
    (32, 0x00FF_0000, 0xFF00, 0xFF, 0, 22),
    (16, 0xF800, 0x07E0, 0x1F, 0, 23),
    (16, 0x7C00, 0x03E0, 0x1F, 0, 24),
    (16, 0x7C00, 0x03E0, 0x1F, 0x8000, 25),
    (16, 0x0F00, 0xF0, 0x0F, 0xF000, 26),
    (8, 0xE0, 0x1C, 0x03, 0, 27),
    (16, 0xE0, 0x1C, 0x03, 0xFF00, 29),
    (16, 0x0F00, 0xF0, 0x0F, 0, 30),
    (32, 0x3FF, 0x000F_FC00, 0x3FF0_0000, 0xC000_0000, 31),
    (32, 0xFFFF, 0xFFFF_0000, 0, 0, 34),
];

/// Bit count, luminance and alpha masks, and the `D3DFORMAT`.
const LUMINANCE_FORMATS: [(u32, u32, u32, u32); 3] = [
    (8, 0xFF, 0, 50),
    (16, 0xFF, 0xFF00, 51),
    (8, 0x0F, 0xF0, 52),
];

/// `D3DXIMAGE_INFO` as D3DX fills it for a DDS file.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ImageInfo {
    /// Width of the top level.
    pub width: u32,
    /// Height of the top level.
    pub height: u32,
    /// Depth: the header's for a volume, else 1.
    pub depth: u32,
    /// The header's mip count when [`FLAG_MIPMAPCOUNT`] is set and it is
    /// not 0, else 1.
    pub mip_levels: u32,
    /// The `D3DFORMAT` or FourCC.
    pub format: u32,
    /// [`RTYPE_TEXTURE`], [`RTYPE_VOLUMETEXTURE`] or [`RTYPE_CUBETEXTURE`].
    pub resource_type: u32,
    /// [`IFF_DDS`].
    pub image_file_format: u32,
}

/// `D3DXGetImageInfoFromFileInMemory` for a DDS file: `None` where the
/// file is not a DDS file D3DX accepts (wrong magic, fewer than 128
/// bytes, a header or pixel-format size other than 124 and 32, or a pixel
/// format D3DX does not know). Only the header is examined.
#[must_use]
pub fn image_info(bytes: &[u8]) -> Option<ImageInfo> {
    let header = DdsHeader::parse(bytes)?;
    if header.size != 124 || header.pf_size != 32 {
        return None;
    }
    let format = header.d3d_format()?;
    let resource_type = if header.caps2 & CAPS2_CUBEMAP != 0 {
        RTYPE_CUBETEXTURE
    } else if header.caps2 & CAPS2_VOLUME != 0 && header.flags & FLAG_DEPTH != 0 {
        RTYPE_VOLUMETEXTURE
    } else {
        RTYPE_TEXTURE
    };
    let depth = if resource_type == RTYPE_VOLUMETEXTURE {
        header.depth.max(1)
    } else {
        1
    };
    let mip_levels = if header.flags & FLAG_MIPMAPCOUNT != 0 && header.mip_map_count != 0 {
        header.mip_map_count
    } else {
        1
    };
    Some(ImageInfo {
        width: header.width,
        height: header.height,
        depth,
        mip_levels,
        format,
        resource_type,
        image_file_format: IFF_DDS,
    })
}
