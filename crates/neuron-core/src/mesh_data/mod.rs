//! Mesh data (work package P1.7): the nodes the `RendComp` and
//! `Deformable2` loaders read from `.vms`, `.3db`, `.cmp`, `.sph` and
//! `.dfm` archives, decoded into owned values. Nothing here draws.
//!
//! Ported from `CppSource/RendComp/` (`VMeshLibrary.cpp`, `VMeshPart.cpp`,
//! `VMeshWire.cpp`, `TriMesh.cpp`, the `SphereMesh` archetype in
//! `Plugins.cpp`, `FRenderManager::MultiLevel` in `RenderManager.cpp`), the
//! records in `CppSource/include/render/rendcomp.h`, and the `.dfm` loaders
//! of `CppSource/Deformable2/Deformable2.cpp`.
//!
//! * [`fvf`]: vertex formats as RendComp decodes them (stride, channels).
//! * [`vmesh`]: the `VMeshData`, `VMeshRef` and `VWireData` leaves.
//! * [`library`]: a `VMeshLibrary` node and its shared-buffer groups.
//! * [`part`]: the `VMeshPart` and `VMeshWire` archetype nodes.
//! * [`trimesh`]: the legacy `Mesh` node (`Geometry`, `Face_groups`).
//! * [`sphere`]: the `Sphere` node of planet meshes.
//! * [`render`]: `MultiLevel` (`Switch2`, `Level<n>`) and the plugin walk
//!   that turns an archetype node into levels of detail.
//! * [`dfm`]: skeletons, `Fractions` and deformable meshes of `.dfm` files.
//!
//! The loaders walk a [`FileSystem`] the way the C++ walks its
//! `IFileSystem`: the same `OpenNode`, `FindFirst`, `CreateInstance` and
//! `OpenByDesc` calls in the same order, so lookups, find order and the
//! current node after a load follow the reference. Leaf bytes go through
//! pure parsers that take `&[u8]`.
//!
//! Floats are kept as `f32` built with [`f32::from_bits`], which keeps
//! every bit; nothing here does arithmetic on them except where the
//! reference does (one division in [`trimesh`]).
//!
//! Where the reference reads or writes outside a buffer (a leaf shorter
//! than its counts, a buffer sized down to whole elements but read whole,
//! an index array that is missing), the port returns an error or reads
//! only what is there (divergence register). Anything the C++ decides at
//! run time (materials found in the material library, engine handles,
//! device buffers, the name-keyed caches) is left to the client.

pub mod dfm;
pub mod fvf;
pub mod library;
pub mod part;
pub mod render;
pub mod sphere;
pub mod trimesh;
pub mod vmesh;

use crate::utf::UtfError;
use crate::vfs::{FileSystem, FindEntry, FsDesc, VfsError, VfsFile};

pub use fvf::{Channel, Fvf};
pub use vmesh::{VMeshData, VMeshDataHeader, VMeshFaceGroup, VMeshRef, VWireData};

/// A malformed archive under a loader: the reference reads outside it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Malformed(pub UtfError);

impl core::fmt::Display for Malformed {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "malformed archive: {}", self.0)
    }
}

impl std::error::Error for Malformed {}

/// `Ok(Some)` for success, `Ok(None)` for a failure the reference handles,
/// `Err` for a malformed archive.
fn soft<T>(result: Result<T, VfsError>) -> Result<Option<T>, Malformed> {
    match result {
        Ok(value) => Ok(Some(value)),
        Err(VfsError::Malformed(error)) => Err(Malformed(error)),
        Err(_) => Ok(None),
    }
}

/// `OpenNode(name)`: whether it succeeded.
fn open_node(fs: &mut dyn FileSystem, name: &[u8]) -> Result<bool, Malformed> {
    Ok(soft(fs.open_node(name))?.is_some())
}

