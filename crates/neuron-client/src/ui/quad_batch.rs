//! The screen-quad batcher of `screen_quad_state.cpp`: screen-space quads
//! queued in material lists and drawn as pre-transformed vertices.
//!
//! # How the reference batches
//!
//! A *material list* is a container in a map keyed by a 32-bit list key
//! (the font atlas, the HUD core bars, the effects, the NeuroNet buttons,
//! the IME: each a CRC or a key of its own). `ScreenQuadSetMaterialKey`
//! picks the list later calls go to. A list holds batch nodes in a live
//! list, each with one texture and blend mode and the records and
//! pre-rotated vertices queued on it, plus a *current* node and a 5-word
//! texture frame (texture, then a UV rect icons and flares read).
//! `ScreenQuadBeginBatchFrame` keeps the current node when its texture
//! matches the frame's, otherwise drops it or, when asked, finds the first
//! live node of that texture; with none it appends a new node.
//! `ScreenQuadFlush` sets the 2D render states, draws every live node of
//! one list in order (four vertices per record, then the rotated
//! vertices, as one indexed triangle list of FVF 0x1C4 from the scratch
//! index buffer), moves the nodes to the free list and restores the
//! states it saved. Lists keep their nodes until a flush names them.
//!
//! The port keeps the lists, the node order, the keep, drop and find rule,
//! and the flush's calls in the reference's order; nodes are values in a
//! vector instead of an intrusive list with a free list of recycled
//! blocks (the reuse is not observable). The device is behind
//! [`QuadDevice`]; [`FfQuadDevice`] is the fixed-function device model.

use std::collections::BTreeMap;

use crate::render::ff::buffers::IndexSource;
use crate::render::ff::device::{Backend, Device};
use crate::render::ff::fvf::Fvf;
use crate::render::texture::device::HandleTable;
use crate::text::font::quad::{QuadSink, SCREEN_QUAD_FVF, ScreenQuadRecord, TlVertex, clip_1d};

/// `D3DRS_CULLMODE`.
pub const RS_CULLMODE: u32 = 0x16;
/// `D3DRS_SRCBLEND`.
pub const RS_SRCBLEND: u32 = 0x13;
/// `D3DRS_DESTBLEND`.
pub const RS_DESTBLEND: u32 = 0x14;
/// `D3DRS_ALPHABLENDENABLE`.
pub const RS_ALPHABLENDENABLE: u32 = 0x1B;
/// `D3DRS_SPECULARENABLE`.
pub const RS_SPECULARENABLE: u32 = 0x1D;
/// `D3DRS_CLIPPING`.
pub const RS_CLIPPING: u32 = 0x88;
/// `D3DRS_LIGHTING`.
pub const RS_LIGHTING: u32 = 0x89;
/// `D3DRS_AMBIENT`.
pub const RS_AMBIENT: u32 = 0x8B;
/// `D3DRS_COLORVERTEX`.
pub const RS_COLORVERTEX: u32 = 0x8D;
/// `D3DRS_DIFFUSEMATERIALSOURCE`.
pub const RS_DIFFUSEMATERIALSOURCE: u32 = 0x91;
/// `D3DPT_TRIANGLELIST`.
pub const TRIANGLE_LIST: u32 = 4;

/// The 5-word texture frame (`unsigned long Frame[5]`): the texture, then
/// the UV rect as `f32`s.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Frame {
    pub texture: u32,
    pub u0: f32,
    pub v0: f32,
    pub u1: f32,
    pub v1: f32,
}

/// `G_ScreenQuadDefaultFrame`: no texture, the unit UV rect.
pub const DEFAULT_FRAME: Frame = Frame {
    texture: 0,
    u0: 0.0,
    v0: 0.0,
    u1: 1.0,
    v1: 1.0,
};

impl Frame {
    /// The frame the string paints pass: the texture and four zero words.
    #[must_use]
    pub fn texture_only(texture: u32) -> Frame {
        Frame {
            texture,
            u0: 0.0,
            v0: 0.0,
            u1: 0.0,
            v1: 0.0,
        }
    }
}

/// `FScreenQuadBatchNode`: one texture and blend mode with what is queued
/// on it.
#[derive(Clone, Debug, PartialEq)]
pub struct BatchNode {
    pub texture: u32,
    /// 0 opaque, 1 `ONE`/`ONE`, 2 `SRCALPHA`/`INVSRCALPHA`.
    pub blend_mode: i32,
    pub records: Vec<ScreenQuadRecord>,
    /// Vertices written already rotated (icons), four per quad.
    pub rotated: Vec<TlVertex>,
}

impl BatchNode {
    fn new(texture: u32, blend_mode: i32) -> BatchNode {
        BatchNode {
            texture,
            blend_mode,
            records: Vec::new(),
            rotated: Vec::new(),
        }
    }

    /// Rotated vertices plus four per record.
    #[must_use]
    pub fn vertex_count(&self) -> usize {
        self.rotated.len() + self.records.len() * 4
    }

    /// The vertices `FlushOneBatch` writes: four per record, then the
    /// rotated ones.
    #[must_use]
    pub fn vertices(&self) -> Vec<TlVertex> {
        let mut out = Vec::with_capacity(self.vertex_count());
        for record in &self.records {
            out.extend_from_slice(&record.vertices());
        }
        out.extend_from_slice(&self.rotated);
        out
    }
}

