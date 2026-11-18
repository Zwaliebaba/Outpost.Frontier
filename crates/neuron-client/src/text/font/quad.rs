//! What the string paints hand to the screen-quad batcher
//! (`screen_quad_state.cpp`): a record per quad, the one-dimensional clip
//! they use, and the vertices the batcher expands a record into.
//!
//! The batcher itself (material lists, render states, the flush to the
//! device) belongs to the UI toolkit (P4.13); the paints talk to it through
//! [`QuadSink`].

/// `kScreenQuadFvf`: `XYZRHW | DIFFUSE | SPECULAR | TEX1`.
pub const SCREEN_QUAD_FVF: u32 = 0x1C4;

/// Blend mode 2 of `ScreenQuadBeginBatchFrame`: `SRCALPHA` /
/// `INVSRCALPHA`, the mode of every string page.
pub const BLEND_ALPHA: i32 = 2;

/// `FScreenQuadRecord`: an axis-aligned quad in screen pixels.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ScreenQuadRecord {
    pub x0: f32,
    pub y0: f32,
    pub x1: f32,
    pub y1: f32,
    /// The vertices' z.
    pub depth_a: f32,
    /// The vertices' reciprocal w.
    pub depth_b: f32,
    pub u0: f32,
    pub v0: f32,
    pub u1: f32,
    pub v1: f32,
    /// A `D3DCOLOR`, moved as raw bits.
    pub color: u32,
}

/// One vertex of FVF 0x1C4 (`FScreenQuadGpuVert`, 32 bytes).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TlVertex {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub rhw: f32,
    pub color: u32,
    pub specular: u32,
    pub u: f32,
    pub v: f32,
}

impl ScreenQuadRecord {
    /// The four vertices `FlushOneBatch` writes for the record:
    /// (x0, y0), (x0, y1), (x1, y1), (x1, y0), specular 0.
    #[must_use]
    pub fn vertices(&self) -> [TlVertex; 4] {
        let v = |x, y, u, v| TlVertex {
            x,
            y,
            z: self.depth_a,
            rhw: self.depth_b,
            color: self.color,
            specular: 0,
            u,
            v,
        };
        [
            v(self.x0, self.y0, self.u0, self.v0),
            v(self.x0, self.y1, self.u0, self.v1),
            v(self.x1, self.y1, self.u1, self.v1),
            v(self.x1, self.y0, self.u1, self.v0),
        ]
    }
}

/// The indices of `quads` quads, `{b, b+1, b+2, b, b+2, b+3}` with `b`
/// stepping by 4, drawn as an indexed triangle list.
#[must_use]
pub fn quad_indices(quads: u16) -> Vec<u16> {
    (0..quads)
        .flat_map(|q| {
            let b = q * 4;
            [b, b + 1, b + 2, b, b + 2, b + 3]
        })
        .collect()
}

/// `ScreenQuadClip1D`: clips `[e0, e1]` to `[lo, hi]`, moving the paired
/// attribute in proportion to `span`. 0 when nothing is left, -1 when an
/// edge moved, 1 otherwise. NaN follows the reference's x87 compares.
pub fn clip_1d(
    e0: &mut f32,
    e1: &mut f32,
    a0: &mut f32,
    a1: &mut f32,
    lo: f32,
    hi: f32,
    span: f32,
) -> i32 {
    if *e1 < lo {
        return 0;
    }
    if f64::from(*e0) >= f64::from(hi) {
        return 0;
    }
    let mut result = 1;
    let delta_lo = f64::from(lo) - f64::from(*e0);
    if delta_lo > 0.0 {
        result = -1;
        *e0 = (delta_lo + f64::from(*e0)) as f32;
        *a0 = ((f64::from(*a1) - f64::from(*a0)) * (delta_lo / f64::from(span)) + f64::from(*a0))
            as f32;
    }
    let delta_hi = f64::from(hi) - f64::from(*e1);
    // Greater, equal and unordered all keep the far edge.
    if delta_hi >= 0.0 || delta_hi.is_nan() {
        return result;
    }
    *e1 = (delta_hi + f64::from(*e1)) as f32;
    *a1 =
        ((f64::from(*a1) - f64::from(*a0)) * (delta_hi / f64::from(span)) + f64::from(*a1)) as f32;
    -1
}

/// The batcher calls of a paint.
pub trait QuadSink {
    /// `ScreenQuadSetMaterialKey`.
    fn set_material_key(&mut self, key: u32);
    /// `ScreenQuadBeginBatchFrame` with a frame holding only the texture.
    fn begin_batch_frame(&mut self, texture: u32, blend_mode: i32, find: bool);
    /// `ScreenQuadAppendRecord` into the current batch.
    fn append(&mut self, record: ScreenQuadRecord);
}