/// `OpenByDesc` with the descriptor `FileDescInit` / `FillFileDesc` fill.
fn open(fs: &dyn FileSystem, name: &[u8]) -> Result<Option<VfsFile>, Malformed> {
    soft(fs.open_file(&FsDesc::read(name)))
}

/// `CreateInstance` for a child named `name`, or for a found entry.
fn child(
    fs: &dyn FileSystem,
    name: &[u8],
    found: Option<&FindEntry>,
) -> Result<Option<Box<dyn FileSystem>>, Malformed> {
    let desc = FsDesc::read(name).with_found(found.and_then(|entry| entry.token));
    soft(fs.create_child(&desc))
}

/// `FindFirst` / `FindNext`: the entries, none where the find fails.
fn find(fs: &dyn FileSystem, pattern: &[u8]) -> Result<Vec<FindEntry>, Malformed> {
    Ok(soft(fs.find(pattern))?.unwrap_or_default())
}

/// The outcome of `GetSize`, `new unsigned char[size]` and one `Read` of
/// the whole size.
enum Whole {
    /// The open failed.
    Missing,
    /// The read failed (a UTF read of 0 bytes, or one that copies nothing).
    Failed,
    /// The read succeeded: the bytes it copied. Where the archive ends
    /// before the size does, the reference's buffer keeps uninitialised
    /// bytes after them; here the leaf is shorter (divergence register).
    Read(Vec<u8>),
}

fn read_file_whole(file: &mut VfsFile) -> Result<Whole, Malformed> {
    let Some(size) = soft(file.size())? else {
        return Ok(Whole::Failed);
    };
    // A read copies no more than the backend holds, so the buffer need not
    // be larger (a hostile size would otherwise allocate gigabytes). With
    // nothing to copy, a one-byte read gets the backend's failure.
    let available = file.contents().map_or(0, <[u8]>::len);
    let length = if size == 0 {
        0
    } else {
        (size as usize).min(available.max(1))
    };
    let mut buf = vec![0; length];
    Ok(match soft(file.read(&mut buf))? {
        Some(got) => {
            buf.truncate(got);
            Whole::Read(buf)
        }
        None => Whole::Failed,
    })
}

/// Opens `name` and reads it whole.
fn read_whole(fs: &dyn FileSystem, name: &[u8]) -> Result<Whole, Malformed> {
    match open(fs, name)? {
        None => Ok(Whole::Missing),
        Some(mut file) => read_file_whole(&mut file),
    }
}

/// The bytes of a whole read, or `None` where the open or the read fails.
fn read_bytes(fs: &dyn FileSystem, name: &[u8]) -> Result<Option<Vec<u8>>, Malformed> {
    Ok(match read_whole(fs, name)? {
        Whole::Read(bytes) => Some(bytes),
        Whole::Missing | Whole::Failed => None,
    })
}

/// Opens `name` and reads `count` bytes; `None` unless the read succeeds
/// and reports exactly `count` bytes (`TriMeshReadInt` and its relatives).
fn read_exact(
    fs: &dyn FileSystem,
    name: &[u8],
    count: usize,
) -> Result<Option<Vec<u8>>, Malformed> {
    let Some(mut file) = open(fs, name)? else {
        return Ok(None);
    };
    let mut buf = vec![0; count];
    Ok(match soft(file.read(&mut buf))? {
        Some(got) if got == count => Some(buf),
        _ => None,
    })
}

/// A four-byte read (`TriMeshReadInt`, the `Count` and `Sides` leaves).
fn read_u32(fs: &dyn FileSystem, name: &[u8]) -> Result<Option<u32>, Malformed> {
    Ok(read_exact(fs, name, 4)?.map(|b| u32_at(&b, 0)))
}

