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
use crate::text::font::quad::{QuadSink, SCREEN_QUAD_FVF, ScreenQuadRecord, TlVertex};

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
