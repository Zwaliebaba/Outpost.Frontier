//! Conversion of a level's source pixels to RGBA8.
//!
//! The reference does not convert pixels itself: `SetTextureLevelData`
//! hands the source format, pitch and palette to
//! `D3DXLoadSurfaceFromMemory`, which converts into the texture's format
//! on a device surface. That conversion needs a Direct3D device and is not
//! checked against the oracle. This module defines the port's conversion
//! to straight (not premultiplied) RGBA8, one `[r, g, b, a]` per pixel,
//! rows in the order of the source:
//!
//! * channels narrower than eight bits are widened by bit replication
//!   (`v << (8 - n) | v >> (2n - 8)`, `v * 17` for four bits, `v * 255`
//!   for one); a format without alpha gives 255;
//! * [`PixelFormat::P8`] looks the index up in a 256-entry RGB palette,
//!   alpha 255, as `SetTextureLevelData` builds `PALETTEENTRY` flags 0xFF;
//! * DXT1 to DXT5 decode 4x4 blocks: the two 5-6-5 end points are widened
//!   as above, the third and fourth colours are `(2a + b) / 3` and
//!   `(a + 2b) / 3` (truncated) when the first end point is greater, else
//!   `(a + b) / 2` and transparent black (DXT1 only); DXT2 and DXT3 take
//!   explicit 4-bit alpha, DXT4 and DXT5 the interpolated 8-bit alpha
//!   (`((7 - i) a0 + i a1) / 7`, or `((5 - i) a0 + i a1) / 5` with 0 and
//!   255 when `a0 <= a1`). DXT2 and DXT4 are not un-premultiplied.
//!
//! Row `r` of pixels (or of 4x4 blocks) starts at `r * pitch`, the pitch
//! the loader passes, so the reference's pitch for non-square DXT levels
//! (see [`super::library`]) is reproduced.

use super::format::PixelFormat;

/// Why a level cannot be converted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ConvertError {
    /// A format this module does not convert (0, 9 and the classes).
    Format(PixelFormat),
    /// Fewer bytes than the rows need at this pitch.
    Truncated,
    /// [`PixelFormat::P8`] without a palette of 768 bytes.
    NoPalette,
    /// More than [`MAX_PIXELS`] pixels.
    TooLarge,
}

impl core::fmt::Display for ConvertError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::Format(format) => write!(f, "no RGBA8 conversion for format {}", format.0),
            Self::Truncated => write!(f, "level shorter than its rows"),
            Self::NoPalette => write!(f, "palettised level without a palette"),
            Self::TooLarge => write!(f, "level larger than {MAX_PIXELS} pixels"),
        }
    }
}

impl std::error::Error for ConvertError {}

/// The largest level converted: 2^26 pixels (8192 by 8192).
pub const MAX_PIXELS: u64 = 1 << 26;

/// Widens an `n`-bit value to eight bits by bit replication.
#[must_use]
pub const fn widen(value: u32, bits: u32) -> u8 {
    match bits {
        0 => 0xFF,
        1 => {
            if value & 1 != 0 {
                0xFF
            } else {
                0
            }
        }
        8.. => (value & 0xFF) as u8,
        _ => {
            let mut out = value << (8 - bits);
            let mut have = bits;
            while have < 8 {
                out |= out >> have;
                have *= 2;
            }
            (out & 0xFF) as u8
        }
    }
}

/// The RGBA8 pixels of a level; see the module documentation.
///
/// # Errors
///
/// [`ConvertError`]: an unconverted format, too few bytes, a P8 level
/// without its palette, or more than [`MAX_PIXELS`] pixels.
pub fn to_rgba8(
    format: PixelFormat,
    width: u32,
    height: u32,
    pitch: u32,
    data: &[u8],
    palette: Option<&[u8]>,
) -> Result<Vec<u8>, ConvertError> {
    if u64::from(width) * u64::from(height) > MAX_PIXELS {
        return Err(ConvertError::TooLarge);
    }
    let (w, h, pitch) = (width as usize, height as usize, pitch as usize);
    let mut out = vec![0u8; w * h * 4];
    if w == 0 || h == 0 {
        return Ok(out);
    }
    if format.is_dxt() {
        decode_dxt(format, w, h, pitch, data, &mut out)?;
        return Ok(out);
    }
    let descriptor = format.descriptor();
    let bytes_per_pixel = (descriptor.bits_per_pixel / 8) as usize;
    if bytes_per_pixel == 0 || descriptor.id != format.0 {
        return Err(ConvertError::Format(format));
    }
    let palette = if format == PixelFormat::P8 {
        Some(
            palette
                .filter(|p| p.len() >= 768)
                .ok_or(ConvertError::NoPalette)?,
        )
    } else {
        None
    };
    for y in 0..h {
        let line = data
            .get(y * pitch..)
            .and_then(|rest| rest.get(..w * bytes_per_pixel))
            .ok_or(ConvertError::Truncated)?;
        for (x, pixel) in line.chunks_exact(bytes_per_pixel).enumerate() {
            let value = pixel
                .iter()
                .rev()
                .fold(0u32, |acc, &b| (acc << 8) | u32::from(b));
            let rgba = if let Some(palette) = palette {
                let i = (value & 0xFF) as usize * 3;
                [palette[i], palette[i + 1], palette[i + 2], 0xFF]
            } else {
                let channel = |c: &super::format::ChannelDesc| {
                    if c.bits == 0 {
                        0xFF
                    } else {
                        widen((value & c.mask) >> c.shift, c.bits)
                    }
                };
                [
                    channel(&descriptor.red),
                    channel(&descriptor.green),
                    channel(&descriptor.blue),
                    channel(&descriptor.alpha),
                ]
            };
            out[(y * w + x) * 4..][..4].copy_from_slice(&rgba);
        }
    }
    Ok(out)
}

