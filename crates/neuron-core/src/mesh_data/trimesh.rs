//! The legacy multi-stream mesh: a `Mesh` node with `Geometry` and
//! `Face_groups` (`FTriMeshArch::Load` and the face-group helpers of
//! `TriMesh.cpp`).
//!
//! `Geometry` holds separate pools (`Points`, `Vertex_normals`, `UV0`,
//! `UV1`, `Color0`), each with a per-corner index array (`*_indices`,
//! dwords), and an optional UV animation (`UV0_anim_lookup`,
//! `UV0_frame_count`, `UV0_anim`, `UV0_fps`, `UV0_interpolate`).
//! `Face_groups` has a `Count` and `Group<n>` nodes with a
//! `Material_name`, `Tristrip_indices` or `Face_indices`, and the optional
//! `Edge_indices` and `Edge_angles`.

use super::{
    Malformed, f32s, open_node, printed_children, printed_name, read_bytes, read_string, read_u32,
    u16s, u32s, vec2s, vec3s,
};
use crate::hash::name_crc32;
use crate::vfs::FileSystem;

/// `D3DPT_TRIANGLELIST`.
pub const TRIANGLE_LIST: u32 = 4;
/// `D3DPT_TRIANGLESTRIP`.
pub const TRIANGLE_STRIP: u32 = 5;

/// One pool of `Geometry` and its corner index array.
#[derive(Clone, Debug, PartialEq)]
pub struct TriMeshStream<T> {
    /// The pool's whole elements.
    pub values: Vec<T>,
    /// The `*_indices` dwords, when that leaf was read.
    pub indices: Option<Vec<u32>>,
}

impl<T> Default for TriMeshStream<T> {
    fn default() -> Self {
        Self {
            values: Vec::new(),
            indices: None,
        }
    }
}

/// The UV0 animation.
#[derive(Clone, Debug, PartialEq)]
pub struct TriMeshUvAnim {
    /// `UV0_anim_lookup`: the vertex slot of each animated entry, -1 to skip.
    pub lookup: Vec<i32>,
    /// `UV0_frame_count`; 0 when it does not read as four bytes.
    pub frame_count: u32,
    /// `UV0_anim`: (u, v) pairs, `lookup.len()` per frame by convention.
    pub frames: Option<Vec<[f32; 2]>>,
    /// `UV0_fps`; 0 when it does not read.
    pub fps: f32,
    /// `UV0_interpolate`; 0 when it does not read.
    pub interpolate: u32,
    /// `(double)(frame_count - 1, as unsigned) / (double)fps`, stored to a
    /// float.
    pub period: f32,
}

/// The vertex source the loader fills (`FRpVertexSrcDesc`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct TriMeshGeometry {
    /// Whether the `Geometry` node exists; without it only `flags` is set.
    pub node: bool,
    /// The descriptor flags: 4, indexed streams.
    pub flags: u32,
    /// The source FVF: `XYZ`, plus normal, texture count and diffuse bits
    /// for the pools that loaded.
    pub src_fvf: u32,
    /// The length of the last corner index array that was read.
    pub src_count: u32,
    /// `Points` and `Point_indices`; empty values when `Points` did not
    /// read.
    pub positions: TriMeshStream<[f32; 3]>,
    /// `Vertex_normals` and `Vertex_normal_indices`, when there is at least
    /// one normal.
    pub normals: Option<TriMeshStream<[f32; 3]>>,
    /// `UV0` and `UV0_indices`, when there is at least one coordinate.
    pub tex0: Option<TriMeshStream<[f32; 2]>>,
    /// `UV1` and `UV1_indices`.
    pub tex1: Option<TriMeshStream<[f32; 2]>>,
    /// `Color0` and `Color0_indices`, as stored.
    pub diffuse: Option<TriMeshStream<u32>>,
    /// The UV0 animation, when `UV0_anim_lookup` has an entry.
    pub uv_anim: Option<TriMeshUvAnim>,
}

