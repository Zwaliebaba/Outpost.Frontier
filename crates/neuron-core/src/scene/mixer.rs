//! The animation mixer of `CppSource/SceneRuntime/SceneRuntimeMixer.cpp`.
//!
//! Every target a typed channel writes to (a joint, an object, a sink) has
//! a list of nodes, kept under the channel's mixer key and its bind
//! descriptor. A list is evaluated as a stack: leaves give their channel's
//! sample, an overlay the value the target had when the first blend began,
//! and a blend eases from the value below it to the one below that over
//! its duration (a raised-cosine table of 256 steps). A walk ticks every
//! node, retires finished blends and channels, evaluates the list and
//! writes the result to the engine.
//!
//! Lists are slabs of nodes linked by index, as the reference links them
//! by pointer.

use std::collections::BTreeMap;
use std::sync::OnceLock;

use crate::diag::{DumpFlags, dump};
use crate::math::{Quaternion, Vector, matrix_to_quaternion, quat_slerp, quaternion_to_matrix};

use super::engine::Engine;
use super::typed::{BindDesc, ChannelEvent, PLAY_ENDED, TypedChannel, TypedId, joint_value_count};

/// `G_MixerEaseStep`: pi / 255 as the reference stores it.
const EASE_STEP: f64 = f64::from_bits(0x3F89_3B35_2540_C735);

/// `MixerEase`: the eased weight of step `index` of 256,
/// `(1 - cos(index * pi / 255)) / 2`. Tier B: it calls `cos`. The
/// reference indexes its table without a check; the port clamps.
#[must_use]
pub fn ease(index: i32) -> f32 {
    static TABLE: OnceLock<[f32; 256]> = OnceLock::new();
    let table = TABLE.get_or_init(|| {
        core::array::from_fn(|step| ((1.0 - (step as f64 * EASE_STEP).cos()) * 0.5) as f32)
    });
    table[index.clamp(0, 255) as usize]
}

fn r(value: f64) -> f32 {
    value as f32
}

fn e(value: f32) -> f64 {
    f64::from(value)
}

/// The inner map's key (`FAnimBindLess`): the kind, then for a joint or an
/// object the instance, for a sink the sink token. Other kinds compare
/// equal.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct BindKey {
    kind: i32,
    value: i64,
}

impl BindKey {
    /// The key of a descriptor.
    #[must_use]
    pub fn of(desc: &BindDesc) -> Self {
        let kind = i32::from_ne_bytes(desc.kind.to_ne_bytes());
        let value = match desc.kind {
            0 | 1 => desc.instance.map_or(-1, |i| {
                i64::from(i32::from_ne_bytes(i.index().to_ne_bytes()))
            }),
            2 => i64::from(desc.extra),
            _ => 0,
        };
        Self { kind, value }
    }
}

/// A blend node (`FMixerBlend`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Blend {
    /// Seconds since it began.
    pub time: f32,
    /// Its length.
    pub duration: f32,
    /// `1 / duration`, rounded to `float`.
    pub inv_duration: f32,
    /// Bit 0: drop the outgoing side when done; bit 1: carry the root's
    /// position through the blend.
    pub flags: u8,
    /// The root's position when it began.
    pub pose_start: [f32; 3],
    /// The outgoing side's velocity.
    pub pose_rate: [f32; 3],
    /// The incoming side's velocity minus the outgoing side's.
    pub pose_ctrl: [f32; 3],
}

/// What a node is.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum NodeKind {
    /// A typed channel's sample.
    Leaf(TypedId),
    /// A blend of the two values below it.
    Blend(Blend),
    /// A frozen value: `count` words.
    Overlay {
        /// The words it holds (1, 2, 3, 4 or 7; 0 for none).
        count: u32,
        /// The words.
        values: [f32; 7],
    },
}

/// A node (`FMixerNode`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Node {
    /// What it is.
    pub kind: NodeKind,
    prev: Option<usize>,
    next: Option<usize>,
    remove: bool,
}

