//! The helpers `Win3DDrawAll` calls around its draw loop
//! (`win3d_renderables.cpp`): the two immediate line batchers, their flush
//! through the Win3D camera, and the overlay preparation that flushes the
//! font atlas's screen quads under a 2D projection.
//!
//! Batcher A takes the wire controls' thick lines and draws them through
//! the Win3D camera's perspective; batcher B takes the HUD's 2D lines and
//! draws them through the camera's orthographic box. A line is eight
//! vertices (each end in its colour, and pushed the width to either side
//! along the perpendicular in the edge colour) and twelve indices.
//!
//! The button montage's ensure and flush (`ButtonMontageEnsure`,
//! `ButtonMontageFlushBatch`) are [`super::montage`]'s; the IME's overlay
//! pass (`ImeFocusDrawPass`) is [`ime_focus_draw_pass`].

use neuron_core::hash::name_crc32;

use super::camera::Camera;
use super::quad_batch::QuadBatcher;
use super::win3d_immediate::{LineVertex, Win3dLayer, Win3dPipeline, capability, rs};

/// A batcher's vertex capacity (`0xc000` bytes of 16-byte vertices).
pub const BATCH_VERTICES: usize = 0xC00;
/// A batcher's index capacity (`0x3000` bytes of 16-bit indices).
pub const BATCH_INDICES: usize = 0x1800;

/// The twelve indices of a line, from its first vertex.
const LINE_INDEX_OFFSETS: [u16; 12] = [0, 1, 2, 2, 3, 1, 4, 5, 6, 6, 7, 5];

/// `D3DBLEND_SRCALPHA`, `D3DBLEND_INVSRCALPHA`.
const BLEND_SRC_ALPHA: u32 = 5;
const BLEND_INV_SRC_ALPHA: u32 = 6;

/// The stage states the flush applies to stage 0 and then stage 1
/// (`kImmedTss`: colour and alpha from the diffuse alone).
const IMMED_TSS: [(u32, u32); 6] = [(1, 1), (2, 2), (3, 1), (4, 1), (5, 2), (6, 1)];

/// `FImmedBatcher`.
#[derive(Clone, Debug, PartialEq)]
pub struct LineBatcher {
    pub vertices: Vec<LineVertex>,
    pub indices: Vec<u16>,
    /// Set by every flush; the reset rewinds a dirty batcher.
    pub dirty: bool,
    /// Flush through the camera's 2D box instead of its perspective.
    pub ortho: bool,
}

impl LineBatcher {
    fn new(ortho: bool) -> LineBatcher {
        LineBatcher {
            vertices: Vec::new(),
            indices: Vec::new(),
            dirty: true,
            ortho,
        }
    }

    /// `ResetImmedBatchDirty` for one batcher.
    fn reset(&mut self) {
        if self.dirty {
            self.dirty = false;
            self.vertices.clear();
            self.indices.clear();
        }
    }

    /// The eight vertices and twelve indices of a line.
    fn push_line(&mut self, a: [f32; 3], b: [f32; 3], color: u32, edge: u32, width: f32) {
        let base = self.vertices.len() as u16;
        self.indices
            .extend(LINE_INDEX_OFFSETS.iter().map(|&o| base.wrapping_add(o)));
        let dx = f64::from(b[0]) - f64::from(a[0]);
        let dy = f64::from(b[1]) - f64::from(a[1]);
        let angle = dx.atan2(dy) + 1.570_796_326_75;
        let cos_angle = angle.cos() as f32;
        let sin_w = angle.sin() * f64::from(width);
        let cos_w = f64::from(cos_angle) * f64::from(width);
        let at = |p: [f32; 3], sign: f64, c: u32| LineVertex {
            x: (f64::from(p[0]) + sign * sin_w) as f32,
            y: (f64::from(p[1]) + sign * cos_w) as f32,
            z: p[2],
            color: c,
        };
        let end = |p: [f32; 3]| LineVertex {
            x: p[0],
            y: p[1],
            z: p[2],
            color,
        };
        self.vertices.extend([
            end(a),
            at(a, 1.0, edge),
            end(b),
            at(b, 1.0, edge),
            end(a),
            at(a, -1.0, edge),
            end(b),
            at(b, -1.0, edge),
        ]);
    }
}