/// One call a paint made.
#[derive(Clone, Debug, PartialEq)]
pub enum QuadCall {
    MaterialKey(u32),
    Frame {
        texture: u32,
        blend_mode: i32,
        find: bool,
    },
    Record(ScreenQuadRecord),
}

/// A [`QuadSink`] that keeps the calls.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct QuadRecorder {
    pub calls: Vec<QuadCall>,
}

impl QuadRecorder {
    /// The records, in order.
    #[must_use]
    pub fn records(&self) -> Vec<ScreenQuadRecord> {
        self.calls
            .iter()
            .filter_map(|c| match c {
                QuadCall::Record(r) => Some(*r),
                _ => None,
            })
            .collect()
    }
}

impl QuadSink for QuadRecorder {
    fn set_material_key(&mut self, key: u32) {
        self.calls.push(QuadCall::MaterialKey(key));
    }

    fn begin_batch_frame(&mut self, texture: u32, blend_mode: i32, find: bool) {
        self.calls.push(QuadCall::Frame {
            texture,
            blend_mode,
            find,
        });
    }

    fn append(&mut self, record: ScreenQuadRecord) {
        self.calls.push(QuadCall::Record(record));
    }
}

/// `NumberPackColor`: the alpha byte, then bytes 0, 1 and 2 of `source`
/// from high to low, so a `D3DCOLOR`'s red and blue trade places.
#[must_use]
pub fn pack_color(alpha: u8, source: u32) -> u32 {
    let b0 = source & 0xFF;
    let b1 = (source >> 8) & 0xFF;
    let b2 = (source >> 16) & 0xFF;
    (u32::from(alpha) << 24) | (b0 << 16) | (b1 << 8) | b2
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn clip_inside_outside_and_partial() {
        let (mut e0, mut e1, mut a0, mut a1) = (10.0f32, 20.0f32, 0.0f32, 1.0f32);
        assert_eq!(
            clip_1d(&mut e0, &mut e1, &mut a0, &mut a1, 0.0, 799.0, 11.0),
            1
        );
        assert_eq!((e0, e1, a0, a1), (10.0, 20.0, 0.0, 1.0));
        let (mut e0, mut e1, mut a0, mut a1) = (-5.0f32, 5.0f32, 0.0f32, 1.0f32);
        assert_eq!(
            clip_1d(&mut e0, &mut e1, &mut a0, &mut a1, 0.0, 799.0, 10.0),
            -1
        );
        assert_eq!((e0, a0), (0.0, 0.5));
        let (mut e0, mut e1, mut a0, mut a1) = (790.0f32, 810.0f32, 0.0f32, 1.0f32);
        assert_eq!(
            clip_1d(&mut e0, &mut e1, &mut a0, &mut a1, 0.0, 799.0, 20.0),
            -1
        );
        assert_eq!((e1, a1), (799.0, 0.45));
        let (mut e0, mut e1, mut a0, mut a1) = (800.0f32, 810.0f32, 0.0f32, 1.0f32);
        assert_eq!(
            clip_1d(&mut e0, &mut e1, &mut a0, &mut a1, 0.0, 799.0, 10.0),
            0
        );
        let (mut e0, mut e1, mut a0, mut a1) = (-20.0f32, -10.0f32, 0.0f32, 1.0f32);
        assert_eq!(
            clip_1d(&mut e0, &mut e1, &mut a0, &mut a1, 0.0, 799.0, 10.0),
            0
        );
    }

    #[test]
    fn records_expand_to_four_vertices() {
        let r = ScreenQuadRecord {
            x0: 1.0,
            y0: 2.0,
            x1: 3.0,
            y1: 4.0,
            depth_a: 1.0,
            depth_b: 1.0,
            u0: 0.0,
            v0: 0.25,
            u1: 0.5,
            v1: 0.75,
            color: 0xFF00_FF00,
        };
        let v = r.vertices();
        assert_eq!((v[1].x, v[1].y, v[1].u, v[1].v), (1.0, 4.0, 0.0, 0.75));
        assert_eq!((v[3].x, v[3].y, v[3].u, v[3].v), (3.0, 2.0, 0.5, 0.25));
        assert_eq!(quad_indices(2), vec![0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7]);
        assert_eq!(pack_color(0x80, 0x0011_2233), 0x8033_2211);
    }
}
