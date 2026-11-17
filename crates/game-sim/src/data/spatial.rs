//! The spatial partitions (`NewSpatial`, `BspPartition`,
//! `LinearPartition`), ported from `CppSource/Common/common_spatial.cpp`.
//!
//! A partition stores spheres with an object handle and answers which of
//! them a sphere, a ray or a frustum's bounding sphere touches. The BSP
//! keeps leaves of at most eight entries; a full leaf is split by a plane
//! chosen from candidates by how evenly it divides the entries. Which plane
//! wins, and so the order of every query's answers, depends on the exact
//! float steps of the reference, which are kept here (Tier A). The frustum
//! query takes a tangent (Tier B).
//!
//! The C++ carves nodes from one process-wide pool; here each partition
//! owns its nodes in an arena and refers to them by index. Object handles
//! are `u32` values, 0 for none (the C++ null pointer).

// The reference's x87 comparisons send unordered operands (NaN) to one
// side; the port spells each outcome as the comparison that is true exactly
// when the reference takes that side, which needs negated comparisons.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use neuron_core::hash::eq_ignore_case;
use neuron_core::math::Vector;

use super::universe::Universe;
use super::zone::Zone;
use super::zone::query::{Frustum, Sphere};

/// The capacity every caller gives its result buffer.
pub const RESULT_CAPACITY: usize = 0x800;

/// A stored sphere (`FSpatialEntry`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SpatialEntry {
    /// The object handle (0 for none).
    pub object: u32,
    /// The centre.
    pub position: Vector,
    /// The radius.
    pub radius: f32,
}

/// The ray a ray query takes (`FSpatialRay`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SpatialRay {
    /// The origin.
    pub origin: Vector,
    /// The unit direction.
    pub direction: Vector,
    /// The length.
    pub length: f32,
}

/// A query's answers (`FSpatialQueryResult`): appended while below the
/// capacity, never cleared by a query.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct QueryResult {
    /// At most this many items.
    pub capacity: usize,
    /// The object handles, in the order found.
    pub items: Vec<u32>,
}

impl Default for QueryResult {
    fn default() -> Self {
        Self {
            capacity: RESULT_CAPACITY,
            items: Vec::new(),
        }
    }
}

impl QueryResult {
    /// `AppendResult`: a set handle while there is room.
    fn append(&mut self, object: u32) {
        if object != 0 && self.items.len() < self.capacity {
            self.items.push(object);
        }
    }

    /// The count a query returns.
    #[must_use]
    pub fn count(&self) -> usize {
        self.items.len()
    }
}

fn e(x: f32) -> f64 {
    f64::from(x)
}

fn v(x: f32, y: f32, z: f32) -> Vector {
    Vector { x, y, z }
}

/// `(x*x + y*y) + z*z`.
fn length_squared(a: Vector) -> f64 {
    (e(a.x) * e(a.x) + e(a.y) * e(a.y)) + e(a.z) * e(a.z)
}

fn length(a: Vector) -> f64 {
    length_squared(a).sqrt()
}

/// `(a.z*b.z + a.y*b.y) + a.x*b.x`.
fn dot(a: Vector, b: Vector) -> f64 {
    (e(a.z) * e(b.z) + e(a.y) * e(b.y)) + e(a.x) * e(b.x)
}

fn sub(a: Vector, b: Vector) -> Vector {
    v(a.x - b.x, a.y - b.y, a.z - b.z)
}

fn add(a: Vector, b: Vector) -> Vector {
    v(a.x + b.x, a.y + b.y, a.z + b.z)
}

/// `Scale`: `s * v`, float stores.
fn scale(a: Vector, s: f32) -> Vector {
    v(s * a.x, s * a.y, s * a.z)
}

/// `Accumulate`: `sum = v + sum`.
fn accumulate(sum: &mut Vector, a: Vector) {
    *sum = v(a.x + sum.x, a.y + sum.y, a.z + sum.z);
}

/// `NormalizeInPlace`.
fn normalize(a: Vector) -> Vector {
    let inv = 1.0 / length(a);
    v(
        (inv * e(a.x)) as f32,
        (inv * e(a.y)) as f32,
        (inv * e(a.z)) as f32,
    )
}

/// `DivideByLength`: `v * (1 / len)`.
fn divide_by_length(a: Vector, len: f32) -> Vector {
    let inv = 1.0 / e(len);
    v(
        (inv * e(a.x)) as f32,
        (inv * e(a.y)) as f32,
        (inv * e(a.z)) as f32,
    )
}

/// `DivideByScalar`: one reciprocal, rounded for x and y, not for z.
fn divide_by_scalar(a: Vector, s: f32) -> Vector {
    let inv_d = 1.0 / e(s);
    let inv_f = inv_d as f32;
    v(inv_f * a.x, inv_f * a.y, (inv_d * e(a.z)) as f32)
}

/// `ScoreCounts`: `|behind - beyond| + 2 * touching`, or 9999 when the
/// unsigned sum of the counts wrapped.
fn score_counts(counts: [u32; 3]) -> u32 {
    let sum = counts[0].wrapping_add(counts[1]).wrapping_add(counts[2]);
    if !(counts[0] < sum && counts[1] < sum && counts[2] < sum) {
        return 9999;
    }
    let diff = i32::from_ne_bytes(counts[2].wrapping_sub(counts[1]).to_ne_bytes());
    let abs = if diff < 0 { diff.wrapping_neg() } else { diff };
    let touching = i32::from_ne_bytes(counts[0].to_ne_bytes());
    u32::from_ne_bytes(abs.wrapping_add(touching.wrapping_mul(2)).to_ne_bytes())
}

