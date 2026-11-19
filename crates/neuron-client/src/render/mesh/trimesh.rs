//! The `TriMesh` archetype as it draws (`TriMesh.cpp`) and the `TriMesh`
//! plugin (`Plugins.cpp`): the archetype made from the `Mesh` node
//! `neuron-core::mesh_data::trimesh` loads, the material hook-up that
//! sorts the face groups into buckets by their materials' render flags,
//! the cached box, centre and bounding sphere, the counts, the clone, the
//! instances with their UV0 animation, and the draw.
//!
//! The archetype's `IMesh` and `IRenderableMesh` faces are
//! [`super::face::MeshFace`]'s; the plugin shares its archetypes and
//! instances with them ([`TriMeshFaces`]). The editing mode of that face
//! (plane splits, vertex compaction and privatised face groups) is not
//! ported (divergence D358); no face group here has vertices of its own.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::rc::Rc;

use neuron_core::math::mul_transform;
use neuron_core::mesh_data::trimesh::{
    self, TRIANGLE_STRIP, TriMesh, TriMeshStream, TriMeshUvAnim,
};
use neuron_core::num::trunc_to_i64;
use neuron_core::vfs::FileSystem;

use super::face::{FaceSource, MeshFace, TriMeshShared};
use super::manager::{DrawRequest, GeometryQuery, RenderComponent};
use super::services::{MeshCamera, MeshServices};
use super::vmesh::remap_draw_flags;
use super::{HANDLE_NONE, Handle, Instance, MeshIid, dump, file, transform};
use crate::render::material::{
    DrawArgs, MaterialId, SubmitContext, VbListing, VertexSource, VertexStream,
};

/// One face group (`FTriMeshGroup`).
#[derive(Clone, Debug, PartialEq)]
pub struct TriMeshGroup {
    /// The primitive type: 4 list, 5 strip.
    pub prim: u32,
    /// The name CRC of its material.
    pub material_crc: u32,
    /// The material the hook-up found, with its reference.
    pub material: Option<MaterialId>,
    /// `+0x0C`, the context the draw passes as `Field20` (0; only the
    /// editing face writes it).
    pub field20: usize,
    /// The smallest raw index, subtracted from the indices.
    pub min_index: u32,
    /// The largest raw index.
    pub max_index: u32,
    /// The rebased indices.
    pub indices: Vec<u16>,
    /// `Edge_indices` (position indices, two per edge).
    pub edge_indices: Option<Vec<u16>>,
    /// `Edge_angles`: every float of the leaf after a load, `EdgeCount / 2`
    /// after a clone (`SetEdges`).
    pub edge_angles: Option<Vec<f32>>,
    /// The group draws from the archetype's vertices; a group
    /// `AddFaceGroup` made has none.
    pub vertices: bool,
    /// The hook-up's sort key, 0 to 4.
    pub bucket: u32,
}

impl TriMeshGroup {
    /// `TriMeshGroupNew` with the primitive type of `AddFaceGroup`: no
    /// material, indices, edges or vertices, bucket 4.
    #[must_use]
    pub fn empty(prim: u32) -> TriMeshGroup {
        TriMeshGroup {
            prim,
            material_crc: 0,
            material: None,
            field20: 0,
            min_index: 0,
            max_index: 0,
            indices: Vec::new(),
            edge_indices: None,
            edge_angles: None,
            vertices: false,
            bucket: 4,
        }
    }

    /// `EdgeCount`: the edge indices.
    #[must_use]
    pub fn edge_count(&self) -> u32 {
        self.edge_indices
            .as_ref()
            .map_or(0, |e| u32::try_from(e.len()).unwrap_or(u32::MAX))
    }
}

/// The archetype (`FTriMeshArch`).
#[derive(Clone, Debug, PartialEq)]
pub struct TriMeshArchetype {
    /// The face groups in draw order.
    pub groups: Vec<TriMeshGroup>,
    /// The shared vertex source: indexed streams (flags 4).
    pub vertices: VertexSource,
    /// The UV0 animation.
    pub uv_anim: Option<TriMeshUvAnim>,
    /// Bit 0 loaded, 1 sphere cached, 2 box cached, 3 centre computed.
    pub flags: u32,
    centre: [f32; 3],
    bounds: [f32; 6],
    sphere_centre: [f32; 3],
    sphere_radius: f32,
}

