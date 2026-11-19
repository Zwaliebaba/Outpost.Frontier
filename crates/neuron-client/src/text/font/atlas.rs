//! The page pool of `font_atlas.cpp`: the power-of-two `A1R5G5B5` textures
//! strings are rasterised into, and the grid of pages one string uses.
//!
//! A string wider or taller than the 256 x 256 scratch is cut into tiles of
//! at most 256 a side, row by row; each tile gets a page whose sides are
//! the tile's rounded up to a power of two. Pages a string gives back go to
//! a free list and are handed out again, first fit, to any request they
//! cover, with whatever their last string left in them.
//!
//! The reference writes a tile into the page through `LockTexture`. The
//! texture interface the port has ([`TexturePipeline`]) uploads whole
//! levels, so each page keeps a copy of its texels and the whole level is
//! uploaded after each tile; the texels outside the tile are those the
//! page held, as in the reference.

use neuron_core::hash::name_crc32;
use neuron_core::image::PixelFormat;

use crate::render::texture::TexturePipeline;

/// The scratch bitmap's sides (`G_AtlasMaxW`, `G_AtlasMaxH`).
pub const SCRATCH_SIZE: u32 = 256;

/// One page (`FFontAtlasPage`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Page {
    /// The texture handle.
    pub texture: u32,
    /// The texture's width, a power of two.
    pub page_w: u32,
    /// The texture's height, a power of two.
    pub page_h: u32,
    /// The measured string height the grid was drawn for (the grid's head
    /// only).
    pub content_h: u32,
    /// The tile's place in the string.
    pub src_x: i32,
    pub src_y: i32,
    /// The part of the page the tile uses.
    pub used_w: u32,
    pub used_h: u32,
    /// The texels, `page_w * page_h`, row by row.
    texels: Vec<u16>,
}

impl Page {
    /// The page's texels as last uploaded.
    #[must_use]
    pub fn texels(&self) -> &[u16] {
        &self.texels
    }
}

/// A page of the pool.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PageId(pub usize);

/// The pages of one string: rows of pages, the first page of the first row
/// the head (the reference links them through `NextY` and `NextX`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PageGrid {
    pub rows: Vec<Vec<PageId>>,
}

impl PageGrid {
    /// The first page.
    #[must_use]
    pub fn head(&self) -> Option<PageId> {
        self.rows.first().and_then(|r| r.first()).copied()
    }
}

/// `RoundUpPow2`: a power of two (or 0) is kept; otherwise from 0x80 when
/// above 0x40, else from 1, doubled until it covers the value.
#[must_use]
pub fn round_up_pow2(value: u32) -> u32 {
    if value & value.wrapping_sub(1) == 0 {
        return value;
    }
    let mut result: u32 = if value > 0x40 { 0x80 } else { 1 };
    while result < value {
        result = result.wrapping_shl(1);
        if result == 0 {
            break;
        }
    }
    result
}

/// The `A1R5G5B5` texel the reference uploads for a coverage value: the
/// scratch pixel of white text on black (grey `coverage`) as
/// `GetDIBits` gives it in 16 bits (five bits a channel), with the alpha
/// bit set when the pixel is not black.
#[must_use]
pub fn coverage_texel(coverage: u8) -> u16 {
    let c = u16::from(coverage >> 3);
    let pixel = (c << 10) | (c << 5) | c;
    if pixel != 0 { 0x8000 | pixel } else { 0 }
}

/// The pool and the drawing state of `font_atlas.cpp`.
#[derive(Clone, Debug)]
pub struct Atlas {
    pages: Vec<Page>,
    free: Vec<PageId>,
    /// `G_AtlasTexBytes`: the bytes of every page created and not
    /// destroyed.
    pub tex_bytes: u32,
    /// `G_AtlasMaterialKey`, `GetCRC32("font")` until swapped.
    pub material_key: u32,
    /// `G_AtlasDrawColor`, the packed tint of string paints.
    pub draw_color: u32,
}

impl Default for Atlas {
    fn default() -> Self {
        Self::new()
    }
}

impl Atlas {
    #[must_use]
    pub fn new() -> Atlas {
        Atlas {
            pages: Vec::new(),
            free: Vec::new(),
            tex_bytes: 0,
            material_key: name_crc32(b"font"),
            draw_color: 0,
        }
    }

    /// A page of the pool.
    #[must_use]
    pub fn page(&self, id: PageId) -> &Page {
        &self.pages[id.0]
    }