/// A list (`FMixerList`) and the descriptor it was made for.
#[derive(Clone, Debug, PartialEq)]
pub struct MixerList {
    /// The descriptor of the channel that made the list (the map's key).
    pub desc: BindDesc,
    nodes: Vec<Option<Node>>,
    head: Option<usize>,
    tail: Option<usize>,
    /// The nodes linked.
    pub count: u32,
    /// The duration of the last blend attached.
    pub blend_duration: f32,
}

/// The typed channels the mixer reads, by id.
pub trait Channels {
    /// The channel `id`.
    fn get(&self, id: TypedId) -> Option<&TypedChannel>;
    /// The channel `id`.
    fn get_mut(&mut self, id: TypedId) -> Option<&mut TypedChannel>;
}

impl MixerList {
    fn new(desc: BindDesc) -> Self {
        Self {
            desc,
            nodes: Vec::new(),
            head: None,
            tail: None,
            count: 0,
            blend_duration: 0.0,
        }
    }

    /// The nodes head first.
    pub fn nodes(&self) -> impl Iterator<Item = &Node> {
        let mut at = self.head;
        core::iter::from_fn(move || {
            let node = self.node(at?)?;
            at = node.next;
            Some(node)
        })
    }

    /// Every live node, in slab order.
    pub(crate) fn slots_mut(&mut self) -> impl Iterator<Item = &mut Node> {
        self.nodes.iter_mut().flatten()
    }

    fn node(&self, at: usize) -> Option<&Node> {
        self.nodes.get(at)?.as_ref()
    }

    fn node_mut(&mut self, at: usize) -> Option<&mut Node> {
        self.nodes.get_mut(at)?.as_mut()
    }

    fn kind(&self, at: Option<usize>) -> Option<&NodeKind> {
        Some(&self.node(at?)?.kind)
    }

    fn is_blend(&self, at: Option<usize>) -> bool {
        matches!(self.kind(at), Some(NodeKind::Blend(_)))
    }

    fn next(&self, at: Option<usize>) -> Option<usize> {
        self.node(at?)?.next
    }

    fn prev(&self, at: Option<usize>) -> Option<usize> {
        self.node(at?)?.prev
    }

    fn alloc(&mut self, kind: NodeKind) -> usize {
        self.nodes.push(Some(Node {
            kind,
            prev: None,
            next: None,
            remove: false,
        }));
        self.nodes.len() - 1
    }

    /// `MixerAppend`.
    fn append(&mut self, at: usize) {
        let tail = self.tail;
        self.tail = Some(at);
        match tail {
            None => self.head = Some(at),
            Some(t) => {
                if let Some(n) = self.node_mut(t) {
                    n.next = Some(at);
                }
            }
        }
        if let Some(n) = self.node_mut(at) {
            n.next = None;
            n.prev = tail;
        }
        self.count = self.count.wrapping_add(1);
    }

    /// `MixerInsertBefore`: before `place`, or at the tail for none.
    fn insert_before(&mut self, place: Option<usize>, at: usize) {
        let before = match place {
            None => {
                let t = self.tail;
                self.tail = Some(at);
                t
            }
            Some(w) => {
                let b = self.prev(Some(w));
                if let Some(n) = self.node_mut(w) {
                    n.prev = Some(at);
                }
                b
            }
        };
        match before {
            None => self.head = Some(at),
            Some(b) => {
                if let Some(n) = self.node_mut(b) {
                    n.next = Some(at);
                }
            }
        }
        if let Some(n) = self.node_mut(at) {
            n.next = place;
            n.prev = before;
        }
        self.count = self.count.wrapping_add(1);
    }

