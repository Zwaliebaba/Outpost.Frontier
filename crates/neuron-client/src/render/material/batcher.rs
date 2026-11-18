//! The material batcher (`FMaterialBatcher`, the `IMaterialBatcher2` slots
//! of `CppSource/Shading/MaterialBatcher.cpp`): two bins that either draw a
//! submission at once or queue a copy of it, and the flush that orders the
//! queued submissions by depth and draws them through their materials'
//! batched faces.
//!
//! The batcher owns a material library ([`MaterialBatcher::library`]),
//! whose `CreateAfterMap` wraps a material in a `BatchedMaterial` when one
//! of the batcher's rules (`[BatchedMaterials]`, kind 1 a type pattern,
//! kind 2 a name pattern) matches.
//!
//! A bin with a negative order key draws at once; bin 0 starts so. Another
//! bin queues each submission under its material with copies of its
//! matrices, the combined matrix `view * world`, and (unless the flags say
//! the caller keeps them alive: 4 for the vertices, 8 for the indices)
//! copies of its vertex streams and indices. A flush builds one dispatch
//! record per queued submission, material by material, keyed by the depth
//! of its vertices' centre in view space (or the depth the caller gave,
//! flag 0x20), sorts them with the C runtime's `qsort` (sort kinds 2 and 4
//! ascending, 3 and 5 descending), for kinds 4 and 5 also sorts each
//! submission's triangles, and draws: the device's buffers (flag 0x100),
//! the matrices, then `Bind` when the batched face changes (or the material
//! asks with render flag 0x20), `Submit`, and `Teardown` of the last one.
//! Sort kind 1 (bin 0's) and kinds 6 and 7 draw nothing.
//!
//! What the reference keeps in pointers is here owned or shared: vertex
//! sources and indices the caller keeps alive are [`Rc`] values the record
//! shares, copies are owned (the reference copies them into a scratch
//! arena of `BufferSize` bytes that it never bounds; divergence D304). The
//! queue is ordered by material handle where the reference orders it by
//! pointer (divergence D305).

use std::collections::BTreeMap;
use std::rc::Rc;

use neuron_core::diag::{self, DumpFlags, Severity};
use neuron_core::hash::eq_ignore_case;
use neuron_core::ini::{EngineIni, EngineSection};
use neuron_core::math::{Matrix, Transform, Vector, mul_transform};
use neuron_core::msvcrt::qsort;

use super::instance::{MaterialFactory, MaterialId};
use super::library::{LINE_BUFFER, MaterialLibrary, parse_batched_line};
use super::regex::MaterialRegex;
use super::{GR_GENERIC, GR_OK};
use crate::render::ff::defaults::profile_number;
use crate::render::texture::format_class::read_line_into;

const SOURCE: &str = "Shading\\Materials\\MaterialBatcher.cpp";

/// `[MaterialBatcher] BufferSize` when unset.
pub const DEFAULT_BUFFER_SIZE: u32 = 0x10_0000;

/// `[MaterialBatcher] Bin1SortOrder` when unset.
pub const DEFAULT_BIN1_SORT_ORDER: u32 = 2;

/// Submission flags the batcher reads.
pub mod flags {
    /// The caller keeps the vertex source alive until the flush.
    pub const SHARED_VERTICES: u32 = 4;
    /// The caller keeps the indices alive until the flush.
    pub const SHARED_INDICES: u32 = 8;
    /// The submission carries its own depth (`Unused`, a float's bits).
    pub const OWN_DEPTH: u32 = 0x20;
    /// The submission draws from device buffers ([`super::VbListing`]).
    pub const DEVICE_BUFFERS: u32 = 0x100;
}

/// A batching rule (`FBatchRule`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BatchRule {
    /// 1 matches the remapped type, 2 the lower-case name; any other kind
    /// never matches.
    pub kind: u32,
    /// The pattern as given.
    pub pattern: Vec<u8>,
    /// `None` when the pattern did not compile.
    pub expression: Option<MaterialRegex>,
}

/// A device vertex buffer and where the submission's vertices start in it
/// (`FVbListing`). Handles are the device's.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct VbListing {
    pub vertex_buffer: u32,
    pub fvf: u32,
    pub base_vertex: u32,
}

/// A device index buffer and the draw's bias (`FIbScratch`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct IbScratch {
    pub index_buffer: u32,
    pub bias: u32,
}

/// One vertex stream (`FRpVertexStream`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct VertexStream {
    /// The stream's bytes from element 0; `None` for a null base.
    pub data: Option<Vec<u8>>,
    pub byte_size: u32,
    pub stride: u32,
    /// One position per vertex of an indexed source; `None` for null.
    pub index: Option<Vec<u32>>,
    pub count: u32,
}