/// One `Group<n>` node.
#[derive(Clone, Debug, PartialEq)]
pub struct TriMeshFaceGroup {
    /// The `n` of `Group<n>`.
    pub node_index: u32,
    /// [`TRIANGLE_LIST`], or [`TRIANGLE_STRIP`] for a strip kept as one.
    pub primitive_type: u32,
    /// `Material_name` (at most 0x3F bytes, to its first NUL), lower-cased;
    /// `None` when it did not read ("No Material_name found").
    pub material_name: Option<Vec<u8>>,
    /// The name CRC of the lower-cased name, 0 without one.
    pub material_crc: u32,
    /// The indices with `min_index` subtracted; `None` when neither
    /// `Tristrip_indices` nor `Face_indices` read. The reference reads
    /// through the missing array there (divergence register).
    pub indices: Option<Vec<u16>>,
    /// The smallest index before the subtraction (0 without indices).
    pub min_index: u32,
    /// The largest index.
    pub max_index: u32,
    /// `Edge_indices`.
    pub edge_indices: Option<Vec<u16>>,
    /// `Edge_angles`, every whole float of the leaf.
    pub edge_angles: Option<Vec<f32>>,
    /// The hook-up sort key the constructor sets (4).
    pub bucket: u32,
}

impl TriMeshFaceGroup {
    /// The edge count the group keeps: the `Edge_indices` count, or 0.
    #[must_use]
    pub fn edge_count(&self) -> u32 {
        self.edge_indices
            .as_ref()
            .map_or(0, |e| u32::try_from(e.len()).unwrap_or(u32::MAX))
    }
}

/// A loaded `Mesh` node.
#[derive(Clone, Debug, PartialEq)]
pub struct TriMesh {
    /// The vertex source.
    pub geometry: TriMeshGeometry,
    /// Whether `Face_groups` exists.
    pub face_groups_node: bool,
    /// Its `Count`, when it reads as four bytes.
    pub face_group_count: Option<u32>,
    /// The groups found, in index order.
    pub groups: Vec<TriMeshFaceGroup>,
}

impl TriMesh {
    /// The number of `FDUMP` warnings the load made ("No Material_name
    /// found").
    #[must_use]
    pub fn dump_count(&self) -> u32 {
        let missing = self
            .groups
            .iter()
            .filter(|g| g.material_name.is_none())
            .count();
        u32::try_from(missing).unwrap_or(u32::MAX)
    }
}

/// `FTriMeshGroup::ConvertStripToList`: each strip triangle that is not
/// degenerate, with the last two corners swapped at odd positions. A strip
/// of fewer than three indices gives none (the reference's count wraps and
/// its allocation fails; divergence register).
#[must_use]
pub fn strip_to_list(strip: &[u16]) -> Vec<u16> {
    let mut list = Vec::with_capacity(strip.len().saturating_sub(2) * 3);
    for (index, w) in strip.windows(3).enumerate() {
        let (a, b, c) = (w[0], w[1], w[2]);
        if a == b || b == c || c == a {
            continue;
        }
        list.push(a);
        if index & 1 != 0 {
            list.extend([c, b]);
        } else {
            list.extend([b, c]);
        }
    }
    list
}

/// `FTriMeshGroup::Rebase` on a fresh group: the minimum and maximum, then
/// the minimum subtracted (16-bit wrapping). An empty array gives 0 and 0
/// (the reference reads before it; divergence register).
#[must_use]
pub fn rebase(indices: &mut [u16]) -> (u32, u32) {
    let (Some(&first), Some(&last)) = (indices.first(), indices.last()) else {
        return (0, 0);
    };
    let mut min = u32::from(first.min(last));
    let mut max = u32::from(first.max(last));
    for &value in indices.iter() {
        let value = u32::from(value);
        if value < min {
            min = value;
        } else if value > max {
            max = value;
        }
    }
    // The subtraction is a 16-bit one in the reference.
    let base = u16::try_from(min).unwrap_or(u16::MAX);
    for value in indices.iter_mut() {
        *value = value.wrapping_sub(base);
    }
    (min, max)
}

