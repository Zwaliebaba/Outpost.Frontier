//! The deformable meshes: `FMeshArchetype` (a `.dfm` mesh: face groups,
//! interleaved points and normals, the skin, the per-bone vertex lists,
//! the vertex remap, the dirty bytes, the UV-deform rows and the bound
//! sphere cache), `FVmeshWrapper` (a mesh node the engine made as a VMesh
//! archetype, drawn through its `IVMeshPart` with palette skinning) and
//! the module's mesh cache by name CRC (`G_MeshCache`).
//!
//! The tables come from `neuron_core::mesh_data::dfm`; this module adds
//! the material lookups and what the reference decides when it draws:
//! `Skin` (CPU skinning into the part's vertex buffer), `FillIndexBuffer`,
//! `Draw` (the face groups through their materials, the eye groups with
//! their UV-deform binding) and the bound sphere.

use std::collections::BTreeMap;

use neuron_core::hash::name_crc32;
use neuron_core::mesh_data::dfm::{DfmMesh, DfmMeshError, SkinPair, SkinVertex, UvDeformRecord};

use crate::render::material::{
    DrawArgs, IbScratch, MaterialId, SubmitContext, VbListing, VertexSource, VertexStream,
};
use crate::render::mesh::MeshCamera;

use super::math::{
    IDENTITY12, accumulate_bone_matrix, inverse_root, mul_transform12, renormalise,
    scale_bone_matrix, transform_direction, transform_point,
};
use super::services::DeformServices;
use super::skeleton::LodHolder;
use super::{EngineHandle, INSTANCE_NONE, UvBinding, dump, file};

#[allow(clippy::cast_possible_truncation)]
fn f(x: f64) -> f32 {
    x as f32
}

/// The vertex format of a deformable mesh after loading (`G_ParseTagUtf`):
/// position, normal, one UV pair.
pub const FVF: u32 = 0x112;

/// `ComputeFvfStride`.
#[must_use]
pub fn fvf_stride(fvf: u32) -> u32 {
    let position = if fvf & 0xC == 0xC {
        0x1C
    } else if fvf & 4 == 4 {
        0x10
    } else if fvf & 2 == 2 {
        0xC
    } else {
        0
    };
    let normal = if fvf & 0x10 != 0 { 0xC } else { 0 };
    let mut colour = 0;
    if fvf & 0x40 != 0 {
        colour += 4;
    }
    if fvf & 0x80 != 0 {
        colour += 4;
    }
    let mut tex = 0;
    let mut bit = 0x10u32;
    for _ in 0..((fvf >> 8) & 0xF) {
        tex += match (fvf >> (bit & 31)) & 3 {
            0 => 8,
            1 => 0xC,
            2 => 0x10,
            _ => 4,
        };
        bit += 2;
    }
    position + normal + colour + tex
}

/// One face group (`FFaceGroup`) as the mesh keeps it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FaceGroup {
    /// The primitive kind (4).
    pub kind: u32,
    /// The material name's CRC.
    pub material_crc: u32,
    /// The material `FindMaterialByCrc` gave, with its reference.
    pub material: Option<MaterialId>,
    /// The smallest index before the rebase.
    pub index_min: u32,
    /// The largest.
    pub index_max: u32,
    /// The rebased indices; none when the material was not found.
    pub indices: Vec<u16>,
    /// The UV-deform row the group binds (`UvBoneIndex`): 0 for an
    /// `EyeMaterial`, `u32::MAX` (-1) for none.
    pub uv_bone_index: u32,
}

/// `FMeshArchetype`.
#[derive(Clone, Debug, PartialEq)]
pub struct MeshArchetype {
    /// The vertex format (`ParseTag`).
    pub fvf: u32,
    /// `Point_indices`: the point of each corner.
    pub point_indices: Vec<u32>,
    /// The face groups.
    pub face_groups: Vec<FaceGroup>,
    /// Points and normals, interleaved.
    pub vertices: Vec<[f32; 6]>,
    /// `UV0_indices`.
    pub uv_indices: Option<Vec<u32>>,
    /// `UV0`.
    pub uvs: Vec<[f32; 2]>,
    /// The skin pairs.
    pub skin_pairs: Vec<SkinPair>,
    /// One run of pairs per point.
    pub skin: Vec<SkinVertex>,
    /// The points each bound bone moves (`BoneVerts`).
    pub bone_verts: Vec<Vec<u16>>,
    /// The corners of each point (`RemapStart`).
    pub remap: Vec<Vec<u32>>,
    /// One byte per point: to be skinned (`FaceFlags`).
    pub dirty: Vec<u8>,
    /// The points the last skin wrote (+0x40).
    pub skinned_vertex_count: u32,
    /// The bones the last skin composed (+0x44).
    pub skinned_bone_count: u32,
    /// The eye texture transforms of the last draw (+0x48).
    pub eye_uv_transform_count: u32,
    /// The UV-deform rows.
    pub uv_deform: Vec<UvDeformRecord>,
    /// Bones compose their world matrix (`Flag70`, the part's level below
    /// its limit); identity matrices and no skinning otherwise.
    pub flag70: bool,
    sphere_ready: bool,
    sphere_center: [f32; 3],
    sphere_radius: f32,
}

