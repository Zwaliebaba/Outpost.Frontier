//! Drawing (`FParticleFx::BeginDraw`, `EndDraw` and `DrawNodes` in
//! `particlefx_update.cpp`, the draw contexts and batch lists of
//! `particlefx_common.cpp`, and each appearance's `DrawOrEnqueue`,
//! `BeginBatchedDraw`, `EndBatchedDraw`, `FillBatchedGeometry` and queued
//! counts in `particlefx_appearances.cpp`).
//!
//! A frame's draw is `begin_draw`, any number of `draw_nodes` (an
//! effect's `Slot9` draws its live root nodes), then `end_draw`. Each
//! billboard class fills its own draw context: a lock of the module's one
//! vertex buffer (FVF `0x142`, 24 bytes a vertex) and of the renderer's
//! dynamic index buffer, closed and drawn as one indexed triangle list
//! with the class's blend states, the frame's texture and a modelview.
//! With the batcher on (`ParticleFx.useMaterialBatcher`), the basic, rect,
//! perp and beam appearances only join their class's batch list during the
//! walk, and `end_draw` fills and draws each list as one draw. The vertex
//! fills are in [`super::fill`] and [`super::ribbon`].
//!
//! What the draws call outside the module goes through [`ParticleDraw`];
//! the camera is the draw walk's cookie (`G_DrawCookie`), a
//! [`Camera`].

use neuron_core::fx_data::NodeClass;

use super::ParticleFx;
use super::node::{Family, NodeId};
use super::pool::{DEFAULT_POOL_SIZE, PoolKind};
use super::props::PropsData;
use super::ramp::IDENTITY;
use super::services::{Camera, ParticleDraw};
use super::system::pool_of;

/// `kParticleFxVbFvf`: `D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1`.
pub const PARTICLE_FVF: u32 = 0x142;
/// The bytes of a vertex (`FParticleFxVertex`).
pub const VERTEX_BYTES: usize = 24;
/// `D3DPT_TRIANGLELIST`, every context's primitive.
const TRIANGLE_LIST: u32 = 4;
/// `GR_OK`.
pub const GR_OK: i32 = 0;
/// `GR_GENERIC`.
pub const GR_GENERIC: i32 = -1;

/// `D3DRS_FILLMODE`.
pub(crate) const RS_FILLMODE: u32 = 8;
/// `D3DRS_ZWRITEENABLE`.
const RS_ZWRITE: u32 = 14;
/// `D3DRS_SRCBLEND`.
pub(crate) const RS_SRCBLEND: u32 = 19;
/// `D3DRS_DESTBLEND`.
pub(crate) const RS_DESTBLEND: u32 = 20;
/// `D3DRS_CULLMODE`.
const RS_CULLMODE: u32 = 22;
/// `D3DRS_ALPHABLENDENABLE`.
const RS_ALPHABLEND: u32 = 27;
/// `D3DRS_LIGHTING`.
const RS_LIGHTING: u32 = 137;

/// The states `BeginDraw` saves and `EndDraw` restores, in that order.
const SAVED_STATES: [u32; 6] = [
    RS_CULLMODE,
    RS_LIGHTING,
    RS_ZWRITE,
    RS_ALPHABLEND,
    RS_SRCBLEND,
    RS_DESTBLEND,
];

/// The draw counters (`G_DrawCounters`, `CopyCounters`): 1 the contexts
/// drawn, 3 to 8 the fills of basic, rect, perp, oriented, beam and dust.
pub const COUNTER_DRAWS: usize = 1;

/// One class's draw context (`FAlDrawContext`): the counters, the
/// requested counts and the offsets of its two locks, and the vertices and
/// indices written into them.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DrawContext {
    /// The primitive type (`+0x00`).
    pub prim: u32,
    /// The vertices submitted (`+0x04`).
    pub vert_count: u16,
    /// The indices written (`+0x06`).
    pub index_count: u16,
    /// Between `Begin` and `End` (`+0x08`).
    pub in_begin: bool,
    /// The primitives written (`+0x0c`), which nothing reads.
    pub prim_count: u16,
    /// The vertices `Begin` asked for (`+0x18`).
    pub requested_verts: u32,
    /// The indices `Begin` asked for (`+0x1c`).
    pub requested_indices: u32,
    /// The vertex lock's offset (`+0x20`), the draw's base vertex.
    pub vb_offset: u32,
    /// The index lock's offset (`+0x24`), the draw's start index.
    pub ib_offset: u32,
    verts: Vec<u8>,
    indices: Vec<u16>,
    vb_locked: bool,
    ib_locked: bool,
}