/// A vertex source (`FRpVertexSrcDesc`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct VertexSource {
    pub flags: u32,
    pub position: VertexStream,
    pub normal: VertexStream,
    pub diffuse: VertexStream,
    pub tex: [VertexStream; 2],
    pub fvf: u32,
    pub count: u32,
}

/// `CloneVertexStream`: `count * stride` bytes (wrapping at 32 bits) and
/// `index_count` indices, padded with zeros where the source is shorter
/// (the reference reads on past it).
fn clone_stream(source: &VertexStream, index_count: u32) -> VertexStream {
    let pad = |data: &[u8], len: usize| {
        let mut out = data[..data.len().min(len)].to_vec();
        out.resize(len, 0);
        out
    };
    VertexStream {
        data: source
            .data
            .as_ref()
            .map(|d| pad(d, source.count.wrapping_mul(source.stride) as usize)),
        byte_size: source.byte_size,
        stride: source.stride,
        index: source.index.as_ref().map(|i| {
            let mut out = i[..i.len().min(index_count as usize)].to_vec();
            out.resize(index_count as usize, 0);
            out
        }),
        count: source.count,
    }
}

impl VertexSource {
    /// `CloneVertexSource`: the header and the five streams, each index
    /// array cut to the source's vertex count.
    #[must_use]
    pub fn clone_for_queue(&self) -> VertexSource {
        VertexSource {
            flags: self.flags,
            position: clone_stream(&self.position, self.count),
            normal: clone_stream(&self.normal, self.count),
            diffuse: clone_stream(&self.diffuse, self.count),
            tex: [
                clone_stream(&self.tex[0], self.count),
                clone_stream(&self.tex[1], self.count),
            ],
            fvf: self.fvf,
            count: self.count,
        }
    }

    /// The `index`th float of the position bytes; 0 outside them.
    fn position_float(&self, byte: usize) -> f32 {
        self.position
            .data
            .as_deref()
            .and_then(|d| d.get(byte..byte + 4))
            .map_or(0.0, |b| f32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }

    fn position_at(&self, byte: usize) -> [f32; 3] {
        [
            self.position_float(byte),
            self.position_float(byte + 4),
            self.position_float(byte + 8),
        ]
    }
}

/// A submission as the caller hands it to [`MaterialBatcher::submit`]
/// (`FMaterialSubmitContext` and what it points to).
#[derive(Clone, Debug)]
pub struct SubmitInput {
    /// `Inst`, opaque to the batcher.
    pub instance: usize,
    /// The world matrix (12 floats, rotation rows then translation).
    pub world: [f32; 12],
    /// The view matrix.
    pub view: [f32; 12],
    pub vertices: Rc<VertexSource>,
    /// The index array from its start.
    pub indices: Rc<[u16]>,
    /// Read with flag 0x100.
    pub vb: VbListing,
    /// Read with flag 0x100.
    pub ib: IbScratch,
    /// `ReplacedMaterial`.
    pub replaced: Option<MaterialId>,
    /// `Field20`, opaque.
    pub field20: usize,
}

/// The context a material draws with (`FMaterialSubmitContext`).
#[derive(Clone, Copy, Debug)]
pub struct SubmitContext<'a> {
    pub instance: usize,
    pub world: &'a [f32; 12],
    pub view: &'a [f32; 12],
    /// `SourceVertices`; `None` for a null source.
    pub vertices: Option<&'a VertexSource>,
    /// The index array from its start; draws read from `ib_start` on.
    pub indices: &'a [u16],
    /// `None` without flag 0x100.
    pub vb: Option<&'a VbListing>,
    pub ib: Option<&'a IbScratch>,
    pub replaced: Option<MaterialId>,
    pub field20: usize,
}

/// The draw arguments of `Submit`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DrawArgs {
    pub prim_type: u32,
    pub start: u32,
    pub count: u32,
    /// The first index (`IbStart`).
    pub ib_start: u32,
    /// The number of indices (`IndexSpan`).
    pub index_span: u32,
    pub flags: u32,
}

/// The renderer calls of a flush (`IRenderPipeline`, `IRPVertexBuffer`,
/// `IRPIndexBuffer`).
pub trait BatchDevice {
    /// `IRPVertexBuffer::SelectVb`.
    fn select_vb(&mut self, vertex_buffer: u32) -> i32;
    /// `IRPIndexBuffer::SelectIb(handle, base vertex, 0, 0)`.
    fn select_ib(&mut self, index_buffer: u32, base_vertex: u32) -> i32;
    /// `GetPipelineState(id)`: the value written, `None` when the call
    /// writes nothing.
    fn pipeline_state(&mut self, id: u32) -> Option<i32>;
    /// `SetModelview`.
    fn set_modelview(&mut self, matrix: &[f32; 12]);
    /// `SetWorld`.
    fn set_world(&mut self, matrix: &[f32; 12]);
    /// `SetView`.
    fn set_view(&mut self, matrix: &[f32; 12]);
}