fn corner_indices(
    fs: &dyn FileSystem,
    name: &[u8],
    geometry: &mut TriMeshGeometry,
) -> Result<Option<Vec<u32>>, Malformed> {
    Ok(read_bytes(fs, name)?.map(|bytes| {
        let indices = u32s(&bytes);
        geometry.src_count = u32::try_from(indices.len()).unwrap_or(u32::MAX);
        indices
    }))
}

fn load_geometry(fs: &mut dyn FileSystem, geometry: &mut TriMeshGeometry) -> Result<(), Malformed> {
    geometry.node = true;
    if let Some(bytes) = read_bytes(fs, b"Points")? {
        geometry.positions.values = vec3s(&bytes);
    }
    geometry.src_fvf = 0x2;
    geometry.positions.indices = corner_indices(fs, b"Point_indices", geometry)?;

    let normals = read_bytes(fs, b"Vertex_normals")?.map(|b| vec3s(&b));
    if let Some(values) = normals.filter(|v| !v.is_empty()) {
        geometry.src_fvf |= 0x10;
        let indices = corner_indices(fs, b"Vertex_normal_indices", geometry)?;
        geometry.normals = Some(TriMeshStream { values, indices });
    }
    let tex0 = read_bytes(fs, b"UV0")?.map(|b| vec2s(&b));
    if let Some(values) = tex0.filter(|v| !v.is_empty()) {
        geometry.src_fvf = (geometry.src_fvf & 0xFFFF_F1FF) | 0x100;
        let indices = corner_indices(fs, b"UV0_indices", geometry)?;
        geometry.tex0 = Some(TriMeshStream { values, indices });
    }
    let tex1 = read_bytes(fs, b"UV1")?.map(|b| vec2s(&b));
    if let Some(values) = tex1.filter(|v| !v.is_empty()) {
        geometry.src_fvf = (geometry.src_fvf & 0xFFFF_F2FF) | 0x200;
        let indices = corner_indices(fs, b"UV1_indices", geometry)?;
        geometry.tex1 = Some(TriMeshStream { values, indices });
    }
    let diffuse = read_bytes(fs, b"Color0")?.map(|b| u32s(&b));
    if let Some(values) = diffuse.filter(|v| !v.is_empty()) {
        geometry.src_fvf |= 0x40;
        let indices = corner_indices(fs, b"Color0_indices", geometry)?;
        geometry.diffuse = Some(TriMeshStream { values, indices });
    }

    let lookup: Vec<i32> = read_bytes(fs, b"UV0_anim_lookup")?
        .map(|b| {
            u32s(&b)
                .into_iter()
                .map(|v| i32::from_ne_bytes(v.to_ne_bytes()))
                .collect()
        })
        .unwrap_or_default();
    if !lookup.is_empty() {
        let frame_count = read_u32(fs, b"UV0_frame_count")?.unwrap_or(0);
        let frames = read_bytes(fs, b"UV0_anim")?.map(|b| vec2s(&b));
        let fps = super::read_exact(fs, b"UV0_fps", 4)?.map_or(0.0, |b| super::f32_at(&b, 0));
        let interpolate = read_u32(fs, b"UV0_interpolate")?.unwrap_or(0);
        // FILD of the zero-extended count - 1, FDIV, FSTP (decision T11).
        #[allow(clippy::cast_possible_truncation)]
        let period = (f64::from(frame_count.wrapping_sub(1)) / f64::from(fps)) as f32;
        geometry.uv_anim = Some(TriMeshUvAnim {
            lookup,
            frame_count,
            frames,
            fps,
            interpolate,
            period,
        });
    }
    let _ = open_node(fs, b"..")?;
    Ok(())
}

