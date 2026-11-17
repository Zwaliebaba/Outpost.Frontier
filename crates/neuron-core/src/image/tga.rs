//! Uncompressed TGA mip levels as the texture library reads them
//! (`IsTgaHeaderSupported`, `GetTgaSourceFormat` and `DecodeTgaTexture` in
//! `TextureLibrary.cpp`). The reference decodes these itself; it does not
//! call D3DX for them.
//!
//! The 18-byte header, little-endian: +0 identifier length, +1 colour-map
//! type, +2 image type, +3 first colour-map entry (u16), +5 colour-map
//! length (u16), +7 colour-map entry bits, +12 width (i16), +14 height
//! (i16), +16 pixel depth, +17 descriptor (bit 5: top-left origin, bit 4:
//! right-to-left). Run-length types (8 to 11) pass the header check but
//! their packets are copied as raw pixels, as in the reference.

use super::format::{PixelFormat, match_source_pixel_format};

/// Bytes of the TGA header.
pub const HEADER_SIZE: usize = 18;
/// Palette entries the decoder always copies.
pub const PALETTE_ENTRIES: usize = 256;

/// Why a TGA level cannot be decoded.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TgaError {
    /// Fewer than 18 bytes.
    TooShort,
    /// The header fails `IsTgaHeaderSupported`.
    Unsupported,
    /// A negative width or height. The reference allocates a buffer of a
    /// wrapped size and ends with an allocation failure or reads past the
    /// level.
    NegativeSize,
    /// The palette or the pixels the decoder copies run past the end of
    /// the level, or the pixel offset is negative. The reference reads
    /// past the mapped level.
    Truncated,
}

impl core::fmt::Display for TgaError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::TooShort => write!(f, "TGA level shorter than its 18-byte header"),
            Self::Unsupported => write!(f, "unsupported TGA header"),
            Self::NegativeSize => write!(f, "TGA level with a negative width or height"),
            Self::Truncated => write!(f, "TGA level shorter than its pixels"),
        }
    }
}

impl std::error::Error for TgaError {}

/// The fields of the 18-byte header.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TgaHeader {
    /// +0: bytes of the image identifier after the header.
    pub id_length: u8,
    /// +1: colour-map type.
    pub colour_map_type: u8,
    /// +2: image type.
    pub image_type: u8,
    /// +3: first colour-map entry.
    pub colour_map_first: u16,
    /// +5: colour-map length.
    pub colour_map_length: u16,
    /// +7: bits of a colour-map entry.
    pub colour_map_entry_bits: u8,
    /// +8: x origin, not read by the reference.
    pub x_origin: u16,
    /// +10: y origin, not read by the reference.
    pub y_origin: u16,
    /// +12: width, read as a signed 16-bit value.
    pub width: i16,
    /// +14: height, read as a signed 16-bit value.
    pub height: i16,
    /// +16: pixel depth in bits.
    pub pixel_depth: u8,
    /// +17: descriptor.
    pub descriptor: u8,
}

fn u16_at(bytes: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([bytes[at], bytes[at + 1]])
}

impl TgaHeader {
    /// The header of `bytes`; `None` for fewer than 18 bytes.
    #[must_use]
    pub fn parse(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < HEADER_SIZE {
            return None;
        }
        Some(Self {
            id_length: bytes[0],
            colour_map_type: bytes[1],
            image_type: bytes[2],
            colour_map_first: u16_at(bytes, 3),
            colour_map_length: u16_at(bytes, 5),
            colour_map_entry_bits: bytes[7],
            x_origin: u16_at(bytes, 8),
            y_origin: u16_at(bytes, 10),
            width: u16_at(bytes, 12).cast_signed(),
            height: u16_at(bytes, 14).cast_signed(),
            pixel_depth: bytes[16],
            descriptor: bytes[17],
        })
    }

    /// `IsTgaHeaderSupported`: colour-map type 0 or 1, image type 0 to 3
    /// (bit 3, run-length, ignored) and a depth of 8, 15, 16, 24 or 32.
    #[must_use]
    pub fn is_supported(&self) -> bool {
        self.colour_map_type <= 1
            && (self.image_type & 0xF7) <= 3
            && matches!(self.pixel_depth, 8 | 15 | 16 | 24 | 32)
    }

    /// `GetTgaSourceFormat`. The type's low three bits decide: a mapped
    /// type 1 with a colour map is [`PixelFormat::P8`]; otherwise a depth
    /// of 16 is [`PixelFormat::X1R5G5B5`]; a greyscale type 3 matches no
    /// format; otherwise 32 bits (the entry bits for a type 1 without a
    /// map) is [`PixelFormat::A8R8G8B8`] and anything else
    /// [`PixelFormat::R8G8B8`]. A depth of 15 therefore reads as 24-bit,
    /// as in the reference. `None` is the C++ `false` with format 0.
    #[must_use]
    pub fn source_format(&self) -> Option<PixelFormat> {
        let kind = self.image_type & 7;
        if kind == 1 && self.colour_map_type == 1 {
            return match_source_pixel_format(8, 0, 0, 0, 0);
        }
        if self.pixel_depth == 16 {
            return match_source_pixel_format(16, 5, 5, 5, 0);
        }
        if kind == 3 {
            return match_source_pixel_format(8, 8, 0, 0, 0);
        }
        let bits = if kind == 1 {
            self.colour_map_entry_bits
        } else {
            self.pixel_depth
        };
        if bits == 32 {
            match_source_pixel_format(32, 8, 8, 8, 8)
        } else {
            match_source_pixel_format(24, 8, 8, 8, 0)
        }
    }
}