/// `FScreenQuadBatchContainer`: a material list.
#[derive(Clone, Debug, PartialEq)]
pub struct MaterialList {
    /// The live nodes in order.
    pub live: Vec<BatchNode>,
    /// The current node, an index into `live`.
    pub current: Option<usize>,
    /// The frame of the last `begin_batch_frame` (`MatKey`).
    pub frame: Frame,
}

impl MaterialList {
    fn new() -> MaterialList {
        MaterialList {
            live: Vec::new(),
            current: None,
            frame: DEFAULT_FRAME,
        }
    }

    /// `ContainerBeginBatch`: the frame becomes the list's; the current
    /// node is kept when its texture matches, otherwise dropped or (with
    /// `find`) replaced by the first live node of the texture; with none a
    /// new node goes to the live tail.
    fn begin(&mut self, frame: Frame, blend_mode: i32, find: bool) {
        self.frame = frame;
        let texture = frame.texture;
        if let Some(c) = self.current
            && self.live[c].texture != texture
        {
            self.current = if find {
                self.live.iter().position(|n| n.texture == texture)
            } else {
                None
            };
        }
        if self.current.is_none() {
            self.live.push(BatchNode::new(texture, blend_mode));
            self.current = Some(self.live.len() - 1);
        }
    }

    /// The current node.
    #[must_use]
    pub fn current_node(&self) -> Option<&BatchNode> {
        self.current.and_then(|c| self.live.get(c))
    }
}

/// What a flush asks of the renderer (`IRenderPipeline`, the vertex buffer
/// manager, the scratch index buffer and `IRPDraw`).
pub trait QuadDevice {
    /// `GetRenderState`; 0 when the device has no such state (the C++
    /// savers start from 0).
    fn render_state(&mut self, state: u32) -> u32;
    fn set_render_state(&mut self, state: u32, value: u32);
    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32);
    fn set_pipeline_state(&mut self, id: i32, value: i32);
    /// `SetTextureStageTexture(0, handle)`; 0 binds no texture.
    fn set_stage_texture(&mut self, texture: u32);
    /// `LockVertexMemory` with FVF 0x1C4, the vertices, the unlock,
    /// `LockIb` on the scratch buffer, the indices, `UnlockIb`, `SelectIb`
    /// with the lock's base vertex and `DrawIndexedPrimitive` (triangle
    /// list, minimum index 0, the vertex count, the locked start, the index
    /// count), stopping at the first call that fails.
    fn draw_indexed_quads(&mut self, vertices: &[TlVertex], indices: &[u16]);
}

/// The three saved-and-restored states of the flush and their values.
const FLUSH_SAVED: [(u32, u32); 3] = [
    (RS_CLIPPING, 0),
    (RS_AMBIENT, 0xFFFF_FFFF),
    (RS_CULLMODE, 1),
];

/// The stage states of the flush's tables, in order: stage 0 colour and
/// alpha, stage 1, then stage 0's texture-coordinate index and addressing.
const FLUSH_STAGE_STATES: [(u32, u32, u32); 16] = [
    (0, 0x0B, 0),
    (0, 1, 4),
    (0, 2, 2),
    (0, 3, 0),
    (0, 4, 4),
    (0, 5, 2),
    (0, 6, 0),
    (1, 1, 1),
    (1, 2, 2),
    (1, 3, 1),
    (1, 4, 1),
    (1, 5, 2),
    (1, 6, 1),
    (0, 0x18, 0),
    (0, 0x0D, 3),
    (0, 0x0E, 3),
];

/// The batcher's state: the list map, the material key and whether the
/// active list (`G_ActiveContainer`) is the current key's.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct QuadBatcher {
    lists: BTreeMap<u32, MaterialList>,
    material_key: u32,
    /// `G_MaterialListKeyDefault` (0, never written).
    default_key: u32,
    active: bool,
    /// The `ScreenQuadSet*` cache of the menu path.
    pub state_cache: StateCache,
}

impl QuadBatcher {
    #[must_use]
    pub fn new() -> QuadBatcher {
        QuadBatcher::default()
    }

    /// `G_ScreenQuadCurrentListKey`: a flush with it draws the current
    /// key's list.
    pub const CURRENT_LIST: u32 = 0xFFFF_FFFF;

    /// The material key calls go to.
    #[must_use]
    pub fn material_key(&self) -> u32 {
        self.material_key
    }

    /// A list by key.
    #[must_use]
    pub fn list(&self, key: u32) -> Option<&MaterialList> {
        self.lists.get(&key)
    }