/// The flush callbacks (`IMaterialBatcherEvents`).
pub trait BatcherEvents {
    /// Before a bin's flush, also of an empty one.
    fn begin_flush(&mut self, clear: u8);
    /// After a bin's flush.
    fn end_flush(&mut self, clear: u8);
    /// Before a record whose tag differs from the last one's (or is 0).
    fn begin_record(&mut self, material: MaterialId, tag: usize, tag_context: usize);
    /// After every record.
    fn end_record(&mut self, material: MaterialId, tag: usize, tag_context: usize);
}

/// Where a queued record's indices live.
#[derive(Clone, Debug)]
enum Indices {
    /// The caller's array (flag 8).
    Shared(Rc<[u16]>),
    /// A copy or a sorted array.
    Owned(Vec<u16>),
}

impl Indices {
    fn as_slice(&self) -> &[u16] {
        match self {
            Indices::Shared(s) => s,
            Indices::Owned(v) => v,
        }
    }
}

/// A queued submission (`FBatchRecord`).
#[derive(Clone, Debug)]
struct QueuedRecord {
    /// Bit 0: the indices are the record's own copy.
    internal_flags: u32,
    instance: usize,
    field20: usize,
    replaced: Option<MaterialId>,
    world: [f32; 12],
    view: [f32; 12],
    combined: [f32; 12],
    prim_type: u32,
    start: u32,
    count: u32,
    ib_start: u32,
    index_span: u32,
    /// Copied with flag 0x100.
    buffers: Option<(VbListing, IbScratch)>,
    flags: u32,
    tag: usize,
    tag_context: usize,
    /// `Unused`: the depth's bits with flag 0x20.
    depth_bits: u32,
    vertices: Rc<VertexSource>,
    indices: Indices,
}

impl QueuedRecord {
    fn context(&self) -> SubmitContext<'_> {
        SubmitContext {
            instance: self.instance,
            world: &self.world,
            view: &self.view,
            vertices: Some(&self.vertices),
            indices: self.indices.as_slice(),
            vb: self.buffers.as_ref().map(|(vb, _)| vb),
            ib: self.buffers.as_ref().map(|(_, ib)| ib),
            replaced: self.replaced,
            field20: self.field20,
        }
    }

    fn args(&self) -> DrawArgs {
        DrawArgs {
            prim_type: self.prim_type,
            start: self.start,
            count: self.count,
            ib_start: self.ib_start,
            index_span: self.index_span,
            flags: self.flags,
        }
    }

    /// `ComputeBatchPositionDepth`: the view-space depth of a position, in
    /// the reference's x87 order.
    fn position_depth(&self, p: [f32; 3]) -> f64 {
        let c = &self.combined;
        let mut depth = f64::from(c[8]) * f64::from(p[2]);
        depth += f64::from(c[5]) * f64::from(p[1]);
        depth += f64::from(c[2]) * f64::from(p[0]);
        depth += f64::from(c[11]);
        depth
    }

    /// `ComputeBatchDepth`: the negated depth the caller gave (flag 0x20),
    /// else the mean depth of the position stream.
    fn depth(&self) -> f32 {
        if self.flags & flags::OWN_DEPTH != 0 {
            return -f32::from_bits(self.depth_bits);
        }
        let position = &self.vertices.position;
        let bytes = position.count.wrapping_mul(position.stride);
        let steps = bytes.checked_div(position.stride).unwrap_or(0);
        let mut sum = 0.0f64;
        for step in 0..steps {
            let at = step as usize * position.stride as usize;
            sum += self.position_depth(self.vertices.position_at(at));
        }
        (sum / f64::from(position.count)) as f32
    }
}

/// A bin (`FBatchBin`).
#[derive(Clone, Debug, Default)]
struct Bin {
    /// `Pad0`: the order key; negative draws at once.
    order_key: i32,
    sort_kind: u32,
    queue: BTreeMap<MaterialId, Vec<QueuedRecord>>,
    record_count: u32,
}

impl Bin {
    fn clear(&mut self) {
        self.queue.clear();
        self.record_count = 0;
    }
}

/// What a bin looks like from outside.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BinState {
    /// The order key (`SetBinOrder`).
    pub order_key: i32,
    /// The sort kind (`SetBinSortKind`).
    pub sort_kind: u32,
    /// Submissions queued since the last clear.
    pub record_count: u32,
    /// The queue's materials and their submission counts, in queue order.
    pub queued: Vec<(MaterialId, usize)>,
}

/// One dispatch record (`FBatchDispatch`).
#[derive(Clone, Copy, Debug)]
struct Dispatch {
    material_flags: u32,
    key: f32,
    material: MaterialId,
    record: usize,
}