/// What the flush reads of the Win3D camera (`Win3DGetCamera`): the
/// getters `Win3DApplyCamera` calls and the view rectangle.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Win3dView {
    pub fov_y_deg: f32,
    pub aspect: f32,
    pub z_near: f32,
    pub z_far: f32,
    /// Left, top, right, bottom, inclusive.
    pub viewport: [i32; 4],
    /// `GetViewTransform`.
    pub view: [f32; 12],
}

impl Win3dView {
    /// The values of a camera now (its view transform rebuilt when dirty).
    pub fn of(camera: &mut Camera) -> Win3dView {
        Win3dView {
            fov_y_deg: camera.fov_y_deg,
            aspect: camera.aspect,
            z_near: camera.z_near,
            z_far: camera.z_far,
            viewport: camera.viewport,
            view: camera.view_transform().to_words(),
        }
    }

    /// `Win3DApplyCamera` with this camera.
    pub fn apply(&self, layer: &mut Win3dLayer, pipe: &mut dyn Win3dPipeline) {
        layer.apply_camera(
            pipe,
            self.fov_y_deg,
            self.aspect,
            self.z_near,
            self.z_far,
            &self.view,
        );
    }
}

/// `G_ImmedBatchA` and `G_ImmedBatchB`.
#[derive(Clone, Debug, PartialEq)]
pub struct LineBatchers {
    pub a: LineBatcher,
    pub b: LineBatcher,
}

impl Default for LineBatchers {
    fn default() -> Self {
        LineBatchers::new()
    }
}

/// Which batcher a line goes to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Batch {
    /// The wire controls' lines, through the camera's perspective.
    A,
    /// The HUD's 2D lines (`Win3DImmedDrawLine`).
    B,
}

impl LineBatchers {
    /// `Win3DImmedBatchCreateAll`: both empty, B flushed in 2D.
    #[must_use]
    pub fn new() -> LineBatchers {
        LineBatchers {
            a: LineBatcher::new(false),
            b: LineBatcher::new(true),
        }
    }

    /// `Win3DImmedBatchReset`: every dirty batcher rewound.
    pub fn reset(&mut self) {
        self.a.reset();
        self.b.reset();
    }

    /// `Win3DImmedBatchFlushAll`: A, then B.
    pub fn flush_all(
        &mut self,
        layer: &mut Win3dLayer,
        pipe: &mut dyn Win3dPipeline,
        camera: &Win3dView,
    ) {
        flush(&mut self.a, layer, pipe, camera);
        flush(&mut self.b, layer, pipe, camera);
    }

    /// `ImmedBatchAddLine`: both batchers flushed and rewound first when
    /// the line would not fit.
    #[allow(clippy::too_many_arguments)]
    pub fn add_line(
        &mut self,
        batch: Batch,
        layer: &mut Win3dLayer,
        pipe: &mut dyn Win3dPipeline,
        camera: &Win3dView,
        a: [f32; 3],
        b: [f32; 3],
        color: u32,
        edge: u32,
        width: f32,
    ) {
        let target = match batch {
            Batch::A => &self.a,
            Batch::B => &self.b,
        };
        if target.indices.len() + 12 >= BATCH_INDICES || target.vertices.len() + 8 >= BATCH_VERTICES
        {
            self.flush_all(layer, pipe, camera);
            self.reset();
        }
        let target = match batch {
            Batch::A => &mut self.a,
            Batch::B => &mut self.b,
        };
        target.push_line(a, b, color, edge, width);
    }
}