    /// The keys of every list, in map order.
    pub fn keys(&self) -> impl Iterator<Item = u32> + '_ {
        self.lists.keys().copied()
    }

    /// `ScreenQuadSetMaterialKey`: a different key is stored and the active
    /// list forgotten; the old list keeps its nodes.
    pub fn set_material_key(&mut self, key: u32) {
        if self.material_key != key {
            self.material_key = key;
            self.active = false;
        }
    }

    /// `ScreenQuadResetDrawBatchCache`: every list destroyed, the default
    /// key back. The reference keeps its active pointer into the freed map
    /// when the key was already the default; here the active list is
    /// always forgotten (divergence D341).
    pub fn reset_draw_batch_cache(&mut self) {
        self.lists.clear();
        self.material_key = self.default_key;
        self.active = false;
    }

    /// `EnsureBatchContainer`: the current key's list, made with the
    /// default frame and an empty node of no texture when new.
    fn ensure_list(&mut self) -> &mut MaterialList {
        let key = self.material_key;
        if !self.active {
            self.active = true;
            self.lists.entry(key).or_insert_with(|| {
                let mut list = MaterialList::new();
                list.begin(DEFAULT_FRAME, 0, true);
                list
            });
        }
        self.lists.entry(key).or_insert_with(MaterialList::new)
    }

    /// `ScreenQuadBeginBatchFrame`.
    pub fn begin_frame(&mut self, frame: Frame, blend_mode: i32, find: bool) {
        self.ensure_list().begin(frame, blend_mode, find);
    }

    /// `ScreenQuadAppendRecord` into the current node. With no current node
    /// (after a flush of the current list without a new frame) the
    /// reference writes through a null node; the record is dropped here
    /// (divergence D341).
    pub fn append_record(&mut self, record: ScreenQuadRecord) {
        let list = self.ensure_list();
        if let Some(c) = list.current {
            list.live[c].records.push(record);
        }
    }

    /// Four already rotated vertices into the current node (the rotated
    /// path of `ScreenQuadEnqueueIcon`).
    pub fn append_rotated(&mut self, quad: [TlVertex; 4]) {
        let list = self.ensure_list();
        if let Some(c) = list.current {
            list.live[c].rotated.extend_from_slice(&quad);
        }
    }

    /// The UV rect of the current list's frame (`BatchU0` ... `BatchV1`).
    pub fn frame_uv(&mut self) -> [f32; 4] {
        let f = self.ensure_list().frame;
        [f.u0, f.v0, f.u1, f.v1]
    }

    /// `ScreenQuadEnqueueIcon`: a square of `size` world units at
    /// `position`, projected through `camera` onto the current list in the
    /// colour `argb` with the list frame's UV rect; a record clipped to the
    /// viewport, or with an angle above 1e-5 four rotated vertices kept
    /// only when every corner lies inside the viewport. Nothing when the
    /// point is behind the near plane or the square under a pixel wide.
    pub fn enqueue_icon(
        &mut self,
        camera: &IconCamera,
        position: [f32; 3],
        argb: u32,
        size: f32,
        angle: f32,
    ) {
        let Some(p) = project_object_extent(camera, position, size) else {
            return;
        };
        let rect = camera.viewport;
        let [u0, v0, u1, v1] = self.frame_uv();
        let depth_a = r32(1.0 - f64::from(camera.z_near) * f64::from(p.inv_depth));
        if f64::from(angle).abs() <= 1.0e-5 || angle.is_nan() {
            let (sx, sy, hw, hh) = (
                f64::from(p.screen_x),
                f64::from(p.screen_y),
                f64::from(p.half_w),
                f64::from(p.half_h),
            );
            let mut x0 = r32(sx - hw + 0.5);
            let mut x1 = r32(sx + hw + 0.5);
            let mut y0 = r32(sy - hh + 0.5);
            let mut y1 = r32(sy + hh + 0.5);
            let (mut cu0, mut cv0, mut cu1, mut cv1) = (u0, v0, u1, v1);
            #[allow(clippy::cast_precision_loss)]
            let (left, top, right, bottom) = (
                rect[0] as f32,
                rect[1] as f32,
                rect[2] as f32,
                rect[3] as f32,
            );
            if clip_1d(
                &mut x0,
                &mut x1,
                &mut cu0,
                &mut cu1,
                left,
                right,
                p.half_w + p.half_w,
            ) == 0
            {
                return;
            }
            if clip_1d(
                &mut y0,
                &mut y1,
                &mut cv0,
                &mut cv1,
                top,
                bottom,
                p.half_h + p.half_h,
            ) == 0
            {
                return;
            }
            self.append_record(ScreenQuadRecord {
                x0,
                y0,
                x1,
                y1,
                depth_a,
                depth_b: p.inv_depth,
                u0: cu0,
                v0: cv0,
                u1: cu1,
                v1: cv1,
                color: argb,
            });
            return;
        }
        let cos = f64::from(angle).cos();
        let sin = f64::from(angle).sin();
        let cos_hw = r32(cos * f64::from(p.half_w));
        let sin_hw = r32(f64::from(p.half_w) * sin);
        let cos_hh = r32(cos * f64::from(p.half_h));
        let sin_hh = r32(sin * f64::from(p.half_h));
        let (cos_hw, sin_hw, cos_hh, sin_hh) = (
            f64::from(cos_hw),
            f64::from(sin_hw),
            f64::from(cos_hh),
            f64::from(sin_hh),
        );
        let x = f64::from(p.screen_x);
        let y = f64::from(p.screen_y);
        let x_minus_cos = x - cos_hw;
        let y_minus_sin = y - sin_hw;
        let corner_x = [
            r32(x_minus_cos - sin_hh),
            r32(x_minus_cos + sin_hh),
            r32((sin_hh + cos_hw) + x),
            r32((cos_hw + x) - sin_hh),
        ];
        let corner_y = [
            r32((sin_hw + y) - cos_hh),
            r32((cos_hh + sin_hw) + y),
            r32(cos_hh + y_minus_sin),
            r32(y_minus_sin - cos_hh),
        ];
        #[allow(clippy::cast_precision_loss)]
        let left = rect[0] as f32;
        for (&cx, &cy) in corner_x.iter().zip(&corner_y) {
            if cx < left
                || f64::from(rect[2]) <= f64::from(cx)
                || f64::from(rect[1]) > f64::from(cy)
                || f64::from(rect[3]) <= f64::from(cy)
            {
                return;
            }
        }
        let us = [u0, u0, u1, u1];
        let vs = [v0, v1, v1, v0];
        let quad = std::array::from_fn(|i| TlVertex {
            x: corner_x[i],
            y: corner_y[i],
            z: depth_a,
            rhw: p.inv_depth,
            color: argb,
            specular: 0,
            u: us[i],
            v: vs[i],
        });
        self.append_rotated(quad);
    }

    /// `ScreenQuadFlush`: the 2D states, every live node of one list drawn
    /// and freed, the material key back to the default, the saved states
    /// restored.
    pub fn flush(&mut self, device: &mut dyn QuadDevice, key: u32) {
        let mut saved = [0u32; 3];
        for (slot, &(state, value)) in saved.iter_mut().zip(&FLUSH_SAVED) {
            *slot = device.render_state(state);
            device.set_render_state(state, value);
        }
        device.set_render_state(RS_LIGHTING, 0);
        device.set_render_state(RS_SPECULARENABLE, 0);
        device.set_render_state(RS_ALPHABLENDENABLE, 0);
        device.set_render_state(RS_COLORVERTEX, 1);
        device.set_render_state(RS_DIFFUSEMATERIALSOURCE, 1);
        for &(stage, state, value) in &FLUSH_STAGE_STATES {
            device.set_stage_state(stage, state, value);
        }

        if key == Self::CURRENT_LIST {
            self.ensure_list();
            let current = self.material_key;
            if let Some(list) = self.lists.get_mut(&current) {
                flush_list(list, device);
            }
        } else if let Some(list) = self.lists.get_mut(&key) {
            flush_list(list, device);
        }

        device.set_render_state(RS_COLORVERTEX, 0);
        device.set_stage_state(0, 0x0D, 1);
        device.set_stage_state(0, 0x0E, 1);
        if self.material_key != self.default_key {
            self.material_key = self.default_key;
            self.active = false;
        }
        self.state_cache.invalidate();
        for (&(state, _), &value) in FLUSH_SAVED.iter().zip(&saved).rev() {
            device.set_render_state(state, value);
        }
    }

    /// `ScreenQuadDrawBigImageTile`: one tile of a big image (a rich-text
    /// window's surface) as a screen quad of the "image" list, clipped to
    /// the surface and flushed at once under a pushed 2D projection, with
    /// the reference's saved and forced states. `dest` is left, bottom,
    /// right and top; `uv` the four corners' `u, v` (top left, top right,
    /// bottom left, bottom right).
    pub fn draw_big_image_tile(
        &mut self,
        layer: &mut super::win3d_immediate::Win3dLayer,
        pipe: &mut dyn super::win3d_immediate::Win3dPipeline,
        handle: u32,
        color: u32,
        dest: [f32; 4],
        uv: [f32; 8],
    ) {
        layer.push_projection(pipe);
        layer.set_ortho_2d(pipe);
        let mut saved = Vec::with_capacity(5);
        for (state, value) in [
            (RS_CLIPPING, 0),
            (RS_AMBIENT, 0xFFFF_FFFF),
            (RS_CULLMODE, 1),
        ] {
            saved.push((state, pipe.render_state(state)));
            pipe.set_render_state(state, value);
        }
        pipe.set_render_state(RS_LIGHTING, 0);
        pipe.set_render_state(RS_SPECULARENABLE, 0);
        pipe.set_render_state(RS_ALPHABLENDENABLE, 0);
        pipe.set_render_state(RS_COLORVERTEX, 1);
        pipe.set_render_state(RS_DIFFUSEMATERIALSOURCE, 1);
        for &(stage, state, value) in &FLUSH_STAGE_STATES {
            pipe.set_stage_state(stage, state, value);
        }
        let saved_10 = pipe.stage_state(0, 0x10);
        let saved_11 = pipe.stage_state(0, 0x11);
        // `G_BigImageTilePointFilter` is 1.
        pipe.set_stage_state(0, 0x10, 1);
        pipe.set_stage_state(0, 0x11, 1);
        for (state, value) in [(0x17, 8), (0x0E, 0)] {
            saved.push((state, pipe.render_state(state)));
            pipe.set_render_state(state, value);
        }

        self.set_material_key(neuron_core::hash::name_crc32(b"image"));
        let frame = Frame {
            texture: handle,
            u0: uv[0],
            v0: uv[7],
            u1: uv[6],
            v1: uv[1],
        };
        self.begin_frame(frame, 2, false);
        let [mut x0, mut y0, mut x1, mut y1] = dest;
        let (mut u0, mut v0, mut u1, mut v1) = (frame.u0, frame.v0, frame.u1, frame.v1);
        let (width, height) = layer.surface;
        let hi = |size: u32| size.wrapping_sub(1).cast_signed() as f32;
        let span_x = (f64::from(x1) - f64::from(x0)) as f32;
        if crate::text::font::quad::clip_1d(
            &mut x0,
            &mut x1,
            &mut u0,
            &mut u1,
            0.0,
            hi(width),
            span_x,
        ) != 0
        {
            let span_y = (f64::from(y1) - f64::from(y0)) as f32;
            if crate::text::font::quad::clip_1d(
                &mut y0,
                &mut y1,
                &mut v0,
                &mut v1,
                0.0,
                hi(height),
                span_y,
            ) != 0
            {
                self.append_record(ScreenQuadRecord {
                    x0,
                    y0,
                    x1,
                    y1,
                    depth_a: 1.0,
                    depth_b: 1.0,
                    u0,
                    v0,
                    u1,
                    v1,
                    color,
                });
                self.flush(pipe, Self::CURRENT_LIST);
            }
        }

        pipe.set_render_state(RS_COLORVERTEX, 0);
        pipe.set_stage_state(0, 0x0D, 1);
        pipe.set_stage_state(0, 0x0E, 1);
        pipe.set_stage_state(0, 0x10, saved_10);
        pipe.set_stage_state(0, 0x11, saved_11);
        layer.pop_projection(pipe);
        for (state, value) in saved.into_iter().rev() {
            pipe.set_render_state(state, value);
        }
    }

    /// `ApplyScreenQuadRenderStates`, the menu path's 2D preset through the
    /// state cache.
    pub fn apply_screen_quad_render_states(&mut self, device: &mut dyn QuadDevice) {
        let cache = &mut self.state_cache;
        for (state, value) in [
            (7, 0),
            (14, 0),
            (0x16, 1),
            (0x1B, 1),
            (0x13, 5),
            (0x14, 6),
            (0x89, 0),
            (0x1C, 0),
            (0x8D, 1),
            (0xA8, 0xF),
        ] {
            cache.set_render_state(device, state, value);
        }
        cache.set_pipeline_state(device, 0, 1);
        for (stage, ty, value) in [
            (0, 1, 4),
            (0, 2, 2),
            (0, 3, 0),
            (0, 4, 4),
            (0, 5, 2),
            (0, 6, 0),
            (0, 0xD, 3),
            (0, 0xE, 3),
            (0, 0x12, 0),
            (1, 1, 1),
        ] {
            cache.set_stage_state(device, stage, ty, value);
        }
    }
}