/// `CompareBatchKeys`: -1 when `a` sorts first, 0 when equal, else 1 (also
/// for NaN).
fn compare_keys(a: f32, b: f32, ascending: bool) -> i32 {
    if if ascending { a < b } else { a > b } {
        return -1;
    }
    if a == b { 0 } else { 1 }
}

fn transform(m: &[f32; 12]) -> Transform {
    Transform {
        rotation: Matrix {
            m: [[m[0], m[1], m[2]], [m[3], m[4], m[5]], [m[6], m[7], m[8]]],
        },
        translation: Vector {
            x: m[9],
            y: m[10],
            z: m[11],
        },
    }
}

fn words(t: &Transform) -> [f32; 12] {
    let r = &t.rotation.m;
    [
        r[0][0],
        r[0][1],
        r[0][2],
        r[1][0],
        r[1][1],
        r[1][2],
        r[2][0],
        r[2][1],
        r[2][2],
        t.translation.x,
        t.translation.y,
        t.translation.z,
    ]
}

/// What the batcher reads from the engine INI (`StartupFromOuter`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BatcherConfig {
    /// `[MaterialBatcher] BufferSize`, the scratch arena's size.
    pub buffer_size: u32,
    /// `[MaterialBatcher] Bin1SortOrder`, bin 1's sort kind.
    pub bin1_sort_order: u32,
}

impl Default for BatcherConfig {
    fn default() -> Self {
        BatcherConfig {
            buffer_size: DEFAULT_BUFFER_SIZE,
            bin1_sort_order: DEFAULT_BIN1_SORT_ORDER,
        }
    }
}

impl BatcherConfig {
    /// `ReadBatcherProfileUnsigned` for the two keys.
    #[must_use]
    pub fn from_engine_ini(ini: &EngineIni) -> BatcherConfig {
        let mut config = BatcherConfig::default();
        if let Some(section) = ini.section(b"MaterialBatcher") {
            if let Some(value) = section.value(b"BufferSize") {
                config.buffer_size = profile_number(value);
            }
            if let Some(value) = section.value(b"Bin1SortOrder") {
                config.bin1_sort_order = profile_number(value);
            }
        }
        config
    }
}

/// The material batcher.
pub struct MaterialBatcher<S: ?Sized> {
    library: MaterialLibrary<S>,
    config: BatcherConfig,
    bins: [Bin; 2],
    /// `BinOrder`: bin indices in flush order, `u32::MAX` for none.
    bin_order: [u32; 2],
    selected: usize,
    events: Option<Box<dyn BatcherEvents>>,
    record_tag: usize,
    record_tag_context: usize,
}

impl<S: ?Sized> std::fmt::Debug for MaterialBatcher<S> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("MaterialBatcher")
            .field("library", &self.library)
            .field("config", &self.config)
            .field("bin_order", &self.bin_order)
            .field("selected", &self.selected)
            .finish_non_exhaustive()
    }
}

impl<S: ?Sized> MaterialBatcher<S> {
    /// The constructor and `StartupFromOuter`: the library with the
    /// `[MaterialMap]` rules, the `[BatchedMaterials]` rules, bin 0 drawing
    /// at once with sort kind 1, bin 1 with order key 1 and
    /// `Bin1SortOrder`; bin 0 selected.
    #[must_use]
    pub fn new(
        factory: Box<dyn MaterialFactory<S>>,
        ini: Option<&EngineIni>,
    ) -> MaterialBatcher<S> {
        let config = ini.map(BatcherConfig::from_engine_ini).unwrap_or_default();
        let mut library = MaterialLibrary::new(factory);
        library.batch_rules = Some(Vec::new());
        let mut batcher = MaterialBatcher {
            library,
            bins: [
                Bin {
                    order_key: -1,
                    sort_kind: 1,
                    ..Bin::default()
                },
                Bin {
                    order_key: 1,
                    sort_kind: config.bin1_sort_order,
                    ..Bin::default()
                },
            ],
            config,
            bin_order: [0, 1],
            selected: 0,
            events: None,
            record_tag: 0,
            record_tag_context: 0,
        };
        if let Some(ini) = ini {
            if let Some(section) = ini.section(b"MaterialMap") {
                batcher.library.parse_material_map(section);
            }
            if let Some(section) = ini.section(b"BatchedMaterials") {
                batcher.parse_batched_materials(section);
            }
        }
        batcher
    }

    /// The batcher's library (`QueryInterface(IID_IMaterialLibrary)`).
    #[must_use]
    pub fn library(&self) -> &MaterialLibrary<S> {
        &self.library
    }

    /// The batcher's library, mutably.
    pub fn library_mut(&mut self) -> &mut MaterialLibrary<S> {
        &mut self.library
    }

    /// The INI settings.
    #[must_use]
    pub fn config(&self) -> &BatcherConfig {
        &self.config
    }

