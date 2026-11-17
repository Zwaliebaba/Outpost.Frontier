//! `.dfm` files: the skeleton, the level fractions and the deformable
//! meshes `Deformable2` loads (`LoadSkeleton`, the `MultiLevel` /
//! `Fractions` read of `ConnectInstanceParts`, and `LoadMeshTables` with
//! `LoadFaceGroups`, `LoadGeometry`, `LoadUvDeformTables`,
//! `BuildVertexRemap` and `BuildBoneVertexLists` in `Deformable2.cpp`).
//! Skinning is not here (work package P4.7).
//!
//! * Bones: every root entry matching `*.3db` whose `Bone to root` (twelve
//!   floats, a 3x4 transform) reads, at most 150.
//! * `MultiLevel\Fractions`: up to eight floats, one per level.
//! * `Mesh*` (under `MultiLevel` when it exists): one mesh per level with
//!   `Face_groups` (`Count`, `Group*` with `Material_name` and
//!   `Tristrip_indices` or `Face_indices`) and `Geometry` (`Point_indices`,
//!   `Points`, `Vertex_normals`, `UV0_indices`, `UV0`, `Point_bone_count`,
//!   `Bone_id_chain`, `Bone_weight_chain`, and the UV-deform columns
//!   `UV_bone_id`, `UV_vertex_count`, `UV_plane_distance`,
//!   `Bone_X_to_U_scale`, `Bone_Y_to_V_scale`, `Min_du`, `Max_du`,
//!   `Min_dv`, `Max_dv`).
//!
//! What the reference decides through the engine or the material library
//! is not decided here: a face group whose material the library lacks
//! keeps its CRC but loses its indices there, the skeleton is valid only
//! when the engine makes an archetype of the file, a mesh whose tables do
//! not load is retried as a VMesh archetype, and meshes are cached by name
//! across files. The level choice from a part's settings is also the
//! client's.

use super::fvf::Fvf;
use super::trimesh::{rebase, strip_to_list};
use super::{
    Malformed, child, f32_at, find, open, open_node, read_bytes, read_exact, u16s, u32_at, u32s,
    vec2s,
};
use crate::hash::{c_str, name_crc32};
use crate::vfs::{FileSystem, VfsFile};

/// Bones a skeleton holds (`G_MaxBoneCount`).
pub const MAX_BONES: usize = 150;
/// Levels a part holds (`G_MaxLodCount`), and fractions read.
pub const MAX_LODS: usize = 8;

/// Why a `.dfm` file does not load.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DfmError {
    /// More than [`MAX_BONES`] bones read; the reference writes past its
    /// skeleton (divergence register).
    TooManyBones,
    /// A malformed archive.
    Malformed(Malformed),
}

impl From<Malformed> for DfmError {
    fn from(error: Malformed) -> Self {
        Self::Malformed(error)
    }
}

impl core::fmt::Display for DfmError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::TooManyBones => write!(f, "more than {MAX_BONES} bones"),
            Self::Malformed(e) => e.fmt(f),
        }
    }
}

impl std::error::Error for DfmError {}

/// Why a mesh's tables do not load. The reference then tries the node as
/// a VMesh archetype, except where it reads through a missing or short
/// array (divergence register), which the port refuses instead.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DfmMeshError {
    /// No `Face_groups` node.
    NoFaceGroups,
    /// No `Geometry` node.
    NoGeometry,
    /// `Points` has data but no view.
    NoPointView,
    /// `Vertex_normals` holds fewer than one normal per point.
    ShortNormals,
    /// `Point_bone_count` holds fewer than one count per point.
    ShortBoneCounts,
    /// `Bone_weight_chain` holds fewer weights than `Bone_id_chain` ids.
    ShortWeights,
    /// A UV-deform column (0 to 8, in the order of [`UvDeformRecord`])
    /// holds fewer values than `UV_bone_id`.
    ShortUvColumn(u8),
    /// A `Tristrip_indices` leaf has data but no view.
    NoStripView,
    /// A vertex's bone influences run past `Bone_id_chain`, or name a bone
    /// at or past the bone count (for [`DfmMesh::bone_vertex_lists`]).
    SkinOutOfRange,
}

