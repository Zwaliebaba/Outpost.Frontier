//! Materials at run time (work package P4.5, first increment): the parts of
//! `CppSource/Shading/` that do not draw.
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
//!
//! The 22 material kinds of `Material.cpp` and `BatchedMaterial` implement
//! [`Material`] in the next increment, with a factory that makes them by
//! name; the texture-stage work they do goes to the device model
//! ([`crate::render::ff`]).
//!
//! No platform code: everything here builds and tests on every host.

pub mod animation;
pub mod batcher;
pub mod global;
pub mod instance;
pub mod library;
pub mod regex;

pub use animation::{AnimationData, AnimationInstance, GroupId, InstanceId, MaterialAnimations};
pub use batcher::{
    BatchDevice, BatchRule, BatcherConfig, BatcherEvents, BinState, DrawArgs, IbScratch,
    MaterialBatcher, SubmitContext, SubmitInput, VbListing, VertexSource, VertexStream,
};
pub use global::{GlobalRecord, GlobalRecords};
pub use instance::{Material, MaterialFactory, MaterialId, Materials};
pub use library::{LibraryLoad, MapEntry, MapRule, MaterialGroup, MaterialLibrary};
pub use regex::MaterialRegex;

/// `GR_OK`.
pub const GR_OK: i32 = 0;
/// `GR_GENERIC`.
pub const GR_GENERIC: i32 = -1;
/// `GR_INTERFACE_UNSUPPORTED`.
pub const GR_INTERFACE_UNSUPPORTED: i32 = -3;
/// The result of `UnsupportedOperation` (`0xFFFFFFF9`).
pub const GR_UNSUPPORTED: i32 = -7;
