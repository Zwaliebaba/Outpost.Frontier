//! The render manager and the mesh kinds (work package P4.6):
//! `CppSource/RendComp/`.
//!
//! * [`lod`]: the levels of detail of an archetype (`FLodSet`) and the
//!   distance rule that picks one.
//! * [`manager`]: `FRenderManager` (`RenderManager.cpp`): archetypes by
//!   engine handle, `MultiLevel` and the plugin walk, instances, the
//!   `IRenderer2` queries and `RenderInstance` with its culling, level
//!   choice and recursion into child instances.
//! * [`vmesh`]: the `VMeshData` registry by name CRC, its shared-buffer
//!   groups with their device buffers, `VMeshLibrary`, the `VMeshPart`
//!   archetype and its draw, and the `VMesh` plugin (`VMeshLibrary.cpp`,
//!   `VMeshPart.cpp`).
//! * [`wire`]: `VMeshWire` (`VMeshWire.cpp`), the wireframes drawn as
//!   lines over a library mesh's vertices.
//! * [`trimesh`]: the `TriMesh` archetype as it draws (the material
//!   hook-up and its buckets, the bounds, the counts), its instances with
//!   the UV animation and the `TriMesh` plugin (`TriMesh.cpp`,
//!   `Plugins.cpp`).
//! * [`sphere`]: the `SphereMesh` plugin (`Plugins.cpp`): the planet
//!   archetype, its instances, the tessellation of the cube faces, the
//!   level formula and the face and atmosphere passes.
//! * [`face`]: the `IMesh` and `IRenderableMesh` faces of the `VMesh`
//!   part and of the `TriMesh` archetype and instance ([`MeshFace`]),
//!   which the render manager's [`MeshDirectory`] finds by archetype or
//!   instance.
//! * [`services`]: what RendComp calls on the renderer, the engine, the
//!   material library and the materials ([`MeshServices`]) and on the
//!   camera ([`MeshCamera`]), one method per call in the reference's
//!   argument order.
//!
//! The archive formats are read by `neuron-core::mesh_data` (P1.7); this
//! module adds what the reference decides at run time. Handles the
//! reference hands out as pointers are numbers here ([`Handle`]); the
//! instance and archetype handles of the engine are the engine's.
//!
//! No platform code: everything here builds and tests on every host.

pub mod face;
pub mod lod;
pub mod manager;
pub mod services;
pub mod sphere;
pub mod trimesh;
pub mod vmesh;
pub mod wire;

pub use face::{FaceData, FaceIndices, FaceSource, MeshFace};
pub use lod::{LodEntry, LodSet};
pub use manager::{MeshDirectory, RenderComponent, RenderManager};
pub use services::{MeshCamera, MeshServices};
pub use sphere::SpherePlugin;
pub use trimesh::{TriMeshFaces, TriMeshPlugin};
pub use vmesh::{VMeshFaces, VMeshLibraryLoad, VMeshPlugin, VMeshRegistry};
pub use wire::VMeshWire;

use neuron_core::math::Transform;
use neuron_core::vfs::{FileSystem, FindEntry, FsDesc};

/// A plugin's archetype handle: the reference's object pointer, here a
/// number the plugin hands out. [`HANDLE_NONE`] is the reference's -1.
pub type Handle = u32;

/// The handle of no archetype (`G_HandleFail`, -1).
pub const HANDLE_NONE: Handle = u32::MAX;

/// An engine instance (`void* InstId`).
pub type Instance = usize;

/// An engine archetype handle as the render manager keys it (`int`); -1
/// is none.
pub type Archetype = i32;

/// The `Field20` value of the sphere's atmosphere pass: the engine
/// (`G_Engine`), which the atmosphere material asks for the instance's
/// extent.
pub const FIELD20_ENGINE: usize = 1;

