//! The Win3D immediate-mode layer (`win3d_immediate.cpp`, the reference
//! build's port of the original's `Client\cmn\OpenGL.cpp`), with the
//! modelview binding of `star_sphere.cpp` (`Win3DBindModelview`) and the
//! camera application of `win3d_render.cpp` (`Win3DApplyCamera`).
//!
//! The layer emulates OpenGL's immediate mode on the renderer: `begin` with
//! a primitive, the current colour and texture coordinate stamped onto each
//! `vertex` of an accumulator of FVF 0x142 vertices (position, diffuse,
//! one texture coordinate), and one draw at `end` (a line loop as a strip
//! and a closing indexed line, quads as an indexed triangle list), with
//! the texture stages set up for the bound texture and the vertex colour
//! driving the diffuse material. It caches the depth and blend states the
//! capability switches re-apply, the projection (an orthographic and a
//! perspective kind with a change cache and a push/pop save) and the last
//! viewport.
//!
//! The renderer calls go through [`Win3dPipeline`] (`IRenderPipeline`'s
//! functions the layer and its renderables call): [`FfPipeline`] is the
//! fixed-function device model, [`RecordingPipeline`] keeps the calls for
//! the oracle comparison. [`Win3dLayer`] holds the module's globals
//! (decision T10).
//!
//! The fuzzy lines the tractor beam draws (`Win3DDrawFuzzyLine`,
//! `Win3DDrawFuzzyArc`) are in [`fuzzy`]. The vertex overflow's
//! rate-limited warning is a counter here (divergence D480).

use std::collections::{BTreeMap, BTreeSet};

use neuron_core::math::{Transform, mul_transform};
use neuron_core::num::trunc_to_i32;

use super::quad_batch::QuadDevice;
use crate::render::ff::buffers::IndexSource;
use crate::render::ff::device::{Backend, Device};
use crate::render::ff::fvf::Fvf;
use crate::render::texture::device::HandleTable;
use crate::text::font::quad::TlVertex;

pub use super::immediate::{capability, primitive};

pub mod fuzzy;

pub use fuzzy::{FuzzyLine, random_vector};

/// The accumulator's vertex format: `XYZ | DIFFUSE | TEX1`.
pub const FVF: u32 = 0x142;
/// The vertex and index capacities (`0x3000` each).
pub const MAX_VERTICES: usize = 0x3000;
pub const MAX_INDICES: usize = 0x3000;
/// The most indices the quad path triangulates.
pub const MAX_QUAD_INDICES: usize = 0xAAA;

/// `D3DRS_*` the layer sets.
pub mod rs {
    pub const ZENABLE: u32 = 7;
    pub const ZWRITEENABLE: u32 = 14;
    pub const SRCBLEND: u32 = 19;
    pub const DESTBLEND: u32 = 20;
    pub const CULLMODE: u32 = 22;
    pub const ZFUNC: u32 = 23;
    pub const ALPHABLENDENABLE: u32 = 27;
    pub const FOGENABLE: u32 = 28;
    pub const FOGCOLOR: u32 = 34;
    pub const FOGTABLEMODE: u32 = 35;
    pub const FOGSTART: u32 = 36;
    pub const FOGEND: u32 = 37;
    pub const FOGDENSITY: u32 = 38;
    pub const CLIPPING: u32 = 136;
    pub const LIGHTING: u32 = 137;
    pub const AMBIENT: u32 = 139;
    pub const FOGVERTEXMODE: u32 = 140;
    pub const COLORVERTEX: u32 = 141;
    pub const DIFFUSEMATERIALSOURCE: u32 = 145;
    pub const DITHERENABLE: u32 = 26;
}

/// The pipeline state that asks whether the pipeline wants one combined
/// modelview (`kPipelineStateViewspace`).
pub const PIPELINE_VIEWSPACE: i32 = 7;
/// The pipeline state of the clear colour.
pub const PIPELINE_CLEAR_COLOR: i32 = 8;

/// What a textured stage 0 is set to (`kStageTextured`: `MODULATE` of the
/// texture and the diffuse, colour and alpha), states 1 to 6.
const STAGE_TEXTURED: [u32; 6] = [4, 2, 0, 4, 2, 0];
/// What an untextured stage is set to (`kStageDisabled`).
const STAGE_DISABLED: [u32; 6] = [1, 2, 1, 1, 2, 1];

/// The projection cache's kinds.
const KIND_PERSPECTIVE: i32 = 1;
const KIND_ORTHO: i32 = 2;

/// `GL_VIEWPORT`, the one query `Win3DGetViewport` answers.
pub const QUERY_VIEWPORT: i32 = 0xBA2;

const IDENTITY12: [f32; 12] = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0];

/// One vertex of FVF 0x142 (`FWin3DVertex`, 24 bytes).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Vertex {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    /// `0xAARRGGBB`.
    pub diffuse: u32,
    pub u: f32,
    pub v: f32,
}

impl Vertex {
    /// The vertex's 24 bytes, little-endian.
    #[must_use]
    pub fn bytes(&self) -> [u8; 24] {
        let mut out = [0u8; 24];
        let words = [
            self.x.to_bits(),
            self.y.to_bits(),
            self.z.to_bits(),
            self.diffuse,
            self.u.to_bits(),
            self.v.to_bits(),
        ];
        for (chunk, word) in out.as_chunks_mut::<4>().0.iter_mut().zip(words) {
            chunk.copy_from_slice(&word.to_le_bytes());
        }
        out
    }
}

/// One vertex of the line batchers (`FImmedVertex`, FVF 0x42, 16 bytes).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct LineVertex {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    /// `0xAARRGGBB`.
    pub color: u32,
}

impl LineVertex {
    /// The vertex's 16 bytes, little-endian.
    #[must_use]
    pub fn bytes(&self) -> [u8; 16] {
        let mut out = [0u8; 16];
        let words = [
            self.x.to_bits(),
            self.y.to_bits(),
            self.z.to_bits(),
            self.color,
        ];
        for (chunk, word) in out.as_chunks_mut::<4>().0.iter_mut().zip(words) {
            chunk.copy_from_slice(&word.to_le_bytes());
        }
        out
    }
}