fn colour_565(value: u16) -> [u32; 3] {
    let v = u32::from(value);
    [
        u32::from(widen(v >> 11, 5)),
        u32::from(widen((v >> 5) & 0x3F, 6)),
        u32::from(widen(v & 0x1F, 5)),
    ]
}

fn colour_block(block: &[u8], allow_transparent: bool) -> [[u8; 4]; 4] {
    let c0 = u16::from_le_bytes([block[0], block[1]]);
    let c1 = u16::from_le_bytes([block[2], block[3]]);
    let (a, b) = (colour_565(c0), colour_565(c1));
    let mix = |f: fn(u32, u32) -> u32| -> [u8; 4] {
        [
            f(a[0], b[0]) as u8,
            f(a[1], b[1]) as u8,
            f(a[2], b[2]) as u8,
            0xFF,
        ]
    };
    let solid = |c: [u32; 3]| [c[0] as u8, c[1] as u8, c[2] as u8, 0xFF];
    if c0 > c1 || !allow_transparent {
        [
            solid(a),
            solid(b),
            mix(|x, y| (2 * x + y) / 3),
            mix(|x, y| (x + 2 * y) / 3),
        ]
    } else {
        [solid(a), solid(b), mix(|x, y| (x + y) / 2), [0, 0, 0, 0]]
    }
}

fn alpha_block(block: &[u8]) -> [u8; 16] {
    let (a0, a1) = (u32::from(block[0]), u32::from(block[1]));
    let mut table = [0u8; 8];
    table[0] = block[0];
    table[1] = block[1];
    if a0 > a1 {
        for i in 1..7u32 {
            table[i as usize + 1] = (((7 - i) * a0 + i * a1) / 7) as u8;
        }
    } else {
        for i in 1..5u32 {
            table[i as usize + 1] = (((5 - i) * a0 + i * a1) / 5) as u8;
        }
        table[6] = 0;
        table[7] = 0xFF;
    }
    let bits = block[2..8]
        .iter()
        .rev()
        .fold(0u64, |acc, &b| (acc << 8) | u64::from(b));
    let mut out = [0u8; 16];
    for (i, alpha) in out.iter_mut().enumerate() {
        *alpha = table[((bits >> (3 * i)) & 7) as usize];
    }
    out
}

fn decode_dxt(
    format: PixelFormat,
    w: usize,
    h: usize,
    pitch: usize,
    data: &[u8],
    out: &mut [u8],
) -> Result<(), ConvertError> {
    let block_bytes = if format == PixelFormat::DXT1 { 8 } else { 16 };
    let blocks_wide = w.div_ceil(4);
    for by in 0..h.div_ceil(4) {
        let line = data
            .get(by * pitch..)
            .and_then(|rest| rest.get(..blocks_wide * block_bytes))
            .ok_or(ConvertError::Truncated)?;
        for (bx, block) in line.chunks_exact(block_bytes).enumerate() {
            let (colours, alpha): ([[u8; 4]; 4], Option<[u8; 16]>) = match format.0 {
                10 => (colour_block(block, true), None),
                11 | 12 => {
                    let mut alpha = [0u8; 16];
                    for (i, a) in alpha.iter_mut().enumerate() {
                        let nibble = (block[i / 2] >> ((i % 2) * 4)) & 0x0F;
                        *a = nibble * 17;
                    }
                    (colour_block(&block[8..], false), Some(alpha))
                }
                _ => (colour_block(&block[8..], false), Some(alpha_block(block))),
            };
            let indices = &block[block_bytes - 4..];
            for py in 0..4 {
                for px in 0..4 {
                    let (x, y) = (bx * 4 + px, by * 4 + py);
                    if x >= w || y >= h {
                        continue;
                    }
                    let i = py * 4 + px;
                    let mut rgba = colours[usize::from((indices[py] >> (px * 2)) & 3)];
                    if let Some(alpha) = alpha {
                        rgba[3] = alpha[i];
                    }
                    out[(y * w + x) * 4..][..4].copy_from_slice(&rgba);
                }
            }
        }
    }
    Ok(())
}