/// The split plane of a node (`FSpatialPlane`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SpatialPlane {
    /// The normal (unit in a split node).
    pub normal: Vector,
    /// The offset along it.
    pub distance: f32,
}

impl SpatialPlane {
    /// `((p.z*n.z + p.y*n.y) + p.x*n.x) - d`.
    fn signed_distance(&self, p: Vector) -> f64 {
        dot(p, self.normal) - e(self.distance)
    }

    /// 1 beyond, 2 behind, 0 touching (and for NaN).
    fn classify(&self, p: Vector, radius: f32) -> usize {
        let s = self.signed_distance(p);
        if s > e(radius) {
            1
        } else if s < -e(radius) {
            2
        } else {
            0
        }
    }

    /// `ClassifyYXZ`: the same test summed y, x, z.
    fn classify_yxz(&self, entry: &SpatialEntry) -> usize {
        let n = self.normal;
        let p = entry.position;
        let s = ((e(p.y) * e(n.y) + e(p.x) * e(n.x)) + e(n.z) * e(p.z)) - e(self.distance);
        if s > e(entry.radius) {
            1
        } else if s < -e(entry.radius) {
            2
        } else {
            0
        }
    }

    /// `Build`: through `point`, normal `direction` made unit (the unit
    /// components are used unrounded for the offset).
    fn build(&mut self, point: Vector, direction: Vector) {
        let inv = 1.0 / length(direction);
        let x = inv * e(direction.x);
        let y = inv * e(direction.y);
        let z = inv * e(direction.z);
        self.normal = v(x as f32, y as f32, z as f32);
        self.distance = ((z * e(point.z) + y * e(point.y)) + x * e(point.x)) as f32;
    }

    /// `BuildFromEdge`: perpendicular to `b - a`, through `a` pushed along
    /// the current normal by its radius plus 0.1. Fails for coincident ends.
    fn build_from_edge(&mut self, a: &SpatialEntry, b: &SpatialEntry) -> bool {
        let dx = b.position.x - a.position.x;
        let dy = b.position.y - a.position.y;
        let dz = b.position.z - a.position.z;
        let dist_sq = (e(dx) * e(dx) + e(dz) * e(dz)) + e(dy) * e(dy);
        if !(dist_sq > 0.001) {
            return false;
        }
        let push = e(a.radius) + e(0.1_f32);
        let px = push * e(self.normal.x) + e(a.position.x);
        let push_y = (push * e(self.normal.y)) as f32;
        let push_z = (push * e(self.normal.z)) as f32;
        let py = e(push_y) + e(a.position.y);
        let pz = e(push_z) + e(a.position.z);
        let len = ((e(dz) * e(dz) + e(dy) * e(dy)) + e(dx) * e(dx)).sqrt() as f32;
        let inv = 1.0 / e(len);
        let nx = (inv * e(dx)) as f32;
        let ny = (inv * e(dy)) as f32;
        let nz = inv * e(dz);
        self.normal = v(nx, ny, nz as f32);
        self.distance = ((nz * pz + e(ny) * py) + e(nx) * px) as f32;
        true
    }

    /// The per-side counts of `entries` (`Classify`).
    fn counts(&self, entries: &[SpatialEntry]) -> [u32; 3] {
        let mut counts = [0u32; 3];
        for entry in entries {
            counts[self.classify(entry.position, entry.radius)] += 1;
        }
        counts
    }

    /// The per-side counts with `ClassifyYXZ` (`ClassifyEntries`).
    fn counts_yxz(&self, entries: &[SpatialEntry]) -> [u32; 3] {
        let mut counts = [0u32; 3];
        for entry in entries {
            counts[self.classify_yxz(entry)] += 1;
        }
        counts
    }

    /// `ComputeInitialSplit`: the centroid plane, then the plane of every
    /// pair (i < j); the best split wins.
    fn compute_initial_split(&mut self, entries: &[SpatialEntry]) {
        let count = entries.len();
        let mut centroid = Vector::ZERO;
        for entry in entries {
            accumulate(&mut centroid, entry.position);
        }
        let inv = (1.0 / count as f64) as f32;
        centroid = scale(centroid, inv);
        let towards_first = || normalize(sub(entries[0].position, centroid));
        let direction = if count == 2 {
            towards_first()
        } else {
            let mut sum = Vector::ZERO;
            for entry in entries {
                let delta = sub(entry.position, centroid);
                let len = length(delta);
                if len > 0.01 {
                    accumulate(&mut sum, scale(delta, (1.0 / len) as f32));
                }
            }
            let sum_len = length(sum);
            if sum_len > 0.25 {
                scale(sum, (1.0 / sum_len) as f32)
            } else {
                towards_first()
            }
        };
        self.build(centroid, direction);
        let mut best = *self;
        let mut best_score = score_counts(best.counts(entries));
        if best_score == 0 {
            return;
        }
        for first in 1..=count {
            let a = entries[first - 1];
            for second in first..count {
                let delta = sub(entries[second].position, a.position);
                let dist_sq =
                    (e(delta.y) * e(delta.y) + e(delta.z) * e(delta.z)) + e(delta.x) * e(delta.x);
                if !(dist_sq > 0.001) {
                    continue;
                }
                let push = (e(a.radius) + e(0.1_f32)) as f32;
                let point = add(a.position, scale(self.normal, push));
                self.build(point, delta);
                let score = score_counts(self.counts(entries));
                if score < best_score {
                    best = *self;
                    best_score = score;
                    if score == 0 {
                        break;
                    }
                }
            }
        }
        *self = best;
    }