impl core::fmt::Display for DfmMeshError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str(match self {
            Self::NoFaceGroups => "no Face_groups node",
            Self::NoGeometry => "no Geometry node",
            Self::NoPointView => "Points cannot be mapped",
            Self::ShortNormals => "fewer normals than points",
            Self::ShortBoneCounts => "fewer bone counts than points",
            Self::ShortWeights => "fewer bone weights than bone ids",
            Self::ShortUvColumn(_) => "a UV-deform column is short",
            Self::NoStripView => "Tristrip_indices cannot be mapped",
            Self::SkinOutOfRange => "bone influences out of range",
        })
    }
}

impl std::error::Error for DfmMeshError {}

/// One bone.
#[derive(Clone, Debug, PartialEq)]
pub struct DfmBone {
    /// The `*.3db` entry name; the engine archetype is made under it.
    pub name: Vec<u8>,
    /// `Bone to root`: twelve floats, rotation then translation.
    pub bone_to_root: [f32; 12],
}

/// One face group as `LoadFaceGroups` leaves it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DfmFaceGroup {
    /// The `Group*` entry it came from; `None` when the child could not be
    /// created ("couldnt open dfm face group").
    pub node: Option<Vec<u8>>,
    /// The mapped `Material_name`, to its first NUL (or the end of the
    /// leaf: the reference reads on past it); `None` when it has no view.
    pub material_name: Option<Vec<u8>>,
    /// The name CRC of the name as stored (not lower-cased), 0 without one.
    pub material_crc: u32,
    /// The primitive kind, 4 (a triangle list).
    pub kind: u32,
    /// The indices with `index_min` subtracted: the strip expanded to a
    /// list, or `Face_indices`; empty without a material name. A group
    /// with a name but neither array keeps none (the reference reads
    /// through a null array; divergence register).
    pub indices: Vec<u16>,
    /// The smallest index before the subtraction.
    pub index_min: u32,
    /// The largest.
    pub index_max: u32,
}

impl Default for DfmFaceGroup {
    /// `ConstructFaceGroup`.
    fn default() -> Self {
        Self {
            node: None,
            material_name: None,
            material_crc: 0,
            kind: 4,
            indices: Vec::new(),
            index_min: 0,
            index_max: 0,
        }
    }
}

/// One bone influence (`Bone_id_chain` and `Bone_weight_chain`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SkinPair {
    /// The bone index.
    pub bone_id: i32,
    /// Its weight.
    pub weight: f32,
}

/// A vertex's run of influences in [`DfmMesh::skin_pairs`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SkinVertex {
    /// `Point_bone_count`.
    pub bone_count: i32,
    /// The index of its first pair: the counts of the vertices before it,
    /// summed (32-bit wrapping).
    pub first_pair: u32,
}

/// One row of the UV-deform table, zipped from nine columns.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct UvDeformRecord {
    /// `UV_bone_id`.
    pub bone_id: i32,
    /// `UV_vertex_count`.
    pub vertex_count: i32,
    /// `UV_plane_distance`.
    pub plane_distance: f32,
    /// `Bone_X_to_U_scale`.
    pub bone_x_to_u: f32,
    /// `Bone_Y_to_V_scale`.
    pub bone_y_to_v: f32,
    /// `Min_du`.
    pub min_du: f32,
    /// `Max_du`.
    pub max_du: f32,
    /// `Min_dv`.
    pub min_dv: f32,
    /// `Max_dv`.
    pub max_dv: f32,
}

/// A deformable mesh's tables (`FMeshArchetype` after `LoadMeshTables`).
#[derive(Clone, Debug, PartialEq)]
pub struct DfmMesh {
    /// `Face_groups\Count`, 0 when it does not read as four bytes.
    pub face_group_count: i32,
    /// The groups processed: one per `Group*` entry in find order, at most
    /// `face_group_count` (none when it is 0 or less; the reference writes
    /// the first entry past its array there, divergence register).
    pub face_groups: Vec<DfmFaceGroup>,
    /// `Point_indices`: the point of each corner.
    pub point_indices: Vec<u32>,
    /// `Points` and `Vertex_normals` interleaved, one entry per point.
    pub vertices: Vec<[f32; 6]>,
    /// `UV0_indices`, when it read.
    pub uv_indices: Option<Vec<u32>>,
    /// `UV0`.
    pub uvs: Vec<[f32; 2]>,
    /// The influences.
    pub skin_pairs: Vec<SkinPair>,
    /// One run of influences per point.
    pub skin: Vec<SkinVertex>,
    /// The UV-deform rows.
    pub uv_deform: Vec<UvDeformRecord>,
}