/// The renderer calls of the layer and its renderables (`IRenderPipeline`,
/// and for the line batchers the vertex buffer, index buffer and draw
/// interfaces), beyond the screen-quad batcher's.
pub trait Win3dPipeline: QuadDevice {
    /// `GetTextureStageState`; 0 when the pipeline has no such state.
    fn stage_state(&mut self, stage: u32, state: u32) -> u32;
    /// `GetPipelineState`: `None` when the pipeline does not answer (the
    /// caller's seed stays).
    fn pipeline_state(&mut self, id: i32) -> Option<i32>;
    /// `IsTexture`: whether the pipeline knows the handle.
    fn is_texture(&mut self, handle: u32) -> bool;
    fn set_modelview(&mut self, m: &[f32; 12]);
    fn set_world(&mut self, m: &[f32; 12]);
    fn set_view(&mut self, m: &[f32; 12]);
    fn set_ortho(&mut self, left: f32, right: f32, bottom: f32, top: f32, near: f32, far: f32);
    fn set_perspective(&mut self, fov_deg: f32, aspect: f32, near: f32, far: f32);
    /// `GetProjection`.
    fn projection(&mut self) -> [f32; 16];
    fn set_projection(&mut self, m: &[f32; 16]);
    /// `SetViewportRect(x, y, width, height)`.
    fn set_viewport_rect(&mut self, x: i32, y: i32, width: i32, height: i32);
    /// `DrawPrimitive(prim, 0x142, vertices, count)`.
    fn draw_primitive(&mut self, prim: u32, vertices: &[Vertex]);
    /// `DrawIndexedPrimitiveUp(prim, 0x142, vertices, count, indices)`.
    fn draw_indexed_primitive(&mut self, prim: u32, vertices: &[Vertex], indices: &[u16]);
    /// A line batcher's flush: `CopyVertices` of the FVF 0x42 vertices and
    /// `CopyIndices` into the pipeline's current buffers, both selected,
    /// one indexed triangle-list draw of all of them.
    fn draw_line_batch(&mut self, vertices: &[LineVertex], indices: &[u16]);
}

/// `ScaleClampToByte`: times 255, truncated, then clamped.
fn scale_clamp_to_byte(component: f32) -> u32 {
    let value = trunc_to_i32(f64::from(component) * 255.0);
    value.clamp(0, 0xFF).cast_unsigned()
}

/// The four floats packed as `Win3DColor4f` packs them.
#[must_use]
pub fn pack_color4f(r: f32, g: f32, b: f32, a: f32) -> u32 {
    (scale_clamp_to_byte(a) << 24)
        | (scale_clamp_to_byte(r) << 16)
        | (scale_clamp_to_byte(g) << 8)
        | scale_clamp_to_byte(b)
}

/// The state of `win3d_immediate.cpp`.
#[derive(Clone, Debug, PartialEq)]
pub struct Win3dLayer {
    vertices: Vec<Vertex>,
    indices: Vec<u16>,
    primitive: i32,
    /// `G_bBufferLocked`: nothing in the layer sets it; a locked buffer
    /// would keep its vertices across begins.
    buffer_locked: bool,
    /// The current colour, `0xAARRGGBB` (0 before any colour call).
    pub colour: u32,
    u: f32,
    v: f32,
    texture_bound: bool,
    bound_texture: u32,
    lighting: bool,
    pub alpha_blend: u32,
    pub src_blend: u32,
    pub dest_blend: u32,
    pub depth_test: bool,
    pub depth_write: u32,
    pub depth_func: u32,
    /// `GWin3DModelview`.
    pub modelview: [f32; 12],
    /// `G_bProjectionCacheEnabled`.
    pub cache_enabled: bool,
    kind: i32,
    params: [f32; 6],
    /// How often the projection went to the pipeline, and how often the
    /// cache spared it, since the last present.
    pub set_count: u32,
    pub skip_count: u32,
    saved_kind: i32,
    saved_params: [f32; 6],
    saved_matrix: [f32; 16],
    /// Left, flipped top, width, height: what `Win3DGetViewport` answers.
    viewport: [i32; 4],
    /// The surface size (`G_RenderSurfaceWidth`, `G_RenderSurfaceHeight`).
    pub surface: (u32, u32),
    /// Vertices dropped because the accumulator was full (the reference
    /// warns at most once a second; D480).
    pub dropped: u32,
}

impl Win3dLayer {
    /// The layer as the static initializers leave it, over a surface of
    /// `width` by `height`.
    #[must_use]
    pub fn new(width: u32, height: u32) -> Win3dLayer {
        Win3dLayer {
            vertices: Vec::with_capacity(64),
            indices: Vec::with_capacity(64),
            primitive: -1,
            buffer_locked: false,
            colour: 0,
            u: 0.0,
            v: 0.0,
            texture_bound: false,
            bound_texture: 0,
            lighting: false,
            alpha_blend: 0,
            src_blend: 2,
            dest_blend: 2,
            depth_test: true,
            depth_write: 1,
            depth_func: 4,
            modelview: IDENTITY12,
            cache_enabled: true,
            kind: 0,
            params: [0.0; 6],
            set_count: 0,
            skip_count: 0,
            saved_kind: 0,
            saved_params: [0.0; 6],
            saved_matrix: [0.0; 16],
            viewport: [0; 4],
            surface: (width, height),
            dropped: 0,
        }
    }

    /// `Win3DResetState`: the defaults `RenderMgrCreateBuffers` installs.
    pub fn reset_state(&mut self) {
        self.src_blend = 2;
        self.dest_blend = 2;
        self.primitive = -1;
        self.vertices.clear();
        self.buffer_locked = false;
        self.indices.clear();
        self.modelview = IDENTITY12;
        self.alpha_blend = 0;
        self.depth_test = true;
        self.depth_write = 1;
        self.depth_func = 4;
        self.texture_bound = false;
        self.bound_texture = 0;
        self.lighting = false;
        self.kind = 0;
        self.saved_kind = 0;
    }

    /// `Win3DBegin`.
    pub fn begin(&mut self, primitive: u32) {
        self.primitive = i32::try_from(primitive).unwrap_or(-1);
        self.indices.clear();
        if !self.buffer_locked {
            self.vertices.clear();
        }
    }