fn floats<const N: usize>(values: &[[f32; N]]) -> Vec<u8> {
    values
        .iter()
        .flatten()
        .flat_map(|v| v.to_le_bytes())
        .collect()
}

fn stream<const N: usize>(source: &TriMeshStream<[f32; N]>, element: u32) -> VertexStream {
    VertexStream {
        data: Some(floats(&source.values)),
        byte_size: element,
        stride: element,
        index: source.indices.clone(),
        count: u32::try_from(source.values.len()).unwrap_or(u32::MAX),
    }
}

impl TriMeshArchetype {
    /// The archetype `FTriMeshArch::Load` leaves before its hook-up.
    #[must_use]
    pub fn from_mesh(mesh: &TriMesh) -> TriMeshArchetype {
        let g = &mesh.geometry;
        let mut vertices = VertexSource {
            flags: g.flags,
            fvf: g.src_fvf,
            count: g.src_count,
            ..VertexSource::default()
        };
        if g.node {
            vertices.position = stream(&g.positions, 0xC);
            if let Some(normals) = &g.normals {
                vertices.normal = stream(normals, 0xC);
            }
            if let Some(tex0) = &g.tex0 {
                vertices.tex[0] = stream(tex0, 8);
            }
            if let Some(tex1) = &g.tex1 {
                vertices.tex[1] = stream(tex1, 8);
            }
            if let Some(diffuse) = &g.diffuse {
                vertices.diffuse = VertexStream {
                    data: Some(
                        diffuse
                            .values
                            .iter()
                            .flat_map(|v| v.to_le_bytes())
                            .collect(),
                    ),
                    byte_size: 4,
                    stride: 4,
                    index: diffuse.indices.clone(),
                    count: u32::try_from(diffuse.values.len()).unwrap_or(u32::MAX),
                };
            }
        }
        let groups = mesh
            .groups
            .iter()
            .map(|group| TriMeshGroup {
                prim: group.primitive_type,
                material_crc: group.material_crc,
                material: None,
                field20: 0,
                min_index: group.min_index,
                max_index: group.max_index,
                indices: group.indices.clone().unwrap_or_default(),
                edge_indices: group.edge_indices.clone(),
                edge_angles: group.edge_angles.clone(),
                vertices: true,
                bucket: group.bucket,
            })
            .collect();
        TriMeshArchetype {
            groups,
            vertices,
            uv_anim: g.uv_anim.clone(),
            flags: 1,
            centre: [0.0; 3],
            bounds: [0.0; 6],
            sphere_centre: [0.0; 3],
            sphere_radius: -1.0,
        }
    }

    /// `FHookFacet::HookUpMaterials`: each face group's material by its
    /// CRC, its bucket from the material's render flags (2 for bit 0, 3
    /// without bit 3, 1 with bits 1 or 4, else 0; 4 without a material),
    /// and the groups reordered by bucket. 0, or -1 when a material was
    /// not found.
    pub fn hook_up<S: MeshServices + ?Sized>(&mut self, services: &mut S) -> i32 {
        let mut failed = false;
        let mut buckets: [Vec<TriMeshGroup>; 5] = Default::default();
        for mut group in std::mem::take(&mut self.groups) {
            let previous = group.material;
            if let Some(previous) = previous {
                services.add_ref_material(previous);
            }
            let mut bound = false;
            if group.material_crc != 0 {
                if let Some(old) = group.material.take() {
                    services.release_material(old);
                }
                let (result, material) = services.find_material(group.material_crc);
                group.material = material;
                bound = result == 0;
            }
            if !bound {
                if let Some(old) = group.material.take() {
                    services.release_material(old);
                }
                failed = true;
            }
            group.bucket = 4;
            if let Some(material) = group.material {
                let low = services.material_flags(material) & 0xFF;
                group.bucket = if low & 1 != 0 {
                    2
                } else if low & 8 == 0 {
                    3
                } else if low & 0x12 != 0 {
                    1
                } else {
                    0
                };
            }
            let bucket = group.bucket as usize;
            buckets[bucket].push(group);
            if let Some(previous) = previous {
                services.release_material(previous);
            }
        }
        self.groups = buckets.into_iter().flatten().collect();
        if failed { -1 } else { 0 }
    }