/// `FlushContainer`: every live node with anything queued is drawn, then
/// all of them leave the live list; no current node.
fn flush_list(list: &mut MaterialList, device: &mut dyn QuadDevice) {
    for node in list.live.drain(..) {
        if node.vertex_count() != 0 {
            flush_node(&node, device);
        }
    }
    list.current = None;
}

/// `FlushOneBatch`.
fn flush_node(node: &BatchNode, device: &mut dyn QuadDevice) {
    device.set_stage_texture(node.texture);
    match node.blend_mode {
        1 => {
            device.set_render_state(RS_SRCBLEND, 2);
            device.set_render_state(RS_DESTBLEND, 2);
        }
        2 => {
            device.set_render_state(RS_SRCBLEND, 5);
            device.set_render_state(RS_DESTBLEND, 6);
        }
        _ => {}
    }
    device.set_render_state(RS_ALPHABLENDENABLE, u32::from(node.blend_mode != 0));
    let vertices = node.vertices();
    if !vertices.is_empty() {
        let quads = vertices.len() >> 2;
        let indices: Vec<u16> = (0..quads)
            .flat_map(|q| {
                let b = (q * 4) as u16;
                [
                    b,
                    b.wrapping_add(1),
                    b.wrapping_add(2),
                    b,
                    b.wrapping_add(2),
                    b.wrapping_add(3),
                ]
            })
            .collect();
        device.draw_indexed_quads(&vertices, &indices);
    }
    device.set_render_state(RS_ALPHABLENDENABLE, 0);
}