    /// `MixerUnlink` (and the free that follows: the slot is emptied).
    fn unlink(&mut self, at: usize) {
        let Some(node) = self.node(at).copied() else {
            return;
        };
        match node.prev {
            None => self.head = node.next,
            Some(p) => {
                if let Some(n) = self.node_mut(p) {
                    n.next = node.next;
                }
            }
        }
        match node.next {
            None => self.tail = node.prev,
            Some(nx) => {
                if let Some(n) = self.node_mut(nx) {
                    n.prev = node.prev;
                }
            }
        }
        self.count = self.count.wrapping_sub(1);
        self.nodes[at] = None;
    }

    /// `MixerUnlink` keeping the node, for a move.
    fn detach(&mut self, at: usize) -> Option<Node> {
        let node = self.node(at).copied()?;
        self.unlink(at);
        self.nodes[at] = Some(node);
        Some(node)
    }

    /// `MixerMarkDropped`: a leaf about to go marks its channel ended.
    fn mark_dropped<C: Channels>(&self, at: Option<usize>, channels: &mut C) {
        if let Some(NodeKind::Leaf(id)) = self.kind(at)
            && let Some(typed) = channels.get_mut(*id)
        {
            typed.play_flags |= PLAY_ENDED;
        }
    }

    /// `MixerUnlinkParentBlend(blend, outer)`: drops the blend, what it was
    /// fading from (a blend there recursively) and, with `outer`, what it
    /// was fading to.
    fn unlink_parent_blend<C: Channels>(&mut self, at: usize, outer: bool, channels: &mut C) {
        let inner = self.prev(Some(at));
        let farther = self.prev(inner);
        if self.is_blend(farther) {
            if let Some(f) = farther {
                self.unlink_parent_blend(f, true, channels);
            }
        } else if let Some(f) = farther {
            self.mark_dropped(Some(f), channels);
            self.unlink(f);
        }
        if outer && let Some(i) = inner {
            self.mark_dropped(Some(i), channels);
            self.unlink(i);
        }
        self.unlink(at);
    }

    /// The first leaf of channel `id`.
    fn find_leaf(&self, id: TypedId) -> Option<usize> {
        let mut at = self.head;
        while let Some(i) = at {
            let node = self.node(i)?;
            if node.kind == NodeKind::Leaf(id) {
                return Some(i);
            }
            at = node.next;
        }
        None
    }

    /// `MixerFreeList` after marking every leaf dropped.
    fn drop_all<C: Channels>(&mut self, channels: &mut C) {
        let mut at = self.head;
        while let Some(i) = at {
            self.mark_dropped(Some(i), channels);
            at = self.next(Some(i));
        }
        while let Some(h) = self.head {
            self.unlink(h);
        }
    }

    /// Slot 3 of a node (`SampleDisplacement`): a leaf's step, an
    /// overlay's zero, a blend's predecessor's.
    fn sample_displacement<C: Channels>(&self, at: Option<usize>, dt: f32, channels: &C) -> Vector {
        match self.kind(at) {
            Some(NodeKind::Leaf(id)) => channels.get(*id).map_or(Vector::ZERO, |t| t.step(dt)),
            Some(NodeKind::Blend(_)) => self.sample_displacement(self.prev(at), dt, channels),
            _ => Vector::ZERO,
        }
    }

    /// A node's `Evaluate(out, stack)`.
    fn evaluate<C: Channels>(
        &self,
        at: usize,
        out: &mut [f32; 7],
        stack: &mut Vec<usize>,
        channels: &C,
    ) {
        match self.kind(Some(at)).copied() {
            Some(NodeKind::Leaf(id)) => {
                if let Some(typed) = channels.get(id) {
                    typed.copy_sampled(out);
                }
            }
            Some(NodeKind::Overlay { count, values }) => {
                let n = (count as usize).min(7);
                out[..n].copy_from_slice(&values[..n]);
            }
            Some(NodeKind::Blend(blend)) => self.evaluate_blend(&blend, out, stack, channels),
            None => {}
        }
    }

