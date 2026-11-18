//! Large images cut into textures no larger than the device allows (the
//! `BigImage` component, `IBigImage2`, of `CppSource/ximage/ximage.cpp`).
//! The client draws its full-screen images and the rich-text windows'
//! pixel buffers through it.
//!
//! An image of `width` by `height` becomes a grid of tiles of the device's
//! largest texture size; the last column and row hold the remainders, in
//! textures rounded up to powers of two (or full tiles on a square-only
//! device), whose level data is stretched over the whole texture by
//! `SetTextureLevelData`. [`BigImage::draw`] works out each tile's texture
//! rectangle for a source rectangle and its screen rectangle for a
//! destination, as the reference hands them to its draw callback.
//!
//! The image-source path of the reference (`IBigImageSource`) has no
//! caller in the client and is not ported. Arithmetic follows the
//! reference's `double` expressions, rounded to `f32` at each `float`
//! store (decision T11).

use neuron_core::image::{FormatMaps, PixelFormat};

use super::pipeline::{GR_GENERIC, GR_INVALID_PARMS, TexturePipeline};
use super::playback::truncate_frame;

/// A `RECT`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Rect {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}

/// One call of the draw callback (`FBigImageDrawTile`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TileDraw {
    /// The tile's texture.
    pub handle: u32,
    /// `0xAARRGGBB`.
    pub color: u32,
    /// Left, bottom, right, top in pixels, each less half a pixel: the
    /// callback's Y endpoints are those of the destination reversed.
    pub dest: [f32; 4],
    /// The four corners' `u, v`: top left, top right, bottom left, bottom
    /// right.
    pub uv: [f32; 8],
}

/// What a big image is made from (`FBigImageDesc` without an image
/// source).
#[derive(Clone, Copy, Debug)]
pub struct BigImageDesc<'a> {
    /// The format the texture format is picked from (`SrcFormat`).
    pub src_format: PixelFormat,
    /// The format of `pixels` (`DstFormat`).
    pub dst_format: PixelFormat,
    pub pixels: &'a [u8],
    pub width: u32,
    pub height: u32,
    pub pitch: u32,
}

/// `kFormatBits` through `FormatBytes`: bytes per pixel, 0 for block and
/// class formats.
fn format_bytes(format: PixelFormat) -> u32 {
    const BITS: [u32; 15] = [0, 8, 24, 16, 16, 16, 16, 32, 32, 0, 0, 0, 0, 0, 0];
    let index = if format.0 <= 14 { format.0 as usize } else { 0 };
    (BITS[index] + 7) >> 3
}

/// `RoundUpPot`: a power of two stays, 0 stays, anything else rounds up.
/// Above `i32::MAX` the reference shifts a negative value for ever; the
/// port gives 0 there (divergence D263).
#[must_use]
pub fn round_up_pot(value: u32) -> u32 {
    if value > i32::MAX as u32 {
        return 0;
    }
    if value & value.wrapping_sub(1) == 0 {
        return value;
    }
    1 << (32 - value.leading_zeros())
}

/// `NormalizeRect`: `None` for an empty rectangle, else the edges in
/// order.
fn normalize(mut r: Rect) -> Option<Rect> {
    let width = r.right.wrapping_sub(r.left);
    if width == 0 {
        return None;
    }
    let height = r.bottom.wrapping_sub(r.top);
    if height == 0 {
        return None;
    }
    if width < 0 {
        std::mem::swap(&mut r.left, &mut r.right);
    }
    if height <= 0 {
        std::mem::swap(&mut r.top, &mut r.bottom);
    }
    Some(r)
}

/// MSVCRT `_ftol` as the reference uses it: the low 32 bits of the
/// truncated 64-bit integer.
fn ftol_low(value: f64) -> i32 {
    truncate_frame(value).cast_signed()
}

/// A tiled image (`FBigImage`).
#[derive(Clone, Debug, PartialEq)]
pub struct BigImage {
    /// Tile textures in row order, 0 for none.
    handles: Vec<u32>,
    /// Each tile's corners, as [`TileDraw::uv`].
    uvs: Vec<[f32; 8]>,
    tiles_x: u32,
    tiles_y: u32,
    width: u32,
    height: u32,
    packed_rgb: u32,
    alpha: u8,
    /// The texture format `PickGpuFormat` chose.
    format: PixelFormat,
    square_only: u32,
    max_width: u32,
    max_height: u32,
}

