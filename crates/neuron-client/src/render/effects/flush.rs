//! `EffectVertexBatchFlush` (`effect_vertex_batch.cpp`): the beams'
//! batch drawn once a frame, after the beam buckets' walk, through the
//! render pipeline's calls in the reference's order ([`EffectDraw`]).

use super::batch::{EFFECT_FVF, EffectVertex, EffectVertexBatch};

/// `D3DPT_TRIANGLELIST`.
const TRIANGLE_LIST: u32 = 4;

/// Direct3D 8's render states the flush sets.
mod rs {
    pub const ZENABLE: u32 = 7;
    pub const ZWRITEENABLE: u32 = 14;
    pub const SRCBLEND: u32 = 19;
    pub const DESTBLEND: u32 = 20;
    pub const CULLMODE: u32 = 22;
    pub const ALPHABLENDENABLE: u32 = 27;
    pub const CLIPPING: u32 = 136;
    pub const LIGHTING: u32 = 137;
    pub const COLORVERTEX: u32 = 0x8d;
    pub const DIFFUSEMATERIALSOURCE: u32 = 0x91;
}

/// `D3DTSS_TEXCOORDINDEX`.
const TSS_TEXCOORDINDEX: u32 = 0xb;

/// Stage 0's table (`kTssStage0`): colour and alpha the texture times the
/// diffuse colour.
const STAGE_0: [(u32, u32); 6] = [(1, 4), (2, 2), (3, 0), (4, 4), (5, 2), (6, 0)];
/// Stage 1's table (`kTssStage1`): both operations disabled.
const STAGE_1: [(u32, u32); 6] = [(1, 1), (2, 2), (3, 1), (4, 1), (5, 2), (6, 1)];

/// The pipeline calls of the flush (`IRenderPipeline`).
pub trait EffectDraw {
    /// `GetRenderState`.
    fn render_state(&mut self, state: u32) -> u32;
    /// `SetRenderState`.
    fn set_render_state(&mut self, state: u32, value: u32);
    /// `SetTextureStageState`.
    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32);
    /// `SetTextureStageTexture` with a GPU handle, 0 for none.
    fn set_stage_texture(&mut self, stage: u32, handle: u32);
    /// `SetModelview` (slot 31).
    fn set_modelview(&mut self, matrix: &[f32; 12]);
    /// `DrawIndexedPrimitiveUp(prim, fvf, vertices, count, indices, count, 0)`.
    fn draw_indexed_up(&mut self, prim: u32, fvf: u32, vertices: &[EffectVertex], indices: &[u16]);
}

/// The vertices as the 0x18-byte records the device reads.
#[must_use]
pub fn vertex_bytes(vertices: &[EffectVertex]) -> Vec<u8> {
    let mut out = Vec::with_capacity(vertices.len() * 0x18);
    for v in vertices {
        for f in [v.x, v.y, v.z] {
            out.extend_from_slice(&f.to_le_bytes());
        }
        out.extend_from_slice(&v.color);
        for f in [v.u, v.v] {
            out.extend_from_slice(&f.to_le_bytes());
        }
    }
    out
}