fn r32(v: f64) -> f32 {
    v as f32
}

/// What `ScreenQuadEnqueueIcon` and `ScreenQuadProjectObjectExtent` read
/// of their camera (`ICamera`'s slots 5, 8, 11, 12 and 16).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct IconCamera {
    /// `GetViewTransform`.
    pub view: [f32; 12],
    pub z_near: f32,
    /// `GetXScaleOverNear` and `GetYScaleOverNear`, unrounded.
    pub x_scale_over_near: f64,
    pub y_scale_over_near: f64,
    /// Left, top, right, bottom.
    pub viewport: [i32; 4],
}

impl IconCamera {
    /// The values of a camera now (its view transform rebuilt when dirty).
    pub fn of(camera: &mut super::camera::Camera) -> IconCamera {
        IconCamera {
            view: camera.view_transform().to_words(),
            z_near: camera.z_near,
            x_scale_over_near: camera.x_scale_over_near(),
            y_scale_over_near: camera.y_scale_over_near(),
            viewport: camera.viewport,
        }
    }
}

/// What `ScreenQuadProjectObjectExtent` writes.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ProjectedExtent {
    pub screen_x: f32,
    pub screen_y: f32,
    pub half_w: f32,
    pub half_h: f32,
    pub inv_depth: f32,
}

