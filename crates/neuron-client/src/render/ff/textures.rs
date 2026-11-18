//! Textures as the renderer interface exposes them (`CreateTexture`,
//! `LockTexture`, `SetTextureLevelData` and the getters in
//! `CppSource/DX8RENDERER/DX8RENDERER.cpp`). Each level's bytes are kept in
//! the texture's own pixel format, as a lock exposes them; the backend
//! converts them when it uploads.
//!
//! Two things depended on the Direct3D 8 driver and are fixed here
//! (divergence D207): the format a texture gets is the one asked for (the
//! C++ asks the driver and falls back along the format class), and
//! `SetTextureLevelData`, which the C++ hands to `D3DXLoadSurfaceFromMemory`,
//! converts through RGBA8 and, when the sizes differ, resamples by area
//! instead of the D3DX triangle filter.

use std::fmt;

use neuron_core::image::PixelFormat;
use neuron_core::image::convert::to_rgba8;

use super::arena::{Arena, Handle};

/// The results the C++ returns for refused texture calls.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TextureError {
    /// Flag 1 (a render target), an invalid handle or source format:
    /// `0xFFFFFFFE`.
    Refused,
    /// `GR_GENERIC` (-1): no usable format, a bad level.
    Generic,
    /// A conversion the port does not make (a compressed target from
    /// another format); the C++ would ask D3DX.
    Unsupported,
    /// The handle names no live texture.
    Stale,
}

impl TextureError {
    #[must_use]
    pub fn code(self) -> u32 {
        match self {
            Self::Refused => 0xFFFF_FFFE,
            Self::Generic | Self::Unsupported | Self::Stale => 0xFFFF_FFFF,
        }
    }
}

impl fmt::Display for TextureError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Refused => "the texture call was refused",
            Self::Generic => "the texture call failed",
            Self::Unsupported => "the conversion is not supported",
            Self::Stale => "the handle names no live texture",
        })
    }
}

impl std::error::Error for TextureError {}

/// The layout of one level: bytes per row (or per row of 4x4 blocks), rows,
/// and total bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LevelLayout {
    pub width: u32,
    pub height: u32,
    pub pitch: u32,
    pub rows: u32,
}

impl LevelLayout {
    #[must_use]
    pub fn bytes(&self) -> usize {
        self.pitch as usize * self.rows as usize
    }
}

/// Bytes per pixel, or per 4x4 block for the DXT formats (negative marks a
/// block size); `None` for a format a texture cannot have.
fn unit_size(format: PixelFormat) -> Option<(u32, bool)> {
    Some(match format {
        PixelFormat::P8 => (1, false),
        PixelFormat::R5G6B5
        | PixelFormat::X1R5G5B5
        | PixelFormat::A4R4G4B4
        | PixelFormat::A1R5G5B5 => (2, false),
        PixelFormat::R8G8B8 => (3, false),
        PixelFormat::A8R8G8B8 | PixelFormat::X8R8G8B8 => (4, false),
        PixelFormat::DXT1 => (8, true),
        PixelFormat::DXT2 | PixelFormat::DXT3 | PixelFormat::DXT4 | PixelFormat::DXT5 => (16, true),
        _ => return None,
    })
}

/// The layout of a `width` by `height` level of `format`.
#[must_use]
pub fn level_layout(format: PixelFormat, width: u32, height: u32) -> Option<LevelLayout> {
    let (size, block) = unit_size(format)?;
    let (cols, rows) = if block {
        (width.div_ceil(4).max(1), height.div_ceil(4).max(1))
    } else {
        (width, height)
    };
    Some(LevelLayout {
        width,
        height,
        pitch: cols * size,
        rows,
    })
}

/// A texture.
#[derive(Clone, Debug)]
pub struct Texture {
    pub width: u32,
    pub height: u32,
    pub format: PixelFormat,
    /// A cube map from `LoadCubemap`: six faces as the levels.
    pub cube: bool,
    levels: Vec<Vec<u8>>,
    locked: Option<u32>,
    /// Levels changed since the backend last saw them, one bit per level.
    dirty: u64,
    /// The backend has not seen the texture yet.
    created: bool,
}