/// The requested count as the reference's signed compares read it.
fn signed(value: u32) -> i64 {
    i64::from(value.cast_signed())
}

impl DrawContext {
    /// `AlSubmitVertex`: written while the counter is not past the
    /// requested count; the index of the vertex, or the requested count
    /// when it was not written. Words are `x, y, z, diffuse, u, v`.
    pub(crate) fn vertex(&mut self, words: [u32; 6]) -> u16 {
        let count = self.vert_count;
        if i64::from(count) > signed(self.requested_verts) {
            return self.requested_verts as u16;
        }
        if self.vb_locked {
            for w in words {
                self.verts.extend_from_slice(&w.to_le_bytes());
            }
        }
        self.vert_count = count.wrapping_add(1);
        count
    }

    /// `AlSubmitQuadIndices(i0, i1, i2, i3)`: `i0, i1, i2, i2, i1, i3`
    /// unless six more would pass the requested count.
    pub(crate) fn quad(&mut self, i0: u16, i1: u16, i2: u16, i3: u16) {
        if i64::from(self.index_count) + 5 > signed(self.requested_indices) {
            return;
        }
        if self.ib_locked {
            self.indices.extend_from_slice(&[i0, i1, i2, i2, i1, i3]);
        }
        self.index_count = self.index_count.wrapping_add(6);
        self.prim_count = self.prim_count.wrapping_add(2);
    }

    /// `AlSubmitTriangleIndices(i0, i1, i2)`.
    pub(crate) fn triangle(&mut self, i0: u16, i1: u16, i2: u16) {
        if i64::from(self.index_count) + 2 > signed(self.requested_indices) {
            return;
        }
        if self.ib_locked {
            self.indices.extend_from_slice(&[i0, i1, i2]);
        }
        self.index_count = self.index_count.wrapping_add(3);
        self.prim_count = self.prim_count.wrapping_add(1);
    }

    /// The list classes' quad (`StoreQuadIndices`): the corners reversed.
    pub(crate) fn list_quad(&mut self, i0: u16, i1: u16, i2: u16, i3: u16) {
        self.quad(i3, i2, i1, i0);
    }
}

/// A batch list (`FBatchList`): head, tail and count; the links are the
/// appearances' own.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct BatchList {
    head: Option<NodeId>,
    tail: Option<NodeId>,
    count: u32,
}

/// The module's draw state: `G_bInDraw`, `G_Defaults.bUseMaterialBatcher`,
/// the vertex buffer and its sizes, the counters, the six contexts and the
/// four batch lists, and the render states `BeginDraw` saved.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DrawState {
    /// Between `BeginDraw` and `EndDraw`.
    pub in_draw: bool,
    /// `ParticleFx.useMaterialBatcher`.
    pub use_batcher: bool,
    /// `G_VbHandle`, 0 for none.
    pub vertex_buffer: u32,
    /// `G_MaxVertices`: the vertex buffer's size.
    pub vb_capacity: u32,
    /// `G_DrawMaxVertices`: the most vertices a context may request.
    pub max_vertices: u32,
    /// `G_DrawMaxIndices`.
    pub max_indices: u32,
    /// `G_DrawCounters`.
    pub counters: [u32; 9],
    /// The contexts: basic, rect, perp, oriented, dust, beam.
    pub contexts: [DrawContext; 6],
    saved: [u32; 6],
    lists: [BatchList; 4],
    draw_vb: u32,
}

impl Default for DrawState {
    fn default() -> Self {
        Self::new(&[DEFAULT_POOL_SIZE; 8])
    }
}