    /// `ComputeCentroidSplit`: through the centroid, normal the mean
    /// direction from it to the entries.
    fn compute_centroid_split(&mut self, entries: &[SpatialEntry]) {
        let count = entries.len();
        let (mut sx, mut sy, mut sz) = (0.0f64, 0.0f64, 0.0f64);
        for entry in entries {
            sx += e(entry.position.x);
            sy += e(entry.position.y);
            sz += e(entry.position.z);
        }
        let sz_f = sz as f32;
        let inv = (1.0 / count as f64) as f32;
        let centroid = v(
            (sx * e(inv)) as f32,
            (sy * e(inv)) as f32,
            (e(sz_f) * e(inv)) as f32,
        );
        let mut sum = Vector::ZERO;
        let mut len = 0.0;
        let mut use_sum = false;
        if count != 2 {
            for entry in entries {
                let dz = entry.position.z - centroid.z;
                let dy = entry.position.y - centroid.y;
                let dx = entry.position.x - centroid.x;
                let d_len = ((e(dz) * e(dz) + e(dy) * e(dy)) + e(dx) * e(dx)).sqrt();
                if d_len > 0.01 {
                    let d_inv = 1.0 / d_len;
                    let tx = (e(dx) * d_inv) as f32;
                    let ty = (e(dy) * d_inv) as f32;
                    let tz = (e(dz) * d_inv) as f32;
                    sum = v(tx + sum.x, ty + sum.y, tz + sum.z);
                }
            }
            len = ((e(sum.z) * e(sum.z) + e(sum.y) * e(sum.y)) + e(sum.x) * e(sum.x)).sqrt();
            use_sum = len > 0.25;
        }
        if !use_sum {
            sum = v(
                entries[0].position.x - centroid.x,
                entries[0].position.y - centroid.y,
                entries[0].position.z - centroid.z,
            );
            len = ((e(sum.z) * e(sum.z) + e(sum.y) * e(sum.y)) + e(sum.x) * e(sum.x)).sqrt();
        }
        let inv2 = 1.0 / len;
        let normal = v(
            (e(sum.x) * inv2) as f32,
            (e(sum.y) * inv2) as f32,
            (e(sum.z) * inv2) as f32,
        );
        self.normal = normalize(normal);
        self.distance = dot(centroid, self.normal) as f32;
    }
}

/// A BSP node (`FSpatialNode`): a split node while its normal has unit
/// length; a fresh node is all zero, so a leaf.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct Node {
    plane: SpatialPlane,
    /// Touching, beyond, behind.
    children: [Option<usize>; 3],
    slots: [SpatialEntry; 8],
}

impl Node {
    fn is_split(&self) -> bool {
        (1.0 - length_squared(self.plane.normal)).abs() < 0.05
    }

    /// The used slots: up to the first empty one.
    fn count_used(&self) -> usize {
        self.slots.iter().take_while(|s| s.object != 0).count()
    }

    /// The first empty slot takes the entry; a full node drops it.
    fn add_entry(&mut self, entry: SpatialEntry) {
        let used = self.count_used();
        if used < 8 {
            self.slots[used] = entry;
        }
    }

    fn full(&self) -> bool {
        self.slots[7].object != 0
    }
}

/// `BspPartition`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct BspPartition {
    nodes: Vec<Node>,
    root: Option<usize>,
    /// The entries stored in leaves, as the C++ counts them (`Remove` does
    /// not count down).
    pub entry_count: i32,
}

/// `LinearPartition`: one array, every query a full scan.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LinearPartition {
    /// The entries, in insertion order (`Remove` keeps them).
    pub entries: Vec<SpatialEntry>,
}

/// A partition (`ISpatialPartition`).
#[derive(Clone, Debug, PartialEq)]
pub enum SpatialPartition {
    /// `"BSP"`.
    Bsp(BspPartition),
    /// `"Linear"`.
    Linear(LinearPartition),
}

/// `NewSpatial(type)`: `BSP` or `Linear` (case-insensitive); `QuadTree`
/// and every other name give none.
#[must_use]
pub fn new_spatial(kind: &[u8]) -> Option<SpatialPartition> {
    if eq_ignore_case(kind, b"BSP") {
        Some(SpatialPartition::Bsp(BspPartition::default()))
    } else if eq_ignore_case(kind, b"Linear") {
        Some(SpatialPartition::Linear(LinearPartition::default()))
    } else {
        None
    }
}

impl SpatialPartition {
    /// `Clear` (+0x08).
    pub fn clear(&mut self) {
        match self {
            Self::Bsp(p) => p.clear(),
            Self::Linear(p) => p.entries.clear(),
        }
    }

    /// `Remove` (+0x0c). The linear partition keeps the entry; the BSP one
    /// panics on an empty partition, where the C++ dereferences null.
    pub fn remove(&mut self, object: u32) {
        if let Self::Bsp(p) = self {
            p.remove(object);
        }
    }

    /// `Add` (+0x10).
    pub fn add(&mut self, entry: SpatialEntry) {
        match self {
            Self::Bsp(p) => p.add(entry),
            Self::Linear(p) => p.entries.push(entry),
        }
    }

    /// `AddArray` (+0x14).
    pub fn add_array(&mut self, entries: &[SpatialEntry]) {
        match self {
            Self::Bsp(p) => p.add_array(entries),
            Self::Linear(p) => p.entries.extend_from_slice(entries),
        }
    }