/// `FlushImmedBatch`: the dirty byte raised; nothing more for an empty
/// batcher; else the camera's perspective (A) or 2D box (B) with lighting,
/// depth, culling and texturing off and blending on, the vertex colour
/// states, lighting saved and off, the stage tables, one indexed
/// triangle-list draw, and the perspective again after a 2D flush.
pub fn flush(
    batch: &mut LineBatcher,
    layer: &mut Win3dLayer,
    pipe: &mut dyn Win3dPipeline,
    camera: &Win3dView,
) {
    batch.dirty = true;
    if batch.vertices.is_empty() {
        return;
    }
    if batch.ortho {
        layer.disable(pipe, capability::LIGHTING);
        layer.disable(pipe, capability::TEXTURE_2D);
        layer.disable(pipe, capability::CULL_FACE);
        layer.disable(pipe, capability::DEPTH_TEST);
        layer.disable(pipe, capability::FOG);
        pipe.set_render_state(rs::CULLMODE, 1);
        layer.set_ortho_2d_for(pipe, camera.viewport);
        pipe.set_render_state(rs::AMBIENT, 0xFFFF_FFFF);
    } else {
        camera.apply(layer, pipe);
        layer.disable(pipe, capability::LIGHTING);
        layer.disable(pipe, capability::DEPTH_TEST);
        layer.disable(pipe, capability::CULL_FACE);
        layer.disable(pipe, capability::TEXTURE_2D);
    }
    layer.enable(pipe, capability::BLEND);
    layer.set_blend_func(pipe, BLEND_SRC_ALPHA, BLEND_INV_SRC_ALPHA);

    pipe.set_render_state(rs::COLORVERTEX, 1);
    pipe.set_render_state(rs::DIFFUSEMATERIALSOURCE, 1);
    let saved_lighting = pipe.render_state(rs::LIGHTING);
    pipe.set_render_state(rs::LIGHTING, 0);
    for stage in 0..2 {
        for (state, value) in IMMED_TSS {
            pipe.set_stage_state(stage, state, value);
        }
    }
    pipe.draw_line_batch(&batch.vertices, &batch.indices);
    pipe.set_render_state(rs::COLORVERTEX, 0);
    if batch.ortho {
        camera.apply(layer, pipe);
    }
    pipe.set_render_state(rs::LIGHTING, saved_lighting);
}

/// `PrepUiOverlayPipeline` (`UiOverlayPrepPipeline`): under a pushed 2D
/// projection, point filtering forced (`G_UiOverlayForceTss`), depth
/// function `ALWAYS` and no depth write, the font atlas's screen-quad list
/// flushed; then everything put back.
pub fn prep_ui_overlay(
    layer: &mut Win3dLayer,
    pipe: &mut dyn Win3dPipeline,
    quads: &mut QuadBatcher,
    font_key: u32,
) {
    layer.push_projection(pipe);
    layer.set_ortho_2d(pipe);
    let saved_10 = pipe.stage_state(0, 0x10);
    let saved_11 = pipe.stage_state(0, 0x11);
    pipe.set_stage_state(0, 0x10, 1);
    pipe.set_stage_state(0, 0x11, 1);
    let saved_zfunc = pipe.render_state(rs::ZFUNC);
    pipe.set_render_state(rs::ZFUNC, 8);
    let saved_zwrite = pipe.render_state(rs::ZWRITEENABLE);
    pipe.set_render_state(rs::ZWRITEENABLE, 0);
    quads.flush(pipe, font_key);
    pipe.set_stage_state(0, 0x10, saved_10);
    pipe.set_stage_state(0, 0x11, saved_11);
    layer.pop_projection(pipe);
    pipe.set_render_state(rs::ZWRITEENABLE, saved_zwrite);
    pipe.set_render_state(rs::ZFUNC, saved_zfunc);
}

/// The states [`ime_pass_begin`] saved for [`ime_pass_end`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ImePassSaved {
    stage_10: u32,
    stage_11: u32,
    zfunc: u32,
    zwrite: u32,
}

/// `ImeFocusDrawPass` up to `IMEFocus::Draw`: the 2D projection pushed,
/// point filtering, depth function `ALWAYS`, no depth write, no culling.
pub fn ime_pass_begin(layer: &mut Win3dLayer, pipe: &mut dyn Win3dPipeline) -> ImePassSaved {
    layer.push_projection(pipe);
    layer.set_ortho_2d(pipe);
    let stage_10 = pipe.stage_state(0, 0x10);
    let stage_11 = pipe.stage_state(0, 0x11);
    pipe.set_stage_state(0, 0x10, 1);
    pipe.set_stage_state(0, 0x11, 1);
    let zfunc = pipe.render_state(rs::ZFUNC);
    pipe.set_render_state(rs::ZFUNC, 8);
    let zwrite = pipe.render_state(rs::ZWRITEENABLE);
    pipe.set_render_state(rs::ZWRITEENABLE, 0);
    layer.disable(pipe, capability::CULL_FACE);
    ImePassSaved {
        stage_10,
        stage_11,
        zfunc,
        zwrite,
    }
}