impl DrawState {
    /// The state `InitializeParticleFxModule` leaves for the pool sizes
    /// (in [`PoolKind::ALL`] order): the buffer sizes from
    /// `ComputeDrawBufferHints` (four vertices and six indices a billboard,
    /// five and 24 a beam segment, products wrapped and compared signed),
    /// the batcher off.
    #[must_use]
    pub fn new(pool_sizes: &[u32; 8]) -> Self {
        let size = |kind: PoolKind| pool_sizes[kind as usize];
        let max = |a: u32, b: u32| {
            if a.cast_signed() > b.cast_signed() {
                a
            } else {
                b
            }
        };
        let mut vertices = size(PoolKind::Basic).wrapping_mul(4);
        let mut indices = size(PoolKind::Basic).wrapping_mul(6);
        for kind in [
            PoolKind::Rect,
            PoolKind::Perp,
            PoolKind::Oriented,
            PoolKind::Dust,
        ] {
            vertices = max(vertices, size(kind).wrapping_mul(4));
            indices = max(indices, size(kind).wrapping_mul(6));
        }
        vertices = max(vertices, size(PoolKind::Beam).wrapping_mul(5));
        indices = max(indices, size(PoolKind::Beam).wrapping_mul(0x18));
        Self {
            in_draw: false,
            use_batcher: false,
            vertex_buffer: 0,
            vb_capacity: vertices,
            max_vertices: vertices,
            max_indices: indices,
            counters: [0; 9],
            contexts: Default::default(),
            saved: [0; 6],
            lists: [BatchList::default(); 4],
            draw_vb: 0,
        }
    }
}

/// The context of a billboard class.
pub(crate) const fn context_of(class: NodeClass) -> Option<usize> {
    match class {
        NodeClass::BasicAppearance => Some(0),
        NodeClass::RectAppearance => Some(1),
        NodeClass::PerpAppearance => Some(2),
        NodeClass::OrientedAppearance => Some(3),
        NodeClass::DustAppearance => Some(4),
        NodeClass::BeamAppearance => Some(5),
        _ => None,
    }
}

/// The batch list of a class that has one.
const fn list_of(class: NodeClass) -> Option<usize> {
    match class {
        NodeClass::BasicAppearance => Some(0),
        NodeClass::RectAppearance => Some(1),
        NodeClass::PerpAppearance => Some(2),
        NodeClass::BeamAppearance => Some(3),
        _ => None,
    }
}

/// `ApplyParticleDrawRenderState`: the fixed states every draw runs under.
fn apply_draw_states(svc: &mut dyn ParticleDraw) {
    svc.set_render_state(RS_LIGHTING, 0);
    svc.set_render_state(RS_ZWRITE, 0);
    svc.set_render_state(RS_ALPHABLEND, 1);
    svc.set_render_state(RS_CULLMODE, 1);
    for (stage, state, value) in [
        (0, 1, 4),
        (0, 2, 2),
        (0, 3, 0),
        (0, 4, 4),
        (0, 5, 2),
        (0, 6, 0),
        (0, 0x0B, 0),
        (0, 0x0D, 3),
        (0, 0x0E, 3),
        (1, 1, 1),
        (1, 4, 1),
    ] {
        svc.set_stage_state(stage, state, value);
    }
    svc.set_stage_texture(1, 0);
}

impl ParticleFx {
    // --- The contexts --------------------------------------------------------------------

