//! `FDeformPart`: one part of an instance, its levels (the cache's meshes),
//! the current level, its dynamic vertex and index buffers and the draw
//! that skins, refills and submits as its flags ask.

use crate::render::material::{IbScratch, VbListing};
use crate::render::mesh::MeshCamera;

use super::mesh::{DeformMesh, MeshCache, MeshId};
use super::services::DeformServices;
use super::skeleton::LodHolder;

/// `FDeformPart`.
#[derive(Clone, Debug, PartialEq)]
pub struct DeformPart {
    /// The holder (`Lod`): the skeleton and its bone runtimes.
    pub lod: Option<LodHolder>,
    /// The current level's mesh (`Current`).
    pub current: Option<MeshId>,
    /// The level meshes (`Meshes`).
    pub meshes: Vec<Option<MeshId>>,
    /// The level asked for last (`LodIndex`).
    pub lod_index: i32,
    /// The vertex buffer's listing (handle, format, lock offset).
    pub vb: VbListing,
    /// Its capacity: the largest level's corner count.
    pub vb_capacity: i32,
    /// The index buffer and its lock offset.
    pub ib: IbScratch,
    /// Its capacity: the largest level's index count.
    pub ib_capacity: i32,
    /// Bit 0: the level changed (both buffers are rebuilt); bit 1: the
    /// skin is stale.
    pub flags: u8,
}

impl Default for DeformPart {
    /// `ConstructPart`: everything 0, the level-changed bit set.
    fn default() -> Self {
        Self {
            lod: None,
            current: None,
            meshes: Vec::new(),
            lod_index: 0,
            vb: VbListing::default(),
            vb_capacity: 0,
            ib: IbScratch::default(),
            ib_capacity: 0,
            flags: 1,
        }
    }
}

impl DeformPart {
    /// `BindMeshes`: the holder and the meshes (their references move to
    /// the part); the capacities become the largest level's counts when
    /// those are above 0. Always `true`.
    pub fn bind_meshes(&mut self, holder: LodHolder, meshes: &[MeshId], cache: &MeshCache) -> bool {
        self.lod = Some(holder);
        self.meshes = meshes.iter().map(|&m| Some(m)).collect();
        let mut max_vertices = 0i32;
        let mut max_indices = 0i32;
        for &mesh in meshes {
            let Some(m) = cache.get(mesh) else { continue };
            let vertices = m.vertex_capacity().cast_signed();
            if vertices > max_vertices {
                max_vertices = vertices;
            }
            let indices = m.index_count().cast_signed();
            if indices > max_indices {
                max_indices = indices;
            }
        }
        if max_vertices > 0 {
            self.vb_capacity = max_vertices;
        }
        if max_indices > 0 {
            self.ib_capacity = max_indices;
        }
        true
    }

    /// `SelectPartLod`: the level's mesh becomes current (none out of
    /// range); bit 0 is set unless the same mesh stays with bit 0 clear.
    /// Whether the current mesh changed.
    pub fn select_lod(&mut self, lod: i32) -> bool {
        let old = self.current;
        let index = usize::try_from(lod).ok().filter(|&i| i < self.meshes.len());
        let Some(index) = index else {
            self.current = None;
            self.lod_index = lod;
            return old != self.current;
        };
        let next = self.meshes[index];
        self.current = next;
        if self.flags & 1 == 0 && old == next {
            self.lod_index = lod;
            return old != self.current;
        }
        self.flags |= 1;
        self.lod_index = lod;
        old != self.current
    }

    /// `MarkPartSkinDirty`.
    pub fn mark_skin_dirty(&mut self) {
        self.flags |= 2;
    }

    /// `DrawPartLive`: nothing without a current mesh; the skin when stale
    /// or the buffer is not selectable (forced when the level changed or
    /// the buffer was lost), the index buffer refilled when the level
    /// changed or it is not selectable, then the mesh's draw and both bits
    /// cleared. 0 on a failure.
    pub fn draw_live(
        &mut self,
        camera: &mut dyn MeshCamera,
        cache: &mut MeshCache,
        services: &mut dyn DeformServices,
    ) -> u8 {
        let Some(current) = self.current else {
            return 1;
        };
        let Some(mesh) = cache.get_mut(current) else {
            return 1;
        };
        let vb_selected = services.select_vb(self.vb.vertex_buffer) >= 0;
        let flags = self.flags;
        let rebuild = flags & 1 != 0 || !vb_selected;
        if flags & 2 != 0 || rebuild {
            // A part without a holder dereferences null in the reference
            // (divergence D395).
            let skinned = match (mesh, self.lod.as_mut()) {
                (DeformMesh::Dfm(m), Some(holder)) => {
                    m.skin(holder, &mut self.vb, self.vb_capacity, rebuild, services)
                }
                _ => 0,
            };
            if skinned == 0 {
                return 0;
            }
            if services.select_vb(self.vb.vertex_buffer) < 0 {
                return 0;
            }
        }
        let Some(mesh) = cache.get_mut(current) else {
            return 1;
        };
        let ib_selected = services.select_ib(self.ib.index_buffer, self.vb.base_vertex) >= 0;
        if self.flags & 1 != 0 || !ib_selected {
            let filled = match &*mesh {
                DeformMesh::Dfm(m) => m.fill_index_buffer(&mut self.ib, self.ib_capacity, services),
                DeformMesh::VMesh(_) => 0,
            };
            if filled == 0 {
                return 0;
            }
            if services.select_ib(self.ib.index_buffer, self.vb.base_vertex) < 0 {
                return 0;
            }
        }
        if let Some(holder) = self.lod.as_ref() {
            match mesh {
                DeformMesh::Dfm(m) => {
                    m.draw(camera, holder, &self.vb, &self.ib, services);
                }
                DeformMesh::VMesh(w) => {
                    w.draw(camera, holder, services);
                }
            }
        }
        self.flags &= 0xFC;
        1
    }

    /// `PartBoundSphere`: the first level's sphere, its centre through the
    /// root bone's pose (`TransformXform`). `None` where the mesh answers
    /// 0 and writes nothing.
    pub fn bound_sphere(&self, cache: &mut MeshCache) -> Option<(f32, [f32; 3])> {
        let first = self.meshes.first().copied().flatten()?;
        let (radius, centre) = cache.get_mut(first)?.bound_sphere()?;
        let root = self
            .lod
            .as_ref()
            .and_then(|h| h.bones.first())
            .map_or([0.0; 12], |b| b.local);
        Some((radius, super::math::transform_xform12(&root, &centre)))
    }

    /// `Reset` (the part's destructor): both buffers destroyed, the
    /// meshes released last to first, then the holder destroyed (the
    /// skeleton's handles released).
    pub fn reset(&mut self, cache: &mut MeshCache, services: &mut dyn DeformServices) {
        services.destroy_vb(self.vb.vertex_buffer);
        self.vb.vertex_buffer = 0;
        self.vb_capacity = 0;
        services.destroy_ib(self.ib.index_buffer);
        self.ib.index_buffer = 0;
        self.ib_capacity = 0;
        for mesh in self.meshes.iter_mut().rev() {
            if let Some(id) = mesh.take() {
                cache.release(id, services);
            }
        }
        self.meshes.clear();
        if let Some(holder) = self.lod.take() {
            holder.destroy(services);
        }
    }
}