    /// `QueryFrustum` (+0x18): the BSP queries the frustum's bounding
    /// sphere; the linear partition answers nothing and returns 0.
    pub fn query_frustum(&self, frustum: &Frustum, result: &mut QueryResult) -> usize {
        match self {
            Self::Bsp(p) => p.query_frustum(frustum, result),
            Self::Linear(_) => 0,
        }
    }

    /// `QuerySphere` (+0x1c): the entries the sphere overlaps (strictly).
    pub fn query_sphere(&self, sphere: &Sphere, result: &mut QueryResult) -> usize {
        match self {
            Self::Bsp(p) => p.query_sphere(sphere, result),
            Self::Linear(p) => p.query_sphere(sphere, result),
        }
    }

    /// `QueryRay` (+0x20): the linear partition answers nothing and
    /// returns 0.
    pub fn query_ray(&self, ray: &SpatialRay, result: &mut QueryResult) -> usize {
        match self {
            Self::Bsp(p) => p.query_ray(ray, result),
            Self::Linear(_) => 0,
        }
    }
}

impl LinearPartition {
    fn query_sphere(&self, sphere: &Sphere, result: &mut QueryResult) -> usize {
        for entry in &self.entries {
            let dx = e(entry.position.x) - e(sphere.center.x);
            let dy = e(entry.position.y) - e(sphere.center.y);
            let dz = e(entry.position.z) - e(sphere.center.z);
            let reach = e(entry.radius) + e(sphere.radius);
            let dist_sq = (dx * dx + dz * dz) + dy * dy;
            if reach * reach > dist_sq {
                result.append(entry.object);
            }
        }
        result.count()
    }
}

/// The leaf test of both ray walks: the sphere holds the origin, or lies
/// ahead and the ray passes closer than its radius.
fn ray_hits_entry(ray: &SpatialRay, entry: &SpatialEntry, delta: Vector, dist_exact: f64) -> bool {
    let dist = dist_exact as f32;
    if dist_exact < e(entry.radius) {
        return true;
    }
    let unit = divide_by_length(delta, dist);
    let proj = dot(ray.direction, unit);
    if proj < 0.0 {
        return false;
    }
    let along = proj * e(dist);
    let perp_sq = e(dist) * e(dist) - along * along;
    let radius_sq = e(entry.radius) * e(entry.radius);
    radius_sq > perp_sq
}

/// The side of `plane` the ray's origin is on (1 beyond, 2 otherwise) and
/// the crossing parameter when the ray reaches the plane within its length.
fn ray_plane(s: f64, dn: f64, length: f32) -> (usize, Option<f32>) {
    let (side, num, den) = if s > 0.0 { (1, s, -dn) } else { (2, -s, dn) };
    if den < 0.001 {
        return (side, None);
    }
    let t = (num / den) as f32;
    if t < 0.0 || t > length {
        return (side, None);
    }
    (side, Some(t))
}

impl BspPartition {
    fn push_node(&mut self) -> usize {
        self.nodes.push(Node::default());
        self.nodes.len() - 1
    }

    /// `Clear`: every node released, the count reset.
    pub fn clear(&mut self) {
        self.nodes.clear();
        self.root = None;
        self.entry_count = 0;
    }

    /// The node holding `object`: its used slots, then the children depth
    /// first (`FindObject`).
    fn find_object(&self, node: usize, object: u32) -> Option<usize> {
        let n = &self.nodes[node];
        if n.slots[..n.count_used()].iter().any(|s| s.object == object) {
            return Some(node);
        }
        n.children
            .iter()
            .flatten()
            .find_map(|&child| self.find_object(child, object))
    }

    /// `Remove`: the last used slot of the node holding `object` moves into
    /// its place.
    pub fn remove(&mut self, object: u32) {
        let root = self
            .root
            .expect("BspPartition::Remove on an empty partition");
        let Some(node) = self.find_object(root, object) else {
            return;
        };
        let n = &mut self.nodes[node];
        let used = n.count_used();
        if let Some(index) = n.slots[..used].iter().position(|s| s.object == object) {
            n.slots[index] = n.slots[used - 1];
            n.slots[used - 1].object = 0;
        }
    }

    /// A new child of `node` on `side` holding `entry`.
    fn new_child(&mut self, node: usize, side: usize, entry: SpatialEntry) {
        let child = self.push_node();
        self.nodes[node].children[side] = Some(child);
        self.nodes[child].add_entry(entry);
        self.entry_count += 1;
    }

    /// `Add`.
    pub fn add(&mut self, entry: SpatialEntry) {
        let node = match self.root {
            Some(root) => root,
            None => {
                let root = self.push_node();
                self.root = Some(root);
                let n = &mut self.nodes[root];
                n.plane.normal = normalize(v(1.0, 0.0, 0.0));
                // `((z + y) + x) * 0` of the unrounded unit components.
                n.plane.distance =
                    ((e(n.plane.normal.z) + e(n.plane.normal.y) + e(n.plane.normal.x)) * 0.0)
                        as f32;
                root
            }
        };
        if self.nodes[node].is_split() {
            self.insert_walk(node, entry);
        } else if !self.nodes[node].full() {
            self.nodes[node].add_entry(entry);
            self.entry_count += 1;
        } else {
            let slots = self.nodes[node].slots;
            self.nodes[node].plane.compute_initial_split(&slots);
            for index in 0..8 {
                let slot = self.nodes[node].slots[index];
                let side = self.nodes[node].plane.classify(slot.position, slot.radius);
                match self.nodes[node].children[side] {
                    Some(child) => self.insert_split(child, slot),
                    None => self.new_child(node, side, slot),
                }
                self.nodes[node].slots[index].object = 0;
                self.entry_count -= 1;
            }
            self.insert_walk(node, entry);
        }
    }