    /// `AlDrawContextBegin(context, prim, verts, indices)`: the requested
    /// counts are stored first; a count past the module's limit fails with
    /// the counters and locks as they were; otherwise the counters reset
    /// and both buffers are locked (both locks are always tried).
    pub(crate) fn context_begin(
        &mut self,
        index: usize,
        verts: u32,
        indices: u32,
        svc: &mut dyn ParticleDraw,
    ) -> bool {
        let state = &mut self.draw;
        let context = &mut state.contexts[index];
        context.requested_verts = verts;
        context.requested_indices = indices;
        if signed(state.max_vertices) < signed(verts) || signed(state.max_indices) < signed(indices)
        {
            // "Max Vertices exceeded" / "Max Indices exceeded"; the fills
            // after a failed batched begin write nowhere (D377).
            context.vb_locked = false;
            context.ib_locked = false;
            return false;
        }
        context.in_begin = true;
        state.draw_vb = state.vertex_buffer;
        context.prim = TRIANGLE_LIST;
        context.index_count = 0;
        context.vert_count = 0;
        context.vb_offset = 0;
        context.ib_offset = 0;
        context.verts.clear();
        context.indices.clear();
        let vb = svc.lock_vb(state.draw_vb, verts);
        let ib = svc.lock_ib(indices);
        context.vb_locked = vb.is_ok();
        context.ib_locked = ib.is_ok();
        if let Ok(offset) = vb {
            context.vb_offset = offset;
        }
        if let Ok(offset) = ib {
            context.ib_offset = offset;
        }
        vb.is_ok() && ib.is_ok()
    }

    /// `AlDrawContextEnd`: outside a begin, 0; otherwise both buffers are
    /// unlocked with what was written, and a context that wrote more
    /// vertices or indices than it asked for draws nothing. Returns the
    /// index count.
    pub(crate) fn context_end(&mut self, index: usize, svc: &mut dyn ParticleDraw) -> u16 {
        let draw_vb = self.draw.draw_vb;
        let context = &mut self.draw.contexts[index];
        if !context.in_begin {
            // "End() called before Begin()".
            return 0;
        }
        // A vertex past the lock (the counter may reach the requested
        // count plus one) lands outside it in the reference (D377).
        let room = (context.requested_verts as usize).saturating_mul(VERTEX_BYTES);
        let verts = std::mem::take(&mut context.verts);
        svc.unlock_vb(draw_vb, &verts[..verts.len().min(room)]);
        let indices = std::mem::take(&mut context.indices);
        svc.unlock_ib(&indices);
        context.vb_locked = false;
        context.ib_locked = false;
        if signed(context.requested_verts) < i64::from(context.vert_count) {
            context.index_count = 0;
        }
        if signed(context.requested_indices) < i64::from(context.index_count) {
            context.index_count = 0;
        }
        context.in_begin = false;
        context.index_count
    }

    /// `AlDrawContextDraw`: the vertex buffer and the index buffer (with
    /// the vertex offset as base) selected, then one indexed draw.
    pub(crate) fn context_draw(&mut self, index: usize, svc: &mut dyn ParticleDraw) -> i32 {
        let draw_vb = self.draw.draw_vb;
        let context = &self.draw.contexts[index];
        if context.in_begin {
            // "End() must be called before Draw()".
            return GR_GENERIC;
        }
        if svc.select_vb(draw_vb) < 0 {
            return GR_OK;
        }
        if svc.select_ib(context.vb_offset) < 0 {
            return GR_OK;
        }
        svc.draw_indexed(
            context.prim,
            0,
            u32::from(context.vert_count),
            context.ib_offset,
            u32::from(context.index_count),
        );
        GR_OK
    }

    // --- Begin, end and the walk ---------------------------------------------------------

    /// `FParticleFx::BeginDraw`: the vertex buffer is (re)created when it
    /// cannot be selected, the batch lists are cleared when the batcher is
    /// on, six render states are saved and the particle states applied,
    /// and the counters reset.
    pub fn begin_draw(&mut self, svc: &mut dyn ParticleDraw) -> i32 {
        if self.draw.in_draw {
            // "BeginDraw called without EndDraw".
            return GR_GENERIC;
        }
        self.draw.in_draw = true;
        if svc.select_vb(self.draw.vertex_buffer) < 0 {
            if self.draw.vertex_buffer != 0 {
                svc.destroy_vb(self.draw.vertex_buffer);
                self.draw.vertex_buffer = 0;
            }
            let (result, handle) = svc.create_vb(PARTICLE_FVF, self.draw.vb_capacity, 1);
            self.draw.vertex_buffer = handle;
            if result < 0 {
                // "Unable to create vertex buffer"; the flag stays set.
                svc.destroy_vb(self.draw.vertex_buffer);
                self.draw.vertex_buffer = 0;
                return GR_GENERIC;
            }
        }
        if self.draw.use_batcher {
            self.draw.lists = [BatchList::default(); 4];
        }
        for (slot, state) in SAVED_STATES.iter().enumerate() {
            self.draw.saved[slot] = svc.render_state(*state);
        }
        apply_draw_states(svc);
        self.draw.counters = [0; 9];
        GR_OK
    }