impl BigImage {
    /// `Initialize` and `CreateTiles`: one texture of one level per tile,
    /// each filled from its part of `pixels`. On a failure every texture
    /// made so far is destroyed, as releasing the half-made object does.
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` when the format maps have no format for the source, a
    /// texture cannot be made or filled, or the device reports a zero
    /// texture size (where the reference divides by zero).
    pub fn create(
        desc: &BigImageDesc<'_>,
        format_maps: &FormatMaps,
        pipeline: &mut dyn TexturePipeline,
    ) -> Result<BigImage, i32> {
        let abilities = pipeline.abilities();
        let mut image = BigImage {
            handles: Vec::new(),
            uvs: Vec::new(),
            tiles_x: 0,
            tiles_y: 0,
            width: 0,
            height: 0,
            packed_rgb: 0x00FF_FFFF,
            alpha: 0xFF,
            format: PixelFormat::UNKNOWN,
            square_only: abilities.square_only,
            max_width: abilities.max_width,
            max_height: abilities.max_height,
        };
        let Some(format) = format_maps.pick(desc.src_format) else {
            return Err(GR_GENERIC);
        };
        image.format = format;
        if image.max_width == 0 || image.max_height == 0 {
            return Err(GR_GENERIC);
        }
        match image.create_tiles(desc, pipeline) {
            Ok(()) => Ok(image),
            Err(result) => {
                image.destroy(pipeline);
                Err(result)
            }
        }
    }

    fn create_tiles(
        &mut self,
        desc: &BigImageDesc<'_>,
        pipeline: &mut dyn TexturePipeline,
    ) -> Result<(), i32> {
        let (tile_w, tile_h) = (self.max_width, self.max_height);
        let (width, height) = (desc.width, desc.height);
        let full_x = width / tile_w;
        let full_y = height / tile_h;
        let rem_x = width % tile_w;
        let rem_y = height % tile_h;
        let count = (full_y + u32::from(rem_y != 0)).wrapping_mul(full_x + u32::from(rem_x != 0));
        self.width = width;
        self.height = height;
        let bpp = format_bytes(desc.dst_format);
        if count == 0 {
            return Ok(());
        }
        self.uvs = vec![[0.0; 8]; count as usize];
        self.handles = vec![0; count as usize];
        let format = self.format;
        let square = self.square_only != 0;
        let mut index = 0usize;
        let mut tile = |this: &mut BigImage,
                        index: usize,
                        col: u32,
                        row: u32,
                        size: (u32, u32),
                        src: (u32, u32)|
         -> Result<(), i32> {
            let handle = pipeline
                .create_texture(size.0, size.1, format, 1)
                .map_err(|_| GR_GENERIC)?;
            this.handles[index] = handle;
            let offset = tile_w
                .wrapping_mul(bpp)
                .wrapping_mul(col)
                .wrapping_add(tile_h.wrapping_mul(row).wrapping_mul(desc.pitch));
            let pixels = desc.pixels.get(offset as usize..).unwrap_or_default();
            if pipeline.set_level_data(
                handle,
                0,
                src.0,
                src.1,
                desc.pitch,
                desc.dst_format,
                pixels,
                None,
            ) != 0
            {
                let _ = pipeline.destroy_texture(handle);
                this.handles[index] = 0;
                return Err(GR_GENERIC);
            }
            Ok(())
        };
        let mut row = 0;
        let mut col = 0;
        while row < full_y {
            col = 0;
            while col < full_x {
                tile(self, index, col, row, (tile_w, tile_h), (tile_w, tile_h))?;
                index += 1;
                col += 1;
            }
            if rem_x != 0 {
                let size = if square {
                    (tile_w, tile_h)
                } else {
                    (round_up_pot(rem_x), tile_h)
                };
                tile(self, index, col, row, size, (rem_x, tile_h))?;
                index += 1;
            }
            row += 1;
        }
        if rem_y != 0 {
            let pot_y = round_up_pot(rem_y);
            col = 0;
            while col < full_x {
                let size = if square {
                    (tile_w, tile_h)
                } else {
                    (tile_w, pot_y)
                };
                tile(self, index, col, row, size, (tile_w, rem_y))?;
                index += 1;
                col += 1;
            }
            if rem_x != 0 {
                let size = if square {
                    let side = round_up_pot(rem_x);
                    let side = if side.cast_signed() <= pot_y.cast_signed() {
                        pot_y
                    } else {
                        side
                    };
                    (side, side)
                } else {
                    (round_up_pot(rem_x), pot_y)
                };
                tile(self, index, col, row, size, (rem_x, rem_y))?;
            }
        }
        self.tiles_x = col + u32::from(rem_x != 0);
        self.tiles_y = row + u32::from(rem_y != 0);
        Ok(())
    }

    /// The destructor: every tile texture destroyed, in order.
    pub fn destroy(&mut self, pipeline: &mut dyn TexturePipeline) {
        for handle in self.handles.drain(..) {
            if handle != 0 {
                let _ = pipeline.destroy_texture(handle);
            }
        }
        self.uvs.clear();
    }

    /// `GetSize`.
    #[must_use]
    pub fn size(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    /// The texture format chosen for the tiles.
    #[must_use]
    pub fn format(&self) -> PixelFormat {
        self.format
    }

    /// The tile grid, columns and rows.
    #[must_use]
    pub fn grid(&self) -> (u32, u32) {
        (self.tiles_x, self.tiles_y)
    }

    /// `GetTileCount`.
    #[must_use]
    pub fn tile_count(&self) -> u32 {
        self.handles.len() as u32
    }

    /// `GetTile`: the tile's texture and the whole image's rectangle.
    ///
    /// # Errors
    ///
    /// `GR_GENERIC` outside the tiles.
    pub fn tile(&self, index: i32) -> Result<(u32, Rect), i32> {
        let handle = usize::try_from(index)
            .ok()
            .and_then(|i| self.handles.get(i))
            .ok_or(GR_GENERIC)?;
        Ok((
            *handle,
            Rect {
                left: 0,
                top: 0,
                right: self.width.cast_signed(),
                bottom: self.height.cast_signed(),
            },
        ))
    }

    /// `SetAlpha`: `alpha * 255` truncated; its low byte is the alpha, and
    /// the whole value the result.
    pub fn set_alpha(&mut self, alpha: f32) -> i32 {
        let value = ftol_low(f64::from(alpha) * 255.0);
        self.alpha = value as u8;
        value
    }

    /// `SetColor`: `0xFFRRGGBB` from the three channels times 255,
    /// truncated. The top byte comes out 0xFF, so the alpha stops showing
    /// in the colour, as in the reference.
    pub fn set_color(&mut self, rgb: [f32; 3]) {
        let g = ftol_low(f64::from(rgb[1]) * 255.0).cast_unsigned() & 0xFF;
        let r = (ftol_low(f64::from(rgb[0]) * 255.0).cast_unsigned() | 0xFFFF_FF00) << 8;
        let b = ftol_low(f64::from(rgb[2]) * 255.0).cast_unsigned() & 0xFF;
        self.packed_rgb = ((g | r) << 8) | b;
    }

    /// `SetFromBuffer`: new pixels of the same size into the existing
    /// tiles.
    pub fn set_from_buffer(
        &mut self,
        pixels: &[u8],
        format: PixelFormat,
        width: u32,
        height: u32,
        pitch: u32,
        pipeline: &mut dyn TexturePipeline,
    ) -> i32 {
        if width != self.width || height != self.height {
            return GR_INVALID_PARMS;
        }
        let (tile_w, tile_h) = (self.max_width, self.max_height);
        let full_x = width / tile_w;
        let full_y = height / tile_h;
        let rem_x = width % tile_w;
        let rem_y = height % tile_h;
        let bpp = format_bytes(format);
        if self.handles.is_empty() {
            return 0;
        }
        let mut upload = |handle: u32, col: u32, row: u32, w: u32, h: u32| -> bool {
            let offset = tile_w
                .wrapping_mul(bpp)
                .wrapping_mul(col)
                .wrapping_add(tile_h.wrapping_mul(row).wrapping_mul(pitch));
            let src = pixels.get(offset as usize..).unwrap_or_default();
            pipeline.set_level_data(handle, 0, w, h, pitch, format, src, None) == 0
        };
        let mut index = 0usize;
        let mut row = 0;
        let mut col;
        while row < full_y {
            col = 0;
            while col < full_x {
                if !upload(self.handles[index], col, row, tile_w, tile_h) {
                    return GR_GENERIC;
                }
                index += 1;
                col += 1;
            }
            if rem_x != 0 {
                if !upload(self.handles[index], col, row, rem_x, tile_h) {
                    return GR_GENERIC;
                }
                index += 1;
            }
            row += 1;
        }
        if rem_y == 0 {
            return 0;
        }
        col = 0;
        while col < full_x {
            if !upload(self.handles[index], col, row, tile_w, rem_y) {
                return GR_GENERIC;
            }
            index += 1;
            col += 1;
        }
        if rem_x != 0 && !upload(self.handles[index], col, row, rem_x, rem_y) {
            return GR_GENERIC;
        }
        0
    }

    /// `RebuildUv`: each tile's texture rectangle for the source rectangle
    /// `src`; columns outside it are zeroed, rows outside it get V 0.
    fn rebuild_uv(&mut self, src: &Rect) {
        let (tiles_x, tiles_y) = (self.tiles_x, self.tiles_y);
        for col in 0..tiles_x {
            let mut span_w = self.max_width;
            let left = src.left.cast_unsigned();
            let right = src.right.cast_unsigned();
            let (left_tile, left_rem) = (left / span_w, left % span_w);
            let (right_tile, right_rem) = (right / span_w, right % span_w);
            if left_tile > col || right_tile < col {
                for row in 0..tiles_y {
                    self.uvs[(tiles_x * row + col) as usize] = [0.0; 8];
                }
                continue;
            }
            if col >= tiles_x - 1 {
                let rem = self.width % span_w;
                if rem != 0 {
                    span_w = rem;
                }
            }
            let span = f64::from(span_w.cast_signed());
            let u0 = if left_tile == col {
                f64::from(left_rem) / span
            } else {
                0.0
            } as f32;
            let u1 = if right_tile == col {
                f64::from(right_rem) / span
            } else {
                1.0
            } as f32;
            for row in 0..tiles_y {
                let mut span_h = self.max_height;
                let top = src.top.cast_unsigned();
                let bottom = src.bottom.cast_unsigned();
                let (top_tile, top_rem) = (top / span_h, top % span_h);
                let (bottom_tile, bottom_rem) = (bottom / span_h, bottom % span_h);
                let uv = &mut self.uvs[(tiles_x * row + col) as usize];
                if top_tile > row || bottom_tile < row {
                    *uv = [u0, 0.0, u1, 0.0, u0, 0.0, u1, 0.0];
                } else {
                    if row >= tiles_y - 1 {
                        let rem = self.height % span_h;
                        if rem != 0 {
                            span_h = rem;
                        }
                    }
                    let span = f64::from(span_h.cast_signed());
                    let v0 = if top_tile == row {
                        f64::from(top_rem) / span
                    } else {
                        0.0
                    } as f32;
                    let v1 = if bottom_tile == row {
                        f64::from(bottom_rem) / span
                    } else {
                        1.0
                    } as f32;
                    *uv = [u0, v0, u1, v0, u0, v1, u1, v1];
                }
            }
        }
    }

    /// `Draw`: the source rectangle (the whole image for `None` or an
    /// empty one) onto `dest`, one [`TileDraw`] per tile whose column has
    /// a texture span. The result is the reference's: 0 for an empty
    /// source, else the column count.
    pub fn draw(&mut self, src: Option<Rect>, dest: Rect) -> (i32, Vec<TileDraw>) {
        let whole = Rect {
            left: 0,
            top: 0,
            right: self.width.cast_signed(),
            bottom: self.height.cast_signed(),
        };
        let rect = src.and_then(normalize).unwrap_or(whole);
        self.rebuild_uv(&rect);
        let mut draws = Vec::new();
        let src_w = rect.right.wrapping_sub(rect.left);
        if src_w == 0 {
            return (0, draws);
        }
        let src_h = rect.bottom.wrapping_sub(rect.top);
        if src_h == 0 {
            return (0, draws);
        }
        let mut dest_x = dest.left as f32;
        let scale_x = (f64::from(dest.right.wrapping_sub(dest.left)) / f64::from(src_w)) as f32;
        let scale_y = (f64::from(dest.bottom.wrapping_sub(dest.top)) / f64::from(src_h)) as f32;
        let tiles_x = self.tiles_x.cast_signed();
        if tiles_x <= 0 {
            return (tiles_x, draws);
        }
        let color = (u32::from(self.alpha) << 24) | self.packed_rgb;
        for col in 0..self.tiles_x {
            let mut dest_y = dest.top as f32;
            let col_uv = self.uvs[col as usize];
            let u_span = col_uv[2] - col_uv[0];
            let mut pix_w = self.max_width;
            if col == self.tiles_x - 1 {
                let rem = self.width % self.max_width;
                if rem != 0 {
                    pix_w = rem;
                }
            }
            let col_w = (f64::from(pix_w) * f64::from(scale_x) * f64::from(u_span)) as f32;
            for row in 0..self.tiles_y {
                let index = (self.tiles_x * row + col) as usize;
                let uv = self.uvs[index];
                let v_span = f64::from(uv[5]) - f64::from(uv[1]);
                let mut pix_h = self.max_height;
                if row == self.tiles_y - 1 {
                    let rem = self.height % self.max_height;
                    if rem != 0 {
                        pix_h = rem;
                    }
                }
                let row_h = (f64::from(pix_h) * f64::from(scale_y) * v_span) as f32;
                if u_span != 0.0 {
                    draws.push(TileDraw {
                        handle: self.handles[index],
                        color,
                        dest: [
                            (f64::from(dest_x) - 0.5) as f32,
                            (f64::from(row_h) + f64::from(dest_y) - 0.5) as f32,
                            (f64::from(col_w) + f64::from(dest_x) - 0.5) as f32,
                            (f64::from(dest_y) - 0.5) as f32,
                        ],
                        uv,
                    });
                    dest_y = (f64::from(row_h) + f64::from(dest_y)) as f32;
                }
            }
            dest_x = (f64::from(col_w) + f64::from(dest_x)) as f32;
        }
        (tiles_x, draws)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::render::texture::pipeline::{Abilities, ContractPipeline, PipelineCall};

    #[test]
    fn pot_rounding() {
        assert_eq!(round_up_pot(0), 0);
        assert_eq!(round_up_pot(1), 1);
        assert_eq!(round_up_pot(3), 4);
        assert_eq!(round_up_pot(256), 256);
        assert_eq!(round_up_pot(257), 512);
    }

    #[test]
    fn tiles_cover_the_image() {
        let mut p = ContractPipeline::new(Abilities {
            square_only: 0,
            max_width: 4,
            max_height: 4,
        });
        let pixels = vec![7u8; 10 * 6 * 2];
        let desc = BigImageDesc {
            src_format: PixelFormat::R5G6B5,
            dst_format: PixelFormat::R5G6B5,
            pixels: &pixels,
            width: 10,
            height: 6,
            pitch: 20,
        };
        let mut image = BigImage::create(&desc, &FormatMaps::defaults(), &mut p).unwrap();
        assert_eq!(image.tile_count(), 6);
        assert_eq!(image.grid(), (3, 2));
        assert_eq!(image.format(), PixelFormat::DAOT);
        let sizes: Vec<(u32, u32)> = p
            .calls
            .iter()
            .filter_map(|c| match c {
                PipelineCall::Create { width, height, .. } => Some((*width, *height)),
                _ => None,
            })
            .collect();
        assert_eq!(sizes, [(4, 4), (4, 4), (2, 4), (4, 2), (4, 2), (2, 2)]);
        let (result, draws) = image.draw(
            None,
            Rect {
                left: 0,
                top: 0,
                right: 10,
                bottom: 6,
            },
        );
        assert_eq!(result, 3);
        assert_eq!(draws.len(), 6);
        assert_eq!(draws[0].dest, [-0.5, 3.5, 3.5, -0.5]);
        assert_eq!(draws[0].color, 0xFFFF_FFFF);
        image.set_color([1.0, 0.5, 0.0]);
        let (_, draws) = image.draw(None, Rect::default());
        assert_eq!(draws[0].color, 0xFFFF_7F00);
        image.destroy(&mut p);
    }
}