impl DfmMesh {
    /// The vertex format of the skinned vertex buffer.
    pub const FVF: Fvf = Fvf::DEFORMABLE;

    /// The face groups' index counts, summed (slot 0 of the mesh).
    #[must_use]
    pub fn index_count(&self) -> usize {
        self.face_groups.iter().map(|g| g.indices.len()).sum()
    }

    /// `BuildVertexRemap`: for each point, the corners that use it, in
    /// order. Corners naming no point are skipped.
    #[must_use]
    pub fn vertex_remap(&self) -> Vec<Vec<u32>> {
        let mut lists = vec![Vec::new(); self.vertices.len()];
        for (corner, &point) in self.point_indices.iter().enumerate() {
            if let Some(list) = lists.get_mut(point as usize) {
                list.push(u32::try_from(corner).unwrap_or(u32::MAX));
            }
        }
        lists
    }

    /// `BuildBoneVertexLists` for a holder with `bone_count` bound bones:
    /// for each bone, the points it influences, in order (as 16-bit
    /// indices).
    ///
    /// # Errors
    ///
    /// [`DfmMeshError::SkinOutOfRange`] where the reference reads or writes
    /// outside its arrays.
    pub fn bone_vertex_lists(&self, bone_count: usize) -> Result<Vec<Vec<u16>>, DfmMeshError> {
        let mut lists = vec![Vec::new(); bone_count];
        for (point, skin) in self.skin.iter().enumerate() {
            for k in 0..skin.bone_count.max(0) as u32 {
                let pair = skin
                    .first_pair
                    .checked_add(k)
                    .and_then(|i| self.skin_pairs.get(i as usize))
                    .ok_or(DfmMeshError::SkinOutOfRange)?;
                let list = usize::try_from(pair.bone_id)
                    .ok()
                    .and_then(|b| lists.get_mut(b))
                    .ok_or(DfmMeshError::SkinOutOfRange)?;
                #[allow(clippy::cast_possible_truncation)]
                list.push(point as u16);
            }
        }
        Ok(lists)
    }
}

/// One `Mesh*` entry.
#[derive(Clone, Debug, PartialEq)]
pub struct DfmMeshNode {
    /// The entry name.
    pub name: Vec<u8>,
    /// Its tables, or why they did not load; `None` when its child file
    /// system could not be created.
    pub mesh: Option<Result<DfmMesh, DfmMeshError>>,
}

/// A loaded `.dfm` file.
#[derive(Clone, Debug, PartialEq)]
pub struct DfmFile {
    /// The bones, in find order.
    pub bones: Vec<DfmBone>,
    /// Whether `MultiLevel` exists.
    pub multi_level: bool,
    /// The level fractions: `Fractions` read up to [`MAX_LODS`] values (0
    /// where the read fails), or one fraction of 1.0 without `MultiLevel`.
    pub fractions: Vec<f32>,
    /// The number of levels the file offers before a part's own limit
    /// (`LodCap`): the fractions read, 0 when `MultiLevel` lacks
    /// `Fractions`, 1 without `MultiLevel`.
    pub lod_cap: u32,
    /// The `Mesh*` entries in find order.
    pub meshes: Vec<DfmMeshNode>,
}

/// `ExpandTristrip`: the same expansion as [`strip_to_list`].
#[must_use]
pub fn expand_tristrip(strip: &[u16]) -> Vec<u16> {
    strip_to_list(strip)
}

/// The mapped view of a node (`OpenMappedFileNode`): `None` without a
/// handle, `Some(None)` without a view.
type MappedNode = Option<(u32, Option<Vec<u8>>)>;