    /// The batching rules in order.
    #[must_use]
    pub fn rules(&self) -> &[BatchRule] {
        self.library.batch_rules.as_deref().unwrap_or_default()
    }

    fn rules_mut(&mut self) -> &mut Vec<BatchRule> {
        self.library.batch_rules.get_or_insert_with(Vec::new)
    }

    /// A bin's state.
    #[must_use]
    pub fn bin(&self, index: usize) -> Option<BinState> {
        self.bins.get(index).map(|bin| BinState {
            order_key: bin.order_key,
            sort_kind: bin.sort_kind,
            record_count: bin.record_count,
            queued: bin.queue.iter().map(|(&m, r)| (m, r.len())).collect(),
        })
    }

    /// The flush order (`BinOrder`).
    #[must_use]
    pub fn bin_order(&self) -> [u32; 2] {
        self.bin_order
    }

    /// The selected bin.
    #[must_use]
    pub fn selected_bin(&self) -> usize {
        self.selected
    }

    /// `ParseBatchedMaterials`: `type = pattern` lines add kind-1 rules,
    /// `name = pattern` lines kind-2 rules.
    pub fn parse_batched_materials(&mut self, section: &EngineSection) {
        let mut line = [0u8; LINE_BUFFER];
        let mut index = 0;
        while read_line_into(section, index, &mut line) {
            if let Some((key, value)) = parse_batched_line(&mut line) {
                if eq_ignore_case(&key, b"type") {
                    self.add_rule(1, &value);
                } else if eq_ignore_case(&key, b"name") {
                    self.add_rule(2, &value);
                }
            }
            index += 1;
        }
    }

    /// `ClearRules` (slot 3).
    pub fn clear_rules(&mut self) -> i32 {
        self.rules_mut().clear();
        GR_OK
    }

    /// `AddRule` (slot 4): a rule tried after the earlier ones.
    pub fn add_rule(&mut self, kind: u32, pattern: &[u8]) -> i32 {
        let pattern = neuron_core::hash::c_str(pattern);
        self.rules_mut().push(BatchRule {
            kind,
            pattern: pattern.to_vec(),
            expression: MaterialRegex::compile(pattern),
        });
        GR_OK
    }

    /// `RemoveRule` (slot 5): the first rule of this kind and exactly this
    /// pattern.
    pub fn remove_rule(&mut self, kind: u32, pattern: &[u8]) -> i32 {
        let pattern = neuron_core::hash::c_str(pattern);
        let rules = self.rules_mut();
        match rules
            .iter()
            .position(|r| r.kind == kind && r.pattern == pattern)
        {
            Some(index) => {
                rules.remove(index);
                GR_OK
            }
            None => GR_GENERIC,
        }
    }

    /// `SelectBin` (slot 6).
    pub fn select_bin(&mut self, index: u32) -> i32 {
        if index >= 2 {
            return GR_GENERIC;
        }
        self.selected = index as usize;
        GR_OK
    }

    /// `SetBinSortKind` (slot 7).
    pub fn set_bin_sort_kind(&mut self, index: u32, kind: u32) -> i32 {
        if index >= 2 {
            return GR_GENERIC;
        }
        self.bins[index as usize].sort_kind = kind;
        GR_OK
    }

    /// `SetBinOrder` (slot 8): sets a bin's order key and rebuilds the
    /// flush order from the bins whose key is not negative. Each bin is
    /// placed after the entries whose key is at most the key just set (the
    /// reference compares with that bin's key, not the placed one's).
    pub fn set_bin_order(&mut self, index: u32, key: i32) -> i32 {
        if index >= 2 {
            return GR_GENERIC;
        }
        let index = index as usize;
        self.bins[index].order_key = key;
        self.bin_order = [u32::MAX; 2];
        let mut count = 0usize;
        for candidate in 0..2usize {
            if self.bins[candidate].order_key < 0 {
                continue;
            }
            let mut position = 0;
            while position < count
                && self.bins[self.bin_order[position] as usize].order_key
                    <= self.bins[index].order_key
            {
                position += 1;
            }
            let mut slot = count;
            while slot > position {
                self.bin_order[slot] = self.bin_order[slot - 1];
                slot -= 1;
            }
            self.bin_order[position] = candidate as u32;
            count += 1;
        }
        GR_OK
    }

    /// `ClearBins` (slot 12).
    pub fn clear_bins(&mut self) -> i32 {
        for bin in &mut self.bins {
            bin.clear();
        }
        GR_OK
    }

    /// `NoOp` (slot 13).
    #[must_use]
    pub fn no_op(&self) -> i32 {
        GR_OK
    }

    /// `SetEvents` (slot 14).
    pub fn set_events(&mut self, events: Option<Box<dyn BatcherEvents>>) -> i32 {
        self.events = events;
        GR_OK
    }