/// Reads at most `limit` bytes of a string leaf; `None` unless the read
/// reports exactly that many (`TriMeshReadString`, the sphere's `M<n>`).
/// The result is cut at its first NUL, as the C string is.
fn read_string(fs: &dyn FileSystem, name: &[u8], limit: u32) -> Result<Option<Vec<u8>>, Malformed> {
    let Some(mut file) = open(fs, name)? else {
        return Ok(None);
    };
    let Some(size) = soft(file.size())? else {
        return Ok(None);
    };
    let length = size.min(limit) as usize;
    let mut buf = vec![0; length];
    Ok(match soft(file.read(&mut buf))? {
        Some(got) if got == length => Some(crate::hash::c_str(&buf).to_vec()),
        _ => None,
    })
}

/// A little-endian `u16` at `at`; the caller has checked the length.
fn u16_at(bytes: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([bytes[at], bytes[at + 1]])
}

/// A little-endian `u32` at `at`; the caller has checked the length.
fn u32_at(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

/// A little-endian `f32` at `at`, bit for bit.
fn f32_at(bytes: &[u8], at: usize) -> f32 {
    f32::from_bits(u32_at(bytes, at))
}

/// The whole elements of `bytes`: the reference sizes its buffer to
/// `size / element * element` and reads the whole leaf into it, which
/// writes past the buffer when the size is not a multiple; the port keeps
/// the whole elements and drops the rest.
fn elements<T>(bytes: &[u8], element: usize, read: impl Fn(&[u8]) -> T) -> Vec<T> {
    bytes.chunks_exact(element).map(read).collect()
}

fn u16s(bytes: &[u8]) -> Vec<u16> {
    elements(bytes, 2, |b| u16_at(b, 0))
}

fn u32s(bytes: &[u8]) -> Vec<u32> {
    elements(bytes, 4, |b| u32_at(b, 0))
}

fn f32s(bytes: &[u8]) -> Vec<f32> {
    elements(bytes, 4, |b| f32_at(b, 0))
}

fn vec2s(bytes: &[u8]) -> Vec<[f32; 2]> {
    elements(bytes, 8, |b| [f32_at(b, 0), f32_at(b, 4)])
}

fn vec3s(bytes: &[u8]) -> Vec<[f32; 3]> {
    elements(bytes, 12, |b| [f32_at(b, 0), f32_at(b, 4), f32_at(b, 8)])
}

/// The index `i` for which `name` equals `format!("{prefix}{}", i as i32)`
/// under ASCII case folding, as `sprintf("%s%d", prefix, (int)i)` spells
/// it: no sign for 0 and up, `-` below, no leading zeros.
fn printed_index(name: &[u8], prefix: &[u8]) -> Option<u32> {
    let name = crate::hash::c_str(name);
    if name.len() <= prefix.len() || !name[..prefix.len()].eq_ignore_ascii_case(prefix) {
        return None;
    }
    let digits = &name[prefix.len()..];
    let text = core::str::from_utf8(digits).ok()?;
    let value: i32 = text.parse().ok()?;
    (value.to_string().as_bytes() == digits).then_some(u32::from_ne_bytes(value.to_ne_bytes()))
}

/// The indices below `count` whose `<prefix><index>` name a child of the
/// open node, ascending. A loop of the reference over `0..count` that
/// opens each name does nothing for the others, so visiting only these
/// gives its result without walking every index.
fn printed_children(fs: &dyn FileSystem, prefix: &[u8], count: u32) -> Result<Vec<u32>, Malformed> {
    let mut indices: Vec<u32> = find(fs, b"*")?
        .iter()
        .filter_map(|entry| printed_index(&entry.data.name, prefix))
        .filter(|&index| index < count)
        .collect();
    indices.sort_unstable();
    indices.dedup();
    Ok(indices)
}

/// `sprintf("%s%d", prefix, (int)index)`.
fn printed_name(prefix: &[u8], index: u32) -> Vec<u8> {
    let mut name = prefix.to_vec();
    name.extend_from_slice(
        i32::from_ne_bytes(index.to_ne_bytes())
            .to_string()
            .as_bytes(),
    );
    name
}

#[cfg(test)]
mod tests;