    pub(crate) fn page_mut(&mut self, id: PageId) -> &mut Page {
        &mut self.pages[id.0]
    }

    /// The free list, in order.
    #[must_use]
    pub fn free_pages(&self) -> &[PageId] {
        &self.free
    }

    /// `CreateAtlasPage`: the first free page at least `need_w` x `need_h`,
    /// as it is; else a new zeroed page with a texture of the request
    /// rounded up to powers of two. `None` when the texture cannot be made.
    pub fn create_page(
        &mut self,
        pipeline: &mut dyn TexturePipeline,
        need_w: u32,
        need_h: u32,
    ) -> Option<PageId> {
        if let Some(at) = self.free.iter().position(|&id| {
            let page = &self.pages[id.0];
            page.page_w >= need_w && page.page_h >= need_h
        }) {
            return Some(self.free.remove(at));
        }
        let tex_w = round_up_pow2(need_w);
        let tex_h = round_up_pow2(need_h);
        let texture = pipeline
            .create_texture(tex_w, tex_h, PixelFormat::A1R5G5B5, 1)
            .ok()?;
        self.tex_bytes = self
            .tex_bytes
            .wrapping_add(tex_w.wrapping_mul(tex_h).wrapping_mul(2));
        self.pages.push(Page {
            texture,
            page_w: tex_w,
            page_h: tex_h,
            content_h: 0,
            src_x: 0,
            src_y: 0,
            used_w: 0,
            used_h: 0,
            texels: vec![0; (tex_w as usize) * (tex_h as usize)],
        });
        Some(PageId(self.pages.len() - 1))
    }

    /// `FontAtlasFreePages`: every page of the grid, row by row, onto the
    /// free list; the textures stay. The grid is emptied.
    pub fn free_grid(&mut self, grid: &mut Option<PageGrid>) {
        if let Some(grid) = grid.take() {
            for row in grid.rows {
                self.free.extend(row);
            }
        }
    }

    /// `FontAtlasReleasePagePool`: the texture of every free page
    /// destroyed and its bytes taken off the total, the free list emptied.
    /// Pages still in a grid are left alone.
    pub fn release_pool(&mut self, pipeline: &mut dyn TexturePipeline) {
        for id in std::mem::take(&mut self.free) {
            let page = &mut self.pages[id.0];
            let _ = pipeline.destroy_texture(page.texture);
            self.tex_bytes = self
                .tex_bytes
                .wrapping_sub(page.page_h.wrapping_mul(page.page_w).wrapping_mul(2));
            page.texture = 0;
            page.texels = Vec::new();
        }
    }

    /// Writes a tile's coverage (`tile_w * tile_h` bytes) into the page as
    /// texels and uploads the page; the upload's result (0 on success).
    pub(crate) fn upload_tile(
        &mut self,
        pipeline: &mut dyn TexturePipeline,
        id: PageId,
        coverage: &[u8],
        tile_w: u32,
        tile_h: u32,
    ) -> i32 {
        let page = &mut self.pages[id.0];
        let pitch = page.page_w as usize;
        for y in 0..(tile_h as usize).min(page.page_h as usize) {
            for x in 0..(tile_w as usize).min(pitch) {
                let c = coverage.get(y * tile_w as usize + x).copied().unwrap_or(0);
                page.texels[y * pitch + x] = coverage_texel(c);
            }
        }
        let bytes: Vec<u8> = page.texels.iter().flat_map(|t| t.to_le_bytes()).collect();
        pipeline.set_level_data(
            page.texture,
            0,
            page.page_w,
            page.page_h,
            page.page_w * 2,
            PixelFormat::A1R5G5B5,
            &bytes,
            None,
        )
    }

    /// The pages of a grid as the reference builds it for `check_w` x
    /// `check_h`: rows of at most 256, columns of at most 256, each tile's
    /// page from [`Atlas::create_page`] with its offset in the string.
    /// `None` for an empty extent or when a texture cannot be made (the
    /// reference does not test the page; divergence D291), the pages made
    /// so far returned to the pool.
    pub(crate) fn build_grid(
        &mut self,
        pipeline: &mut dyn TexturePipeline,
        check_w: u32,
        check_h: u32,
    ) -> Option<PageGrid> {
        let mut grid = PageGrid::default();
        let mut left_h = check_h.cast_signed();
        while left_h > 0 {
            let tile_h = left_h.cast_unsigned().min(SCRATCH_SIZE);
            let src_y = check_h.cast_signed().wrapping_sub(left_h);
            let mut row = Vec::new();
            let mut left_w = check_w.cast_signed();
            while left_w > 0 {
                let tile_w = left_w.cast_unsigned().min(SCRATCH_SIZE);
                let Some(id) = self.create_page(pipeline, tile_w, tile_h) else {
                    grid.rows.push(row);
                    self.free_grid(&mut Some(grid));
                    return None;
                };
                let page = &mut self.pages[id.0];
                page.src_x = check_w.cast_signed().wrapping_sub(left_w);
                page.src_y = src_y;
                row.push(id);
                left_w -= SCRATCH_SIZE.cast_signed();
            }
            grid.rows.push(row);
            left_h -= SCRATCH_SIZE.cast_signed();
        }
        grid.head().map(|_| grid)
    }

