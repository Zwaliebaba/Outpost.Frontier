//! `G_EffectVertexBatch` (`effect_vertex_batch.cpp`): the growable
//! XYZ | DIFFUSE | TEX1 vertex list (FVF 0x142, stride 0x18) and 16-bit
//! index list that the beams fill each frame, with the current colour and
//! texture coordinates that every appended vertex takes.
//!
//! The reference grows its buffers by `Growth` (0x80) vertices and three
//! times as many indices, and leaves the batch as it is when VC6's
//! `operator new` returns null. Vectors here grow on demand, so neither the
//! capacities nor the allocation failure are kept.

use neuron_core::num::trunc_to_i64;

/// The vertex format (`D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1`).
pub const EFFECT_FVF: u32 = 0x142;

/// One vertex in the reference's layout (`FEffectVertex`, 0x18 bytes).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct EffectVertex {
    /// +0x00.
    pub x: f32,
    /// +0x04.
    pub y: f32,
    /// +0x08.
    pub z: f32,
    /// +0x0c the `D3DCOLOR` bytes in memory order: blue, green, red, alpha.
    pub color: [u8; 4],
    /// +0x10.
    pub u: f32,
    /// +0x14.
    pub v: f32,
}

/// `FEffectVertexBatch` without its buffers' capacities and pipeline
/// reference.
#[derive(Clone, Debug, PartialEq)]
pub struct EffectVertexBatch {
    /// +0x04 / +0x08.
    pub vertices: Vec<EffectVertex>,
    /// +0x10 / +0x14.
    pub indices: Vec<u16>,
    /// +0x28.
    pub flag_28: u8,
    /// +0x29: strip mode, in which every fourth append first repeats two
    /// earlier vertices.
    pub strip_mode: bool,
    /// +0x2c.
    pub strip_counter: u32,
    /// +0x30..+0x33 the current colour: red, green, blue, alpha.
    pub color: [u8; 4],
    /// +0x34.
    pub u: f32,
    /// +0x38.
    pub v: f32,
}

impl Default for EffectVertexBatch {
    /// `EffectVertexBatchStaticInit`: empty, white, texture coordinates 0.
    fn default() -> Self {
        Self {
            vertices: Vec::new(),
            indices: Vec::new(),
            flag_28: 0,
            strip_mode: false,
            strip_counter: 0,
            color: [0xff; 4],
            u: 0.0,
            v: 0.0,
        }
    }
}

/// The low byte of `_ftol(value)`, as the reference stores only `AL`.
#[must_use]
pub fn ftol_low_byte(value: f64) -> u8 {
    trunc_to_i64(value).to_le_bytes()[0]
}

impl EffectVertexBatch {
    /// `EffectVertexBatchReset`: no vertices or indices, strip mode off.
    pub fn reset(&mut self) {
        self.vertices.clear();
        self.strip_mode = false;
        self.flag_28 = 0;
        self.strip_counter = 0;
        self.indices.clear();
    }

    /// `EffectVertexBatchAppend`: one vertex at `(x, y, z)` with the current
    /// colour and texture coordinates; the answer is its index. In strip
    /// mode, every fourth call first repeats the vertices three back and one
    /// back.
    ///
    /// # Panics
    ///
    /// In strip mode, when the repeat would read before the first vertex,
    /// which the reference reads out of bounds.
    pub fn append(&mut self, x: f32, y: f32, z: f32) -> usize {
        if self.strip_mode {
            if self.strip_counter & 3 == 3 {
                let n = self.vertices.len();
                let (a, b) = (self.vertices[n - 3], self.vertices[n - 1]);
                self.vertices.push(a);
                self.vertices.push(b);
            }
            self.strip_counter = self.strip_counter.wrapping_add(1);
        }
        let [r, g, b, a] = self.color;
        self.vertices.push(EffectVertex {
            x,
            y,
            z,
            color: [b, g, r, a],
            u: self.u,
            v: self.v,
        });
        self.vertices.len() - 1
    }

    /// `EffectVertexBatchSetColor`: each component times 255.0, the low byte
    /// of the truncation, alpha 0xff.
    pub fn set_color(&mut self, r: f32, g: f32, b: f32) {
        self.color = [
            ftol_low_byte(f64::from(r) * 255.0),
            ftol_low_byte(f64::from(g) * 255.0),
            ftol_low_byte(f64::from(b) * 255.0),
            0xff,
        ];
    }

    /// `EffectVertexBatchPushTriangle`.
    pub fn push_triangle(&mut self, i0: u16, i1: u16, i2: u16) {
        self.indices.extend_from_slice(&[i0, i1, i2]);
    }

    /// `EffectVertexBatchPushQuad`: `(i0, i1, i2)` and `(i0, i2, i3)`.
    pub fn push_quad(&mut self, i0: u16, i1: u16, i2: u16, i3: u16) {
        self.indices.extend_from_slice(&[i0, i1, i2, i0, i2, i3]);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn appends_take_the_current_colour_and_coordinates() {
        let mut batch = EffectVertexBatch::default();
        batch.set_color(1.0, 0.5, 0.0);
        batch.u = 0.25;
        batch.v = 0.75;
        assert_eq!(batch.append(1.0, 2.0, 3.0), 0);
        let vertex = batch.vertices[0];
        assert_eq!(vertex.color, [0, 127, 255, 255]);
        assert_eq!((vertex.u, vertex.v), (0.25, 0.75));
        batch.push_quad(0, 1, 2, 3);
        assert_eq!(batch.indices, [0, 1, 2, 0, 2, 3]);
    }

    #[test]
    fn colours_keep_the_low_byte_without_a_clamp() {
        // 2.0 * 255 = 510 = 0x1fe; negative values wrap.
        assert_eq!(ftol_low_byte(510.0), 0xfe);
        assert_eq!(ftol_low_byte(-1.5), 0xff);
        assert_eq!(ftol_low_byte(f64::NAN), 0);
    }

    #[test]
    fn strip_mode_repeats_two_vertices_every_fourth_append() {
        let mut batch = EffectVertexBatch {
            strip_mode: true,
            ..EffectVertexBatch::default()
        };
        for i in 0..4 {
            batch.append(f32::from(i as u8), 0.0, 0.0);
        }
        let xs: Vec<f32> = batch.vertices.iter().map(|v| v.x).collect();
        assert_eq!(xs, [0.0, 1.0, 2.0, 0.0, 2.0, 3.0]);
        assert_eq!(batch.strip_counter, 4);
    }
}