fn mapped(fs: &dyn FileSystem, name: &[u8]) -> Result<MappedNode, Malformed> {
    let Some(file) = open(fs, name)? else {
        return Ok(None);
    };
    let size = super::soft(file.size())?.unwrap_or(0);
    let view = map_whole(&file)?;
    Ok(Some((size, view)))
}

fn map_whole(file: &VfsFile) -> Result<Option<Vec<u8>>, Malformed> {
    Ok(super::soft(file.map_view(0, 0))?.map(<[u8]>::to_vec))
}

/// The dwords of a mapped node: `None` without a handle (size 0), the
/// view's dwords otherwise (none without a view).
fn mapped_u32s(fs: &dyn FileSystem, name: &[u8]) -> Result<(u32, Vec<u32>), Malformed> {
    Ok(match mapped(fs, name)? {
        None => (0, Vec::new()),
        Some((size, view)) => (size, view.map(|v| u32s(&v)).unwrap_or_default()),
    })
}

fn load_face_group(
    group_fs: &dyn FileSystem,
    group: &mut DfmFaceGroup,
) -> Result<Result<(), DfmMeshError>, Malformed> {
    let Some((_, Some(view))) = mapped(group_fs, b"Material_name")? else {
        return Ok(Ok(()));
    };
    let name = c_str(&view).to_vec();
    group.material_crc = name_crc32(&name);
    group.material_name = Some(name);
    // The material library lookup is the client's; the port takes it as
    // found.
    let mut indices = match mapped(group_fs, b"Tristrip_indices")? {
        Some((size, view)) if size != 0 => {
            let Some(view) = view else {
                return Ok(Err(DfmMeshError::NoStripView));
            };
            let strip = u16s(&view);
            let count = (size >> 1) as usize;
            expand_tristrip(&strip[..count.min(strip.len())])
        }
        _ => read_bytes(group_fs, b"Face_indices")?
            .map(|b| u16s(&b))
            .unwrap_or_default(),
    };
    group.kind = 4;
    let (min, max) = rebase(&mut indices);
    group.index_min = min;
    group.index_max = max;
    group.indices = indices;
    Ok(Ok(()))
}

fn load_face_groups(
    fs: &mut dyn FileSystem,
    mesh: &mut DfmMesh,
) -> Result<Result<(), DfmMeshError>, Malformed> {
    if !open_node(fs, b"Face_groups")? {
        return Ok(Err(DfmMeshError::NoFaceGroups));
    }
    let count =
        read_exact(fs, b"Count", 4)?.map_or(0, |b| i32::from_ne_bytes(u32_at(&b, 0).to_ne_bytes()));
    mesh.face_group_count = count;
    let wanted = usize::try_from(count).unwrap_or(0);
    if wanted > 0 {
        for entry in find(fs, b"Group*")? {
            let mut group = DfmFaceGroup::default();
            if let Some(group_fs) = child(fs, &entry.data.name, Some(&entry))? {
                group.node = Some(entry.data.name.clone());
                if let Err(e) = load_face_group(group_fs.as_ref(), &mut group)? {
                    return Ok(Err(e));
                }
            }
            mesh.face_groups.push(group);
            if mesh.face_groups.len() >= wanted {
                break;
            }
        }
    }
    let _ = open_node(fs, b"..")?;
    Ok(Ok(()))
}