    /// `FMixerBlend::Evaluate`.
    fn evaluate_blend<C: Channels>(
        &self,
        blend: &Blend,
        out: &mut [f32; 7],
        stack: &mut Vec<usize>,
        channels: &C,
    ) {
        // The reference pops two nodes and calls them; a blend with fewer
        // below it dereferences null. The port leaves `out` as it is.
        let (Some(right), Some(left)) = (stack.pop(), stack.pop()) else {
            return;
        };
        let mut left_value = [0.0; 7];
        let mut right_value = [0.0; 7];
        self.evaluate(left, &mut left_value, stack, channels);
        self.evaluate(right, &mut right_value, stack, channels);
        let index = if blend.duration > 0.0 {
            crate::num::trunc_to_i32_via_i64(
                (e(blend.inv_duration) * e(blend.time) * 255.0).floor(),
            )
        } else {
            0xFF
        };
        let weight = ease(index);
        let leaf = match (self.kind(Some(right)), self.kind(Some(left))) {
            (Some(NodeKind::Leaf(id)), _) | (_, Some(NodeKind::Leaf(id))) => Some(*id),
            _ => None,
        };
        if let Some(id) = leaf {
            let Some(typed) = channels.get(id) else {
                return;
            };
            typed.interpolate_pose(out, &left_value, &right_value, e(weight));
            if blend.flags & 2 == 0 {
                return;
            }
            let axis = typed.loop_flags;
            let kept = if axis & 0x8 != 0 {
                Some(out[0])
            } else if axis & 0x10 != 0 {
                Some(out[1])
            } else if axis & 0x20 != 0 {
                Some(out[2])
            } else {
                None
            };
            let inv = e(blend.inv_duration);
            let now = e(blend.time);
            let ctrl0 = e(blend.pose_ctrl[0]) * 0.5;
            let ctrl1 = e(blend.pose_ctrl[1]) * 0.5;
            let ctrl2 = r(e(blend.pose_ctrl[2]) * 0.5);
            let ctrl0_inv = r(ctrl0 * inv);
            let ctrl1_inv = ctrl1 * inv;
            let ctrl2_inv_now = e(ctrl2) * inv * now;
            let term0 = r(e(ctrl0_inv) * now);
            let term1 = r(ctrl1_inv * now);
            let rate0 = e(term0) + e(blend.pose_rate[0]);
            let rate1 = r(e(term1) + e(blend.pose_rate[1]));
            let rate2 = r(ctrl2_inv_now + e(blend.pose_rate[2]));
            let pos0 = rate0 * now;
            let pos1 = e(rate1) * now;
            let pos2 = r(e(rate2) * now);
            out[0] = r(pos0 + e(blend.pose_start[0]));
            out[1] = r(pos1 + e(blend.pose_start[1]));
            out[2] = r(e(pos2) + e(blend.pose_start[2]));
            if let Some(kept) = kept {
                if axis & 0x8 != 0 {
                    out[0] = kept;
                } else if axis & 0x10 != 0 {
                    out[1] = kept;
                } else {
                    out[2] = kept;
                }
            }
            return;
        }
        let (
            Some(NodeKind::Overlay { count, values: lv }),
            Some(NodeKind::Overlay { values: rv, .. }),
        ) = (self.kind(Some(left)), self.kind(Some(right)))
        else {
            return;
        };
        let w = e(weight);
        match count {
            1 => out[0] = r((e(rv[0]) - e(lv[0])) * w + e(lv[0])),
            3 => {
                for i in 0..3 {
                    out[i] = r((e(rv[i]) - e(lv[i])) * w + e(lv[i]));
                }
            }
            4 => {
                let q = quat_slerp(&quat(&lv[..4]), &quat(&rv[..4]), weight);
                out[..4].copy_from_slice(&q.to_words());
            }
            7 => blend_pos_quat(out, lv, rv, weight),
            _ => {}
        }
    }
}