impl Texture {
    #[must_use]
    pub fn level_count(&self) -> u32 {
        self.levels.len() as u32
    }

    /// The layout of `level` (a cube face for a cube map).
    #[must_use]
    pub fn layout(&self, level: u32) -> Option<LevelLayout> {
        if level >= self.level_count() {
            return None;
        }
        let (w, h) = if self.cube {
            (self.width, self.height)
        } else {
            ((self.width >> level).max(1), (self.height >> level).max(1))
        };
        level_layout(self.format, w, h)
    }

    #[must_use]
    pub fn level(&self, level: u32) -> Option<&[u8]> {
        self.levels.get(level as usize).map(Vec::as_slice)
    }
}

pub type TextureHandle = Handle<Texture>;

/// The textures of a device.
#[derive(Debug, Default)]
pub struct Textures {
    textures: Arena<Texture>,
    /// Textures destroyed since the backend last saw the changes.
    released: Vec<TextureHandle>,
}

/// The dirty bits of levels `0..levels`.
fn level_bits(levels: u32) -> u64 {
    if levels >= 64 {
        u64::MAX
    } else {
        (1u64 << levels) - 1
    }
}

/// A change to a texture the backend must apply before the next draw.
#[derive(Clone, Copy, Debug)]
pub enum TextureEvent<'a> {
    /// Level `level` of `texture` changed; `created` is set on the first
    /// event of a new texture, whose description the backend reads from
    /// `desc`.
    Level {
        texture: TextureHandle,
        desc: &'a Texture,
        created: bool,
        level: u32,
        bytes: &'a [u8],
    },
    Release(TextureHandle),
}

/// The number of levels of a full chain.
fn full_chain(width: u32, height: u32) -> u32 {
    32 - width.max(height).max(1).leading_zeros()
}

impl Textures {
    #[must_use]
    pub fn new() -> Textures {
        Textures::default()
    }

    pub fn get(&self, h: TextureHandle) -> Result<&Texture, TextureError> {
        self.textures.get(h).map_err(|_| TextureError::Stale)
    }

    /// `CreateTexture`: flag 1 asks for a render target, which this
    /// interface refuses; `levels` 0 is a full chain.
    pub fn create(
        &mut self,
        width: u32,
        height: u32,
        format: PixelFormat,
        levels: u32,
        flags: u32,
    ) -> Result<TextureHandle, TextureError> {
        if flags & 1 != 0 {
            return Err(TextureError::Refused);
        }
        if width == 0 || height == 0 {
            return Err(TextureError::Generic);
        }
        let chain = full_chain(width, height);
        let levels = if levels == 0 {
            chain
        } else {
            levels.min(chain)
        };
        let mut texture = Texture {
            width,
            height,
            format,
            cube: false,
            levels: Vec::new(),
            locked: None,
            dirty: 0,
            created: true,
        };
        for level in 0..levels {
            let layout =
                texture_layout(format, width, height, level).ok_or(TextureError::Generic)?;
            texture.levels.push(vec![0; layout.bytes()]);
        }
        texture.dirty = level_bits(levels);
        Ok(self.textures.insert(texture))
    }

    /// A cube map of six faces of `size` by `size`.
    pub fn create_cube(
        &mut self,
        size: u32,
        format: PixelFormat,
        faces: [Vec<u8>; 6],
    ) -> Result<TextureHandle, TextureError> {
        let layout = level_layout(format, size, size).ok_or(TextureError::Generic)?;
        if faces.iter().any(|f| f.len() != layout.bytes()) {
            return Err(TextureError::Generic);
        }
        Ok(self.textures.insert(Texture {
            width: size,
            height: size,
            format,
            cube: true,
            levels: faces.into(),
            locked: None,
            dirty: 0b11_1111,
            created: true,
        }))
    }