/// Why [`MeshArchetype::from_tables`] refuses a mesh.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MeshTableError {
    /// A skin pair names a bone at or past the bound bone count, or runs
    /// past the pairs (the reference writes outside its lists; divergence
    /// D394).
    SkinOutOfRange,
}

impl MeshArchetype {
    /// The mesh `LoadMeshTables` leaves: the tables, the materials found
    /// for the face groups (`None` for a group without a material name or
    /// whose lookup failed; such a group keeps no indices) with their
    /// class names, and the bone lists for `bone_count` bound bones.
    ///
    /// # Errors
    ///
    /// [`MeshTableError`].
    pub fn from_tables(
        tables: &DfmMesh,
        materials: &[(Option<MaterialId>, Vec<u8>)],
        bone_count: usize,
    ) -> Result<Self, MeshTableError> {
        let face_groups = tables
            .face_groups
            .iter()
            .enumerate()
            .map(|(index, group)| {
                let (material, class) = materials
                    .get(index)
                    .map_or((None, &[][..]), |(m, c)| (*m, c.as_slice()));
                let found = material.is_some();
                let eye = found && class.len() >= 11 && class[..11] == *b"EyeMaterial" && {
                    class.len() == 11 || class[11] == 0
                };
                FaceGroup {
                    kind: group.kind,
                    material_crc: group.material_crc,
                    material,
                    index_min: if found { group.index_min } else { 0 },
                    index_max: if found { group.index_max } else { 0 },
                    indices: if found {
                        group.indices.clone()
                    } else {
                        Vec::new()
                    },
                    uv_bone_index: if eye { 0 } else { u32::MAX },
                }
            })
            .collect();
        let bone_verts = tables
            .bone_vertex_lists(bone_count)
            .map_err(|_: DfmMeshError| MeshTableError::SkinOutOfRange)?;
        Ok(Self {
            fvf: FVF,
            point_indices: tables.point_indices.clone(),
            face_groups,
            vertices: tables.vertices.clone(),
            uv_indices: tables.uv_indices.clone(),
            uvs: tables.uvs.clone(),
            skin_pairs: tables.skin_pairs.clone(),
            skin: tables.skin.clone(),
            bone_verts,
            remap: tables.vertex_remap(),
            dirty: vec![0; tables.vertices.len()],
            skinned_vertex_count: 0,
            skinned_bone_count: 0,
            eye_uv_transform_count: 0,
            uv_deform: tables.uv_deform.clone(),
            flag70: false,
            sphere_ready: false,
            sphere_center: [0.0; 3],
            sphere_radius: 0.0,
        })
    }

    /// Slot 0: the face groups' index counts, summed.
    #[must_use]
    pub fn index_count(&self) -> u32 {
        self.face_groups.iter().fold(0u32, |sum, g| {
            sum.wrapping_add(u32::try_from(g.indices.len()).unwrap_or(u32::MAX))
        })
    }

    /// Slot 1: the corner count (plus a field the reference never writes).
    #[must_use]
    pub fn vertex_capacity(&self) -> u32 {
        u32::try_from(self.point_indices.len()).unwrap_or(u32::MAX)
    }