fn quat(w: &[f32]) -> Quaternion {
    Quaternion::new(w[0], w[1], w[2], w[3])
}

/// `MixerBlendPosQuat`.
fn blend_pos_quat(out: &mut [f32; 7], a: &[f32; 7], b: &[f32; 7], t: f32) {
    for i in 0..3 {
        out[i] = r((e(b[i]) - e(a[i])) * e(t) + e(a[i]));
    }
    let q = quat_slerp(&quat(&a[3..7]), &quat(&b[3..7]), t);
    out[3..7].copy_from_slice(&q.to_words());
}

/// `FMixerOverlay(desc)`: the joint's parameters or the object's pose as
/// they are now. A joint without a parent leaves the reference's count
/// uninitialised; the port holds nothing (divergence register).
fn overlay(engine: &Engine, desc: &BindDesc) -> NodeKind {
    let mut values = [0.0; 7];
    let count = match desc.kind {
        0 => match engine.joint_params(desc.instance, 0) {
            Some(params) if engine.instance_relative(desc.instance).is_some() => {
                let n = params.len().min(7);
                values[..n].copy_from_slice(&params[..n]);
                joint_value_count(engine, desc.instance) as u32
            }
            _ => 0,
        },
        1 => {
            values[..3].copy_from_slice(&engine.instance_position(desc.instance).to_words());
            let q = matrix_to_quaternion(&engine.instance_orientation(desc.instance));
            values[3..].copy_from_slice(&q.to_words());
            7
        }
        _ => 0,
    };
    NodeKind::Overlay { count, values }
}

/// The mixer: lists by mixer key, then by bind key.
pub type MixerMap = BTreeMap<i32, BTreeMap<BindKey, MixerList>>;

/// `MixerWalkPlayback`: ticks the list's nodes, retires what finished,
/// evaluates the list and writes the result: a joint's parameters, an
/// object's position and orientation (nothing for a sink).
pub fn walk_list<C: Channels>(
    engine: &mut Engine,
    list: &mut MixerList,
    channels: &mut C,
    dt: f32,
    events: &mut Vec<ChannelEvent>,
) {
    let desc = list.desc;
    let snapshot: Vec<usize> = {
        let mut out = Vec::new();
        let mut at = list.head;
        while let Some(i) = at {
            out.push(i);
            at = list.next(Some(i));
        }
        out
    };
    for at in snapshot {
        let Some(node) = list.node(at).copied() else {
            continue;
        };
        let finished = match node.kind {
            NodeKind::Leaf(id) => channels
                .get_mut(id)
                .map_or(0, |t| t.tick(engine, id, dt, events)),
            NodeKind::Blend(mut blend) => {
                let sum = e(dt) + e(blend.time);
                blend.time = r(sum);
                if let Some(n) = list.node_mut(at) {
                    n.kind = NodeKind::Blend(blend);
                }
                u32::from(sum >= e(blend.duration))
            }
            NodeKind::Overlay { .. } => 0,
        };
        if finished != 1 {
            continue;
        }
        match node.kind {
            NodeKind::Blend(blend) => {
                if blend.flags & 2 != 0
                    && let Some(NodeKind::Leaf(prev)) = list.kind(node.prev).copied()
                {
                    let position = engine.instance_position(desc.instance);
                    if let Some(t) = channels.get_mut(prev) {
                        t.set_overlay_pos(position);
                    }
                }
                if blend.flags & 1 != 0 {
                    let outer = list.prev(node.prev);
                    if list.is_blend(outer) {
                        if let Some(o) = outer {
                            list.unlink_parent_blend(o, true, channels);
                        }
                    } else if let Some(o) = outer {
                        list.mark_dropped(Some(o), channels);
                        list.unlink(o);
                    }
                }
                list.unlink(at);
            }
            NodeKind::Leaf(id) => {
                if let Some(next) = node.next {
                    if list.is_blend(Some(next)) || list.is_blend(list.next(Some(next))) {
                        if let Some(t) = channels.get_mut(id) {
                            t.apply_from_archetype();
                        }
                    } else {
                        list.unlink(at);
                    }
                } else if let Some(before) = node.prev {
                    let duration = list.blend_duration;
                    let back = list.alloc(NodeKind::Blend(Blend {
                        time: 0.0,
                        duration,
                        inv_duration: r(1.0 / e(duration)),
                        flags: 1,
                        ..Blend::default()
                    }));
                    if let Some(t) = channels.get_mut(id) {
                        t.apply_from_archetype();
                    }
                    list.detach(at);
                    list.insert_before(Some(before), at);
                    list.append(back);
                } else {
                    if let Some(t) = channels.get_mut(id) {
                        t.apply_from_archetype();
                    }
                    if let Some(n) = list.node_mut(at) {
                        n.remove = true;
                    }
                }
            }
            NodeKind::Overlay { .. } => {}
        }
    }

    let mut stack: Vec<usize> = Vec::new();
    let mut at = list.head;
    while let Some(i) = at {
        stack.push(i);
        at = list.next(Some(i));
    }
    if let Some(top) = stack.pop() {
        if desc.kind > 2 {
            dump(DumpFlags::FATAL, "Unknown data type!\n");
        }
        let mut out = [0.0f32; 7];
        list.evaluate(top, &mut out, &mut stack, channels);
        match desc.kind {
            0 => {
                engine.set_joint_params(desc.instance, 0, &out);
            }
            1 => {
                engine.set_instance_position(desc.instance, Vector::new(out[0], out[1], out[2]));
                let m = quaternion_to_matrix(&quat(&out[3..7]));
                engine.set_instance_orientation(desc.instance, &m);
            }
            _ => {}
        }
    }

    let mut at = list.head;
    while let Some(i) = at {
        at = list.next(Some(i));
        if list.node(i).is_some_and(|n| n.remove) {
            list.unlink(i);
        }
    }
}