    /// `Win3DVertex3f`: a vertex at the current colour and texture
    /// coordinate, dropped when either buffer is full.
    pub fn vertex(&mut self, x: f32, y: f32, z: f32) {
        if self.indices.len() >= MAX_INDICES || self.vertices.len() >= MAX_VERTICES {
            self.dropped += 1;
            return;
        }
        self.indices.push(self.vertices.len() as u16);
        self.vertices.push(Vertex {
            x,
            y,
            z,
            diffuse: self.colour,
            u: self.u,
            v: self.v,
        });
    }

    /// `Win3DColor4f`.
    pub fn color4f(&mut self, r: f32, g: f32, b: f32, a: f32) {
        self.colour = pack_color4f(r, g, b, a);
    }

    /// `Win3DColor3f`: alpha 0xFF.
    pub fn color3f(&mut self, r: f32, g: f32, b: f32) {
        self.colour = 0xFF00_0000
            | (scale_clamp_to_byte(r) << 16)
            | (scale_clamp_to_byte(g) << 8)
            | scale_clamp_to_byte(b);
    }

    /// `Win3DColor3ub` (and `Win3DColor3ubv`): alpha 0xFF.
    pub fn color3ub(&mut self, r: u8, g: u8, b: u8) {
        self.colour = u32::from_be_bytes([0xFF, r, g, b]);
    }

    /// `Win3DColor4ub` (and `Win3DColor4ubv` with the bytes R, G, B, A).
    pub fn color4ub(&mut self, r: u8, g: u8, b: u8, a: u8) {
        self.colour = u32::from_be_bytes([a, r, g, b]);
    }

    /// `Win3DTexCoord2f`.
    pub fn tex_coord(&mut self, u: f32, v: f32) {
        self.u = u;
        self.v = v;
    }

    /// `Win3DBindTexture`: the handle for the next `end`, and the bound
    /// flag raised.
    pub fn bind_texture(&mut self, handle: u32) {
        self.bound_texture = handle;
        self.texture_bound = true;
    }

    /// `Win3DSetPipelineClearColor`: packed as `Win3DColor4f` packs, as
    /// pipeline state 8.
    pub fn set_pipeline_clear_color(
        &mut self,
        pipe: &mut dyn Win3dPipeline,
        r: f32,
        g: f32,
        b: f32,
        a: f32,
    ) {
        pipe.set_pipeline_state(PIPELINE_CLEAR_COLOR, pack_color4f(r, g, b, a).cast_signed());
    }

    fn apply_depth(&self, pipe: &mut dyn Win3dPipeline, enable: u32) {
        pipe.set_render_state(rs::ZFUNC, self.depth_func);
        pipe.set_render_state(rs::ZENABLE, enable);
        pipe.set_render_state(rs::ZWRITEENABLE, self.depth_write);
    }

    /// `Win3DEnable`; a capability it does not know does nothing (the
    /// reference warns).
    pub fn enable(&mut self, pipe: &mut dyn Win3dPipeline, cap: u32) {
        match cap {
            capability::DEPTH_TEST => {
                self.depth_test = true;
                self.apply_depth(pipe, 1);
            }
            capability::CULL_FACE => pipe.set_render_state(rs::CULLMODE, 2),
            capability::LIGHTING => self.lighting = true,
            capability::BLEND => {
                self.alpha_blend = 1;
                pipe.set_render_state(rs::ALPHABLENDENABLE, 1);
            }
            capability::TEXTURE_2D => self.texture_bound = true,
            _ => {}
        }
    }

    /// `Win3DDisable`.
    pub fn disable(&mut self, pipe: &mut dyn Win3dPipeline, cap: u32) {
        match cap {
            capability::DEPTH_TEST => {
                self.depth_test = false;
                self.apply_depth(pipe, 0);
            }
            capability::CULL_FACE => pipe.set_render_state(rs::CULLMODE, 1),
            capability::LIGHTING => self.lighting = false,
            capability::FOG => {
                for state in [
                    rs::FOGENABLE,
                    rs::FOGTABLEMODE,
                    rs::FOGVERTEXMODE,
                    rs::FOGCOLOR,
                    rs::FOGSTART,
                    rs::FOGEND,
                    rs::FOGDENSITY,
                ] {
                    pipe.set_render_state(state, 0);
                }
            }
            capability::BLEND => {
                self.alpha_blend = 0;
                pipe.set_render_state(rs::ALPHABLENDENABLE, 0);
            }
            capability::TEXTURE_2D => {
                self.texture_bound = false;
                pipe.set_stage_texture(0);
            }
            _ => {}
        }
    }

    /// `Win3DSetTextureFilterLinear`: stage 0's magnification and
    /// minification filters linear.
    pub fn set_texture_filter_linear(&self, pipe: &mut dyn Win3dPipeline) {
        pipe.set_stage_state(0, 0x0D, 3);
        pipe.set_stage_state(0, 0x0E, 3);
    }

    /// `Win3DSetTextureFilterPoint`.
    pub fn set_texture_filter_point(&self, pipe: &mut dyn Win3dPipeline) {
        pipe.set_stage_state(0, 0x0D, 1);
        pipe.set_stage_state(0, 0x0E, 1);
    }

    /// `Win3DSetDepthFunc`.
    pub fn set_depth_func(&mut self, pipe: &mut dyn Win3dPipeline, func: u32) {
        self.depth_func = func;
        self.apply_depth(pipe, u32::from(self.depth_test));
    }

    /// `Win3DSetBlendFunc`, with the blend enable re-applied.
    pub fn set_blend_func(&mut self, pipe: &mut dyn Win3dPipeline, src: u32, dest: u32) {
        self.src_blend = src;
        self.dest_blend = dest;
        pipe.set_render_state(rs::SRCBLEND, src);
        pipe.set_render_state(rs::DESTBLEND, dest);
        pipe.set_render_state(rs::ALPHABLENDENABLE, self.alpha_blend);
    }

    /// `Win3DSetDepthWrite`.
    pub fn set_depth_write(&mut self, pipe: &mut dyn Win3dPipeline, on: u32) {
        self.depth_write = on;
        self.apply_depth(pipe, u32::from(self.depth_test));
    }