    /// Slot 4, `ComputeMeshBoundSphere`: Ritter's sphere, cached once
    /// made. The reference walks the interleaved points and normals as an
    /// array of points of the point count (its first half: points and
    /// normals alternately); so does this.
    #[allow(clippy::many_single_char_names)]
    pub fn bound_sphere(&mut self) -> (f32, [f32; 3]) {
        if !self.sphere_ready {
            let flat: Vec<f32> = self.vertices.iter().flatten().copied().collect();
            let count = self.vertices.len();
            let p = |i: usize, k: usize| flat[i * 3 + k];
            self.sphere_center = [0.0; 3];
            self.sphere_radius = 0.0;
            let non_empty = count != 0;
            if non_empty {
                let (mut min, mut max) = ([0usize; 3], [0usize; 3]);
                for i in 1..count {
                    for k in 0..3 {
                        if p(i, k) < p(min[k], k) {
                            min[k] = i;
                        } else if p(i, k) > p(max[k], k) {
                            max[k] = i;
                        }
                    }
                }
                let sub = |a: f32, b: f32| f(f64::from(a) - f64::from(b));
                let add = |a: f32, b: f32| f(f64::from(a) + f64::from(b));
                let dx = sub(p(max[0], 0), p(min[0], 0));
                let dy = sub(p(max[1], 1), p(min[1], 1));
                let dz = sub(p(max[2], 2), p(min[2], 2));
                let sum = [
                    add(p(min[0], 0), p(max[0], 0)),
                    add(p(max[1], 1), p(min[1], 1)),
                    add(p(max[2], 2), p(min[2], 2)),
                ];
                self.sphere_center = sum.map(|s| f(f64::from(s) * 0.5));
                let mut extent = if dx > dy { dx } else { dy };
                if !(extent > dz) {
                    extent = dz;
                }
                let radius = f64::from(extent) * 0.5;
                self.sphere_radius = f(radius);
                let mut radius_sq = f(radius * radius);
                for i in 0..count {
                    let pt = [p(i, 0), p(i, 1), p(i, 2)];
                    let c = self.sphere_center;
                    let d = [sub(pt[0], c[0]), sub(pt[1], c[1]), sub(pt[2], c[2])];
                    let dist_sq = (f64::from(d[1]) * f64::from(d[1])
                        + f64::from(d[2]) * f64::from(d[2]))
                        + f64::from(d[0]) * f64::from(d[0]);
                    if dist_sq > f64::from(radius_sq) {
                        let dist = dist_sq.sqrt();
                        let dist_f = f(dist);
                        let new_radius = (dist + f64::from(self.sphere_radius)) * 0.5;
                        self.sphere_radius = f(new_radius);
                        radius_sq = f(new_radius * new_radius);
                        let shift = f(f64::from(dist_f) - new_radius);
                        let toward = pt.map(|v| f(f64::from(v) * f64::from(shift)));
                        let kept = c.map(|v| f(f64::from(v) * f64::from(self.sphere_radius)));
                        let blend = [
                            add(toward[0], kept[0]),
                            add(toward[1], kept[1]),
                            add(toward[2], kept[2]),
                        ];
                        let inv = 1.0 / f64::from(dist_f);
                        self.sphere_center = blend.map(|b| f(f64::from(b) * inv));
                    }
                }
            }
            self.sphere_ready = non_empty;
        }
        (self.sphere_radius, self.sphere_center)
    }

    /// `FillMeshStreamDesc`: the source `CopyVertices` converts.
    #[must_use]
    pub fn stream_desc(&self) -> VertexSource {
        let interleaved: Vec<u8> = self
            .vertices
            .iter()
            .flatten()
            .flat_map(|v| v.to_le_bytes())
            .collect();
        let count = u32::try_from(self.vertices.len()).unwrap_or(u32::MAX);
        let mut desc = VertexSource {
            fvf: self.fvf,
            flags: 6,
            count: self.vertex_capacity(),
            position: VertexStream {
                data: Some(interleaved.clone()),
                byte_size: 0xC,
                stride: 0x18,
                index: Some(self.point_indices.clone()),
                count,
            },
            normal: VertexStream {
                data: Some(
                    interleaved
                        .get(0xC..)
                        .map(<[u8]>::to_vec)
                        .unwrap_or_default(),
                ),
                byte_size: 0xC,
                stride: 0x18,
                index: Some(self.point_indices.clone()),
                count,
            },
            ..VertexSource::default()
        };
        if !self.uvs.is_empty() {
            desc.tex[0] = VertexStream {
                data: Some(
                    self.uvs
                        .iter()
                        .flatten()
                        .flat_map(|v| v.to_le_bytes())
                        .collect(),
                ),
                byte_size: 8,
                stride: 8,
                index: self.uv_indices.clone(),
                count: u32::try_from(self.uvs.len()).unwrap_or(u32::MAX),
            };
        }
        desc
    }