/// `FDUMP` flags (`kFdumpError`, `kFdumpWarning`, `kFdumpNotice`).
pub mod dump {
    /// `kFdumpError`.
    pub const ERROR: i32 = 0x0010_0001;
    /// `kFdumpWarning`.
    pub const WARNING: i32 = 0x0010_0002;
    /// `kFdumpNotice`.
    pub const NOTICE: i32 = 0x0010_0003;
}

/// The `__FILE__` strings RendComp passes to `FDUMP`.
pub mod file {
    /// `G_FileTriMesh`.
    pub const TRIMESH: &str = "RendComp\\TriMesh\\TriMesh.cpp";
    /// `G_FileTriMeshArchetype`.
    pub const TRIMESH_ARCHETYPE: &str = "RendComp\\TriMesh\\TriMeshArchetype.cpp";
    /// `G_FileRenderMgr`.
    pub const RENDER_MGR: &str = "RendComp\\RenderMgr.cpp";
    /// `G_FileVMeshData`.
    pub const VMESH_DATA: &str = "RendComp\\VMesh\\VMeshData.cpp";
    /// `G_FileVMeshLibrary`.
    pub const VMESH_LIBRARY: &str = "RendComp\\VMesh\\VMeshLibrary.cpp";
    /// `G_FileVMeshWire`.
    pub const VMESH_WIRE: &str = "RendComp\\VMesh\\VMeshWire.cpp";
    /// `G_FileVMPartArch`.
    pub const VM_PART_ARCH: &str = "RendComp\\VMesh\\VMPartArch.cpp";
    /// `G_FileSphereMeshArchetype`.
    pub const SPHERE_ARCHETYPE: &str = "RendComp\\SphereMesh\\SphereMeshArchetype.cpp";
}

/// The `IRenderComponent::QueryHandle` and `QueryMeshInstance` interfaces
/// the plugins answer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MeshIid {
    /// `IID_IMesh`.
    Mesh,
    /// `IID_IMatHookUp`.
    MatHookUp,
    /// `IID_IRenderableMesh`.
    RenderableMesh,
    /// `IID_INeuronComponent`.
    NeuronComponent,
    /// `IID_ITriMeshAnim`.
    TriMeshAnim,
    /// `IID_ISphereMesh`.
    SphereMesh,
    /// `IID_IVMeshLibrary` (the `VMeshPart`'s own face).
    VMeshLibrary,
    /// `IID_IVMeshWire`.
    VMeshWire,
    /// Any other interface.
    Other,
}

/// The engine's transform of an instance as the twelve words
/// `GetInstanceTransform` points to.
fn transform(words: [f32; 12]) -> Transform {
    Transform::from_words(words)
}

/// `CreateInstance` of a child file system named `name` (or of a found
/// entry): `None` where it fails. A malformed archive is a failure here
/// (divergence D350).
fn child_fs(
    fs: &dyn FileSystem,
    name: &[u8],
    found: Option<&FindEntry>,
) -> Option<Box<dyn FileSystem>> {
    let desc = FsDesc::read(name).with_found(found.and_then(|entry| entry.token));
    fs.create_child(&desc).ok()
}

/// `OpenByDesc(name)`, `GetSize`, a buffer of that size and one `Read` of
/// it: the bytes the read copied, `None` where the open or the read fails.
fn read_leaf(fs: &dyn FileSystem, name: &[u8]) -> Option<Vec<u8>> {
    let mut file = fs.open_file(&FsDesc::read(name)).ok()?;
    let size = file.size().ok()?;
    let available = file.contents().map_or(0, <[u8]>::len);
    let length = if size == 0 {
        0
    } else {
        (size as usize).min(available.max(1))
    };
    let mut buf = vec![0; length];
    let got = file.read(&mut buf).ok()?;
    buf.truncate(got);
    Some(buf)
}

/// `OpenNode(name)` succeeded.
fn open_node(fs: &mut dyn FileSystem, name: &[u8]) -> bool {
    fs.open_node(name).is_ok()
}