    /// `GetEvents` (slot 15).
    pub fn events_mut(&mut self) -> Option<&mut (dyn BatcherEvents + 'static)> {
        self.events.as_deref_mut()
    }

    /// `SetRecordTag` (slot 16): the tag queued records carry.
    pub fn set_record_tag(&mut self, tag: usize) -> i32 {
        self.record_tag = tag;
        GR_OK
    }

    /// `GetRecordTag` (slot 17).
    #[must_use]
    pub fn record_tag(&self) -> usize {
        self.record_tag
    }

    /// `SetRecordTagContext` (slot 18).
    pub fn set_record_tag_context(&mut self, tag: usize) -> i32 {
        self.record_tag_context = tag;
        GR_OK
    }

    /// `GetRecordTagContext` (slot 19).
    #[must_use]
    pub fn record_tag_context(&self) -> usize {
        self.record_tag_context
    }
}

impl<S: BatchDevice + ?Sized> MaterialBatcher<S> {
    /// `Submit` (slot 11). In a bin whose order key is negative the
    /// material draws now: through its batched face (`Bind`, `Submit`,
    /// `Teardown`, whose result is returned) or its own `Submit`. Otherwise
    /// the submission is queued; `depth_bits` is the float the caller
    /// gives with flag 0x20.
    pub fn submit(
        &mut self,
        services: &mut S,
        material: MaterialId,
        input: &SubmitInput,
        args: &DrawArgs,
        depth_bits: u32,
    ) -> i32 {
        let bin = &mut self.bins[self.selected];
        if bin.order_key < 0 {
            let context = SubmitContext {
                instance: input.instance,
                world: &input.world,
                view: &input.view,
                vertices: Some(&input.vertices),
                indices: &input.indices,
                vb: Some(&input.vb),
                ib: Some(&input.ib),
                replaced: input.replaced,
                field20: input.field20,
            };
            let Some(object) = self.library.materials_mut().get_mut(material) else {
                return GR_GENERIC;
            };
            if object.is_batched() {
                object.batch_bind(services, &context);
                object.batch_submit(services, &context, args);
                return object.batch_teardown(services, &context);
            }
            return object.submit(services, &context, args);
        }
        bin.record_count = bin.record_count.wrapping_add(1);
        let combined = words(&mul_transform(
            &transform(&input.view),
            &transform(&input.world),
        ));
        let mut record = QueuedRecord {
            internal_flags: 0,
            instance: input.instance,
            field20: input.field20,
            replaced: input.replaced,
            world: input.world,
            view: input.view,
            combined,
            prim_type: args.prim_type,
            start: args.start,
            count: args.count,
            ib_start: args.ib_start,
            index_span: args.index_span,
            buffers: (args.flags & flags::DEVICE_BUFFERS != 0).then_some((input.vb, input.ib)),
            flags: args.flags,
            tag: self.record_tag,
            tag_context: self.record_tag_context,
            depth_bits,
            vertices: if args.flags & flags::SHARED_VERTICES != 0 {
                Rc::clone(&input.vertices)
            } else {
                Rc::new(input.vertices.clone_for_queue())
            },
            indices: Indices::Shared(Rc::clone(&input.indices)),
        };
        if args.flags & flags::SHARED_INDICES == 0 {
            // `CloneIndices(IndexSource + Count, Zero)`: the reference copies
            // `ib_start` indices from index `count` on.
            let from = args.count as usize;
            let copy: Vec<u16> = (0..args.ib_start as usize)
                .map(|i| input.indices.get(from + i).copied().unwrap_or(0))
                .collect();
            record.indices = Indices::Owned(copy);
            record.internal_flags |= 1;
            record.ib_start = 0;
        }
        bin.queue.entry(material).or_default().push(record);
        GR_OK
    }

    /// `IMaterial::Submit` on a material of the batcher's library: a
    /// batched material selects its bin and submits itself here
    /// ([`Material::batch_route`], `FBatchedMaterial::Submit`); any other
    /// material draws now. `GR_GENERIC` for a handle that names nothing.
    ///
    /// [`Material::batch_route`]: super::Material::batch_route
    pub fn submit_material(
        &mut self,
        services: &mut S,
        material: MaterialId,
        input: &SubmitInput,
        args: &DrawArgs,
    ) -> i32 {
        let context = SubmitContext {
            instance: input.instance,
            world: &input.world,
            view: &input.view,
            vertices: Some(&input.vertices),
            indices: &input.indices,
            vb: Some(&input.vb),
            ib: Some(&input.ib),
            replaced: input.replaced,
            field20: input.field20,
        };
        let Some(object) = self.library.materials_mut().get_mut(material) else {
            return GR_GENERIC;
        };
        match object.batch_route(&context, args) {
            None => object.submit(services, &context, args),
            Some(Err(result)) => result,
            Some(Ok(route)) => {
                self.select_bin(route.bin);
                let args = DrawArgs {
                    flags: route.flags,
                    ..*args
                };
                self.submit(services, material, input, &args, route.depth_bits)
            }
        }
    }