    /// Slot 3, `Skin`: the bones whose pose changed (or every bone when
    /// `force`) compose their matrices and mark their points; the buffer
    /// is made sure of and locked, refilled from the source when `force`,
    /// and every marked point's position and normal are written to each
    /// of its corners. 0 on a failure.
    #[allow(clippy::too_many_lines)]
    pub fn skin(
        &mut self,
        holder: &mut LodHolder,
        vb: &mut VbListing,
        vb_capacity: i32,
        force: bool,
        services: &mut dyn DeformServices,
    ) -> u8 {
        let Some(root) = holder.bones.first().map(|b| b.local) else {
            return 0;
        };
        let inverse = inverse_root(&root);
        self.skinned_vertex_count = 0;
        self.skinned_bone_count = 0;
        for index in 0..holder.bound() {
            let bone_to_root = holder
                .skeleton
                .bones
                .get(index)
                .map_or(IDENTITY12, |b| b.bone_to_root);
            let bone = &mut holder.bones[index];
            if bone.flags & 2 != 0 || force {
                bone.flags &= 0xFD;
                let list = self.bone_verts.get(index).map_or(&[][..], Vec::as_slice);
                if !list.is_empty() {
                    self.skinned_bone_count += 1;
                    if self.flag70 {
                        bone.compose(&bone_to_root, &inverse);
                        for &v in list {
                            if let Some(d) = self.dirty.get_mut(usize::from(v)) {
                                *d = 1;
                            }
                        }
                    } else {
                        bone.matrix = IDENTITY12;
                    }
                }
            }
        }

        let stride = fvf_stride(self.fvf) as usize;
        let capacity = self.vertex_capacity();
        vb.fvf = self.fvf;
        vb.base_vertex = 0;
        if services.is_vb_valid(vb.vertex_buffer) == 0 {
            services.destroy_vb(vb.vertex_buffer);
            vb.vertex_buffer = 0;
            match services.create_vb(vb.fvf, vb_capacity.cast_unsigned()) {
                Ok(handle) => vb.vertex_buffer = handle,
                Err(_) => return 0,
            }
        }
        vb.base_vertex = 0;
        let (base, mut locked) = match services.lock_vb(vb.vertex_buffer, capacity) {
            Ok(lock) => lock,
            Err(_) => return 0,
        };
        vb.base_vertex = base;
        if force {
            let desc = self.stream_desc();
            if services.copy_vertices(vb.vertex_buffer, base, &desc, 0, capacity, &mut locked) < 0 {
                services.dump(
                    dump::WARNING,
                    file::DEFORMABLE_PART,
                    0x134,
                    "failed to copy deformable verts during update_verts",
                );
                services.unlock_vb(vb.vertex_buffer, locked);
                return 0;
            }
        }

        for vertex in 0..self.vertices.len() {
            if self.dirty[vertex] == 0 {
                continue;
            }
            self.dirty[vertex] = 0;
            self.skinned_vertex_count += 1;
            let skin = self.skin[vertex];
            let pair = |k: u32| {
                self.skin_pairs
                    .get(skin.first_pair.wrapping_add(k) as usize)
                    .copied()
                    .unwrap_or(SkinPair {
                        bone_id: 0,
                        weight: 0.0,
                    })
            };
            let matrix = |p: SkinPair| {
                usize::try_from(p.bone_id)
                    .ok()
                    .and_then(|b| holder.bones.get(b))
                    .map_or(IDENTITY12, |b| b.matrix)
            };
            let v = self.vertices[vertex];
            let point = [v[0], v[1], v[2]];
            let normal = [v[3], v[4], v[5]];
            let first = pair(0);
            let count = skin.bone_count;
            let mut out = [0.0f32; 6];
            if count == 1 {
                let m = matrix(first);
                out[..3].copy_from_slice(&transform_point(&m, &point));
                out[3..].copy_from_slice(&transform_direction(&m, &normal));
            } else {
                let mut blend = scale_bone_matrix(&matrix(first), first.weight);
                if count == 4 {
                    let p = pair(3);
                    accumulate_bone_matrix(&mut blend, &matrix(p), p.weight);
                }
                if count == 4 || count == 3 {
                    let p = pair(2);
                    accumulate_bone_matrix(&mut blend, &matrix(p), p.weight);
                }
                if count == 4 || count == 3 || count == 2 {
                    let p = pair(1);
                    accumulate_bone_matrix(&mut blend, &matrix(p), p.weight);
                }
                out[..3].copy_from_slice(&transform_point(&blend, &point));
                let n = renormalise(transform_direction(&blend, &normal));
                out[3..].copy_from_slice(&n);
            }
            let bytes: Vec<u8> = out.iter().flat_map(|w| w.to_le_bytes()).collect();
            for &corner in &self.remap[vertex] {
                let at = corner as usize * stride;
                if let Some(dest) = locked.get_mut(at..at + 24) {
                    dest.copy_from_slice(&bytes);
                }
            }
        }
        services.unlock_vb(vb.vertex_buffer, locked);
        1
    }