/// `ScreenQuadProjectObjectExtent`: the point in camera space through the
/// view transform, its reciprocal depth and the half extents of `size`
/// there; `None` behind the near plane or under a pixel wide.
#[must_use]
pub fn project_object_extent(
    camera: &IconCamera,
    position: [f32; 3],
    size: f32,
) -> Option<ProjectedExtent> {
    let view = neuron_core::math::Transform::from_words(camera.view);
    let cam =
        neuron_core::math::transform_xform(&view, neuron_core::math::Vector::from_words(position));
    if f64::from(cam.z) - f64::from(camera.z_near) >= 0.0 {
        return None;
    }
    let inv_depth = r32(-1.0 / f64::from(cam.z));
    let near = f64::from(camera.z_near);
    let scale_x = r32(near * f64::from(r32(camera.x_scale_over_near)));
    let scale_y = r32(near * f64::from(r32(camera.y_scale_over_near)));
    let (inv, s) = (f64::from(inv_depth), f64::from(size));
    let half_w = f64::from(scale_x) * inv * s * f64::from(0.5f32);
    let half_h = (f64::from(scale_y) * inv * s * f64::from(0.5f32)).abs();
    if half_w < 1.0 {
        return None;
    }
    let rect = camera.viewport;
    let half_span_y = f64::from(rect[3].wrapping_sub(rect[1])) * f64::from(0.5f32);
    let half_span_x = f64::from(rect[2].wrapping_sub(rect[0])) * f64::from(0.5f32);
    Some(ProjectedExtent {
        screen_x: r32(
            (half_span_x + f64::from(cam.x) * f64::from(scale_x) * inv) + f64::from(rect[0])
        ),
        screen_y: r32(
            (f64::from(cam.y) * f64::from(scale_y) * inv + f64::from(rect[1])) + half_span_y,
        ),
        half_w: r32(half_w),
        half_h: r32(half_h),
        inv_depth,
    })
}

impl QuadSink for QuadBatcher {
    fn set_material_key(&mut self, key: u32) {
        QuadBatcher::set_material_key(self, key);
    }

    fn begin_batch_frame(&mut self, texture: u32, blend_mode: i32, find: bool) {
        self.begin_frame(Frame::texture_only(texture), blend_mode, find);
    }

    fn append(&mut self, record: ScreenQuadRecord) {
        self.append_record(record);
    }
}

/// One slot of a `ScreenQuadSet*` cache table.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct CachedPair {
    key: i32,
    value: u32,
    valid: bool,
}

/// The menu path's state caches: 64 render states, 16 pipeline states,
/// 32 stage states (key `stage << 16 | type`). A value equal to the cached
/// one is not sent; a full table sends every new key.
#[derive(Clone, Debug, PartialEq)]
pub struct StateCache {
    render: [CachedPair; 64],
    pipeline: [CachedPair; 16],
    stage: [CachedPair; 32],
}

impl Default for StateCache {
    fn default() -> Self {
        StateCache {
            render: [CachedPair::default(); 64],
            pipeline: [CachedPair::default(); 16],
            stage: [CachedPair::default(); 32],
        }
    }
}

/// `CacheLookupOrStore`: whether the value changed.
fn lookup_or_store(table: &mut [CachedPair], key: i32, value: u32) -> bool {
    let mut free = None;
    for (i, slot) in table.iter_mut().enumerate() {
        if !slot.valid {
            free.get_or_insert(i);
            continue;
        }
        if slot.key != key {
            continue;
        }
        if slot.value == value {
            return false;
        }
        slot.value = value;
        return true;
    }
    if let Some(i) = free {
        table[i] = CachedPair {
            key,
            value,
            valid: true,
        };
    }
    true
}

impl StateCache {
    /// `ScreenQuadInvalidateStateCache`.
    pub fn invalidate(&mut self) {
        *self = StateCache::default();
    }

    /// `ScreenQuadSetRenderState`.
    pub fn set_render_state(&mut self, device: &mut dyn QuadDevice, state: u32, value: u32) {
        if lookup_or_store(&mut self.render, state.cast_signed(), value) {
            device.set_render_state(state, value);
        }
    }

    /// `ScreenQuadSetPipelineState`.
    pub fn set_pipeline_state(&mut self, device: &mut dyn QuadDevice, id: i32, value: i32) {
        if lookup_or_store(&mut self.pipeline, id, value.cast_unsigned()) {
            device.set_pipeline_state(id, value);
        }
    }

    /// `ScreenQuadSetTextureStageState`.
    pub fn set_stage_state(
        &mut self,
        device: &mut dyn QuadDevice,
        stage: u32,
        ty: u32,
        value: u32,
    ) {
        let key = ((stage << 16) | (ty & 0xFFFF)).cast_signed();
        if lookup_or_store(&mut self.stage, key, value) {
            device.set_stage_state(stage, ty, value);
        }
    }
}

/// A vertex of FVF 0x1C4 as its 32 bytes, little-endian.
#[must_use]
pub fn vertex_bytes(v: &TlVertex) -> [u8; 32] {
    let mut out = [0u8; 32];
    let words = [
        v.x.to_bits(),
        v.y.to_bits(),
        v.z.to_bits(),
        v.rhw.to_bits(),
        v.color,
        v.specular,
        v.u.to_bits(),
        v.v.to_bits(),
    ];
    for (chunk, word) in out.as_chunks_mut::<4>().0.iter_mut().zip(words) {
        chunk.copy_from_slice(&word.to_le_bytes());
    }
    out
}

/// [`QuadDevice`] on the fixed-function device model, with the renderer
/// interface's texture numbers resolved through a [`HandleTable`].
pub struct FfQuadDevice<'a, B: Backend> {
    pub device: &'a mut Device<B>,
    pub handles: &'a HandleTable,
}