    /// The positions as floats, `Position.Count` of them.
    fn points(&self) -> Vec<[f32; 3]> {
        let stream = &self.vertices.position;
        let data = stream.data.as_deref().unwrap_or_default();
        (0..stream.count as usize)
            .map(|i| {
                let at = |k: usize| {
                    data.get(i * 12 + k * 4..i * 12 + k * 4 + 4)
                        .map_or(0.0, |b| f32::from_le_bytes([b[0], b[1], b[2], b[3]]))
                };
                [at(0), at(1), at(2)]
            })
            .collect()
    }

    /// `GetBox`: max x, min x, max y, min y, max z, min z of the
    /// positions, cached once there is one. `None` before a load.
    pub fn bounds(&mut self) -> Option<[f32; 6]> {
        if self.flags & 1 == 0 {
            return None;
        }
        if self.flags & 4 == 0 {
            self.bounds = [0.0; 6];
            let points = self.points();
            if let Some(first) = points.first() {
                let b = &mut self.bounds;
                *b = [first[0], first[0], first[1], first[1], first[2], first[2]];
                for p in &points[1..] {
                    for axis in 0..3 {
                        if p[axis] > b[axis * 2] {
                            b[axis * 2] = p[axis];
                        } else if p[axis] < b[axis * 2 + 1] {
                            b[axis * 2 + 1] = p[axis];
                        }
                    }
                }
                self.flags |= 4;
            }
        }
        Some(self.bounds)
    }

    /// `GetCenter`. The reference tests the sphere's cache bit (0x2), not
    /// its own (0x8), and adds the first position `Count` times; both kept.
    pub fn centre(&mut self) -> Option<[f32; 3]> {
        if self.flags & 1 == 0 {
            return None;
        }
        if self.flags & 2 != 0 {
            return Some(self.centre);
        }
        self.centre = [0.0; 3];
        let points = self.points();
        if let Some(first) = points.first() {
            let c = &mut self.centre;
            for _ in 0..points.len() {
                c[0] += first[0];
                c[1] += first[1];
                c[2] += first[2];
            }
            let scale = 1.0 / points.len() as f64;
            for value in c.iter_mut() {
                *value = (scale * f64::from(*value)) as f32;
            }
            self.flags |= 8;
        }
        Some(self.centre)
    }

    /// `GetSphere`: the centre and radius of the extreme points' largest
    /// extent, grown point by point (Ritter's method in the reference's
    /// order), cached.
    pub fn sphere(&mut self) -> Option<([f32; 3], f32)> {
        if self.flags & 1 == 0 {
            return None;
        }
        if self.flags & 2 != 0 {
            return Some((self.sphere_centre, self.sphere_radius));
        }
        self.sphere_centre = [0.0; 3];
        self.sphere_radius = 0.0;
        let points = self.points();
        if !points.is_empty() {
            let (mut min, mut max) = ([0usize; 3], [0usize; 3]);
            for (index, p) in points.iter().enumerate().skip(1) {
                for axis in 0..3 {
                    if p[axis] < points[min[axis]][axis] {
                        min[axis] = index;
                    } else if p[axis] > points[max[axis]][axis] {
                        max[axis] = index;
                    }
                }
            }
            let extent = |axis: usize| points[max[axis]][axis] - points[min[axis]][axis];
            let (ex, ey, ez) = (extent(0), extent(1), extent(2));
            let sx = points[min[0]][0] + points[max[0]][0];
            let sy = points[max[1]][1] + points[min[1]][1];
            let sz = points[max[2]][2] + points[min[2]][2];
            let c = &mut self.sphere_centre;
            *c = [0.5 * sx, 0.5 * sy, 0.5 * sz];
            let larger_xy = if ex > ey { ex } else { ey };
            let largest = if larger_xy > ez { larger_xy } else { ez };
            let radius = f64::from(largest) * 0.5;
            self.sphere_radius = radius as f32;
            let mut radius_squared = (radius * radius) as f32;
            for p in &points {
                let d = [p[0] - c[0], p[1] - c[1], p[2] - c[2]];
                let distance_squared = (f64::from(d[1]) * f64::from(d[1])
                    + f64::from(d[2]) * f64::from(d[2]))
                    + f64::from(d[0]) * f64::from(d[0]);
                if distance_squared.partial_cmp(&f64::from(radius_squared))
                    != Some(std::cmp::Ordering::Greater)
                {
                    continue;
                }
                let distance = distance_squared.sqrt();
                let distance_float = distance as f32;
                let new_radius = (distance + f64::from(self.sphere_radius)) * 0.5;
                self.sphere_radius = new_radius as f32;
                radius_squared = (new_radius * new_radius) as f32;
                let toward = (f64::from(distance_float) - new_radius) as f32;
                let inverse = 1.0 / f64::from(distance_float);
                for axis in 0..3 {
                    let part = toward * p[axis];
                    let keep = self.sphere_radius * c[axis];
                    let sum = part + keep;
                    c[axis] = (f64::from(sum) * inverse) as f32;
                }
            }
            self.flags |= 2;
        }
        Some((self.sphere_centre, self.sphere_radius))
    }