    /// Slot 2, `FillIndexBuffer`: the buffer made sure of, locked for the
    /// index count and filled with each group's indices plus its minimum.
    pub fn fill_index_buffer(
        &self,
        ib: &mut IbScratch,
        ib_capacity: i32,
        services: &mut dyn DeformServices,
    ) -> u8 {
        let count = self.index_count();
        if services.is_ib_valid(ib.index_buffer) == 0 {
            services.destroy_ib(ib.index_buffer);
            ib.index_buffer = 0;
            match services.create_ib(ib_capacity.cast_unsigned()) {
                Ok(handle) => ib.index_buffer = handle,
                Err(_) => return 0,
            }
        }
        ib.bias = 0;
        match services.lock_ib(ib.index_buffer, count) {
            Ok(bias) => ib.bias = bias,
            Err(_) => return 0,
        }
        let mut written = Vec::with_capacity(count as usize);
        for group in &self.face_groups {
            #[allow(clippy::cast_possible_truncation)]
            let min = group.index_min as u16;
            written.extend(group.indices.iter().map(|&i| i.wrapping_add(min)));
        }
        services.unlock_ib(ib.index_buffer, &written);
        1
    }

    /// Slot 5, `Draw`: the modelview (view times the root's pose, or an
    /// identity world without a view), then every face group with indices
    /// through its material (`Bind`, `Submit` with flag 0x100, `Teardown`),
    /// the eye groups with the binding of their UV-deform row and bone.
    pub fn draw(
        &mut self,
        camera: &mut dyn MeshCamera,
        holder: &LodHolder,
        vb: &VbListing,
        ib: &IbScratch,
        services: &mut dyn DeformServices,
    ) -> u8 {
        let root_bone = holder.bones.first();
        let root = root_bone.map_or([0.0; 12], |b| b.local);
        let view = camera.view_transform();
        let have_view = services.pipeline_state(7).unwrap_or(1);
        if have_view != 0 {
            services.set_modelview(&mul_transform12(&view, &root));
        } else {
            services.set_world(&IDENTITY12);
        }
        let context = |field20: usize| SubmitContext {
            instance: root_bone
                .map_or(INSTANCE_NONE, |b| b.instance)
                .cast_unsigned() as usize,
            world: &root,
            view: &view,
            vertices: None,
            indices: &[],
            vb: Some(vb),
            ib: Some(ib),
            replaced: None,
            field20,
        };
        let mut binding = UvBinding::default();
        let mut start = 0u32;
        for group in &self.face_groups {
            let mut bound = false;
            if !self.uv_deform.is_empty()
                && group.uv_bone_index.cast_signed() >= 0
                && let Some(record) = self.uv_deform.get(group.uv_bone_index as usize)
            {
                binding.record = Some(*record);
                let bone = usize::try_from(record.bone_id)
                    .ok()
                    .and_then(|b| holder.bones.get(b));
                binding.bone_local = bone.map_or([0.0; 12], |b| b.local);
                binding.bone_instance = bone.map_or(INSTANCE_NONE, |b| b.instance);
                bound = true;
            }
            let count = u32::try_from(group.indices.len()).unwrap_or(u32::MAX);
            if count != 0 {
                let num_verts = group
                    .index_max
                    .wrapping_sub(group.index_min)
                    .wrapping_add(1);
                let ctx = context(usize::from(bound));
                if let Some(material) = group.material {
                    services.bind(material, &ctx, bound.then_some(&mut binding));
                    services.submit(
                        material,
                        &ctx,
                        &DrawArgs {
                            prim_type: group.kind,
                            start: group.index_min,
                            count: num_verts,
                            ib_start: start,
                            index_span: count,
                            flags: 0x100,
                        },
                    );
                    services.teardown(material, &ctx);
                }
            }
            start = start.wrapping_add(count);
        }
        self.eye_uv_transform_count = binding.eye_uv_transform_count;
        1
    }