    /// `FParticleFx::EndDraw`: with the batcher on, every batch list is
    /// filled and drawn (with `camera`, the cookie of the last walk); the
    /// saved render states are restored.
    pub fn end_draw(&mut self, camera: &mut dyn Camera, svc: &mut dyn ParticleDraw) -> i32 {
        if !self.draw.in_draw {
            // "EndDraw called without BeginDraw".
            return GR_GENERIC;
        }
        self.draw.in_draw = false;
        if self.draw.use_batcher {
            self.flush_batches(camera, svc);
        }
        for (slot, state) in SAVED_STATES.iter().enumerate() {
            svc.set_render_state(*state, self.draw.saved[slot]);
        }
        GR_OK
    }

    /// `FParticleFx::DrawNodes(camera, start)`: every node of the subtree at
    /// `start` in pre-order draws or joins its batch list.
    pub fn draw_nodes(
        &mut self,
        start: NodeId,
        camera: &mut dyn Camera,
        svc: &mut dyn ParticleDraw,
    ) -> i32 {
        if !self.draw.in_draw {
            // "DrawNodes called without BeginDraw".
            return GR_GENERIC;
        }
        let mut cursor = Some(start);
        while let Some(id) = cursor {
            if self.node(id).is_none() {
                break;
            }
            self.draw_or_enqueue(id, camera, svc);
            let node = self.n(id);
            if let Some(child) = node.first_child {
                cursor = Some(child);
            } else if id == start {
                cursor = None;
            } else if let Some(next) = node.next {
                cursor = Some(next);
            } else {
                cursor = None;
                let mut parent = node.parent;
                while let Some(p) = parent.filter(|&p| p != start) {
                    cursor = self.n(p).next;
                    parent = self.n(p).parent;
                    if cursor.is_some() {
                        break;
                    }
                }
            }
        }
        GR_OK
    }

    // --- The batch lists -----------------------------------------------------------------

    /// `EnqueueAppearanceToList`: appended through the appearance's own
    /// links, which an appearance already in a list leaves where they are
    /// (as the reference does, the list then skips what followed it).
    fn enqueue(&mut self, list: usize, id: NodeId) {
        let old_tail = self.draw.lists[list].tail;
        self.draw.lists[list].tail = Some(id);
        match old_tail {
            None => self.draw.lists[list].head = Some(id),
            Some(t) => {
                if let Some(a) = self.node_mut(t).and_then(|n| n.appearance_mut()) {
                    a.batch_next = Some(id);
                }
            }
        }
        let app = self.app_mut(id);
        app.batch_next = None;
        app.batch_prev = old_tail;
        self.draw.lists[list].count += 1;
    }

    /// The members of a list from its head (a released member ends it,
    /// D378).
    fn members(&self, head: NodeId) -> Vec<NodeId> {
        let mut out = Vec::new();
        let mut cursor = Some(head);
        while let Some(id) = cursor {
            let Some(app) = self.node(id).and_then(|n| n.appearance()) else {
                break;
            };
            out.push(id);
            if out.len()
                > self
                    .draw
                    .lists
                    .iter()
                    .map(|l| l.count as usize)
                    .sum::<usize>()
            {
                break;
            }
            cursor = app.batch_next;
        }
        out
    }