    /// `TriangleCount`: `IndexCount - 2` for a strip, `IndexCount / 3` for
    /// a list, over every group (unsigned, wrapping).
    #[must_use]
    pub fn triangle_count(&self) -> i32 {
        let total = self.groups.iter().fold(0u32, |total, g| {
            let count = u32::try_from(g.indices.len()).unwrap_or(u32::MAX);
            total.wrapping_add(if g.prim == TRIANGLE_STRIP {
                count.wrapping_sub(2)
            } else {
                count / 3
            })
        });
        i32::from_ne_bytes(total.to_ne_bytes())
    }

    /// `VertexCount`: the shared source's count once (no group has its own).
    #[must_use]
    pub fn vertex_count(&self) -> i32 {
        if self.groups.is_empty() {
            0
        } else {
            i32::from_ne_bytes(self.vertices.count.to_ne_bytes())
        }
    }

    /// `FTriMeshArch::Clone`: the vertex source with `SrcCount` indices per
    /// stream, the groups with a reference on each material, the flags and
    /// the cached bounds; the UV0 animation is not copied.
    pub fn clone_with<S: MeshServices + ?Sized>(&self, services: &mut S) -> TriMeshArchetype {
        let count = self.vertices.count as usize;
        let mut copy = self.clone();
        let v = &mut copy.vertices;
        let [tex0, tex1] = &mut v.tex;
        for s in [&mut v.position, &mut v.normal, &mut v.diffuse, tex0, tex1] {
            if let Some(index) = s.index.as_mut() {
                index.resize(count, 0);
            }
        }
        copy.uv_anim = None;
        for group in &mut copy.groups {
            // `CopyFrom`'s `SetEdges(EdgeCount, EdgeIndices, EdgeAngles)`.
            let count = group.edge_count() as usize;
            if count == 0 {
                group.edge_indices = None;
                group.edge_angles = None;
            } else if let Some(angles) = group.edge_angles.as_mut() {
                angles.resize(count / 2, 0.0);
            }
        }
        for group in &copy.groups {
            if let Some(material) = group.material {
                services.add_ref_material(material);
            }
        }
        copy
    }

    /// `FTriMeshArch::Reset` after its `CloseFaceGroup`: the bounds
    /// cleared, every group's material released and the groups dropped,
    /// the vertices and the UV animation dropped, the flags cleared.
    pub fn reset<S: MeshServices + ?Sized>(&mut self, services: &mut S) {
        self.centre = [0.0; 3];
        self.bounds = [0.0; 6];
        self.sphere_centre = [0.0; 3];
        self.sphere_radius = -1.0;
        self.destroy(services);
        self.groups.clear();
        self.vertices = VertexSource::default();
        self.uv_anim = None;
        self.flags = 0;
    }

    /// The destructor: every group's material released.
    fn destroy<S: MeshServices + ?Sized>(&mut self, services: &mut S) {
        for group in &mut self.groups {
            if let Some(material) = group.material.take() {
                services.release_material(material);
            }
        }
    }
}

/// A `TriMesh` instance (`FTriMeshAnim`).
#[derive(Clone, Debug, PartialEq)]
pub struct TriMeshInstance {
    /// The archetype it was made on (`Mesh`).
    pub mesh: Handle,
    /// The archetype it draws (`MeshLive`).
    pub mesh_live: Handle,
    /// Its own copy of the texture set 0 stream when the archetype animates
    /// it.
    pub uv_track: Option<VertexStream>,
    /// The animation clock.
    pub clock: f32,
}