    /// The destructor: the face groups' materials released, last to first.
    pub fn destroy(&self, services: &mut dyn DeformServices) {
        for group in self.face_groups.iter().rev() {
            if let Some(material) = group.material {
                services.release_material(material);
            }
        }
    }
}

/// `FVmeshWrapper`: a mesh node made as a VMesh archetype.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VMeshWrapper {
    /// The engine archetype (`EngineHandle`).
    pub handle: EngineHandle,
    /// The `IVMeshPart` with a reference (`VMeshPart`).
    pub part: Option<u32>,
}

/// `G_VmeshPaletteSkinning`: 1 and never written.
pub const VMESH_PALETTE_SKINNING: i32 = 1;

/// `D3DRS_SOFTWAREVERTEXPROCESSING`.
pub const RS_SOFTWARE_VERTEX_PROCESSING: u32 = 153;
/// `D3DRS_INDEXEDVERTEXBLENDENABLE`.
pub const RS_INDEXED_VERTEX_BLEND: u32 = 167;
/// `D3DRS_VERTEXBLEND`.
pub const RS_VERTEX_BLEND: u32 = 151;
/// `D3DVBF_3WEIGHTS`.
pub const VBF_3WEIGHTS: i32 = 3;

impl VMeshWrapper {
    /// Slot 5, `FVmeshWrapper::Draw`: the bound bones' matrices (times the
    /// view when there is one) as world matrices 1 to n, the three blend
    /// states saved and set for indexed palette skinning, the part drawn
    /// on the root instance, the states put back in reverse order.
    ///
    /// The reference never reaches it: the part's skin (the base slot,
    /// 0) fails first in `DrawPartLive`.
    pub fn draw(
        &self,
        camera: &mut dyn MeshCamera,
        holder: &LodHolder,
        services: &mut dyn DeformServices,
    ) -> u8 {
        let view = camera.view_transform();
        let have_view = services.pipeline_state(7).unwrap_or(1);
        let Some(part) = self.part else {
            return 1;
        };
        if VMESH_PALETTE_SKINNING != 0 && holder.bind_cursor > 0 {
            for (index, bone) in holder.bones.iter().take(holder.bound()).enumerate() {
                let world = u32::try_from(index + 1).unwrap_or(u32::MAX);
                if have_view != 0 {
                    services.set_world_n(world, &mul_transform12(&view, &bone.matrix));
                } else {
                    services.set_world_n(world, &bone.matrix);
                }
            }
        }
        let saved_software = services.render_state(RS_SOFTWARE_VERTEX_PROCESSING);
        services.set_render_state(RS_SOFTWARE_VERTEX_PROCESSING, VMESH_PALETTE_SKINNING);
        let saved_indexed = services.render_state(RS_INDEXED_VERTEX_BLEND);
        services.set_render_state(RS_INDEXED_VERTEX_BLEND, VMESH_PALETTE_SKINNING);
        let saved_blend = services.render_state(RS_VERTEX_BLEND);
        services.set_render_state(
            RS_VERTEX_BLEND,
            if VMESH_PALETTE_SKINNING != 0 {
                VBF_3WEIGHTS
            } else {
                0
            },
        );
        let root = holder.bones.first().map_or(INSTANCE_NONE, |b| b.instance);
        services.draw_vmesh_part(part, root);
        services.set_render_state(RS_VERTEX_BLEND, saved_blend);
        services.set_render_state(RS_INDEXED_VERTEX_BLEND, saved_indexed);
        services.set_render_state(RS_SOFTWARE_VERTEX_PROCESSING, saved_software);
        1
    }

    /// The destructor: the archetype released unless -1, the part
    /// released.
    pub fn destroy(&self, services: &mut dyn DeformServices) {
        if self.handle != INSTANCE_NONE {
            services.release_handle(self.handle);
        }
        if let Some(part) = self.part {
            services.release_vmesh_part(part);
        }
    }
}

/// A mesh of the cache: either kind (`FDeformMesh`).
#[derive(Clone, Debug, PartialEq)]
pub enum DeformMesh {
    /// A `.dfm` mesh.
    Dfm(Box<MeshArchetype>),
    /// A VMesh-backed mesh.
    VMesh(VMeshWrapper),
}