    fn surface_height(&self) -> i32 {
        i32::try_from(self.surface.1).unwrap_or(i32::MAX)
    }

    /// `Win3DSetViewport`: the camera's view rectangle (left, top, right,
    /// bottom, inclusive), or the whole surface for `None`.
    pub fn set_viewport(&mut self, pipe: &mut dyn Win3dPipeline, rect: Option<[i32; 4]>) {
        let (left, top, width, height) = match rect {
            Some(r) => (
                r[0],
                r[1],
                r[2].wrapping_sub(r[0]).wrapping_add(1),
                r[3].wrapping_sub(r[1]).wrapping_add(1),
            ),
            None => (
                0,
                0,
                i32::try_from(self.surface.0).unwrap_or(i32::MAX),
                self.surface_height(),
            ),
        };
        self.viewport = [
            left,
            self.surface_height().wrapping_sub(height).wrapping_sub(top),
            width,
            height,
        ];
        pipe.set_viewport_rect(left, top, width, height);
    }

    /// `Win3DGetViewport`: left, flipped top, width and height for the
    /// `GL_VIEWPORT` query, `None` for any other.
    #[must_use]
    pub fn get_viewport(&self, query: i32) -> Option<[i32; 4]> {
        (query == QUERY_VIEWPORT).then_some(self.viewport)
    }

    /// `Win3DSetViewportRect`: `y` is the flipped origin.
    pub fn set_viewport_rect(
        &mut self,
        pipe: &mut dyn Win3dPipeline,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
    ) {
        self.viewport = [x, y, width, height];
        pipe.set_viewport_rect(
            x,
            self.surface_height().wrapping_sub(y).wrapping_sub(height),
            width,
            height,
        );
    }

    /// `Win3DSetOrtho2DForCamera`: the 2D box of a camera's view rectangle.
    pub fn set_ortho_2d_for(&mut self, pipe: &mut dyn Win3dPipeline, rect: [i32; 4]) {
        let width = rect[2].wrapping_sub(rect[0]).wrapping_add(1) as f32;
        let height = rect[3].wrapping_sub(rect[1]).wrapping_add(1) as f32;
        self.set_projection(pipe, [0.0, width, height, 0.0, -1.0, 1.0]);
    }

    fn put_modelview(&self, pipe: &mut dyn Win3dPipeline, identity_world_view: bool) {
        let viewspace = pipe.pipeline_state(PIPELINE_VIEWSPACE).unwrap_or(1);
        if viewspace != 0 {
            pipe.set_modelview(&self.modelview);
        } else if identity_world_view {
            pipe.set_world(&self.modelview);
            pipe.set_view(&self.modelview);
        }
    }

    /// `Win3DSetProjection`: the modelview reset to the identity first,
    /// then the orthographic projection through the change cache.
    /// `args` are left, right, bottom, top, near and far.
    pub fn set_projection(&mut self, pipe: &mut dyn Win3dPipeline, args: [f32; 6]) {
        self.modelview = IDENTITY12;
        self.put_modelview(pipe, true);
        if self.cache_enabled {
            let mut changed = self.kind != KIND_ORTHO;
            for (slot, arg) in self.params.iter_mut().zip(args) {
                if !changed && *slot != arg {
                    changed = true;
                }
                *slot = arg;
            }
            if !changed {
                self.skip_count += 1;
                return;
            }
            self.set_count += 1;
            self.kind = KIND_ORTHO;
        }
        let [l, r, b, t, n, f] = args;
        pipe.set_ortho(l, r, b, t, n, f);
    }

    /// `Win3DSetOrtho2D`: the surface in pixels, y down.
    pub fn set_ortho_2d(&mut self, pipe: &mut dyn Win3dPipeline) {
        let w = i32::try_from(self.surface.0).unwrap_or(i32::MAX) as f32;
        let h = self.surface_height() as f32;
        self.set_projection(pipe, [0.0, w, h, 0.0, -1.0, 1.0]);
    }

    /// `Win3DSetPerspective`: the change cache on the first four slots;
    /// the modelview is left alone.
    pub fn set_perspective(
        &mut self,
        pipe: &mut dyn Win3dPipeline,
        fov_deg: f32,
        aspect: f32,
        near: f32,
        far: f32,
    ) {
        if self.cache_enabled {
            let args = [fov_deg, aspect, near, far];
            let mut changed = self.kind != KIND_PERSPECTIVE;
            for (slot, arg) in self.params.iter_mut().zip(args) {
                if !changed && *slot != arg {
                    changed = true;
                }
                *slot = arg;
            }
            if !changed {
                self.skip_count += 1;
                return;
            }
            self.set_count += 1;
            self.kind = KIND_PERSPECTIVE;
        }
        pipe.set_perspective(fov_deg, aspect, near, far);
    }

    /// `Win3DPushProjection`: the cache's kind and parameters, or the
    /// pipeline's matrix when the cache is off.
    pub fn push_projection(&mut self, pipe: &mut dyn Win3dPipeline) {
        if self.cache_enabled {
            self.saved_kind = self.kind;
            self.saved_params = self.params;
            return;
        }
        self.saved_matrix = pipe.projection();
    }

    /// `Win3DPopProjection`.
    pub fn pop_projection(&mut self, pipe: &mut dyn Win3dPipeline) {
        if !self.cache_enabled {
            pipe.set_projection(&self.saved_matrix);
            return;
        }
        let p = self.saved_params;
        if self.saved_kind == KIND_PERSPECTIVE {
            self.set_perspective(pipe, p[0], p[1], p[2], p[3]);
        } else if self.saved_kind == KIND_ORTHO {
            self.set_projection(pipe, p);
        }
        self.saved_kind = 0;
    }

    /// `Win3DOnPresentFrame`: the counters zeroed, the cache on unless the
    /// `LIKE_PETAL` option (0x3E) is set.
    pub fn on_present_frame(&mut self, like_petal: i32) {
        self.set_count = 0;
        self.skip_count = 0;
        self.cache_enabled = like_petal == 0;
    }

