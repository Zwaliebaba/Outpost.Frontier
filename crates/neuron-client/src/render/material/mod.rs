//! Materials at run time (work package P4.5): `CppSource/Shading/`'s
//! materials, their library, animation and batcher.
//!
//! * [`regex`]: `MaterialRegex.cpp`, the reference's Henry Spencer regular
//!   expressions, which the map and batching rules use.
//! * [`library`]: `MaterialLibrary.cpp`, map rules and the `[MaterialMap]`
//!   section, type resolution, creation through a [`MaterialFactory`], the
//!   master map by name CRC and the groups.
//! * [`instance`]: the material objects as an arena of [`Material`] values
//!   with the reference counts the reference keeps.
//! * [`global`]: the global material records.
//! * [`animation`]: `MaterialAnimation.cpp`, animations, load groups and
//!   instances evaluated over time into texture matrices.
//! * [`batcher`]: `MaterialBatcher.cpp`, the batching rules, the bins, the
//!   queued submissions, their depth sort and the dispatch to the
//!   materials' batched faces and a [`BatchDevice`].
//! * [`services`]: the calls the material kinds make, as
//!   [`MaterialServices`].
//! * [`kinds`]: the 22 material kinds of `Material.cpp`
//!   ([`ShadingMaterial`]): texture verification, `ApplyVerify`, binding,
//!   teardown and submission.
//! * [`batched`]: `BatchedMaterial`, which draws with an inner material and
//!   submits through the batcher.
//! * [`factory`]: the factories `Shading.cpp` registers, by exact name.
//! * [`surface`]: the 19 materials of `surfacematerials`
//!   ([`SurfaceMaterial`]) and their factory ([`SurfaceFactory`]), the
//!   fallback of [`ShadingFactory`].
//! * [`shading`]: [`MaterialServices`], [`SurfaceServices`] and
//!   [`BatchDevice`] on the device model ([`crate::render::ff`]) and the
//!   texture library ([`crate::render::texture`]).
//!
//! No platform code: everything here builds and tests on every host.

pub mod animation;
pub mod batched;
pub mod batcher;
pub mod factory;
pub mod global;
pub mod instance;
pub mod kinds;
pub mod library;
pub mod regex;
pub mod services;
pub mod shading;
pub mod surface;

pub use animation::{AnimationData, AnimationInstance, GroupId, InstanceId, MaterialAnimations};
pub use batched::BatchedMaterial;
pub use batcher::{
    BatchDevice, BatchRule, BatcherConfig, BatcherEvents, BinState, DrawArgs, IbScratch,
    MaterialBatcher, SubmitContext, SubmitInput, VbListing, VertexSource, VertexStream,
};
pub use factory::ShadingFactory;
pub use global::{GlobalRecord, GlobalRecords};
pub use instance::{BatchRoute, Material, MaterialFactory, MaterialId, Materials};
pub use kinds::{MaterialState, ShadingMaterial, TextureState};
pub use library::{LibraryLoad, MapEntry, MapRule, MaterialGroup, MaterialLibrary};
pub use regex::MaterialRegex;
pub use services::{MaterialServices, VertexLock};
pub use shading::{InstanceExtents, Shading};
pub use surface::{
    InstanceValue, InstanceValues, SurfaceFactory, SurfaceMaterial, SurfaceServices,
};

/// `GR_OK`.
pub const GR_OK: i32 = 0;
/// `GR_GENERIC`.
pub const GR_GENERIC: i32 = -1;
/// `GR_INTERFACE_UNSUPPORTED`.
pub const GR_INTERFACE_UNSUPPORTED: i32 = -3;
/// The result of `UnsupportedOperation` (`0xFFFFFFF9`).
pub const GR_UNSUPPORTED: i32 = -7;