/// `MixerBlendAttach(typed, duration, flags)`: adds channel `id` to its
/// target's list. A positive duration blends it in over that long (from
/// an overlay of the target's value when the list was empty), else it
/// replaces everything in the list.
pub fn blend_attach<C: Channels>(
    mixers: &mut MixerMap,
    engine: &Engine,
    channels: &mut C,
    id: TypedId,
    duration: f32,
    mut flags: u32,
) {
    let Some(typed) = channels.get(id) else {
        return;
    };
    let desc = typed.bind;
    let key = typed.key;
    let inner = mixers.entry(key.0).or_default();
    let list = inner
        .entry(BindKey::of(&desc))
        .or_insert_with(|| MixerList::new(desc));
    if list.find_leaf(id).is_some() {
        return;
    }
    let mut anchor = None;
    let blend = duration > 0.0;
    if blend {
        if list.count == 0 {
            flags &= !0x80;
            let o = list.alloc(overlay(engine, &list.desc));
            list.append(o);
            anchor = Some(o);
        } else {
            anchor = list.tail;
        }
    } else if list.count != 0 {
        list.drop_all(channels);
    }
    let leaf = list.alloc(NodeKind::Leaf(id));
    list.append(leaf);
    if !blend {
        return;
    }
    let mut b = Blend {
        time: 0.0,
        duration,
        inv_duration: r(1.0 / e(duration)),
        flags: 1,
        ..Blend::default()
    };
    if (flags & 0xFF) as u8 & 0x80 != 0 {
        b.flags &= 0xFE;
    }
    let at = list.alloc(NodeKind::Blend(b));
    list.append(at);
    list.blend_duration = duration;
    if desc.kind != 1 || flags & 0x38 == 0 {
        return;
    }
    let predicted = list.sample_displacement(Some(leaf), duration, channels);
    b.flags |= 2;
    b.pose_start = engine.instance_position(desc.instance).to_words();
    b.pose_rate = list
        .sample_displacement(anchor, duration, channels)
        .to_words();
    let inv = e(b.inv_duration);
    for v in &mut b.pose_rate {
        *v = r(inv * e(*v));
    }
    let scaled1 = r(e(predicted.y) * inv);
    let scaled2 = r(e(predicted.z) * inv);
    b.pose_ctrl = [
        r(e(predicted.x) * inv - e(b.pose_rate[0])),
        r(e(scaled1) - e(b.pose_rate[1])),
        r(e(scaled2) - e(b.pose_rate[2])),
    ];
    if let Some(n) = list.node_mut(at) {
        n.kind = NodeKind::Blend(b);
    }
}