    /// `FlushAppearanceBatches`: per list, the members' queued counts
    /// summed, the head's batched begin (its result unused), every
    /// member's fill, the head's batched end and draw.
    fn flush_batches(&mut self, camera: &mut dyn Camera, svc: &mut dyn ParticleDraw) {
        for list in 0..self.draw.lists.len() {
            let Some(head) = self.draw.lists[list].head else {
                continue;
            };
            let members = self.members(head);
            let (mut verts, mut indices) = (0_u32, 0_u32);
            for &member in &members {
                let (v, i) = self.queued_counts(member);
                verts = verts.wrapping_add(v);
                indices = indices.wrapping_add(i);
            }
            let Some(index) = context_of(self.n(head).class()) else {
                continue;
            };
            self.context_begin(index, verts, indices, svc);
            for &member in &members {
                self.fill_batched(member, camera, svc);
            }
            self.end_batched(head, camera, svc);
        }
    }

    // --- Per class -----------------------------------------------------------------------

    /// `GetQueuedVertCount` and `GetQueuedIndexCount`: four vertices and
    /// six indices a particle, a beam five and 24 (five and 24 more with
    /// its cap, flag `0x100`); the products wrap.
    pub(crate) fn queued_counts(&self, id: NodeId) -> (u32, u32) {
        let node = self.n(id);
        let live = node.appearance().map_or(0, |a| a.live);
        match node.class() {
            NodeClass::BeamAppearance => {
                let cap = billboard_flags(node) & 0x100 == 0x100;
                let verts = live.wrapping_mul(5);
                let indices = live.wrapping_mul(0x18);
                if cap {
                    (verts.wrapping_add(5), indices.wrapping_add(0x18))
                } else {
                    (verts, indices)
                }
            }
            class if context_of(class).is_some() => (live.wrapping_mul(4), live.wrapping_mul(6)),
            _ => (0, 0),
        }
    }

    /// The node's `DrawOrEnqueue` (IGenericFx `+0x18`).
    fn draw_or_enqueue(&mut self, id: NodeId, camera: &mut dyn Camera, svc: &mut dyn ParticleDraw) {
        let node = self.n(id);
        if node.family() != Family::Appearance {
            return;
        }
        let class = node.class();
        match class {
            NodeClass::OrientedAppearance => {
                self.draw_oriented(id, camera, svc);
                return;
            }
            NodeClass::DustAppearance => {
                self.draw_dust(id, camera, svc);
                return;
            }
            // Mesh appearances draw their meshes (none in this increment);
            // particle appearances' child effects draw as effects.
            NodeClass::MeshAppearance | NodeClass::ParticleAppearance => return,
            _ => {}
        }
        let (Some(index), Some(list)) = (context_of(class), list_of(class)) else {
            return;
        };
        if self.app(id).head.is_none() {
            return;
        }
        if self.draw.use_batcher {
            self.enqueue(list, id);
            return;
        }
        let (verts, indices) = self.queued_counts(id);
        if !self.context_begin(index, verts, indices, svc) {
            return;
        }
        self.fill_batched(id, camera, svc);
        self.end_batched(id, camera, svc);
    }

    /// `FillBatchedGeometry` (outer `+0xA4`): the class's fill.
    fn fill_batched(&mut self, id: NodeId, camera: &mut dyn Camera, svc: &mut dyn ParticleDraw) {
        match self.n(id).class() {
            NodeClass::BasicAppearance => {
                self.draw.counters[3] = self.draw.counters[3].wrapping_add(1);
                if billboard_flags(self.n(id)) & 2 != 0 {
                    self.fill_basic_quads(id, camera, svc);
                } else {
                    self.fill_basic_triangles(id, camera, svc);
                }
            }
            NodeClass::RectAppearance => self.fill_rect(id, camera, svc),
            NodeClass::PerpAppearance => self.fill_perp(id, camera, svc),
            NodeClass::BeamAppearance => self.fill_ribbon(id, camera, svc),
            _ => {}
        }
    }

    /// The blend states of a set, the frame's texture.
    pub(crate) fn blend_and_texture(&self, id: NodeId, svc: &mut dyn ParticleDraw) {
        let node = self.n(id);
        let blend = match &node.props.data {
            PropsData::Billboard(b) => b.blend_info,
            _ => [0, 0],
        };
        svc.set_render_state(RS_SRCBLEND, blend[0]);
        svc.set_render_state(RS_DESTBLEND, blend[1]);
        let texture = node.appearance().map_or(0, |a| a.texture.frame.handle);
        svc.set_stage_texture(0, texture);
    }