    /// `Win3DApplyCamera`: the camera's perspective (far, near, aspect and
    /// field of view read in that order by the caller), then its view
    /// transform as the modelview, or with world and view split, an
    /// identity view and the view as the world.
    pub fn apply_camera(
        &mut self,
        pipe: &mut dyn Win3dPipeline,
        fov_deg: f32,
        aspect: f32,
        near: f32,
        far: f32,
        view: &[f32; 12],
    ) {
        self.set_perspective(pipe, fov_deg, aspect, near, far);
        self.modelview = *view;
        let viewspace = pipe.pipeline_state(PIPELINE_VIEWSPACE).unwrap_or(1);
        if viewspace != 0 {
            pipe.set_modelview(&self.modelview);
            return;
        }
        pipe.set_view(&IDENTITY12);
        pipe.set_world(view);
    }

    /// `Win3DBindModelview(camera, xform)`: with a camera's view transform,
    /// the modelview is the view times `xform` and the view goes to the
    /// pipeline with the world split; without one, the modelview is
    /// `xform` (the caller applies the widescreen shear to it) and the
    /// view is the identity. The world is `world`, the caller's matrix.
    pub fn bind_modelview(
        &mut self,
        pipe: &mut dyn Win3dPipeline,
        view: Option<&Transform>,
        xform: &[f32; 12],
        world: &[f32; 12],
    ) {
        let local = match view {
            Some(v) => mul_transform(v, &Transform::from_words(*xform)).to_words(),
            None => *xform,
        };
        self.modelview = local;
        let viewspace = pipe.pipeline_state(PIPELINE_VIEWSPACE).unwrap_or(1);
        if viewspace != 0 {
            pipe.set_modelview(&self.modelview);
            return;
        }
        match view {
            Some(v) => pipe.set_view(&v.to_words()),
            None => pipe.set_view(&IDENTITY12),
        }
        pipe.set_world(world);
    }

    /// `ApplyTextureStages`.
    fn apply_texture_stages(&mut self, pipe: &mut dyn Win3dPipeline) {
        let table = |pipe: &mut dyn Win3dPipeline, stage: u32, t: &[u32; 6]| {
            for (i, &value) in (1u32..).zip(t) {
                pipe.set_stage_state(stage, i, value);
            }
        };
        if !self.texture_bound {
            pipe.set_stage_texture(0);
            table(pipe, 0, &STAGE_DISABLED);
            return;
        }
        if !pipe.is_texture(self.bound_texture) {
            pipe.set_stage_texture(0);
            table(pipe, 0, &STAGE_DISABLED);
            self.bound_texture = 0;
            return;
        }
        pipe.set_stage_texture(self.bound_texture);
        pipe.set_stage_state(0, 0x0B, 0);
        table(pipe, 0, &STAGE_TEXTURED);
        table(pipe, 1, &STAGE_DISABLED);
    }

    /// `Win3DEnd`: the draw of what was accumulated, then everything reset
    /// whether or not anything was drawn.
    pub fn end(&mut self, pipe: &mut dyn Win3dPipeline) {
        pipe.set_render_state(rs::CLIPPING, 1);
        self.apply_texture_stages(pipe);
        if !self.vertices.is_empty() {
            pipe.set_render_state(rs::COLORVERTEX, 1);
            pipe.set_render_state(rs::DIFFUSEMATERIALSOURCE, 1);
            let saved_lighting = pipe.render_state(rs::LIGHTING);
            pipe.set_render_state(rs::LIGHTING, u32::from(self.lighting));
            pipe.set_modelview(&self.modelview);
            let v = &self.vertices;
            match self.primitive.cast_unsigned() {
                primitive::POINTS => pipe.draw_primitive(1, v),
                primitive::LINES if !self.buffer_locked => pipe.draw_primitive(2, v),
                primitive::LINES => pipe.draw_indexed_primitive(2, v, &self.indices),
                primitive::LINE_STRIP => pipe.draw_primitive(3, v),
                primitive::TRIANGLES if !self.buffer_locked => pipe.draw_primitive(4, v),
                primitive::TRIANGLES => pipe.draw_indexed_primitive(4, v, &self.indices),
                primitive::TRIANGLE_STRIP => pipe.draw_primitive(5, v),
                primitive::TRIANGLE_FAN | primitive::POLYGON => pipe.draw_primitive(6, v),
                primitive::LINE_LOOP => {
                    pipe.draw_primitive(3, v);
                    let closing = [(v.len() - 1) as u16, 0];
                    pipe.draw_indexed_primitive(2, v, &closing);
                }
                primitive::QUADS => {
                    let usable = self.indices.len().min(MAX_QUAD_INDICES);
                    let mut quads = Vec::with_capacity(usable / 4 * 6);
                    for q in self.indices[..usable].as_chunks::<4>().0 {
                        quads.extend_from_slice(&[q[0], q[1], q[2], q[0], q[2], q[3]]);
                    }
                    pipe.draw_indexed_primitive(4, v, &quads);
                }
                // An invalid mode draws nothing (the reference warns).
                _ => {}
            }
            pipe.set_render_state(rs::COLORVERTEX, 0);
            pipe.set_render_state(rs::LIGHTING, saved_lighting);
        }
        self.primitive = -1;
        self.vertices.clear();
        self.indices.clear();
        self.buffer_locked = false;
        self.texture_bound = false;
        self.bound_texture = 0;
        pipe.set_stage_texture(0);
    }

    /// `Win3DGetVertexCount`.
    #[must_use]
    pub fn vertex_count(&self) -> usize {
        self.vertices.len()
    }

    /// `Win3DGetIndexCount`.
    #[must_use]
    pub fn index_count(&self) -> usize {
        self.indices.len()
    }

    /// `Win3DGetVertex`.
    #[must_use]
    pub fn vertex_at(&self, index: usize) -> Option<&Vertex> {
        self.vertices.get(index)
    }

    /// The projection cache: its kind (0 none, 1 perspective, 2
    /// orthographic) and its six parameters.
    #[must_use]
    pub fn projection_cache(&self) -> (i32, [f32; 6]) {
        (self.kind, self.params)
    }