    /// `InsertWalk`: down the planes to a leaf; a full leaf is split by
    /// `ComputeInitialSplit` and its entries walked down again.
    fn insert_walk(&mut self, mut node: usize, entry: SpatialEntry) {
        loop {
            let side = self.nodes[node]
                .plane
                .classify(entry.position, entry.radius);
            let Some(child) = self.nodes[node].children[side] else {
                self.new_child(node, side, entry);
                return;
            };
            if !self.nodes[child].is_split() {
                if !self.nodes[child].full() {
                    self.nodes[child].add_entry(entry);
                    self.entry_count += 1;
                    return;
                }
                let slots = self.nodes[child].slots;
                self.nodes[child].plane.compute_initial_split(&slots);
                for index in 0..8 {
                    let slot = self.nodes[child].slots[index];
                    self.insert_walk(child, slot);
                    self.nodes[child].slots[index].object = 0;
                    self.entry_count -= 1;
                }
            }
            node = child;
        }
    }

    /// `InsertSplit`: as [`Self::insert_walk`], but a full leaf is split by
    /// the centroid plane and then every edge plane.
    fn insert_split(&mut self, mut node: usize, entry: SpatialEntry) {
        loop {
            if !self.nodes[node].is_split() {
                if !self.nodes[node].full() {
                    self.nodes[node].add_entry(entry);
                    self.entry_count += 1;
                    return;
                }
                self.split_full_leaf(node);
            }
            let side = self.nodes[node]
                .plane
                .classify(entry.position, entry.radius);
            match self.nodes[node].children[side] {
                Some(child) => node = child,
                None => {
                    self.new_child(node, side, entry);
                    return;
                }
            }
        }
    }

    /// The split of a full leaf in `InsertSplit`.
    fn split_full_leaf(&mut self, node: usize) {
        let slots = self.nodes[node].slots;
        let mut plane = self.nodes[node].plane;
        plane.compute_centroid_split(&slots);
        let mut best = plane;
        let mut best_score = score_counts(plane.counts_yxz(&slots));
        if best_score != 0 {
            for first in 1..=8 {
                for second in first..8 {
                    if !plane.build_from_edge(&slots[first - 1], &slots[second]) {
                        continue;
                    }
                    let score = score_counts(plane.counts_yxz(&slots));
                    if score < best_score {
                        best = plane;
                        best_score = score;
                        if score == 0 {
                            break;
                        }
                    }
                }
            }
            plane = best;
        }
        self.nodes[node].plane = plane;
        for index in 0..8 {
            let slot = self.nodes[node].slots[index];
            let side = plane.classify(slot.position, slot.radius);
            match self.nodes[node].children[side] {
                Some(child) => self.insert_split(child, slot),
                None => self.new_child(node, side, slot),
            }
            self.nodes[node].slots[index].object = 0;
            self.entry_count -= 1;
        }
    }

    /// The best centroid-to-entry plane of `SplitEntries` and `Build`.
    /// `edge` picks `Build`'s plane construction (`FSpatialPlane::Build`)
    /// over `SplitEntries`' own.
    fn best_plane(entries: &[SpatialEntry], build: bool) -> SpatialPlane {
        let mut sum = Vector::ZERO;
        for entry in entries {
            accumulate(&mut sum, entry.position);
        }
        let centroid = divide_by_scalar(sum, entries.len() as f32);
        let mut best_score = 9999i32;
        let mut best = SpatialPlane::default();
        for entry in entries {
            let delta = sub(entry.position, centroid);
            let dist_sq = if build {
                length_squared(delta)
            } else {
                (e(delta.y) * e(delta.y) + e(delta.z) * e(delta.z)) + e(delta.x) * e(delta.x)
            };
            if dist_sq < 0.05 {
                continue;
            }
            let mut candidate = SpatialPlane::default();
            if build {
                candidate.build(centroid, delta);
            } else {
                let inv = 1.0 / length(delta);
                candidate.normal = v(
                    (e(delta.x) * inv) as f32,
                    (e(delta.y) * inv) as f32,
                    (e(delta.z) * inv) as f32,
                );
                candidate.distance = dot(candidate.normal, centroid) as f32;
            }
            let score = i32::from_ne_bytes(score_counts(candidate.counts(entries)).to_ne_bytes());
            if score < best_score {
                best_score = score;
                best = candidate;
                if score == 0 {
                    break;
                }
            }
        }
        best
    }

    /// The entries by side of `plane`: touching, beyond, behind.
    fn divide(plane: &SpatialPlane, entries: &[SpatialEntry]) -> [Vec<SpatialEntry>; 3] {
        let mut sides: [Vec<SpatialEntry>; 3] = Default::default();
        for entry in entries {
            sides[plane.classify(entry.position, entry.radius)].push(*entry);
        }
        sides
    }

    /// `AddArray`: builds an empty partition top-down; a partition that
    /// holds anything drops the entries.
    pub fn add_array(&mut self, entries: &[SpatialEntry]) {
        if self.root.is_some() {
            return;
        }
        let root = self.push_node();
        self.root = Some(root);
        if entries.len() <= 8 {
            for entry in entries {
                self.nodes[root].add_entry(*entry);
                self.entry_count += 1;
            }
            return;
        }
        let plane = Self::best_plane(entries, false);
        let [touching, beyond, behind] = Self::divide(&plane, entries);
        self.nodes[root].plane = plane;
        for entry in touching {
            self.insert_walk(root, entry);
        }
        self.build_children(root, &beyond, &behind);
    }