/// One decoded level: the source-format pixels the reference hands to
/// `SetTextureLevelData`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TgaImage {
    /// The source format ([`PixelFormat::UNKNOWN`] where none matches).
    pub format: PixelFormat,
    /// Width from the header.
    pub width: u32,
    /// Height from the header.
    pub height: u32,
    /// Bytes of a row: bytes per pixel of `format` times the width.
    pub pitch: u32,
    /// `height * pitch` bytes. Rows are in the order the reference
    /// produces (see [`decode`]).
    pub pixels: Vec<u8>,
    /// For image type 1: 256 entries of the colour-map entry width with
    /// each entry's bytes reversed (BGR becomes RGB). `None` otherwise.
    pub palette: Option<Vec<u8>>,
}

impl TgaImage {
    /// The palette as the renderer reads it: 256 entries of three bytes.
    /// Bytes the palette does not have (an entry width below three) are 0;
    /// the reference passes uninitialised stack bytes there.
    #[must_use]
    pub fn palette_rgb(&self) -> Option<Vec<u8>> {
        self.palette.as_ref().map(|palette| {
            let mut rgb = vec![0; PALETTE_ENTRIES * 3];
            let n = palette.len().min(rgb.len());
            rgb[..n].copy_from_slice(&palette[..n]);
            rgb
        })
    }
}

/// `DecodeTgaTexture`.
///
/// The palette of a type 1 image is always 256 entries read from after
/// the identifier, whatever the colour-map length says. Pixels start after
/// the identifier, and for type 1 after `(first + length) * 3` more bytes
/// (three whatever the entry width). Rows are then copied:
///
/// * neither descriptor bit 4 nor bit 5: as stored (bottom row first for
///   a standard TGA);
/// * bit 5 only: in reverse row order;
/// * bit 4 with [`PixelFormat::P8`]: each row mirrored, rows reversed
///   unless bit 5 is set;
/// * bit 4 otherwise: each row's bytes reversed (not its pixels), rows
///   reversed unless bit 5 is set.
///
/// # Errors
///
/// [`TgaError`]: a short or unsupported header, a negative size, or bytes
/// the decoder would read past the level.
pub fn decode(bytes: &[u8]) -> Result<TgaImage, TgaError> {
    let header = TgaHeader::parse(bytes).ok_or(TgaError::TooShort)?;
    if !header.is_supported() {
        return Err(TgaError::Unsupported);
    }
    let format = header.source_format().unwrap_or(PixelFormat::UNKNOWN);
    let kind = header.image_type & 7;
    let data_start = HEADER_SIZE + usize::from(header.id_length);

    let palette = if kind == 1 {
        let entry_bytes = usize::from(header.colour_map_entry_bits >> 3);
        let source = bytes
            .get(data_start..data_start + PALETTE_ENTRIES * entry_bytes)
            .ok_or(TgaError::Truncated)?;
        let mut palette = vec![0; PALETTE_ENTRIES * entry_bytes];
        for (out, entry) in palette
            .chunks_exact_mut(entry_bytes.max(1))
            .zip(source.chunks_exact(entry_bytes.max(1)))
        {
            for (byte, value) in out.iter_mut().zip(entry.iter().rev()) {
                *byte = *value;
            }
        }
        Some(palette)
    } else {
        None
    };

    if header.width < 0 || header.height < 0 {
        return Err(TgaError::NegativeSize);
    }
    let width = header.width.cast_unsigned();
    let height = header.height.cast_unsigned();
    let pitch = format.bits_per_pixel().div_ceil(8) * u32::from(width);
    let total = usize::from(height) * pitch as usize;

    let mut source_start = i64::try_from(data_start).unwrap_or(i64::MAX);
    if kind == 1 {
        let skip = i64::from(header.colour_map_first.cast_signed())
            + i64::from(header.colour_map_length.cast_signed());
        source_start += skip * 3;
    }
    let source_start = usize::try_from(source_start).map_err(|_| TgaError::Truncated)?;
    let source = bytes
        .get(source_start..)
        .and_then(|rest| rest.get(..total))
        .ok_or(TgaError::Truncated)?;

    let pitch_bytes = pitch as usize;
    let rows = usize::from(height);
    let top_left = header.descriptor & 0x20 != 0;
    let right_to_left = header.descriptor & 0x10 != 0;
    let mut pixels = vec![0; total];
    if !top_left && !right_to_left {
        pixels.copy_from_slice(source);
    } else if pitch_bytes != 0 {
        for (row, line) in source.chunks_exact(pitch_bytes).enumerate() {
            let destination_row = if right_to_left && top_left {
                row
            } else {
                rows - row - 1
            };
            let out = &mut pixels[destination_row * pitch_bytes..][..pitch_bytes];
            if !right_to_left {
                out.copy_from_slice(line);
            } else if format == PixelFormat::P8 {
                // Pixels mirrored; for P8 a pixel is a byte, so the same
                // as reversing the bytes.
                for (column, &value) in line.iter().enumerate() {
                    out[pitch_bytes - column - 1] = value;
                }
            } else {
                for (index, &value) in line.iter().enumerate() {
                    out[pitch_bytes - index - 1] = value;
                }
            }
        }
    }
    Ok(TgaImage {
        format,
        width: u32::from(width),
        height: u32::from(height),
        pitch,
        pixels,
        palette,
    })
}