    /// `FlushBin` (slot 9): flushes one bin if it holds records, then with
    /// `clear` empties it.
    pub fn flush_bin(&mut self, services: &mut S, index: u32, clear: u8) -> i32 {
        if index >= 2 {
            return GR_GENERIC;
        }
        let index = index as usize;
        if self.bins[index].record_count == 0 {
            return GR_OK;
        }
        self.flush_one(services, index, clear);
        if clear != 0 {
            self.bins[index].clear();
        }
        GR_OK
    }

    /// `FlushAll` (slot 10): flushes the bins in flush order that hold
    /// records, each emptied after with `clear`.
    pub fn flush_all(&mut self, services: &mut S, clear: u8) -> i32 {
        for position in 0..2 {
            let index = self.bin_order[position];
            if index >= 2 {
                continue;
            }
            let index = index as usize;
            if self.bins[index].record_count == 0 {
                continue;
            }
            self.flush_one(services, index, clear);
            if clear != 0 {
                self.bins[index].clear();
            }
        }
        GR_OK
    }

    /// `FlushOne`: the events around the bin's sort kind.
    fn flush_one(&mut self, services: &mut S, index: usize, clear: u8) {
        if let Some(events) = self.events.as_deref_mut() {
            events.begin_flush(clear);
        }
        match self.bins[index].sort_kind {
            1 => diag::dump(
                DumpFlags::WARNING,
                &diag::general_line(
                    SOURCE,
                    0x5d9,
                    Severity::Warning,
                    "MaterialBatcher: sort statte not well implemented, contact stravis\n",
                ),
            ),
            2..=5 => self.process_queued_bin(services, index),
            6 | 7 => diag::dump(
                DumpFlags::NOTICE,
                &diag::general_line(
                    SOURCE,
                    0x5d2,
                    Severity::Notice,
                    "IMB_SO_PRIM_BACK_TO_FRONT/FRONT_TO_BACK sort order unimplemented, contact pbleisch\n",
                ),
            ),
            _ => {}
        }
        if let Some(events) = self.events.as_deref_mut() {
            events.end_flush(clear);
        }
    }

    /// `ProcessQueuedBin` with `BuildBatchDispatch`.
    fn process_queued_bin(&mut self, services: &mut S, index: usize) {
        let bin = &mut self.bins[index];
        let sort_kind = bin.sort_kind;
        // The queue flattened, material by material; the dispatch records
        // name its entries.
        let mut records: Vec<(MaterialId, QueuedRecord)> = std::mem::take(&mut bin.queue)
            .into_iter()
            .flat_map(|(material, list)| list.into_iter().map(move |r| (material, r)))
            .collect();
        let mut dispatch: Vec<Dispatch> = Vec::new();
        // The material of the records seen last, with its render flags when
        // it has the batched face.
        let mut current: Option<(MaterialId, Option<u32>)> = None;
        for (at, (material, record)) in records.iter().enumerate() {
            if current.map(|(m, _)| m) != Some(*material) {
                let flags = self
                    .library
                    .materials_mut()
                    .get_mut(*material)
                    .and_then(|object| {
                        let flags = object.render_flags(services);
                        object.is_batched().then_some(flags)
                    });
                current = Some((*material, flags));
            }
            if let Some((_, Some(material_flags))) = current {
                dispatch.push(Dispatch {
                    material_flags,
                    key: record.depth(),
                    material: *material,
                    record: at,
                });
            }
        }
        match sort_kind {
            2 | 4 => qsort(&mut dispatch, |a, b| compare_keys(a.key, b.key, true)),
            3 | 5 => qsort(&mut dispatch, |a, b| compare_keys(a.key, b.key, false)),
            _ => {}
        }
        let mut active: Option<MaterialId> = None;
        let mut previous: Option<usize> = None;
        let mut active_tag: usize = 0;
        for item in &dispatch {
            let record = &mut records[item.record].1;
            if sort_kind == 4 || sort_kind == 5 {
                sort_triangles(record, sort_kind == 4);
            }
            if record.flags & flags::DEVICE_BUFFERS != 0
                && let Some((vb, ib)) = record.buffers
                && (services.select_vb(vb.vertex_buffer) < 0
                    || services.select_ib(ib.index_buffer, vb.base_vertex) < 0)
            {
                record.flags &= !flags::DEVICE_BUFFERS;
            }
            let state = services.pipeline_state(7).unwrap_or(1);
            if state != 0 {
                services.set_modelview(&record.combined);
            } else {
                services.set_world(&record.world);
                services.set_view(&record.view);
            }
            let record = &records[item.record].1;
            if active != Some(item.material) || item.material_flags & 0x20 != 0 {
                if let (Some(face), Some(prev)) = (active, previous)
                    && let Some(object) = self.library.materials_mut().get_mut(face)
                {
                    object.batch_teardown(services, &records[prev].1.context());
                }
                active = Some(item.material);
                if let Some(object) = self.library.materials_mut().get_mut(item.material) {
                    object.batch_bind(services, &record.context());
                }
            }
            if let Some(events) = self.events.as_deref_mut()
                && (record.tag == 0 || active_tag != record.tag)
            {
                events.begin_record(item.material, record.tag, record.tag_context);
                active_tag = record.tag;
            }
            previous = Some(item.record);
            if let Some(face) = active
                && let Some(object) = self.library.materials_mut().get_mut(face)
            {
                object.batch_submit(services, &record.context(), &record.args());
            }
            if let Some(events) = self.events.as_deref_mut() {
                events.end_record(item.material, record.tag, record.tag_context);
            }
        }
        if let (Some(face), Some(prev)) = (active, previous)
            && let Some(object) = self.library.materials_mut().get_mut(face)
        {
            object.batch_teardown(services, &records[prev].1.context());
        }
        // The flush may change the records (sorted indices, cleared device
        // buffer flags); they stay queued until the bin is cleared.
        let queue = &mut self.bins[index].queue;
        for (material, record) in records {
            queue.entry(material).or_default().push(record);
        }
    }
}