    /// `Win3DDrawSphereGrid`: texturing off, every ring but the first and
    /// the last as a line loop, then every column as a line strip.
    pub fn draw_sphere_grid(
        &mut self,
        pipe: &mut dyn Win3dPipeline,
        points: &[[f32; 3]],
        cols: usize,
        rows: usize,
    ) {
        self.disable(pipe, capability::TEXTURE_2D);
        if rows > 2 {
            for ring in 1..rows - 1 {
                self.begin(primitive::LINE_LOOP);
                for p in &points[ring * cols..(ring + 1) * cols] {
                    self.vertex(p[0], p[1], p[2]);
                }
                self.end(pipe);
            }
        }
        for col in 0..cols {
            self.begin(primitive::LINE_STRIP);
            for row in 0..rows {
                let p = points[row * cols + col];
                self.vertex(p[0], p[1], p[2]);
            }
            self.end(pipe);
        }
    }
}

/// `Win3DBuildSpherePoints`: `rows` rings of `cols` points from pole to
/// pole at `radius`, the last column from the table's last entry (at most
/// 256 columns).
#[must_use]
pub fn sphere_points(radius: f32, cols: usize, rows: usize) -> Vec<[f32; 3]> {
    let cols = cols.min(256);
    let phi_step = (f64::from(std::f32::consts::PI) / (rows as f64 - 1.0)) as f32;
    let theta_step = (f64::from(std::f32::consts::TAU) / cols as f64) as f32;
    let mut cos_table = Vec::with_capacity(cols);
    let mut sin_table = Vec::with_capacity(cols);
    let mut theta = 0.0f64;
    for _ in 0..cols {
        cos_table.push(theta.cos() as f32);
        sin_table.push(theta.sin() as f32);
        theta += f64::from(theta_step);
    }
    let mut out = Vec::with_capacity(cols * rows);
    let mut phi = 0.0f64;
    for _ in 0..rows {
        let y = (phi.cos() * f64::from(radius)) as f32;
        let sr = f64::from((phi.sin() * f64::from(radius)) as f32);
        for (c, s) in cos_table.iter().zip(&sin_table) {
            out.push([(sr * f64::from(*c)) as f32, y, (sr * f64::from(*s)) as f32]);
        }
        phi += f64::from(phi_step);
    }
    out
}

/// [`Win3dPipeline`] on the fixed-function device model, the renderer
/// interface's texture numbers resolved through a [`HandleTable`].
pub struct FfPipeline<'a, B: Backend> {
    pub device: &'a mut Device<B>,
    pub handles: &'a HandleTable,
}

impl<B: Backend> FfPipeline<'_, B> {
    fn quads(&mut self) -> super::quad_batch::FfQuadDevice<'_, B> {
        super::quad_batch::FfQuadDevice {
            device: self.device,
            handles: self.handles,
        }
    }
}

impl<B: Backend> QuadDevice for FfPipeline<'_, B> {
    fn render_state(&mut self, state: u32) -> u32 {
        self.quads().render_state(state)
    }
    fn set_render_state(&mut self, state: u32, value: u32) {
        self.quads().set_render_state(state, value);
    }
    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32) {
        self.quads().set_stage_state(stage, state, value);
    }
    fn set_pipeline_state(&mut self, id: i32, value: i32) {
        self.quads().set_pipeline_state(id, value);
    }
    fn set_stage_texture(&mut self, texture: u32) {
        self.quads().set_stage_texture(texture);
    }
    fn draw_indexed_quads(&mut self, vertices: &[TlVertex], indices: &[u16]) {
        self.quads().draw_indexed_quads(vertices, indices);
    }
}

fn vertex_bytes(vertices: &[Vertex]) -> Vec<u8> {
    vertices.iter().flat_map(Vertex::bytes).collect()
}

impl<B: Backend> Win3dPipeline for FfPipeline<'_, B> {
    fn stage_state(&mut self, stage: u32, state: u32) -> u32 {
        self.device.state.stage_state_raw(stage, state).unwrap_or(0)
    }
    fn pipeline_state(&mut self, id: i32) -> Option<i32> {
        self.device.state.pipeline_raw(id).map(u32::cast_signed)
    }
    fn is_texture(&mut self, handle: u32) -> bool {
        self.handles
            .get(handle)
            .is_some_and(|h| self.device.textures.get(h).is_ok())
    }
    fn set_modelview(&mut self, m: &[f32; 12]) {
        self.device.set_modelview(m);
    }
    fn set_world(&mut self, m: &[f32; 12]) {
        self.device.transforms.set_world(m);
    }
    fn set_view(&mut self, m: &[f32; 12]) {
        self.device.transforms.set_view(m);
    }
    fn set_ortho(&mut self, left: f32, right: f32, bottom: f32, top: f32, near: f32, far: f32) {
        self.device
            .transforms
            .set_ortho(left, right, bottom, top, near, far);
    }
    fn set_perspective(&mut self, fov_deg: f32, aspect: f32, near: f32, far: f32) {
        let _ = self
            .device
            .transforms
            .set_perspective(fov_deg, aspect, near, far);
    }
    fn projection(&mut self) -> [f32; 16] {
        self.device.transforms.projection()
    }
    fn set_projection(&mut self, m: &[f32; 16]) {
        self.device.transforms.set_projection(m);
    }
    fn set_viewport_rect(&mut self, x: i32, y: i32, width: i32, height: i32) {
        let u = |v: i32| u32::try_from(v).unwrap_or(0);
        self.device.set_viewport(u(x), u(y), u(width), u(height));
    }
    fn draw_primitive(&mut self, prim: u32, vertices: &[Vertex]) {
        let Ok(count) = u32::try_from(vertices.len()) else {
            return;
        };
        let _ = self
            .device
            .draw_up(prim, Fvf(FVF), &vertex_bytes(vertices), count);
    }
    fn draw_indexed_primitive(&mut self, prim: u32, vertices: &[Vertex], indices: &[u16]) {
        let Ok(count) = u32::try_from(vertices.len()) else {
            return;
        };
        let _ =
            self.device
                .draw_indexed_up(prim, Fvf(FVF), &vertex_bytes(vertices), count, indices);
    }
    fn draw_line_batch(&mut self, vertices: &[LineVertex], indices: &[u16]) {
        let (Ok(count), Ok(index_count)) =
            (u32::try_from(vertices.len()), u32::try_from(indices.len()))
        else {
            return;
        };
        let buffers = &mut self.device.buffers;
        let Ok((listing, lock)) = buffers.lock_vertex_memory(Fvf(0x42), count) else {
            return;
        };
        let bytes: Vec<u8> = vertices.iter().flat_map(LineVertex::bytes).collect();
        if buffers
            .write_vb(listing.buffer, lock.offset, &bytes)
            .is_err()
            || buffers.unlock_vb(listing.buffer).is_err()
        {
            return;
        }
        let Ok(start) = buffers.copy_indices(IndexSource::Scratch, 0, indices) else {
            return;
        };
        if buffers
            .select_ib(Some(IndexSource::Scratch), listing.base_vertex)
            .is_err()
        {
            return;
        }
        let _ = self.device.draw_indexed(4, 0, count, start, index_count);
    }
}