/// `EffectVertexBatchFlush(camera)`: the effect states, the `code_beam`
/// texture (`texture`, its frame 0's GPU handle), additive blending without
/// depth writes or lighting, the camera's view as the modelview, one
/// indexed draw of the whole batch (made with any counts, as the reference
/// tests none), and the states put back.
pub fn flush(batch: &EffectVertexBatch, view: &[f32; 12], texture: u32, draw: &mut dyn EffectDraw) {
    draw.set_stage_state(0, TSS_TEXCOORDINDEX, 0);
    for (state, value) in STAGE_0 {
        draw.set_stage_state(0, state, value);
    }
    for (state, value) in STAGE_1 {
        draw.set_stage_state(1, state, value);
    }
    draw.set_render_state(rs::COLORVERTEX, 1);
    draw.set_render_state(rs::DIFFUSEMATERIALSOURCE, 1);
    draw.set_stage_texture(0, texture);
    draw.set_render_state(rs::CLIPPING, 1);
    draw.set_render_state(rs::CULLMODE, 1);
    draw.set_render_state(rs::ZENABLE, 1);
    draw.set_render_state(rs::ZWRITEENABLE, 0);
    draw.set_render_state(rs::ALPHABLENDENABLE, 1);
    draw.set_render_state(rs::SRCBLEND, 2);
    draw.set_render_state(rs::DESTBLEND, 2);
    let lighting = draw.render_state(rs::LIGHTING);
    draw.set_render_state(rs::LIGHTING, 0);
    draw.set_modelview(view);
    draw.draw_indexed_up(TRIANGLE_LIST, EFFECT_FVF, &batch.vertices, &batch.indices);
    draw.set_render_state(rs::LIGHTING, lighting);
    draw.set_render_state(rs::ALPHABLENDENABLE, 0);
    draw.set_render_state(rs::ZWRITEENABLE, 1);
    draw.set_render_state(rs::CULLMODE, 2);
    draw.set_render_state(rs::COLORVERTEX, 0);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct Recorder {
        calls: Vec<String>,
    }

    impl EffectDraw for Recorder {
        fn render_state(&mut self, state: u32) -> u32 {
            self.calls.push(format!("get {state}"));
            1
        }
        fn set_render_state(&mut self, state: u32, value: u32) {
            self.calls.push(format!("rs {state} {value}"));
        }
        fn set_stage_state(&mut self, stage: u32, state: u32, value: u32) {
            self.calls.push(format!("tss {stage} {state} {value}"));
        }
        fn set_stage_texture(&mut self, stage: u32, handle: u32) {
            self.calls.push(format!("tex {stage} {handle}"));
        }
        fn set_modelview(&mut self, _matrix: &[f32; 12]) {
            self.calls.push("modelview".into());
        }
        fn draw_indexed_up(&mut self, prim: u32, fvf: u32, v: &[EffectVertex], i: &[u16]) {
            self.calls
                .push(format!("draw {prim} {fvf:x} {} {}", v.len(), i.len()));
        }
    }

    #[test]
    fn the_flush_draws_once_between_its_states() {
        let mut batch = EffectVertexBatch::default();
        for _ in 0..3 {
            batch.append(0.0, 0.0, 0.0);
        }
        batch.push_triangle(0, 1, 2);
        let mut rec = Recorder::default();
        flush(&batch, &[0.0; 12], 0x101, &mut rec);
        let c = &rec.calls;
        // 13 stage states, 9 render states and the texture, the lighting's get and set,
        // the modelview, the draw and 5 restores.
        assert_eq!(c.len(), 32);
        assert_eq!(c[0], "tss 0 11 0");
        assert_eq!(c[15], "tex 0 257");
        let at = c
            .iter()
            .position(|s| s.starts_with("draw"))
            .expect("the draw");
        assert_eq!(c[at], "draw 4 142 3 3");
        assert_eq!(&c[at - 3..at], ["get 137", "rs 137 0", "modelview"]);
        assert_eq!(c[at + 1], "rs 137 1");
        assert_eq!(c.last().map(String::as_str), Some("rs 141 0"));
    }

    #[test]
    fn vertices_are_the_device_s_records() {
        let v = EffectVertex {
            x: 1.0,
            color: [1, 2, 3, 4],
            v: 0.5,
            ..EffectVertex::default()
        };
        let bytes = vertex_bytes(&[v]);
        assert_eq!(bytes.len(), 0x18);
        assert_eq!(&bytes[0..4], &1.0f32.to_le_bytes());
        assert_eq!(&bytes[12..16], &[1, 2, 3, 4]);
        assert_eq!(&bytes[20..24], &0.5f32.to_le_bytes());
    }
}