impl<B: Backend> QuadDevice for FfQuadDevice<'_, B> {
    fn render_state(&mut self, state: u32) -> u32 {
        self.device.state.render_state_raw(state).unwrap_or(0)
    }

    fn set_render_state(&mut self, state: u32, value: u32) {
        let _ = self.device.state.set_render_state_raw(state, value);
    }

    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32) {
        let _ = self.device.state.set_stage_state_raw(stage, state, value);
    }

    fn set_pipeline_state(&mut self, id: i32, value: i32) {
        self.device.state.set_pipeline_raw(id, value);
    }

    fn set_stage_texture(&mut self, texture: u32) {
        let handle = if texture == 0 {
            None
        } else {
            self.handles.get(texture)
        };
        self.device.set_stage_texture(0, handle);
    }

    fn draw_indexed_quads(&mut self, vertices: &[TlVertex], indices: &[u16]) {
        let Ok(count) = u32::try_from(vertices.len()) else {
            return;
        };
        let Ok(index_count) = u32::try_from(indices.len()) else {
            return;
        };
        let fvf = Fvf(SCREEN_QUAD_FVF);
        let buffers = &mut self.device.buffers;
        let Ok((listing, lock)) = buffers.lock_vertex_memory(fvf, count) else {
            return;
        };
        let bytes: Vec<u8> = vertices.iter().flat_map(vertex_bytes).collect();
        let _ = buffers.write_vb(listing.buffer, lock.offset, &bytes);
        let _ = buffers.unlock_vb(listing.buffer);
        let Ok(ib_lock) = buffers.lock_ib(IndexSource::Scratch, 0, index_count) else {
            return;
        };
        let _ = buffers.write_ib(IndexSource::Scratch, ib_lock.start, indices);
        let _ = buffers.unlock_ib(IndexSource::Scratch);
        if buffers
            .select_ib(Some(IndexSource::Scratch), listing.base_vertex)
            .is_err()
        {
            return;
        }
        let _ = self
            .device
            .draw_indexed(TRIANGLE_LIST, 0, count, ib_lock.start, index_count);
    }
}

/// One call a flush made.
#[derive(Clone, Debug, PartialEq)]
pub enum DeviceCall {
    GetRenderState(u32),
    RenderState(u32, u32),
    StageState(u32, u32, u32),
    PipelineState(i32, i32),
    Texture(u32),
    Draw {
        vertices: Vec<TlVertex>,
        indices: Vec<u16>,
    },
}

/// A [`QuadDevice`] that keeps the calls; `GetRenderState` answers from the
/// last value set (0 before any).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RecordingQuadDevice {
    pub calls: Vec<DeviceCall>,
    pub states: BTreeMap<u32, u32>,
}

impl QuadDevice for RecordingQuadDevice {
    fn render_state(&mut self, state: u32) -> u32 {
        self.calls.push(DeviceCall::GetRenderState(state));
        self.states.get(&state).copied().unwrap_or(0)
    }

    fn set_render_state(&mut self, state: u32, value: u32) {
        self.states.insert(state, value);
        self.calls.push(DeviceCall::RenderState(state, value));
    }

    fn set_stage_state(&mut self, stage: u32, state: u32, value: u32) {
        self.calls.push(DeviceCall::StageState(stage, state, value));
    }

    fn set_pipeline_state(&mut self, id: i32, value: i32) {
        self.calls.push(DeviceCall::PipelineState(id, value));
    }

    fn set_stage_texture(&mut self, texture: u32) {
        self.calls.push(DeviceCall::Texture(texture));
    }

    fn draw_indexed_quads(&mut self, vertices: &[TlVertex], indices: &[u16]) {
        self.calls.push(DeviceCall::Draw {
            vertices: vertices.to_vec(),
            indices: indices.to_vec(),
        });
    }
}