/// `ImeFocusDrawPass` after `IMEFocus::Draw`: the "IMEbkground" and
/// "IMEtext" lists flushed, then everything put back.
pub fn ime_pass_end(
    layer: &mut Win3dLayer,
    pipe: &mut dyn Win3dPipeline,
    quads: &mut QuadBatcher,
    saved: ImePassSaved,
) {
    quads.flush(pipe, ime_background_key());
    quads.flush(pipe, ime_text_key());
    pipe.set_stage_state(0, 0x10, saved.stage_10);
    pipe.set_stage_state(0, 0x11, saved.stage_11);
    layer.enable(pipe, capability::CULL_FACE);
    layer.pop_projection(pipe);
    pipe.set_render_state(rs::ZWRITEENABLE, saved.zwrite);
    pipe.set_render_state(rs::ZFUNC, saved.zfunc);
}

/// `ImeFocusDrawPass`, for an IME focus with input boxes: under a pushed
/// 2D projection, point filtering, depth function `ALWAYS`, no depth write
/// and no culling, `IMEFocus::Draw` and then the "IMEbkground" and
/// "IMEtext" screen-quad lists flushed; then everything put back.
///
/// This one has no `IMEFocus::Draw` between the two halves; the engine
/// runs the client's draw (`ui::ime::ImeBoxes::draw`) between
/// [`ime_pass_begin`] and [`ime_pass_end`].
pub fn ime_focus_draw_pass(
    layer: &mut Win3dLayer,
    pipe: &mut dyn Win3dPipeline,
    quads: &mut QuadBatcher,
) {
    let saved = ime_pass_begin(layer, pipe);
    ime_pass_end(layer, pipe, quads, saved);
}

/// `G_ImeBackgroundKey`: the CRC of "IMEbkground".
#[must_use]
pub fn ime_background_key() -> u32 {
    name_crc32(b"IMEbkground")
}

/// `G_ImeTextKey`: the CRC of "IMEtext".
#[must_use]
pub fn ime_text_key() -> u32 {
    name_crc32(b"IMEtext")
}

/// The font atlas's material key (`FontAtlasMaterialKey`: the CRC of
/// "font").
#[must_use]
pub fn font_atlas_key() -> u32 {
    name_crc32(b"font")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ui::widescreen::Surface;
    use crate::ui::win3d_immediate::{PipeCall, RecordingPipeline};

    #[test]
    fn a_line_is_eight_vertices_across_its_perpendicular() {
        let mut b = LineBatchers::new();
        let mut layer = Win3dLayer::new(640, 480);
        let mut pipe = RecordingPipeline::default();
        let cam = Win3dView::of(&mut Camera::win3d(Surface::new(640, 480)));
        b.reset();
        b.add_line(
            Batch::A,
            &mut layer,
            &mut pipe,
            &cam,
            [0.0, 0.0, -1.0],
            [10.0, 0.0, -1.0],
            0xFF00_00FF,
            0x0000_00FF,
            2.0,
        );
        assert_eq!(b.a.vertices.len(), 8);
        assert_eq!(b.a.indices[..6], [0, 1, 2, 2, 3, 1]);
        // A horizontal line is pushed along y.
        let v = &b.a.vertices;
        assert!(
            (v[1].x - 0.0).abs() < 1e-5 && (v[1].y + 2.0).abs() < 1e-5,
            "{v:?}"
        );
        assert_eq!(v[1].color, 0x0000_00FF);
        b.flush_all(&mut layer, &mut pipe, &cam);
        assert_eq!(
            pipe.calls
                .iter()
                .filter(|c| matches!(c, PipeCall::LineBatch(..)))
                .count(),
            1
        );
        assert!(b.a.dirty && b.b.dirty);
        b.reset();
        assert!(b.a.vertices.is_empty());
    }
}
