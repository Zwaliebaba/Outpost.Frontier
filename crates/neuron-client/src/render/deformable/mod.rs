//! Deformable models (work package P4.7): `CppSource/Deformable2/`
//! (`Deformable2.cpp`, `EyeMaterial.cpp`), the engine component the client
//! draws its characters with.
//!
//! * [`skeleton`]: `FSkeleton`, the bone runtimes (`FBoneRuntime`, the
//!   `IInstanceTransform` the engine writes each bone's pose into) and
//!   `FLodHolder`, which binds a skeleton to the engine's instance tree
//!   through the bone link of `CreateInstance`.
//! * [`mesh`]: the `.dfm` mesh at run time (`FMeshArchetype`: CPU
//!   skinning into the part's vertex buffer, the index buffer, the draw
//!   through the face groups' materials with the eye groups' UV-deform
//!   binding, the bound sphere), the VMesh-backed mesh with its palette
//!   draw (`FVmeshWrapper`) and the mesh cache.
//! * [`part`]: `FDeformPart`, a part's levels, buffers and draw.
//! * [`svd`]: the singular value decomposition the IK solvers invert their
//!   Jacobian with.
//! * [`ik`]: aim solving (`FIkEvent`): the aim event and its channels, the
//!   single-joint look-at with the Euler clamp, and the two
//!   damped-least-squares chain solvers.
//! * [`instance`]: `FDeformInstance` and the `IDeformable` calls: creation
//!   from part descriptions (`ConnectInstanceParts`), the list of playing
//!   animations and aim events, the pose calls, levels, the draw, bounds,
//!   statistics and hardpoints, and the module state ([`Deformable`]:
//!   `DeformRate` and the mesh cache).
//! * [`eye`]: the `EyeMaterial`: its parameter table, value cells,
//!   load and save, texture set-up, translucency and the bind that turns
//!   an eye bone's direction into a texture offset.
//! * [`material`]: the `EyeMaterial` as a material of
//!   `render::material` ([`EyeHost`], [`EyeFactory`],
//!   [`bind_eye_materials`]).
//! * [`hooks`]: what the client forwards into an instance: the engine's
//!   transform calls to the bone runtimes ([`TransformCall`]) and the
//!   typed channels' owner callbacks to the aim events
//!   ([`OwnerBind`]).
//! * [`services`]: what the component calls outside itself
//!   ([`DeformServices`]: [`DeformHost`] and [`DeformRender`]).
//!
//! The `.dfm` tables are read by `neuron_core::mesh_data::dfm` (P1.7);
//! the pose itself is the engine's (`neuron_core::scene`, P1.8), whose
//! transform calls the client forwards into the bone runtimes. The render
//! half of the services is [`crate::render::stack::StackDeform`] on the
//! device model. No platform code.

// The reference's x87 branches treat an unordered comparison as one side of
// a test; the negated comparisons keep that.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

pub mod eye;
pub mod hooks;
pub mod ik;
pub mod instance;
pub mod material;
pub mod math;
pub mod mesh;
pub mod part;
pub mod services;
pub mod skeleton;
pub mod svd;

pub use eye::{EyeMaterial, EyeServices};
pub use hooks::{OwnerBind, TransformCall};
pub use ik::{AimDesc, IkEvent};
pub use instance::{DeformInstance, Deformable, LodDesc, PartDesc};
pub use material::{EyeFactory, EyeHost, bind_eye_materials};
pub use mesh::{DeformMesh, MeshArchetype, MeshCache, MeshId, VMeshWrapper};
pub use part::DeformPart;
pub use services::{BoneLink, ChannelSample, DeformHost, DeformRender, DeformServices};
pub use skeleton::{BoneRuntime, BoneSlot, LodHolder, Skeleton};

use neuron_core::mesh_data::dfm::UvDeformRecord;

/// An engine archetype handle (`void*`); -1 is none.
pub type EngineHandle = i32;

/// An engine instance (`void*`); -1 is none.
pub type Instance = i32;

/// The handle and instance of nothing (`G_InvalidEngineHandle`).
pub const INSTANCE_NONE: Instance = -1;

/// The triple a UV-deformed face group hands its material through the
/// context's `Field20` (`FUvDeformBinding`): the row, the bone's pose and
/// instance, and the count of eye texture transforms set during the draw.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct UvBinding {
    /// The UV-deform row (`Record`); `None` before the first eye group.
    pub record: Option<UvDeformRecord>,
    /// The bone's pose (`Bone->LocalTransform`).
    pub bone_local: [f32; 12],
    /// The bone's instance (`Bone->Instance`).
    pub bone_instance: Instance,
    /// `EyeUvTransformCount`.
    pub eye_uv_transform_count: u32,
}

/// `FDUMP` flags.
pub mod dump {
    /// `0x100001`: an error.
    pub const ERROR: i32 = 0x0010_0001;
    /// `0x100002`: a warning.
    pub const WARNING: i32 = 0x0010_0002;
    /// `0x100003`: a notice.
    pub const NOTICE: i32 = 0x0010_0003;
}

/// The `__FILE__` strings `Deformable2` passes to `FDUMP`.
pub mod file {
    /// `G_FdumpFileDeformableMesh`.
    pub const DEFORMABLE_MESH: &str = "Deformable2\\DeformableMesh.cpp";
    /// `G_FdumpFileDeformablePart`.
    pub const DEFORMABLE_PART: &str = "Deformable2\\DeformablePart.cpp";
    /// `G_FdumpFileDeformablePartArchetype`.
    pub const DEFORMABLE_PART_ARCHETYPE: &str = "Deformable2\\DeformablePartArchetype.cpp";
    /// `G_FdumpFileMat`.
    pub const MAT: &str = "Deformable2\\Mat.h";
    /// `G_FdumpFileDeformableObject`.
    pub const DEFORMABLE_OBJECT: &str = "Deformable2\\DeformableObject.cpp";
    /// `G_FdumpFileDeformIk`.
    pub const DEFORM_IK: &str = "Deformable2\\deformIK.cpp";
    /// `G_FdumpFileMaterialImpl`.
    pub const MATERIAL_IMPL: &str = "include\\imaterialimpl.h";
}