/// `TriMeshStreamCopy(source, dest, index_count)`: `Count * Stride` bytes
/// and `index_count` indices (zeros past the source's; divergence D354).
fn copy_stream(source: &VertexStream, index_count: u32) -> VertexStream {
    let bytes = source.count.wrapping_mul(source.stride) as usize;
    VertexStream {
        data: source.data.as_ref().map(|d| {
            let mut out = d[..d.len().min(bytes)].to_vec();
            out.resize(bytes, 0);
            out
        }),
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

/// `TriMeshFtol`: truncation to 64 bits, the low 32 kept.
fn ftol(value: f64) -> u32 {
    trunc_to_i64(value) as u32
}

impl TriMeshInstance {
    /// `FTriMeshAnim::Init`: a track copied from the archetype's texture
    /// set 0 when its UV0 animation has entries.
    fn new(handle: Handle, archetype: &TriMeshArchetype) -> TriMeshInstance {
        let animated = archetype
            .uv_anim
            .as_ref()
            .is_some_and(|a| !a.lookup.is_empty());
        TriMeshInstance {
            mesh: handle,
            mesh_live: handle,
            uv_track: animated
                .then(|| copy_stream(&archetype.vertices.tex[0], archetype.vertices.count)),
            clock: 0.0,
        }
    }

    /// `AdvanceUvAnimation(dt)` with the drawn archetype's animation: the
    /// clock advances (wrapping at the period), the frame and the fraction
    /// follow from it, and each looked-up slot of the track gets the
    /// frame's coordinates, interpolated towards the next frame or (without
    /// interpolation) copied when the frame changed.
    fn advance(&mut self, uv: &TriMeshUvAnim, dt: f32) {
        let Some(track) = self.uv_track.as_mut() else {
            return;
        };
        if uv.lookup.is_empty() {
            return;
        }
        let sum = f64::from(dt) + f64::from(self.clock);
        self.clock = sum as f32;
        if sum >= f64::from(uv.period) {
            self.clock = (sum - f64::from(uv.period)) as f32;
        }
        let position = f64::from(self.clock) * f64::from(uv.fps);
        let frames = uv.frame_count.wrapping_sub(1);
        if frames == 0 {
            return;
        }
        let frame = ftol(position) % frames;
        let t = (position - f64::from(i32::from_ne_bytes(frame.to_ne_bytes()))) as f32;
        let count = uv.lookup.len();
        let data = uv.frames.as_deref().unwrap_or_default();
        let pair = |entry: usize| data.get(entry).copied().unwrap_or([0.0; 2]);
        let dest = track.data.get_or_insert_with(Vec::new);
        let mut store = |slot: i32, u: f32, v: f32| {
            let Ok(slot) = usize::try_from(slot) else {
                return;
            };
            if let Some(out) = dest.get_mut(slot * 8..slot * 8 + 8) {
                out[..4].copy_from_slice(&u.to_le_bytes());
                out[4..].copy_from_slice(&v.to_le_bytes());
            }
        };
        let frame = frame as usize;
        if uv.interpolate != 0 {
            for (index, &slot) in uv.lookup.iter().enumerate() {
                if slot == -1 {
                    continue;
                }
                let a = pair(count * frame + index);
                let b = pair(count * frame + count + index);
                let u =
                    ((f64::from(b[0]) - f64::from(a[0])) * f64::from(t) + f64::from(a[0])) as f32;
                let v =
                    ((f64::from(b[1]) - f64::from(a[1])) * f64::from(t) + f64::from(a[1])) as f32;
                store(slot, u, v);
            }
            return;
        }
        let previous = ftol((f64::from(self.clock) - f64::from(dt)) * f64::from(uv.fps));
        if frame as u32 == previous {
            return;
        }
        for (index, &slot) in uv.lookup.iter().enumerate() {
            if slot == -1 {
                continue;
            }
            let a = pair(count * frame + index);
            store(slot, a[0], a[1]);
        }
    }
}

/// `FTriMeshAnim::Draw`: the transforms (`PipelineApplySubmit`), the
/// track swapped in for texture set 0, the vertices copied into vertex
/// memory, then every group with indices and a material bound, submitted
/// and torn down; a group of bucket 2 or more drops the vertex listing for
/// the groups after it.
fn draw<S: MeshServices + ?Sized>(
    shared: &TriMeshShared,
    track: &mut Option<VertexStream>,
    services: &mut S,
    camera: &mut dyn MeshCamera,
    id: Instance,
    request: &DrawRequest,
) -> i32 {
    let view_words = camera.view_transform();
    let world_words = services.instance_transform(id);
    let (view, world) = (transform(view_words), transform(world_words));
    let state = services.pipeline_state(7).unwrap_or(1);
    if state != 0 {
        let modelview = match &request.matrix {
            Some(extra) => mul_transform(&view, &mul_transform(extra, &world)),
            None => mul_transform(&view, &world),
        };
        services.set_modelview(&modelview.to_words());
    } else {
        if let Some(extra) = &request.matrix {
            let _ = mul_transform(extra, &world);
        }
        services.set_world(&world_words);
    }
    let mut flags = request.flags | 0xC;
    if let Some(track) = track.as_mut() {
        std::mem::swap(&mut shared.arch.borrow_mut().vertices.tex[0], track);
        flags &= !0x4;
    }
    let archetype = shared.arch.borrow();
    let mut listing = VbListing::default();
    let source = &archetype.vertices;
    if source.flags != 0
        && flags & 0x80 == 0
        && let Ok(mut lock) = services.lock_vertex_memory(source.fvf, source.count)
    {
        let fvf = lock.listing.fvf;
        let mut bytes = std::mem::take(&mut lock.bytes);
        services.copy_vertex_data(&mut bytes, fvf, source, 0, source.count);
        lock.bytes = bytes;
        listing = lock.listing;
        services.unlock_vertex_memory(lock);
    }
    let mut vb = Some(listing);
    for group in &archetype.groups {
        let (material, replaced) = match request.extra {
            Some(extra) => (Some(extra), group.material),
            None => (group.material, None),
        };
        let Some(material) = material.filter(|_| !group.indices.is_empty()) else {
            continue;
        };
        let context = SubmitContext {
            instance: id,
            world: &world_words,
            view: &view_words,
            vertices: Some(source),
            indices: &group.indices,
            vb: vb.as_ref(),
            ib: None,
            replaced,
            field20: group.field20,
        };
        services.bind(material, &context);
        let args = DrawArgs {
            prim_type: group.prim,
            start: group.min_index,
            count: group
                .max_index
                .wrapping_sub(group.min_index)
                .wrapping_add(1),
            ib_start: 0,
            index_span: u32::try_from(group.indices.len()).unwrap_or(u32::MAX),
            flags,
        };
        services.submit(material, &context, &args);
        services.teardown(material, &context);
        if group.bucket >= 2 {
            vb = None;
        }
    }
    drop(archetype);
    if let Some(track) = track.as_mut() {
        std::mem::swap(&mut shared.arch.borrow_mut().vertices.tex[0], track);
    }
    1
}

/// The archetypes of a `TriMesh` plugin, shared with the faces it hands
/// out.
pub type SharedArchetypes = Rc<RefCell<Vec<Option<Rc<TriMeshShared>>>>>;

/// The instances of a `TriMesh` plugin, shared with the faces.
pub type SharedInstances = Rc<RefCell<BTreeMap<Instance, TriMeshInstance>>>;

/// The `TriMesh` render plugin (`FTriMeshPlugin`).
#[derive(Debug, Default)]
pub struct TriMeshPlugin {
    tristrips: bool,
    archetypes: SharedArchetypes,
    instances: SharedInstances,
}

impl TriMeshPlugin {
    /// A plugin with the `[TriMesh] Tristrips` setting (the plugin's
    /// default is on; the client's `NeuronComponents.ini` turns it off).
    #[must_use]
    pub fn new(tristrips: bool) -> TriMeshPlugin {
        TriMeshPlugin {
            tristrips,
            ..TriMeshPlugin::default()
        }
    }

    /// An archetype by handle (1 and up).
    #[must_use]
    pub fn archetype(&self, handle: Handle) -> Option<Rc<TriMeshShared>> {
        archetype_of(&self.archetypes, handle)
    }

    /// An instance.
    #[must_use]
    pub fn instance(&self, instance: Instance) -> Option<TriMeshInstance> {
        self.instances.borrow().get(&instance).cloned()
    }

    /// The faces of the plugin's archetypes and instances, for the render
    /// manager's directory.
    #[must_use]
    pub fn faces(&self) -> TriMeshFaces {
        TriMeshFaces {
            archetypes: Rc::clone(&self.archetypes),
            instances: Rc::clone(&self.instances),
        }
    }

    fn insert(&mut self, archetype: TriMeshArchetype, cursor: usize) -> Handle {
        let mut archetypes = self.archetypes.borrow_mut();
        archetypes.push(Some(TriMeshShared::new(archetype, cursor)));
        u32::try_from(archetypes.len()).unwrap_or(HANDLE_NONE)
    }

    fn tick<S: ?Sized>(&mut self, _services: &mut S, instance: Instance, dt: f32) {
        let mut instances = self.instances.borrow_mut();
        let Some(anim) = instances.get_mut(&instance) else {
            return;
        };
        let uv = archetype_of(&self.archetypes, anim.mesh_live)
            .and_then(|a| a.arch.borrow().uv_anim.clone());
        if let Some(uv) = uv {
            anim.advance(&uv, dt);
        }
    }
}

/// An archetype of a shared table by handle (1 and up).
fn archetype_of(archetypes: &SharedArchetypes, handle: Handle) -> Option<Rc<TriMeshShared>> {
    let index = usize::try_from(handle).ok()?.checked_sub(1)?;
    archetypes.borrow().get(index).and_then(Clone::clone)
}

fn valid(handle: Handle) -> bool {
    handle != HANDLE_NONE && handle != 0
}

/// The `IMesh` faces of a `TriMesh` plugin: an archetype's
/// (`QueryHandle`) and an instance's (`QueryMeshInstance`, the instance's
/// live archetype behind the instance's own face).
#[derive(Clone, Debug)]
pub struct TriMeshFaces {
    archetypes: SharedArchetypes,
    instances: SharedInstances,
}

impl FaceSource for TriMeshFaces {
    fn face(&self, handle: Handle) -> Option<MeshFace> {
        if !valid(handle) {
            return None;
        }
        Some(MeshFace::TriMesh {
            mesh: archetype_of(&self.archetypes, handle)?,
            instance: false,
        })
    }

    fn instance_face(&self, instance: Instance, _handle: Handle) -> Option<MeshFace> {
        let live = self.instances.borrow().get(&instance)?.mesh_live;
        Some(MeshFace::TriMesh {
            mesh: archetype_of(&self.archetypes, live)?,
            instance: true,
        })
    }
}

impl<S: MeshServices + ?Sized> RenderComponent<S> for TriMeshPlugin {
    fn claim(&mut self, services: &mut S, fs: &mut dyn FileSystem) -> Handle {
        let Ok(Some(mesh)) = trimesh::load(fs, self.tristrips) else {
            return HANDLE_NONE;
        };
        let mut archetype = TriMeshArchetype::from_mesh(&mesh);
        for group in &mesh.groups {
            if group.material_name.is_none() {
                services.dump(
                    dump::WARNING,
                    file::TRIMESH_ARCHETYPE,
                    0x61A,
                    "No Material_name found",
                );
            }
        }
        if mesh.face_groups_node {
            archetype.hook_up(services);
        }
        let cursor = archetype.groups.len();
        self.insert(archetype, cursor)
    }

    fn clone_handle(&mut self, services: &mut S, handle: Handle) -> Handle {
        if !valid(handle) {
            return HANDLE_NONE;
        }
        let Some(copy) = self
            .archetype(handle)
            .map(|a| a.arch.borrow().clone_with(services))
        else {
            return HANDLE_NONE;
        };
        let cursor = copy.groups.len();
        self.insert(copy, cursor)
    }

    fn destroy_handle(&mut self, services: &mut S, handle: Handle) {
        if !valid(handle) {
            return;
        }
        let Some(index) = usize::try_from(handle).ok().and_then(|h| h.checked_sub(1)) else {
            return;
        };
        let taken = self
            .archetypes
            .borrow_mut()
            .get_mut(index)
            .and_then(Option::take);
        if let Some(archetype) = taken {
            archetype.arch.borrow_mut().destroy(services);
        }
    }

    fn query_handle(&mut self, handle: Handle, iid: MeshIid) -> i32 {
        if !valid(handle) || self.archetype(handle).is_none() {
            return -1;
        }
        match iid {
            MeshIid::Mesh
            | MeshIid::NeuronComponent
            | MeshIid::MatHookUp
            | MeshIid::RenderableMesh => 0,
            _ => -1,
        }
    }

    fn create_mesh_instance(
        &mut self,
        services: &mut S,
        instance: Instance,
        handle: Handle,
    ) -> i32 {
        if !valid(handle) {
            return 0;
        }
        if self.instances.borrow().contains_key(&instance) {
            return 1;
        }
        let Some(archetype) = self.archetype(handle) else {
            return 0;
        };
        let mut archetype = archetype.arch.borrow_mut();
        let anim = TriMeshInstance::new(handle, &archetype);
        let (centre, radius) = archetype.sphere().unwrap_or(([0.0; 3], 0.0));
        drop(archetype);
        self.instances.borrow_mut().insert(instance, anim);
        services.set_instance_extent(instance, radius, centre);
        1
    }

    fn destroy_mesh_instance(
        &mut self,
        _services: &mut S,
        instance: Instance,
        _handle: Handle,
    ) -> i32 {
        self.instances.borrow_mut().remove(&instance);
        0
    }

    fn update_mesh_instance(&mut self, services: &mut S, instance: Instance, arg: u32) -> i32 {
        self.tick(services, instance, f32::from_bits(arg));
        0
    }

    fn query_mesh_instance(&mut self, instance: Instance, _handle: Handle, iid: MeshIid) -> i32 {
        if !self.instances.borrow().contains_key(&instance) {
            return -1;
        }
        match iid {
            MeshIid::Mesh
            | MeshIid::MatHookUp
            | MeshIid::NeuronComponent
            | MeshIid::TriMeshAnim => 0,
            _ => -1,
        }
    }

    fn update_mesh_instances(&mut self, services: &mut S, arg: u32) -> i32 {
        let keys: Vec<Instance> = self.instances.borrow().keys().copied().collect();
        for instance in keys {
            self.tick(services, instance, f32::from_bits(arg));
        }
        0
    }

    fn query_geometry(
        &mut self,
        handle: Handle,
        _lod_bits: u32,
        query: GeometryQuery,
    ) -> (i32, Option<i32>) {
        if !valid(handle) || !query.can_write() {
            return (0, None);
        }
        match self.archetype(handle) {
            Some(a) if query.mode == 3 => (1, Some(a.arch.borrow().vertex_count())),
            Some(a) => (1, Some(a.arch.borrow().triangle_count())),
            None => (0, None),
        }
    }

    fn bounds(&mut self, handle: Handle, _lod: f32) -> (i32, Option<[f32; 6]>) {
        if !valid(handle) {
            return (0, None);
        }
        match self.archetype(handle) {
            Some(a) => (1, a.arch.borrow_mut().bounds()),
            None => (0, None),
        }
    }

    fn centroid(&mut self, handle: Handle, _lod: f32) -> (i32, Option<[f32; 3]>) {
        if !valid(handle) {
            return (0, None);
        }
        match self.archetype(handle) {
            Some(a) => (1, a.arch.borrow_mut().centre()),
            None => (0, None),
        }
    }

    fn render_instance(
        &mut self,
        services: &mut S,
        camera: &mut dyn MeshCamera,
        _handle: Handle,
        instance: Instance,
        request: &DrawRequest,
    ) -> i32 {
        // The instance's track is taken out while it draws, and neither
        // table stays borrowed across the materials' calls.
        let (live, mut track) = {
            let mut instances = self.instances.borrow_mut();
            let Some(anim) = instances.get_mut(&instance) else {
                return 0;
            };
            (anim.mesh_live, anim.uv_track.take())
        };
        let result = match self.archetype(live) {
            Some(archetype) => {
                let request = DrawRequest {
                    flags: remap_draw_flags(request.flags),
                    ..*request
                };
                draw(&archetype, &mut track, services, camera, instance, &request)
            }
            None => 0,
        };
        if let Some(anim) = self.instances.borrow_mut().get_mut(&instance) {
            anim.uv_track = track;
        }
        result
    }

    fn bind_mesh_instance(&mut self, instance: Instance, handle: Handle) -> i32 {
        if !valid(handle) {
            return -1;
        }
        let mut instances = self.instances.borrow_mut();
        let Some(anim) = instances.get_mut(&instance) else {
            return -1;
        };
        if anim.mesh_live != anim.mesh {
            return -1;
        }
        anim.mesh = handle;
        anim.mesh_live = handle;
        0
    }

    fn destroy(&mut self, services: &mut S) {
        for &instance in self.instances.borrow().keys() {
            let message = format!(
                "TriMesh: dtor: instance {} dangling\n",
                i32::from_ne_bytes((instance as u32).to_ne_bytes())
            );
            services.dump(dump::NOTICE, file::TRIMESH, 0x86, &message);
        }
    }
}