fn load_group(
    fs: &mut dyn FileSystem,
    node_index: u32,
    tristrips: bool,
) -> Result<TriMeshFaceGroup, Malformed> {
    let material_name = read_string(fs, b"Material_name", 0x3F)?.map(|mut name| {
        name.make_ascii_lowercase();
        name
    });
    let material_crc = material_name.as_deref().map_or(0, name_crc32);
    let mut primitive_type = TRIANGLE_LIST;
    let mut indices = match read_bytes(fs, b"Tristrip_indices")? {
        Some(bytes) => {
            let strip = u16s(&bytes);
            if tristrips {
                primitive_type = TRIANGLE_STRIP;
                Some(strip)
            } else {
                Some(strip_to_list(&strip))
            }
        }
        None => read_bytes(fs, b"Face_indices")?.map(|b| u16s(&b)),
    };
    let edge_indices = read_bytes(fs, b"Edge_indices")?.map(|b| u16s(&b));
    let edge_angles = read_bytes(fs, b"Edge_angles")?.map(|b| f32s(&b));
    let (min_index, max_index) = indices.as_deref_mut().map_or((0, 0), rebase);
    Ok(TriMeshFaceGroup {
        node_index,
        primitive_type,
        material_name,
        material_crc,
        indices,
        min_index,
        max_index,
        edge_indices,
        edge_angles,
        bucket: 4,
    })
}

/// `FTriMeshPlugin::Claim` / `FTriMeshArch::Load`. `None` when there is no
/// `Mesh` node (the plugin returns -1). `tristrips` is the `[TriMesh]
/// tristrips` setting: the plugin's default is on, the client's
/// `NeuronComponents.ini` turns it off, and off converts every strip to a
/// list. The current node ends where it started.
///
/// # Errors
///
/// A malformed archive.
pub fn load(fs: &mut dyn FileSystem, tristrips: bool) -> Result<Option<TriMesh>, Malformed> {
    if !open_node(fs, b"Mesh")? {
        return Ok(None);
    }
    let mut geometry = TriMeshGeometry {
        flags: 4,
        ..TriMeshGeometry::default()
    };
    if open_node(fs, b"Geometry")? {
        load_geometry(fs, &mut geometry)?;
    }
    let mut mesh = TriMesh {
        geometry,
        face_groups_node: false,
        face_group_count: None,
        groups: Vec::new(),
    };
    if open_node(fs, b"Face_groups")? {
        mesh.face_groups_node = true;
        mesh.face_group_count = read_u32(fs, b"Count")?;
        if let Some(count) = mesh.face_group_count {
            for index in printed_children(fs, b"Group", count)? {
                if !open_node(fs, &printed_name(b"Group", index))? {
                    continue;
                }
                mesh.groups.push(load_group(fs, index, tristrips)?);
                let _ = open_node(fs, b"..")?;
            }
        }
        let _ = open_node(fs, b"..")?;
    }
    let _ = open_node(fs, b"..")?;
    Ok(Some(mesh))
}

#[cfg(test)]
mod tests {
    use super::{rebase, strip_to_list};

    #[test]
    fn strips_drop_degenerates_and_alternate() {
        assert_eq!(strip_to_list(&[0, 1, 2, 3]), [0, 1, 2, 1, 3, 2]);
        assert_eq!(strip_to_list(&[0, 1, 1, 2, 3]), [1, 2, 3]);
        assert_eq!(
            strip_to_list(&[4, 4, 0, 1, 2, 3]),
            [4, 1, 0, 0, 1, 2, 1, 3, 2]
        );
        assert!(strip_to_list(&[0, 1]).is_empty());
    }

    #[test]
    fn rebase_subtracts_the_minimum() {
        let mut indices = [7, 5, 9, 6];
        assert_eq!(rebase(&mut indices), (5, 9));
        assert_eq!(indices, [2, 0, 4, 1]);
        assert_eq!(rebase(&mut []), (0, 0));
    }
}