/// One call a [`RecordingPipeline`] kept.
#[derive(Clone, Debug, PartialEq)]
pub enum PipeCall {
    GetRenderState(u32, u32),
    RenderState(u32, u32),
    StageState(u32, u32, u32),
    GetStageState(u32, u32, u32),
    PipelineState(i32, i32),
    GetPipelineState(i32, Option<i32>),
    Texture(u32),
    IsTexture(u32, bool),
    Modelview([f32; 12]),
    World([f32; 12]),
    View([f32; 12]),
    Ortho([f32; 6]),
    Perspective([f32; 4]),
    GetProjection,
    SetProjection([f32; 16]),
    Viewport([i32; 4]),
    Draw(u32, Vec<Vertex>),
    DrawIndexed(u32, Vec<Vertex>, Vec<u16>),
    LineBatch(Vec<LineVertex>, Vec<u16>),
    Quads(Vec<TlVertex>, Vec<u16>),
}

/// A [`Win3dPipeline`] that keeps every call. Render and stage states read
/// back the last value set (0 before any), pipeline states answer only
/// once set, the textures in `textures` are known, and the projection is
/// the last one set with `set_projection`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RecordingPipeline {
    pub calls: Vec<PipeCall>,
    pub states: BTreeMap<u32, u32>,
    pub stage_states: BTreeMap<(u32, u32), u32>,
    pub pipeline_states: BTreeMap<i32, i32>,
    pub textures: BTreeSet<u32>,
    pub projection: [f32; 16],
}

impl RecordingPipeline {
    /// The draws (of every kind) kept.
    #[must_use]
    pub fn draw_count(&self) -> usize {
        self.calls
            .iter()
            .filter(|c| {
                matches!(
                    c,
                    PipeCall::Draw(..)
                        | PipeCall::DrawIndexed(..)
                        | PipeCall::LineBatch(..)
                        | PipeCall::Quads(..)
                )
            })
            .count()
    }
}

impl QuadDevice for RecordingPipeline {
    fn render_state(&mut self, state: u32) -> u32 {
        let v = self.states.get(&state).copied().unwrap_or(0);
        self.calls.push(PipeCall::GetRenderState(state, v));
        v
    }
    fn set_render_state(&mut self, state: u32, value: u32) {
        self.states.insert(state, value);
        self.calls.push(PipeCall::RenderState(state, value));
    }
    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32) {
        self.stage_states.insert((stage, state), value);
        self.calls.push(PipeCall::StageState(stage, state, value));
    }
    fn set_pipeline_state(&mut self, id: i32, value: i32) {
        self.pipeline_states.insert(id, value);
        self.calls.push(PipeCall::PipelineState(id, value));
    }
    fn set_stage_texture(&mut self, texture: u32) {
        self.calls.push(PipeCall::Texture(texture));
    }
    fn draw_indexed_quads(&mut self, vertices: &[TlVertex], indices: &[u16]) {
        self.calls
            .push(PipeCall::Quads(vertices.to_vec(), indices.to_vec()));
    }
}