    pub fn destroy(&mut self, h: TextureHandle) -> Result<(), TextureError> {
        self.textures.remove(h).map_err(|_| TextureError::Stale)?;
        self.released.push(h);
        Ok(())
    }

    /// Hands every change since the last call to `apply`, releases first,
    /// and forgets them.
    pub fn drain_changes(&mut self, mut apply: impl FnMut(TextureEvent<'_>)) {
        for h in self.released.drain(..) {
            apply(TextureEvent::Release(h));
        }
        let dirty: Vec<TextureHandle> = self
            .textures
            .iter()
            .filter(|(_, t)| t.dirty != 0 || t.created)
            .map(|(h, _)| h)
            .collect();
        for h in dirty {
            let t = self.textures.get_mut(h).expect("collected from the arena");
            let bits = std::mem::take(&mut t.dirty);
            let mut created = std::mem::take(&mut t.created);
            let t = self.textures.get(h).expect("collected from the arena");
            for level in 0..t.level_count() {
                if bits & (1 << level.min(63)) != 0 || created {
                    apply(TextureEvent::Level {
                        texture: h,
                        desc: t,
                        created,
                        level,
                        bytes: &t.levels[level as usize],
                    });
                    created = false;
                }
            }
        }
    }

    /// `LockTexture`: the level's layout and bytes to write.
    pub fn lock(
        &mut self,
        h: TextureHandle,
        level: u32,
    ) -> Result<(LevelLayout, &mut [u8]), TextureError> {
        let t = self.textures.get_mut(h).map_err(|_| TextureError::Stale)?;
        let layout = t.layout(level).ok_or(TextureError::Generic)?;
        t.locked = Some(level);
        t.dirty |= 1 << level.min(63);
        Ok((layout, &mut t.levels[level as usize]))
    }

    pub fn unlock(&mut self, h: TextureHandle, level: u32) -> Result<(), TextureError> {
        let t = self.textures.get_mut(h).map_err(|_| TextureError::Stale)?;
        if level >= t.level_count() {
            return Err(TextureError::Generic);
        }
        t.locked = None;
        Ok(())
    }

    /// `SetTextureLevelData`. A level other than 0 is ignored while
    /// `TEXTURE_LOD` is off, as in the C++.
    #[allow(clippy::too_many_arguments)]
    pub fn set_level_data(
        &mut self,
        h: TextureHandle,
        level: u32,
        src_width: u32,
        src_height: u32,
        pitch: u32,
        src_format: PixelFormat,
        pixels: &[u8],
        palette: Option<&[u8]>,
        texture_lod: bool,
    ) -> Result<(), TextureError> {
        if src_format == PixelFormat::UNKNOWN {
            return Err(TextureError::Refused);
        }
        let t = self
            .textures
            .get_mut(h)
            .map_err(|_| TextureError::Refused)?;
        if level != 0 && !texture_lod {
            return Ok(());
        }
        let layout = t.layout(level).ok_or(TextureError::Generic)?;
        t.dirty |= 1 << level.min(63);
        let same_size = layout.width == src_width && layout.height == src_height;
        if same_size && src_format == t.format && palette.is_none() {
            copy_rows(&mut t.levels[level as usize], &layout, pixels, pitch)?;
            return Ok(());
        }
        let rgba = to_rgba8(src_format, src_width, src_height, pitch, pixels, palette)
            .map_err(|_| TextureError::Generic)?;
        let rgba = if same_size {
            rgba
        } else {
            resample_area(&rgba, src_width, src_height, layout.width, layout.height)
        };
        encode(&rgba, t.format, &layout, &mut t.levels[level as usize])
    }

    /// Every live texture.
    pub fn iter(&self) -> impl Iterator<Item = (TextureHandle, &Texture)> {
        self.textures.iter()
    }
}

fn texture_layout(format: PixelFormat, width: u32, height: u32, level: u32) -> Option<LevelLayout> {
    level_layout(format, (width >> level).max(1), (height >> level).max(1))
}

fn copy_rows(
    dst: &mut [u8],
    layout: &LevelLayout,
    src: &[u8],
    pitch: u32,
) -> Result<(), TextureError> {
    let row = layout.pitch as usize;
    for y in 0..layout.rows as usize {
        let from = y * pitch as usize;
        let s = src.get(from..from + row).ok_or(TextureError::Generic)?;
        dst[y * row..(y + 1) * row].copy_from_slice(s);
    }
    Ok(())
}

/// Area-weighted resampling of RGBA8 pixels.
fn resample_area(src: &[u8], sw: u32, sh: u32, dw: u32, dh: u32) -> Vec<u8> {
    let mut out = vec![0u8; dw as usize * dh as usize * 4];
    let sx = f64::from(sw) / f64::from(dw);
    let sy = f64::from(sh) / f64::from(dh);
    for y in 0..dh {
        let y0 = f64::from(y) * sy;
        let y1 = y0 + sy;
        for x in 0..dw {
            let x0 = f64::from(x) * sx;
            let x1 = x0 + sx;
            let mut acc = [0.0f64; 4];
            let mut total = 0.0;
            let mut py = y0.floor() as u32;
            while f64::from(py) < y1 && py < sh {
                let wy = (y1.min(f64::from(py + 1)) - y0.max(f64::from(py))).max(0.0);
                let mut px = x0.floor() as u32;
                while f64::from(px) < x1 && px < sw {
                    let wx = (x1.min(f64::from(px + 1)) - x0.max(f64::from(px))).max(0.0);
                    let w = wx * wy;
                    let at = (py as usize * sw as usize + px as usize) * 4;
                    for c in 0..4 {
                        acc[c] += w * f64::from(src[at + c]);
                    }
                    total += w;
                    px += 1;
                }
                py += 1;
            }
            let at = (y as usize * dw as usize + x as usize) * 4;
            for c in 0..4 {
                out[at + c] = if total > 0.0 {
                    (acc[c] / total).round().clamp(0.0, 255.0) as u8
                } else {
                    0
                };
            }
        }
    }
    out
}

/// Encodes RGBA8 pixels into an uncompressed format, rounding each channel
/// to its width.
fn encode(
    rgba: &[u8],
    format: PixelFormat,
    layout: &LevelLayout,
    dst: &mut [u8],
) -> Result<(), TextureError> {
    fn q(v: u8, bits: u32) -> u32 {
        (u32::from(v) * ((1 << bits) - 1) + 127) / 255
    }
    let (bpp, _) = unit_size(format).ok_or(TextureError::Unsupported)?;
    if matches!(
        format,
        PixelFormat::P8
            | PixelFormat::DXT1
            | PixelFormat::DXT2
            | PixelFormat::DXT3
            | PixelFormat::DXT4
            | PixelFormat::DXT5
    ) {
        return Err(TextureError::Unsupported);
    }
    for y in 0..layout.height as usize {
        for x in 0..layout.width as usize {
            let s = (y * layout.width as usize + x) * 4;
            let [r, g, b, a] = [rgba[s], rgba[s + 1], rgba[s + 2], rgba[s + 3]];
            let value: u32 = match format {
                PixelFormat::R5G6B5 => (q(r, 5) << 11) | (q(g, 6) << 5) | q(b, 5),
                PixelFormat::X1R5G5B5 => (q(r, 5) << 10) | (q(g, 5) << 5) | q(b, 5),
                PixelFormat::A1R5G5B5 => {
                    (u32::from(a >= 128) << 15) | (q(r, 5) << 10) | (q(g, 5) << 5) | q(b, 5)
                }
                PixelFormat::A4R4G4B4 => {
                    (q(a, 4) << 12) | (q(r, 4) << 8) | (q(g, 4) << 4) | q(b, 4)
                }
                PixelFormat::R8G8B8 | PixelFormat::X8R8G8B8 => {
                    (0xff << 24) | (u32::from(r) << 16) | (u32::from(g) << 8) | u32::from(b)
                }
                PixelFormat::A8R8G8B8 => {
                    (u32::from(a) << 24) | (u32::from(r) << 16) | (u32::from(g) << 8) | u32::from(b)
                }
                _ => return Err(TextureError::Unsupported),
            };
            let d = y * layout.pitch as usize + x * bpp as usize;
            dst[d..d + bpp as usize].copy_from_slice(&value.to_le_bytes()[..bpp as usize]);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layouts_by_format() {
        let l = level_layout(PixelFormat::A4R4G4B4, 5, 3).unwrap();
        assert_eq!((l.pitch, l.rows, l.bytes()), (10, 3, 30));
        let d = level_layout(PixelFormat::DXT1, 5, 3).unwrap();
        assert_eq!((d.pitch, d.rows), (16, 1));
        let d5 = level_layout(PixelFormat::DXT5, 1, 1).unwrap();
        assert_eq!(d5.bytes(), 16);
        assert!(level_layout(PixelFormat::DAOT, 4, 4).is_none());
    }

    #[test]
    fn full_chains_and_locks() {
        let mut t = Textures::new();
        assert_eq!(
            t.create(8, 8, PixelFormat::A8R8G8B8, 0, 1),
            Err(TextureError::Refused)
        );
        let h = t.create(8, 2, PixelFormat::A8R8G8B8, 0, 0).unwrap();
        assert_eq!(t.get(h).unwrap().level_count(), 4);
        assert_eq!(t.get(h).unwrap().layout(3).unwrap().width, 1);
        let (layout, bytes) = t.lock(h, 1).unwrap();
        assert_eq!((layout.width, layout.height, layout.pitch), (4, 1, 16));
        bytes[0] = 9;
        t.unlock(h, 1).unwrap();
        assert_eq!(t.get(h).unwrap().level(1).unwrap()[0], 9);
        assert_eq!(t.lock(h, 4).unwrap_err(), TextureError::Generic);
        t.destroy(h).unwrap();
        assert_eq!(t.get(h).unwrap_err(), TextureError::Stale);
    }

    #[test]
    fn level_data_copies_converts_and_resamples() {
        let mut t = Textures::new();
        let h = t.create(2, 2, PixelFormat::A8R8G8B8, 1, 0).unwrap();
        // Same format and size: rows copied with the source pitch.
        let src: Vec<u8> = (0..24).collect();
        t.set_level_data(h, 0, 2, 2, 12, PixelFormat::A8R8G8B8, &src, None, true)
            .unwrap();
        assert_eq!(
            t.get(h).unwrap().level(0).unwrap(),
            &[0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15, 16, 17, 18, 19]
        );
        // A 4x4 white A8R8G8B8 source into the 2x2 level averages to white.
        let white = vec![0xff; 64];
        t.set_level_data(h, 0, 4, 4, 16, PixelFormat::A8R8G8B8, &white, None, true)
            .unwrap();
        assert!(
            t.get(h)
                .unwrap()
                .level(0)
                .unwrap()
                .iter()
                .all(|&b| b == 0xff)
        );
        // Into 4-4-4-4: full white is 0xffff.
        let g = t.create(1, 1, PixelFormat::A4R4G4B4, 1, 0).unwrap();
        t.set_level_data(g, 0, 1, 1, 4, PixelFormat::A8R8G8B8, &[0xff; 4], None, true)
            .unwrap();
        assert_eq!(t.get(g).unwrap().level(0).unwrap(), &[0xff, 0xff]);
        // Level 1 is ignored while TEXTURE_LOD is off.
        assert_eq!(
            t.set_level_data(g, 1, 1, 1, 4, PixelFormat::A8R8G8B8, &[0; 4], None, false),
            Ok(())
        );
        assert_eq!(
            t.set_level_data(g, 0, 1, 1, 4, PixelFormat::UNKNOWN, &[0; 4], None, true),
            Err(TextureError::Refused)
        );
    }

    #[test]
    fn area_resampling_averages() {
        let src = [0, 0, 0, 0, 200, 100, 50, 255];
        let out = resample_area(&src, 2, 1, 1, 1);
        assert_eq!(out, [100, 50, 25, 128]);
    }
}