fn load_geometry(
    fs: &mut dyn FileSystem,
    mesh: &mut DfmMesh,
) -> Result<Result<(), DfmMeshError>, Malformed> {
    if !open_node(fs, b"Geometry")? {
        return Ok(Err(DfmMeshError::NoGeometry));
    }
    if let Some(bytes) = read_bytes(fs, b"Point_indices")? {
        mesh.point_indices = u32s(&bytes);
    }
    let points = mapped(fs, b"Points")?;
    let point_count = points.as_ref().map_or(0, |(size, _)| (size / 12) as usize);
    if point_count > 0 {
        let Some((_, Some(points))) = points else {
            return Ok(Err(DfmMeshError::NoPointView));
        };
        let normals = match mapped(fs, b"Vertex_normals")? {
            Some((_, Some(view))) => view,
            _ => Vec::new(),
        };
        if normals.len() < point_count * 12 {
            return Ok(Err(DfmMeshError::ShortNormals));
        }
        mesh.vertices = (0..point_count)
            .map(|i| {
                let p = |k: usize| f32_at(&points, i * 12 + k * 4);
                let n = |k: usize| f32_at(&normals, i * 12 + k * 4);
                [p(0), p(1), p(2), n(0), n(1), n(2)]
            })
            .collect();
    }
    mesh.uv_indices = read_bytes(fs, b"UV0_indices")?.map(|b| u32s(&b));
    if let Some(bytes) = read_bytes(fs, b"UV0")? {
        mesh.uvs = vec2s(&bytes);
    }
    let (_, counts) = mapped_u32s(fs, b"Point_bone_count")?;
    let (id_size, ids) = mapped_u32s(fs, b"Bone_id_chain")?;
    let (_, weights) = mapped_u32s(fs, b"Bone_weight_chain")?;
    let pair_count = (id_size >> 2) as usize;
    if ids.len() < pair_count || weights.len() < pair_count {
        return Ok(Err(DfmMeshError::ShortWeights));
    }
    mesh.skin_pairs = ids[..pair_count]
        .iter()
        .zip(&weights)
        .map(|(&id, &weight)| SkinPair {
            bone_id: i32::from_ne_bytes(id.to_ne_bytes()),
            weight: f32::from_bits(weight),
        })
        .collect();
    if counts.len() < mesh.vertices.len() {
        return Ok(Err(DfmMeshError::ShortBoneCounts));
    }
    let mut first_pair = 0u32;
    mesh.skin = counts[..mesh.vertices.len()]
        .iter()
        .map(|&count| {
            let vertex = SkinVertex {
                bone_count: i32::from_ne_bytes(count.to_ne_bytes()),
                first_pair,
            };
            first_pair = first_pair.wrapping_add(count);
            vertex
        })
        .collect();
    let _ = open_node(fs, b"..")?;
    Ok(Ok(()))
}

const UV_COLUMNS: [&[u8]; 9] = [
    b"UV_bone_id",
    b"UV_vertex_count",
    b"UV_plane_distance",
    b"Bone_X_to_U_scale",
    b"Bone_Y_to_V_scale",
    b"Min_du",
    b"Max_du",
    b"Min_dv",
    b"Max_dv",
];

fn load_uv_deform(
    fs: &mut dyn FileSystem,
    mesh: &mut DfmMesh,
) -> Result<Result<(), DfmMeshError>, Malformed> {
    if !open_node(fs, b"Geometry")? {
        return Ok(Err(DfmMeshError::NoGeometry));
    }
    let mut columns = Vec::with_capacity(UV_COLUMNS.len());
    let mut row_count = 0;
    for (index, name) in UV_COLUMNS.iter().enumerate() {
        let (size, values) = mapped_u32s(fs, name)?;
        if index == 0 {
            row_count = (size >> 2) as usize;
        }
        columns.push(values);
    }
    if row_count != 0 {
        if let Some(short) = columns.iter().position(|c| c.len() < row_count) {
            #[allow(clippy::cast_possible_truncation)]
            return Ok(Err(DfmMeshError::ShortUvColumn(short as u8)));
        }
        let int = |v: u32| i32::from_ne_bytes(v.to_ne_bytes());
        mesh.uv_deform = (0..row_count)
            .map(|i| UvDeformRecord {
                bone_id: int(columns[0][i]),
                vertex_count: int(columns[1][i]),
                plane_distance: f32::from_bits(columns[2][i]),
                bone_x_to_u: f32::from_bits(columns[3][i]),
                bone_y_to_v: f32::from_bits(columns[4][i]),
                min_du: f32::from_bits(columns[5][i]),
                max_du: f32::from_bits(columns[6][i]),
                min_dv: f32::from_bits(columns[7][i]),
                max_dv: f32::from_bits(columns[8][i]),
            })
            .collect();
    }
    let _ = open_node(fs, b"..")?;
    Ok(Ok(()))
}