impl DeformMesh {
    /// Slot 0 (0 for the VMesh kind).
    #[must_use]
    pub fn index_count(&self) -> u32 {
        match self {
            Self::Dfm(m) => m.index_count(),
            Self::VMesh(_) => 0,
        }
    }

    /// Slot 1 (0 for the VMesh kind).
    #[must_use]
    pub fn vertex_capacity(&self) -> u32 {
        match self {
            Self::Dfm(m) => m.vertex_capacity(),
            Self::VMesh(_) => 0,
        }
    }

    /// Slot 4: `None` where the slot answers 0 (the VMesh kind) and
    /// writes nothing.
    pub fn bound_sphere(&mut self) -> Option<(f32, [f32; 3])> {
        match self {
            Self::Dfm(m) => Some(m.bound_sphere()),
            Self::VMesh(_) => None,
        }
    }
}

/// A mesh of the cache.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct MeshId(u32);

#[derive(Clone, Debug, PartialEq)]
struct CacheEntry {
    crc: u32,
    refs: i32,
    mesh: DeformMesh,
}

/// `G_MeshCache`: meshes by the CRC of their name (`<part base name><level
/// digit>`), with the reference count each keeps.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct MeshCache {
    entries: Vec<Option<CacheEntry>>,
    by_crc: BTreeMap<u32, MeshId>,
}

impl MeshCache {
    /// The cached mesh of `name` with a new reference
    /// (`AddRefDeformMesh`).
    pub fn find(&mut self, name: &[u8]) -> Option<MeshId> {
        let id = *self.by_crc.get(&name_crc32(name))?;
        if let Some(entry) = self.entry_mut(id) {
            entry.refs += 1;
        }
        Some(id)
    }

    /// A new mesh under `name` at one reference (`G_MeshCache[key] =`).
    pub fn insert(&mut self, name: &[u8], mesh: DeformMesh) -> MeshId {
        let crc = name_crc32(name);
        let slot = self
            .entries
            .iter()
            .position(Option::is_none)
            .unwrap_or(self.entries.len());
        let id = MeshId(u32::try_from(slot).unwrap_or(u32::MAX));
        let entry = Some(CacheEntry { crc, refs: 1, mesh });
        if slot == self.entries.len() {
            self.entries.push(entry);
        } else {
            self.entries[slot] = entry;
        }
        self.by_crc.insert(crc, id);
        id
    }

    fn entry_mut(&mut self, id: MeshId) -> Option<&mut CacheEntry> {
        self.entries.get_mut(id.0 as usize)?.as_mut()
    }

    /// The mesh.
    #[must_use]
    pub fn get(&self, id: MeshId) -> Option<&DeformMesh> {
        Some(&self.entries.get(id.0 as usize)?.as_ref()?.mesh)
    }

    /// The mesh, to change.
    pub fn get_mut(&mut self, id: MeshId) -> Option<&mut DeformMesh> {
        Some(&mut self.entry_mut(id)?.mesh)
    }

    /// The reference count.
    #[must_use]
    pub fn refs(&self, id: MeshId) -> i32 {
        self.entries
            .get(id.0 as usize)
            .and_then(Option::as_ref)
            .map_or(0, |e| e.refs)
    }

    /// The cached meshes by name CRC, in CRC order (`G_MeshCache`'s).
    pub fn iter(&self) -> impl Iterator<Item = (u32, MeshId)> + '_ {
        self.by_crc.iter().map(|(&crc, &id)| (crc, id))
    }

    /// The meshes alive.
    pub fn len(&self) -> usize {
        self.entries.iter().flatten().count()
    }

    /// No mesh is alive.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// `ReleaseDeformMesh`: the last reference erases the cache's entry
    /// for the mesh's CRC and destroys it. The count left.
    pub fn release(&mut self, id: MeshId, services: &mut dyn DeformServices) -> i32 {
        let Some(entry) = self.entry_mut(id) else {
            return 0;
        };
        entry.refs -= 1;
        let remaining = entry.refs;
        if remaining > 0 {
            return remaining;
        }
        let crc = entry.crc;
        let Some(entry) = self.entries[id.0 as usize].take() else {
            return remaining;
        };
        self.by_crc.remove(&crc);
        match &entry.mesh {
            DeformMesh::Dfm(m) => m.destroy(services),
            DeformMesh::VMesh(w) => w.destroy(services),
        }
        remaining
    }
}