    /// Whether the grid's first row covers `check_w` and its first column
    /// `check_h` (sums of page sides, unsigned).
    pub(crate) fn grid_covers(&self, grid: &PageGrid, check_w: u32, check_h: u32) -> bool {
        let sum_w = grid.rows.first().map_or(0u32, |row| {
            row.iter()
                .fold(0u32, |s, id| s.wrapping_add(self.pages[id.0].page_w))
        });
        if sum_w < check_w {
            return false;
        }
        let sum_h = grid.rows.iter().fold(0u32, |s, row| {
            s.wrapping_add(row.first().map_or(0, |id| self.pages[id.0].page_h))
        });
        sum_h >= check_h
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::render::texture::{Abilities, ContractPipeline, PipelineCall};

    #[test]
    fn powers_of_two_as_the_reference_rounds() {
        assert_eq!(round_up_pow2(0), 0);
        assert_eq!(round_up_pow2(1), 1);
        assert_eq!(round_up_pow2(3), 4);
        assert_eq!(round_up_pow2(64), 64);
        assert_eq!(round_up_pow2(65), 128);
        assert_eq!(round_up_pow2(129), 256);
        assert_eq!(round_up_pow2(300), 512);
    }

    #[test]
    fn coverage_becomes_a1r5g5b5() {
        assert_eq!(coverage_texel(0), 0);
        assert_eq!(coverage_texel(7), 0);
        assert_eq!(coverage_texel(8), 0x8421);
        assert_eq!(coverage_texel(255), 0xFFFF);
        assert_eq!(coverage_texel(128), 0x8000 | (16 << 10) | (16 << 5) | 16);
    }

    #[test]
    fn grid_tiles_and_pool_reuse() {
        let mut pipe = ContractPipeline::new(Abilities::default());
        let mut atlas = Atlas::new();
        let grid = atlas.build_grid(&mut pipe, 300, 20).unwrap();
        assert_eq!(grid.rows.len(), 1);
        assert_eq!(grid.rows[0].len(), 2);
        let (a, b) = (atlas.page(grid.rows[0][0]), atlas.page(grid.rows[0][1]));
        assert_eq!((a.page_w, a.page_h, a.src_x, a.src_y), (256, 32, 0, 0));
        assert_eq!((b.page_w, b.page_h, b.src_x, b.src_y), (64, 32, 256, 0));
        assert_eq!(atlas.tex_bytes, 256 * 32 * 2 + 64 * 32 * 2);
        assert!(atlas.grid_covers(&grid, 300, 20));
        assert!(!atlas.grid_covers(&grid, 330, 20));
        let mut grid = Some(grid);
        atlas.free_grid(&mut grid);
        assert_eq!(atlas.free_pages().len(), 2);
        // A 40 x 10 request takes the first free page that covers it: the
        // 256 x 32 page, with its old offsets.
        let id = atlas.create_page(&mut pipe, 40, 10).unwrap();
        assert_eq!(atlas.page(id).page_w, 256);
        assert_eq!(atlas.free_pages().len(), 1);
        atlas.release_pool(&mut pipe);
        assert_eq!(atlas.tex_bytes, 256 * 32 * 2);
        assert!(
            pipe.calls
                .iter()
                .any(|c| matches!(c, PipelineCall::Destroy { handle: 2 }))
        );
    }

    #[test]
    fn tall_strings_take_rows() {
        let mut pipe = ContractPipeline::new(Abilities::default());
        let mut atlas = Atlas::new();
        let grid = atlas.build_grid(&mut pipe, 10, 300).unwrap();
        assert_eq!(grid.rows.len(), 2);
        let second = atlas.page(grid.rows[1][0]);
        assert_eq!((second.src_y, second.page_h), (256, 64));
        assert!(atlas.build_grid(&mut pipe, 0, 10).is_none());
    }
}