    fn build_children(&mut self, node: usize, beyond: &[SpatialEntry], behind: &[SpatialEntry]) {
        if !beyond.is_empty() {
            let child = self.push_node();
            self.nodes[node].children[1] = Some(child);
            self.build(child, beyond);
        }
        if !behind.is_empty() {
            let child = self.push_node();
            self.nodes[node].children[2] = Some(child);
            self.build(child, behind);
        }
    }

    /// `Build`: one node of the top-down build.
    fn build(&mut self, node: usize, entries: &[SpatialEntry]) {
        if entries.len() <= 8 {
            for entry in entries {
                self.nodes[node].add_entry(*entry);
                self.entry_count += 1;
            }
            return;
        }
        let plane = Self::best_plane(entries, true);
        let [touching, beyond, behind] = Self::divide(&plane, entries);
        self.nodes[node].plane = plane;
        for entry in touching {
            let side = plane.classify(entry.position, entry.radius);
            match self.nodes[node].children[side] {
                Some(child) => self.insert_split(child, entry),
                None => self.new_child(node, side, entry),
            }
        }
        self.build_children(node, &beyond, &behind);
    }

    /// `QueryFrustum`: the frustum's bounding sphere (from the smaller
    /// angle's tangent), then [`Self::query_sphere`].
    pub fn query_frustum(&self, f: &Frustum, result: &mut QueryResult) -> usize {
        let angle = if !(f.angle_y < f.angle_x) {
            f.angle_y
        } else {
            f.angle_x
        };
        let half = (e(angle) * 0.5).tan() * e(f.distance);
        let (radius, offset) = if !(e(f.distance) >= half) {
            (half as f32, e(f.distance))
        } else {
            let r = (half * half + e(f.distance) * e(f.distance)) / (e(f.distance) + e(f.distance));
            (r as f32, r)
        };
        let m = &f.xform.rotation.m;
        let offset_x = (e(m[0][2]) * offset) as f32;
        let offset_y = offset * e(m[1][2]);
        let offset_z = (e(m[2][2]) * offset) as f32;
        let t = f.xform.translation;
        let bounds = Sphere {
            center: v(t.x - offset_x, (e(t.y) - offset_y) as f32, t.z - offset_z),
            radius,
        };
        self.query_sphere(&bounds, result)
    }

    /// `QuerySphere`.
    pub fn query_sphere(&self, sphere: &Sphere, result: &mut QueryResult) -> usize {
        let mut node = self.root;
        while let Some(index) = node {
            let n = &self.nodes[index];
            if !n.is_split() {
                for slot in n.slots.iter().take_while(|s| s.object != 0) {
                    let d = sub(slot.position, sphere.center);
                    let reach = e(slot.radius) + e(sphere.radius);
                    let dist_sq = (e(d.x) * e(d.x) + e(d.y) * e(d.y)) + e(d.z) * e(d.z);
                    if reach * reach > dist_sq {
                        result.append(slot.object);
                    }
                }
                break;
            }
            let s = dot(sphere.center, n.plane.normal) - e(n.plane.distance);
            self.query_sphere_sides(n, s, sphere, result);
            node = n.children[0];
        }
        result.count()
    }

    fn query_sphere_sides(&self, n: &Node, s: f64, sphere: &Sphere, result: &mut QueryResult) {
        let r = e(sphere.radius);
        if s > r {
            self.query_sphere_node(n.children[1], sphere, result);
        } else if s < -r {
            self.query_sphere_node(n.children[2], sphere, result);
        } else {
            self.query_sphere_node(n.children[1], sphere, result);
            self.query_sphere_node(n.children[2], sphere, result);
        }
    }

    fn query_sphere_node(
        &self,
        mut node: Option<usize>,
        sphere: &Sphere,
        result: &mut QueryResult,
    ) {
        while let Some(index) = node {
            let n = &self.nodes[index];
            if !n.is_split() {
                for slot in n.slots.iter().take_while(|s| s.object != 0) {
                    let dx = e(slot.position.x) - e(sphere.center.x);
                    let dy = e(slot.position.y) - e(sphere.center.y);
                    let dz = e(slot.position.z) - e(sphere.center.z);
                    let reach = e(slot.radius) + e(sphere.radius);
                    let dist_sq = (dz * dz + dx * dx) + dy * dy;
                    if reach * reach > dist_sq {
                        result.append(slot.object);
                    }
                }
                return;
            }
            let c = sphere.center;
            let p = &n.plane;
            let s = ((e(c.z) * e(p.normal.z) + e(c.y) * e(p.normal.y)) + e(p.normal.x) * e(c.x))
                - e(p.distance);
            self.query_sphere_sides(n, s, sphere, result);
            node = n.children[0];
        }
    }