/// One triangle of the triangle sort (`FBatchTriangle`).
#[derive(Clone, Copy, Debug)]
struct Triangle {
    key: f32,
    indices: [u16; 3],
}

/// The triangle sort of sort kinds 4 and 5: each triangle keyed by the
/// mean depth of its corners (read as packed float triples whatever the
/// stride, through the position index array when there is one), sorted,
/// written to the record's own array (a new one when the indices are the
/// caller's).
fn sort_triangles(record: &mut QueuedRecord, ascending: bool) {
    let count = (record.index_span / 3) as usize;
    let source = Rc::clone(&record.vertices);
    let remap = source.position.index.as_deref();
    let base = record.ib_start as usize;
    let indices = record.indices.as_slice();
    let index_at = |i: usize| indices.get(base + i).copied().unwrap_or(0);
    let position_index = |i: u16| match remap {
        Some(remap) => remap.get(usize::from(i)).copied().unwrap_or(0),
        None => u32::from(i),
    };
    let mut triangles: Vec<Triangle> = (0..count)
        .map(|t| {
            let corners = [index_at(t * 3), index_at(t * 3 + 1), index_at(t * 3 + 2)];
            let p = corners.map(|c| source.position_at(position_index(c) as usize * 12));
            let mut depth = record.position_depth(p[1]);
            depth += record.position_depth(p[2]);
            depth += record.position_depth(p[0]);
            Triangle {
                key: (depth * f64::from(0.333_333_34_f32)) as f32,
                indices: corners,
            }
        })
        .collect();
    qsort(&mut triangles, |a, b| compare_keys(a.key, b.key, ascending));
    let sorted = triangles.iter().flat_map(|t| t.indices);
    if record.internal_flags & 1 == 0 {
        record.indices = Indices::Owned(sorted.collect());
        record.ib_start = 0;
    } else {
        let start = record.ib_start as usize;
        let mut own = match std::mem::replace(&mut record.indices, Indices::Owned(Vec::new())) {
            Indices::Owned(v) => v,
            Indices::Shared(s) => s.to_vec(),
        };
        for (i, index) in sorted.enumerate() {
            if own.len() <= start + i {
                own.resize(start + i + 1, 0);
            }
            own[start + i] = index;
        }
        record.indices = Indices::Owned(own);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn key_comparison() {
        assert_eq!(compare_keys(1.0, 2.0, true), -1);
        assert_eq!(compare_keys(2.0, 1.0, true), 1);
        assert_eq!(compare_keys(2.0, 1.0, false), -1);
        assert_eq!(compare_keys(1.0, 1.0, false), 0);
        assert_eq!(compare_keys(f32::NAN, 1.0, true), 1);
        assert_eq!(compare_keys(1.0, f32::NAN, false), 1);
    }

    #[test]
    fn clone_pads_short_streams() {
        let source = VertexSource {
            position: VertexStream {
                data: Some(vec![1, 2, 3]),
                stride: 4,
                count: 2,
                index: Some(vec![7]),
                byte_size: 12,
            },
            count: 3,
            ..VertexSource::default()
        };
        let copy = source.clone_for_queue();
        assert_eq!(copy.position.data, Some(vec![1, 2, 3, 0, 0, 0, 0, 0]));
        assert_eq!(copy.position.index, Some(vec![7, 0, 0]));
        assert_eq!(copy.normal.data, None);
    }
}