    /// `EndBatchedDraw` (outer `+0xA0`): the context closed; one that wrote
    /// indices counts a draw, sets the blend states, the texture and the
    /// modelview (identity for basic, whose vertices are in camera space;
    /// the camera's view times the node for rect and perp, whose vertices
    /// are the node's), and draws. The beam's states are its own
    /// (`ApplyDrawState`).
    fn end_batched(&mut self, id: NodeId, camera: &mut dyn Camera, svc: &mut dyn ParticleDraw) {
        let class = self.n(id).class();
        let Some(index) = context_of(class) else {
            return;
        };
        if self.context_end(index, svc) == 0 {
            return;
        }
        self.draw.counters[COUNTER_DRAWS] = self.draw.counters[COUNTER_DRAWS].wrapping_add(1);
        if class == NodeClass::BeamAppearance {
            self.beam_draw_state(id, camera, svc);
        } else {
            self.blend_and_texture(id, svc);
            if class == NodeClass::BasicAppearance {
                svc.set_modelview(&IDENTITY);
            } else {
                let view = camera.view_transform();
                svc.set_modelview(&super::ramp::mul(&view, &self.n(id).world));
            }
        }
        self.context_draw(index, svc);
    }

    /// Use the material batcher (`SetUseMaterialBatcher`).
    pub fn set_use_material_batcher(&mut self, on: bool) {
        self.draw.use_batcher = on;
    }

    /// `CopyCounters`.
    #[must_use]
    pub const fn draw_counters(&self) -> [u32; 9] {
        self.draw.counters
    }
}

/// The billboard set's flags (0 for other sets).
pub(crate) fn billboard_flags(node: &super::node::Node) -> u32 {
    match &node.props.data {
        PropsData::Billboard(b) => b.flags,
        _ => 0,
    }
}

/// The pool of a node that must be an appearance.
pub(crate) fn pool_kind(class: NodeClass) -> PoolKind {
    pool_of(class).unwrap_or(PoolKind::Basic)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn buffer_hints_follow_the_pools() {
        let state = DrawState::default();
        assert_eq!((state.max_vertices, state.max_indices), (5000, 24000));
        let mut sizes = [10; 8];
        sizes[PoolKind::Rect as usize] = 3000;
        let state = DrawState::new(&sizes);
        assert_eq!((state.vb_capacity, state.max_indices), (12000, 18000));
    }

    #[test]
    fn a_context_counts_past_its_request_and_keeps_what_fits() {
        let mut context = DrawContext {
            requested_verts: 1,
            requested_indices: 6,
            vb_locked: true,
            ib_locked: true,
            ..DrawContext::default()
        };
        assert_eq!(context.vertex([0; 6]), 0);
        assert_eq!(context.vertex([1; 6]), 1);
        // Past the request: the requested count, nothing written.
        assert_eq!(context.vertex([2; 6]), 1);
        assert_eq!(context.vert_count, 2);
        assert_eq!(context.verts.len(), 2 * VERTEX_BYTES);
        context.list_quad(0, 1, 2, 3);
        assert_eq!(context.indices, [3, 2, 1, 1, 2, 0]);
        context.triangle(0, 1, 2);
        assert_eq!(context.index_count, 6);
    }

    #[test]
    fn the_sse_sine_and_cosine() {
        // The polynomial is good to about 2e-4; its cosine of 0 is
        // 0.9998432, its sine of 0 exactly 0.
        for angle in [0.0_f32, std::f32::consts::FRAC_PI_2, -1.0, 0.5, 3.0] {
            let [c, s] = super::super::fill::sin_cos(angle);
            assert!((c - angle.cos()).abs() < 2e-4, "cos {angle}: {c}");
            assert!((s - angle.sin()).abs() < 2e-4, "sin {angle}: {s}");
        }
        assert_eq!(super::super::fill::sin_cos(0.0)[1].to_bits(), 0);
    }
}