/// `MixerUnbindTyped(typed)`: takes channel `id` out of its list. A
/// channel a later blend fades from leaves an overlay of the target's
/// value in its place; a channel a blend fades to takes that blend and
/// what it faded from with it. An emptied list is dropped.
pub fn unbind<C: Channels>(mixers: &mut MixerMap, engine: &Engine, channels: &mut C, id: TypedId) {
    let Some(typed) = channels.get(id) else {
        return;
    };
    let key = typed.key;
    let bind = BindKey::of(&typed.bind);
    let Some(inner) = mixers.get_mut(&key.0) else {
        return;
    };
    let Some(list) = inner.get_mut(&bind) else {
        return;
    };
    if let Some(leaf) = list.find_leaf(id) {
        let after = list.next(Some(leaf));
        if after.is_some() && list.is_blend(list.next(after)) {
            let o = list.alloc(overlay(engine, &list.desc));
            list.insert_before(after, o);
            let blend = list.next(list.next(Some(o)));
            if let Some(n) = blend.and_then(|b| list.node_mut(b))
                && let NodeKind::Blend(b) = &mut n.kind
            {
                b.flags |= 1;
            }
        } else if list.is_blend(after) {
            let after_next = list.next(after);
            if after_next.is_some() && list.is_blend(list.next(after_next)) {
                let o = list.alloc(overlay(engine, &list.desc));
                list.insert_before(after_next, o);
            }
            let outer = list.prev(list.prev(after));
            if list.is_blend(outer) {
                if let Some(o) = outer {
                    list.unlink_parent_blend(o, true, channels);
                }
            } else if let Some(o) = outer {
                list.mark_dropped(Some(o), channels);
                list.unlink(o);
            }
            if let Some(a) = after {
                list.unlink(a);
            }
        }
        list.unlink(leaf);
    }
    if list.count == 0 {
        inner.remove(&bind);
    }
}

/// `MixerHasLeaf`: whether channel `id` is a leaf of its list.
#[must_use]
pub fn has_leaf<C: Channels>(mixers: &MixerMap, channels: &C, id: TypedId) -> bool {
    let Some(typed) = channels.get(id) else {
        return false;
    };
    mixers
        .get(&typed.key.0)
        .and_then(|inner| inner.get(&BindKey::of(&typed.bind)))
        .is_some_and(|list| list.find_leaf(id).is_some())
}

/// Walks every list under `key` and drops what emptied (`MixerWalkKey`).
pub fn walk_key<C: Channels>(
    mixers: &mut MixerMap,
    engine: &mut Engine,
    channels: &mut C,
    key: i32,
    dt: f32,
    events: &mut Vec<ChannelEvent>,
) {
    if key == -1 {
        return;
    }
    let Some(inner) = mixers.get_mut(&key) else {
        return;
    };
    let keys: Vec<BindKey> = inner.keys().copied().collect();
    for bind in keys {
        let Some(list) = inner.get_mut(&bind) else {
            continue;
        };
        walk_list(engine, list, channels, dt, events);
        if list.count == 0 {
            inner.remove(&bind);
        }
    }
    if inner.is_empty() {
        mixers.remove(&key);
    }
}