    /// `QueryRay`.
    pub fn query_ray(&self, ray: &SpatialRay, result: &mut QueryResult) -> usize {
        let Some(root) = self.root else {
            return result.count();
        };
        let n = &self.nodes[root];
        if !n.is_split() {
            for slot in n.slots.iter().take_while(|s| s.object != 0) {
                let delta = sub(slot.position, ray.origin);
                if ray_hits_entry(ray, slot, delta, length(delta)) {
                    result.append(slot.object);
                }
            }
            return result.count();
        }
        if n.children[0].is_some() {
            self.query_ray_node(n.children[0], ray, result);
        }
        let p = &n.plane;
        let o = ray.origin;
        let d = ray.direction;
        let s = ((e(o.z) * e(p.normal.z) + e(o.y) * e(p.normal.y)) + e(o.x) * e(p.normal.x))
            - e(p.distance);
        let dn = (e(d.z) * e(p.normal.z) + e(d.y) * e(p.normal.y)) + e(p.normal.x) * e(d.x);
        let (side, t) = ray_plane(s, dn, ray.length);
        let Some(t) = t else {
            if n.children[side].is_some() {
                self.query_ray_node(n.children[side], ray, result);
            }
            return result.count();
        };
        let hit = v(
            (e(t) * e(d.x) + e(o.x)) as f32,
            (e(t) * e(d.y) + e(o.y)) as f32,
            (e(t) * e(d.z) + e(o.z)) as f32,
        );
        if n.children[side].is_some() {
            let to_hit = sub(hit, o);
            let near = SpatialRay {
                length: ((e(to_hit.z) * e(to_hit.z) + e(to_hit.x) * e(to_hit.x))
                    + e(to_hit.y) * e(to_hit.y))
                .sqrt() as f32,
                ..*ray
            };
            self.query_ray_node(n.children[side], &near, result);
        }
        if n.children[side ^ 3].is_some() {
            let far = SpatialRay {
                origin: hit,
                ..*ray
            };
            self.query_ray_node(n.children[side ^ 3], &far, result);
        }
        result.count()
    }

    fn query_ray_node(&self, mut node: Option<usize>, ray: &SpatialRay, result: &mut QueryResult) {
        let mut ray = *ray;
        while let Some(index) = node {
            let n = &self.nodes[index];
            if !n.is_split() {
                for slot in n.slots.iter().take_while(|s| s.object != 0) {
                    let delta = sub(slot.position, ray.origin);
                    let dist = ((e(delta.y) * e(delta.y) + e(delta.z) * e(delta.z))
                        + e(delta.x) * e(delta.x))
                    .sqrt();
                    if ray_hits_entry(&ray, slot, delta, dist) {
                        result.append(slot.object);
                    }
                }
                return;
            }
            if n.children[0].is_some() {
                self.query_ray_node(n.children[0], &ray, result);
            }
            let p = &n.plane;
            let d = ray.direction;
            let s = dot(ray.origin, p.normal) - e(p.distance);
            let dn = (e(d.z) * e(p.normal.z) + e(d.y) * e(p.normal.y)) + e(d.x) * e(p.normal.x);
            let (side, t) = ray_plane(s, dn, ray.length);
            let Some(t) = t else {
                node = n.children[side];
                continue;
            };
            let hit = add(ray.origin, scale(ray.direction, t));
            if n.children[side].is_some() {
                let dx = e(hit.x) - e(ray.origin.x);
                let dy = e(hit.y) - e(ray.origin.y);
                let dz = e(hit.z) - e(ray.origin.z);
                let near = SpatialRay {
                    length: ((dz * dz + dx * dx) + dy * dy).sqrt() as f32,
                    ..ray
                };
                self.query_ray_node(n.children[side], &near, result);
            }
            if n.children[side ^ 3].is_none() {
                return;
            }
            ray = SpatialRay { origin: hit, ..ray };
            node = n.children[side ^ 3];
        }
    }
}

// --- The system zone tree --------------------------------------------------

/// The radius the smallest-zone queries start from (`kNoZoneRadius`).
const NO_ZONE_RADIUS: f32 = 1.0e20;

impl Universe {
    /// `SYSTEM::BuildZoneTree`: a BSP partition of the system's zones in
    /// their order, each `{zone, position, radius}`; the handle of a zone is
    /// its index in the system's zone list plus one. The C++ builds the tree
    /// once and keeps it; the zones never change, so building it again gives
    /// the same tree.
    #[must_use]
    pub fn zone_tree(&self, system: u32) -> (SpatialPartition, Vec<&Zone>) {
        let zones: Vec<&Zone> = self.system_zones(system).collect();
        let mut tree = SpatialPartition::Bsp(BspPartition::default());
        for (index, zone) in zones.iter().enumerate() {
            tree.add(SpatialEntry {
                object: u32::try_from(index + 1).unwrap_or(u32::MAX),
                position: zone.pos,
                radius: zone.radius(),
            });
        }
        (tree, zones)
    }