impl Win3dPipeline for RecordingPipeline {
    fn stage_state(&mut self, stage: u32, state: u32) -> u32 {
        let v = self.stage_states.get(&(stage, state)).copied().unwrap_or(0);
        self.calls.push(PipeCall::GetStageState(stage, state, v));
        v
    }
    fn pipeline_state(&mut self, id: i32) -> Option<i32> {
        let v = self.pipeline_states.get(&id).copied();
        self.calls.push(PipeCall::GetPipelineState(id, v));
        v
    }
    fn is_texture(&mut self, handle: u32) -> bool {
        let known = self.textures.contains(&handle);
        self.calls.push(PipeCall::IsTexture(handle, known));
        known
    }
    fn set_modelview(&mut self, m: &[f32; 12]) {
        self.calls.push(PipeCall::Modelview(*m));
    }
    fn set_world(&mut self, m: &[f32; 12]) {
        self.calls.push(PipeCall::World(*m));
    }
    fn set_view(&mut self, m: &[f32; 12]) {
        self.calls.push(PipeCall::View(*m));
    }
    fn set_ortho(&mut self, left: f32, right: f32, bottom: f32, top: f32, near: f32, far: f32) {
        self.calls
            .push(PipeCall::Ortho([left, right, bottom, top, near, far]));
    }
    fn set_perspective(&mut self, fov_deg: f32, aspect: f32, near: f32, far: f32) {
        self.calls
            .push(PipeCall::Perspective([fov_deg, aspect, near, far]));
    }
    fn projection(&mut self) -> [f32; 16] {
        self.calls.push(PipeCall::GetProjection);
        self.projection
    }
    fn set_projection(&mut self, m: &[f32; 16]) {
        self.projection = *m;
        self.calls.push(PipeCall::SetProjection(*m));
    }
    fn set_viewport_rect(&mut self, x: i32, y: i32, width: i32, height: i32) {
        self.calls.push(PipeCall::Viewport([x, y, width, height]));
    }
    fn draw_primitive(&mut self, prim: u32, vertices: &[Vertex]) {
        self.calls.push(PipeCall::Draw(prim, vertices.to_vec()));
    }
    fn draw_indexed_primitive(&mut self, prim: u32, vertices: &[Vertex], indices: &[u16]) {
        self.calls.push(PipeCall::DrawIndexed(
            prim,
            vertices.to_vec(),
            indices.to_vec(),
        ));
    }
    fn draw_line_batch(&mut self, vertices: &[LineVertex], indices: &[u16]) {
        self.calls
            .push(PipeCall::LineBatch(vertices.to_vec(), indices.to_vec()));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::render::ff::defaults::Defaults;
    use crate::render::ff::device::RecordingBackend;

    #[test]
    fn colours_pack_as_the_reference_packs_them() {
        let mut l = Win3dLayer::new(640, 480);
        assert_eq!(l.colour, 0);
        l.color4f(1.0, 0.5, -2.0, 2.0);
        // 0.5 * 255 = 127.5 truncates to 127; out of range clamps.
        assert_eq!(l.colour, 0xFFFF_7F00);
        l.color4f(f32::NAN, 0.0, 0.0, 0.0);
        assert_eq!(l.colour, 0);
        l.color3f(0.0, 1.0, 0.0);
        assert_eq!(l.colour, 0xFF00_FF00);
        l.color3ub(1, 2, 3);
        assert_eq!(l.colour, 0xFF01_0203);
        l.color4ub(1, 2, 3, 4);
        assert_eq!(l.colour, 0x0401_0203);
    }

    #[test]
    fn a_quad_becomes_two_triangles_and_the_state_resets() {
        let mut l = Win3dLayer::new(640, 480);
        let mut p = RecordingPipeline::default();
        p.textures.insert(9);
        l.bind_texture(9);
        l.color3ub(255, 0, 0);
        l.begin(primitive::QUADS);
        for (x, y) in [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)] {
            l.tex_coord(x, y);
            l.vertex(x, y, 0.0);
        }
        l.end(&mut p);
        let draws: Vec<_> = p
            .calls
            .iter()
            .filter_map(|c| match c {
                PipeCall::DrawIndexed(prim, v, i) => Some((*prim, v.len(), i.clone())),
                _ => None,
            })
            .collect();
        assert_eq!(draws, vec![(4, 4, vec![0, 1, 2, 0, 2, 3])]);
        assert!(p.calls.contains(&PipeCall::Texture(9)));
        assert_eq!(p.calls.last(), Some(&PipeCall::Texture(0)));
        assert_eq!(l.vertex_count(), 0);
        // An unknown texture leaves the stage disabled.
        l.bind_texture(5);
        l.begin(primitive::LINE_LOOP);
        l.vertex(0.0, 0.0, 0.0);
        l.vertex(1.0, 0.0, 0.0);
        l.end(&mut p);
        assert!(p.calls.contains(&PipeCall::IsTexture(5, false)));
        assert!(
            p.calls
                .iter()
                .any(|c| matches!(c, PipeCall::DrawIndexed(2, _, i) if i == &vec![1, 0]))
        );
    }

    #[test]
    fn the_projection_cache_skips_repeats_and_push_pop_replays() {
        let mut l = Win3dLayer::new(800, 600);
        let mut p = RecordingPipeline::default();
        l.set_ortho_2d(&mut p);
        l.set_ortho_2d(&mut p);
        assert_eq!((l.set_count, l.skip_count), (1, 1));
        l.push_projection(&mut p);
        l.set_perspective(&mut p, 50.0, 1.25, 0.1, 1000.0);
        l.pop_projection(&mut p);
        let projections: Vec<_> = p
            .calls
            .iter()
            .filter(|c| matches!(c, PipeCall::Ortho(_) | PipeCall::Perspective(_)))
            .cloned()
            .collect();
        assert_eq!(
            projections,
            vec![
                PipeCall::Ortho([0.0, 800.0, 600.0, 0.0, -1.0, 1.0]),
                PipeCall::Perspective([50.0, 1.25, 0.1, 1000.0]),
                PipeCall::Ortho([0.0, 800.0, 600.0, 0.0, -1.0, 1.0]),
            ]
        );
        l.on_present_frame(1);
        assert!(!l.cache_enabled);
    }

    #[test]
    fn the_viewport_query_answers_the_flipped_origin() {
        let mut l = Win3dLayer::new(800, 600);
        let mut p = RecordingPipeline::default();
        l.set_viewport(&mut p, Some([10, 20, 109, 69]));
        assert_eq!(l.get_viewport(QUERY_VIEWPORT), Some([10, 530, 100, 50]));
        assert_eq!(l.get_viewport(0), None);
        let v = l.get_viewport(QUERY_VIEWPORT).unwrap();
        l.set_viewport_rect(&mut p, v[0], v[1], v[2], v[3]);
        assert_eq!(p.calls.last(), Some(&PipeCall::Viewport([10, 20, 100, 50])));
    }

    #[test]
    fn the_device_draws_what_the_layer_ends() {
        let mut device = Device::new(Defaults::builtin(), 64, 64, RecordingBackend::default());
        let handles = HandleTable::new();
        let mut l = Win3dLayer::new(64, 64);
        let mut p = FfPipeline {
            device: &mut device,
            handles: &handles,
        };
        l.set_ortho_2d(&mut p);
        l.begin(primitive::TRIANGLE_FAN);
        for (x, y) in [(0.0, 0.0), (10.0, 0.0), (10.0, 10.0), (0.0, 10.0)] {
            l.vertex(x, y, 0.0);
        }
        l.end(&mut p);
        l.begin(primitive::LINES);
        l.vertex(0.0, 0.0, 0.0);
        l.vertex(5.0, 5.0, 0.0);
        l.end(&mut p);
        p.draw_line_batch(&[LineVertex::default(); 4], &[0, 1, 2, 2, 3, 1]);
        assert_eq!(device.backend.draws.len(), 3);
        assert_eq!(device.backend.draws[0].primitive_count, 2);
    }

    #[test]
    fn sphere_points_run_pole_to_pole() {
        let pts = sphere_points(2.0, 4, 3);
        assert_eq!(pts.len(), 12);
        assert_eq!(pts[0], [0.0, 2.0, 0.0]);
        assert!((pts[11][1] + 2.0).abs() < 1e-6);
        let mut l = Win3dLayer::new(64, 64);
        let mut p = RecordingPipeline::default();
        l.draw_sphere_grid(&mut p, &pts, 4, 3);
        // One inner ring (a strip and the closing line), four columns.
        assert_eq!(p.draw_count(), 6);
    }
}