/// `LoadMeshTables` without the parts that need the engine or the material
/// library: face groups, geometry and the UV-deform table, from a mesh
/// node's file system. The vertex remap and the bone lists are derived on
/// request ([`DfmMesh::vertex_remap`], [`DfmMesh::bone_vertex_lists`]).
///
/// # Errors
///
/// [`DfmMeshError`] inside `Ok`; `Err` for a malformed archive.
pub fn load_mesh(fs: &mut dyn FileSystem) -> Result<Result<DfmMesh, DfmMeshError>, Malformed> {
    let mut mesh = DfmMesh {
        face_group_count: 0,
        face_groups: Vec::new(),
        point_indices: Vec::new(),
        vertices: Vec::new(),
        uv_indices: None,
        uvs: Vec::new(),
        skin_pairs: Vec::new(),
        skin: Vec::new(),
        uv_deform: Vec::new(),
    };
    if let Err(e) = load_face_groups(fs, &mut mesh)? {
        return Ok(Err(e));
    }
    if let Err(e) = load_geometry(fs, &mut mesh)? {
        return Ok(Err(e));
    }
    if let Err(e) = load_uv_deform(fs, &mut mesh)? {
        return Ok(Err(e));
    }
    Ok(Ok(mesh))
}

/// `LoadSkeleton`'s bone walk over the root of a `.dfm` file: each
/// `*.3db` entry as a child, `OpenNode("DFMSkeleton")` in it, its `Bone to
/// root` read as 0x30 bytes. When `DFMSkeleton` opened, the reference
/// calls `OpenNode("..")` on the root rather than the child; so does this.
///
/// # Errors
///
/// [`DfmError::TooManyBones`], or a malformed archive.
pub fn load_bones(root: &mut dyn FileSystem) -> Result<Vec<DfmBone>, DfmError> {
    let mut bones = Vec::new();
    for entry in find(root, b"*.3db")? {
        let Some(mut bone_fs) = child(root, &entry.data.name, Some(&entry))? else {
            continue;
        };
        let skeleton_node = open_node(bone_fs.as_mut(), b"DFMSkeleton")?;
        let matrix = read_exact(bone_fs.as_ref(), b"Bone to root", 0x30)?;
        if skeleton_node {
            let _ = open_node(root, b"..")?;
        }
        if let Some(bytes) = matrix {
            if bones.len() == MAX_BONES {
                return Err(DfmError::TooManyBones);
            }
            bones.push(DfmBone {
                name: entry.data.name.clone(),
                bone_to_root: core::array::from_fn(|k| f32_at(&bytes, k * 4)),
            });
        }
    }
    Ok(bones)
}

/// Loads a `.dfm` file from its root file system: the bones, then
/// `OpenNode("MultiLevel")` and `Fractions`, then every `Mesh*` entry
/// (under `MultiLevel` when it opened) through [`load_mesh`].
///
/// # Errors
///
/// [`DfmError`].
pub fn load(root: &mut dyn FileSystem) -> Result<DfmFile, DfmError> {
    let bones = load_bones(root)?;
    let multi_level = open_node(root, b"MultiLevel")?;
    let (fractions, lod_cap) = if multi_level {
        match open(root, b"Fractions")? {
            None => (Vec::new(), 0),
            Some(mut file) => {
                let available = super::soft(file.size())?.unwrap_or(0) >> 2;
                let wanted = available.min(MAX_LODS as u32);
                let mut buf = vec![0u8; wanted as usize * 4];
                let _ = super::soft(file.read(&mut buf))?;
                (super::f32s(&buf), wanted)
            }
        }
    } else {
        (vec![1.0], 1)
    };
    let mut meshes = Vec::new();
    for entry in find(root, b"Mesh*")? {
        let mesh = match child(root, &entry.data.name, Some(&entry))? {
            Some(mut mesh_fs) => Some(load_mesh(mesh_fs.as_mut())?),
            None => None,
        };
        meshes.push(DfmMeshNode {
            name: entry.data.name.clone(),
            mesh,
        });
    }
    Ok(DfmFile {
        bones,
        multi_level,
        fractions,
        lod_cap,
        meshes,
    })
}