impl RecordingQuadDevice {
    /// The draws, in order.
    #[must_use]
    pub fn draws(&self) -> Vec<(&[TlVertex], &[u16])> {
        self.calls
            .iter()
            .filter_map(|c| match c {
                DeviceCall::Draw { vertices, indices } => {
                    Some((vertices.as_slice(), indices.as_slice()))
                }
                _ => None,
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::render::ff::defaults::Defaults;
    use crate::render::ff::device::RecordingBackend;

    fn record(x0: f32, color: u32) -> ScreenQuadRecord {
        ScreenQuadRecord {
            x0,
            y0: 2.0,
            x1: x0 + 10.0,
            y1: 12.0,
            depth_a: 0.5,
            depth_b: 1.0,
            u0: 0.0,
            v0: 0.0,
            u1: 1.0,
            v1: 1.0,
            color,
        }
    }

    #[test]
    fn a_new_list_starts_with_an_empty_node_of_no_texture() {
        let mut b = QuadBatcher::new();
        b.set_material_key(7);
        b.begin_frame(Frame::texture_only(3), 2, false);
        let list = b.list(7).unwrap();
        assert_eq!(list.live.len(), 2);
        assert_eq!((list.live[0].texture, list.live[0].blend_mode), (0, 0));
        assert_eq!((list.live[1].texture, list.live[1].blend_mode), (3, 2));
        assert_eq!(list.current, Some(1));
    }

    #[test]
    fn keep_drop_and_find() {
        let mut b = QuadBatcher::new();
        b.set_material_key(1);
        b.begin_frame(Frame::texture_only(5), 2, false);
        b.append_record(record(0.0, 1));
        b.begin_frame(Frame::texture_only(6), 2, false);
        b.append_record(record(1.0, 2));
        // Drop: a new node of texture 5 although one is live.
        b.begin_frame(Frame::texture_only(5), 2, false);
        b.append_record(record(2.0, 3));
        assert_eq!(b.list(1).unwrap().live.len(), 4);
        // Find: the first live node of texture 6.
        b.begin_frame(Frame::texture_only(6), 1, true);
        b.append_record(record(3.0, 4));
        let list = b.list(1).unwrap();
        assert_eq!(list.live.len(), 4);
        assert_eq!(list.current, Some(2));
        assert_eq!(list.live[2].records.len(), 2);
        assert_eq!(list.live[2].blend_mode, 2);
        // Keep: same texture, blend mode not changed.
        b.begin_frame(Frame::texture_only(6), 0, false);
        assert_eq!(b.list(1).unwrap().current, Some(2));
    }

    #[test]
    fn a_key_change_leaves_the_old_list_queued() {
        let mut b = QuadBatcher::new();
        b.set_material_key(1);
        b.begin_frame(Frame::texture_only(5), 2, false);
        b.append_record(record(0.0, 1));
        b.set_material_key(2);
        b.begin_frame(Frame::texture_only(5), 2, false);
        b.set_material_key(1);
        b.append_record(record(1.0, 1));
        assert_eq!(b.list(1).unwrap().live[1].records.len(), 2);
        assert_eq!(b.keys().collect::<Vec<_>>(), vec![1, 2]);
    }

    #[test]
    fn flush_draws_one_list_in_order_and_restores_the_states() {
        let mut b = QuadBatcher::new();
        b.set_material_key(9);
        b.begin_frame(Frame::texture_only(5), 2, false);
        b.append_record(record(0.0, 0xFF00_0000));
        b.append_record(record(20.0, 0xFF00_0001));
        b.begin_frame(Frame::texture_only(6), 1, false);
        b.append_record(record(40.0, 0xFF00_0002));
        let mut d = RecordingQuadDevice::default();
        d.states.insert(RS_CULLMODE, 3);
        b.flush(&mut d, 9);
        let draws = d.draws();
        assert_eq!(draws.len(), 2);
        assert_eq!(draws[0].0.len(), 8);
        assert_eq!(draws[0].1, &[0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7]);
        assert_eq!(draws[0].0[4].x, 20.0);
        assert_eq!(draws[1].0[0].color, 0xFF00_0002);
        // The empty first node draws nothing and binds nothing.
        let textures: Vec<u32> = d
            .calls
            .iter()
            .filter_map(|c| match c {
                DeviceCall::Texture(t) => Some(*t),
                _ => None,
            })
            .collect();
        assert_eq!(textures, vec![5, 6]);
        assert!(d.calls.contains(&DeviceCall::RenderState(RS_SRCBLEND, 2)));
        assert_eq!(
            d.calls.last(),
            Some(&DeviceCall::RenderState(RS_CLIPPING, 0))
        );
        assert_eq!(d.states[&RS_CULLMODE], 3);
        assert!(b.list(9).unwrap().live.is_empty());
        assert_eq!(b.material_key(), 0);
        // A second flush draws nothing.
        let mut d = RecordingQuadDevice::default();
        b.flush(&mut d, 9);
        assert!(d.draws().is_empty());
    }

    #[test]
    fn the_current_list_flush_creates_it() {
        let mut b = QuadBatcher::new();
        b.set_material_key(4);
        let mut d = RecordingQuadDevice::default();
        b.flush(&mut d, QuadBatcher::CURRENT_LIST);
        assert!(b.list(4).is_some());
        assert!(d.draws().is_empty());
    }

    #[test]
    fn the_state_cache_skips_unchanged_values() {
        let mut b = QuadBatcher::new();
        let mut d = RecordingQuadDevice::default();
        b.apply_screen_quad_render_states(&mut d);
        let first = d.calls.len();
        assert_eq!(first, 21);
        b.apply_screen_quad_render_states(&mut d);
        assert_eq!(d.calls.len(), first);
        b.flush(&mut d, 1);
        let before = d.calls.len();
        b.apply_screen_quad_render_states(&mut d);
        assert_eq!(d.calls.len(), before + 21);
    }

    #[test]
    fn the_device_model_draws_the_quads() {
        let mut device = Device::new(Defaults::builtin(), 640, 480, RecordingBackend::default());
        let handles = HandleTable::new();
        let mut b = QuadBatcher::new();
        b.set_material_key(1);
        b.begin_frame(Frame::texture_only(0), 2, false);
        b.append_record(record(0.0, 0x8000_00FF));
        let mut quad_device = FfQuadDevice {
            device: &mut device,
            handles: &handles,
        };
        b.flush(&mut quad_device, 1);
        let draws = &device.backend.draws;
        assert_eq!(draws.len(), 1);
        assert_eq!(draws[0].fvf, Fvf(SCREEN_QUAD_FVF));
        assert_eq!(draws[0].primitive_count, 2);
        assert_eq!(draws[0].vertex_count, 4);
        assert_eq!(
            device.state.render_state_raw(RS_ALPHABLENDENABLE).unwrap(),
            0
        );
    }
}