    /// The zones of the system the zone tree finds for `sphere`, in order.
    fn zones_near<'a>(tree: &(SpatialPartition, Vec<&'a Zone>), sphere: &Sphere) -> Vec<&'a Zone> {
        let mut result = QueryResult::default();
        tree.0.query_sphere(sphere, &mut result);
        result
            .items
            .iter()
            .filter_map(|&h| tree.1.get(usize::try_from(h).ok()? - 1).copied())
            .collect()
    }

    /// `GetSphereProperties`: the OR of the property flags of the zones
    /// the sphere reaches, except that of the zones with any of `0x7fc0`
    /// only the smallest counts.
    #[must_use]
    pub fn sphere_properties(&self, system: u32, sphere: &Sphere) -> u32 {
        let tree = self.zone_tree(system);
        let mut combined = 0;
        let mut exclusive: Option<(u32, f32)> = None;
        for zone in Self::zones_near(&tree, sphere) {
            if zone.sphere_cull(sphere) {
                continue;
            }
            let flags = zone.property_flags;
            if flags & 0x7fc0 == 0 {
                combined |= flags;
                continue;
            }
            match exclusive {
                Some((_, radius)) if !(zone.radius() < radius) => {}
                _ => exclusive = Some((flags, zone.radius())),
            }
        }
        combined | exclusive.map_or(0, |(flags, _)| flags)
    }

    /// `GetNestedProperties`: the OR of the property flags of every zone
    /// the sphere reaches.
    #[must_use]
    pub fn nested_properties(&self, system: u32, sphere: &Sphere) -> u32 {
        let tree = self.zone_tree(system);
        Self::zones_near(&tree, sphere)
            .iter()
            .filter(|z| !z.sphere_cull(sphere))
            .fold(0, |acc, z| acc | z.property_flags)
    }

    /// The smallest zone holding `point` among those a query sphere of
    /// `query_radius` finds and `keep` accepts; `radius_first` tests the
    /// radius before the containment, as slots 11 and 12 do.
    fn smallest_zone(
        &self,
        system: u32,
        point: Vector,
        query_radius: f32,
        radius_first: bool,
        keep: impl Fn(&Zone) -> bool,
    ) -> Option<u32> {
        let tree = self.zone_tree(system);
        let sphere = Sphere {
            center: point,
            radius: query_radius,
        };
        let mut best = None;
        let mut best_radius = NO_ZONE_RADIUS;
        for zone in Self::zones_near(&tree, &sphere) {
            if !keep(zone) {
                continue;
            }
            if radius_first {
                let radius = zone.radius();
                if !(radius < best_radius) {
                    continue;
                }
                if zone.in_zone(point, 0.0) {
                    best = Some(zone.nickname);
                    best_radius = radius;
                }
            } else {
                if !zone.in_zone(point, 0.0) {
                    continue;
                }
                let radius = zone.radius();
                if radius < best_radius {
                    best_radius = radius;
                    best = Some(zone.nickname);
                }
            }
        }
        best
    }

    /// `GetSmallestZoneAt`: the id of the smallest zone holding `point`.
    #[must_use]
    pub fn smallest_zone_at(&self, system: u32, point: Vector) -> Option<u32> {
        self.smallest_zone(system, point, 0.1, false, |_| true)
    }

    /// `GetSmallestPhysicalZoneAt`: as [`Universe::smallest_zone_at`] among
    /// zones with a physical effect.
    #[must_use]
    pub fn smallest_physical_zone_at(&self, system: u32, point: Vector) -> Option<u32> {
        self.smallest_zone(system, point, 0.1, true, Zone::has_physical_effect)
    }

    /// `GetSmallestZoneWithPropertiesAt`: among zones with any flag of
    /// `mask`, with a query sphere of radius 1.
    #[must_use]
    pub fn smallest_zone_with_properties_at(
        &self,
        system: u32,
        point: Vector,
        mask: u32,
    ) -> Option<u32> {
        self.smallest_zone(system, point, 1.0, true, |z| mask & z.property_flags != 0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(object: u32, x: f32, y: f32, z: f32, radius: f32) -> SpatialEntry {
        SpatialEntry {
            object,
            position: v(x, y, z),
            radius,
        }
    }

    #[test]
    fn bsp_and_linear_agree_on_spheres() {
        let mut bsp = new_spatial(b"bsp").expect("BSP");
        let mut linear = new_spatial(b"LINEAR").expect("Linear");
        assert!(new_spatial(b"QuadTree").is_none());
        let mut state = 7u32;
        let mut next = || {
            state = state.wrapping_mul(1_103_515_245).wrapping_add(12345);
            f32::from(u16::try_from((state >> 8) & 0xFFFF).unwrap_or(0)) - 32768.0
        };
        for object in 1..=60 {
            let e = entry(
                object,
                next(),
                next() * 0.1,
                next(),
                50.0 + next().abs() * 0.01,
            );
            bsp.add(e);
            linear.add(e);
        }
        for _ in 0..30 {
            let sphere = Sphere {
                center: v(next(), next() * 0.1, next()),
                radius: next().abs() * 0.3,
            };
            let mut a = QueryResult::default();
            let mut b = QueryResult::default();
            bsp.query_sphere(&sphere, &mut a);
            linear.query_sphere(&sphere, &mut b);
            a.items.sort_unstable();
            a.items.dedup();
            b.items.sort_unstable();
            assert_eq!(a.items, b.items);
        }
        bsp.remove(5);
        let mut all = QueryResult::default();
        bsp.query_sphere(
            &Sphere {
                center: Vector::ZERO,
                radius: 1.0e6,
            },
            &mut all,
        );
        assert!(!all.items.contains(&5));
        assert_eq!(all.items.len(), 59);
    }

    #[test]
    fn rays_find_what_they_cross() {
        let mut bsp = BspPartition::default();
        let entries: Vec<SpatialEntry> = (0..20)
            .map(|i| entry(i + 1, (i as f32) * 100.0, 0.0, 0.0, 10.0))
            .collect();
        bsp.add_array(&entries);
        let ray = SpatialRay {
            origin: v(-50.0, 0.0, 0.0),
            direction: v(1.0, 0.0, 0.0),
            length: 1000.0,
        };
        let mut result = QueryResult::default();
        bsp.query_ray(&ray, &mut result);
        // The leaves do not test the ray's length: only the plane crossings
        // are bounded by it, so entries beyond it in a reached leaf count.
        assert!((1..=11).all(|i| result.items.contains(&i)));
        let behind = SpatialRay {
            origin: v(5000.0, 0.0, 0.0),
            ..ray
        };
        let mut none = QueryResult::default();
        bsp.query_ray(&behind, &mut none);
        assert!(none.items.is_empty());
    }
}
