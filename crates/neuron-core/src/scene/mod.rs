//! The scene runtime (work package P1.8): `CppSource/SceneRuntime/`
//! (`SceneRuntime.cpp`, `SceneRuntimeMixer.cpp`, `SceneRuntime.h`) and its
//! interfaces in `CppSource/include/engine/sceneruntime.h`. Nothing here
//! draws; the library makes no renderer calls.
//!
//! * [`engine`]: archetypes (compound models from `Cmpnd`: parts and
//!   `Cons` constraints), instance trees, joints, transforms and extents
//!   (`IEngine`).
//! * [`joint`]: joint types, the 23-dword joint payload and the local
//!   transform of each type.
//! * [`hardpoint`]: the `Hardpoint` engine component (`Hardpoints\Fixed`,
//!   `Hardpoints\Revolute`) and hardpoint connections.
//! * [`sample`]: channel sampling (frame lookup, the fraction between two
//!   frames, event keys) over the channel archetypes of [`crate::anim`].
//! * [`typed`]: typed channels, which play a channel onto a joint, an
//!   object or a sink.
//! * [`mixer`]: the mixer (leaf, blend and overlay nodes).
//! * [`animation`]: scripts playing on instances (`IAnimation2`) and free
//!   channels (`IChannel`).
//!
//! The component manager, reference-counted COM facets and the engine's
//! `[Engine]` profile walk are gone (decision T9): an [`Engine`] owns its
//! archetypes, instances and [`EngineComponent`]s; an [`Animation`] owns
//! its playbacks, typed channels and mixer, and takes the engine it works
//! on as an argument (the reference's `G_AnimEngine`). Archetypes,
//! instances and channels are named by arena ids where the reference uses
//! pointers. An instance's world transform is always the engine's own
//! (`FInstanceTransform`); transforms a caller supplies are the client's
//! concern.
//!
//! Loading is Tier A: the same file system calls in the same order, the
//! same payload bits. Evaluation is Tier B: `f64` arithmetic rounded to
//! `f32` at each store the reference makes (decision T11); the joint
//! angles go through `sin` and `cos`, the mixer's ease table through `cos`.
//!
//! Where the reference has undefined behaviour (null dereferences on
//! tracks without a channel, a division by a record size of 0, reads of
//! uninitialised members, cyclic or doubly connected parts), the port
//! defines it; the divergence register lists each case.

pub mod animation;
pub mod engine;
pub mod hardpoint;
pub mod joint;
pub mod mixer;
pub mod sample;
pub mod typed;

#[cfg(test)]
mod tests;

use crate::utf::UtfError;
use crate::vfs::{FileSystem, FindEntry, VfsError};

pub use animation::{Animation, FIRST_PLAYBACK_ID, Playback};
pub use engine::{
    Archetype, ArchetypeId, Constraint, Engine, EngineComponent, Instance, InstanceId,
    InstanceTransform, PartChild,
};
pub use hardpoint::{
    HardpointData, HardpointRecord, Hardpoints, connect_hardpoints, connect_payload,
};
pub use joint::{JointKind, JointPayload, PAYLOAD_DWORDS};
pub use typed::{BindDesc, ChannelEvent, MixerKey, PlayHeader, TypedChannel, TypedClass, TypedId};

/// A failure the reference has no defined behaviour for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SceneError {
    /// The archive is malformed where the reference reads outside it.
    Malformed(UtfError),
}

impl core::fmt::Display for SceneError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::Malformed(e) => write!(f, "malformed scene archive: {e}"),
        }
    }
}

impl std::error::Error for SceneError {}

/// `Ok(Some)` for success, `Ok(None)` for a failure the reference handles,
/// `Err` for a malformed archive.
fn soft<T>(result: Result<T, VfsError>) -> Result<Option<T>, SceneError> {
    match result {
        Ok(value) => Ok(Some(value)),
        Err(VfsError::Malformed(error)) => Err(SceneError::Malformed(error)),
        Err(_) => Ok(None),
    }
}

/// `OpenNode(name)`: whether it succeeded.
fn open_node(fs: &mut dyn FileSystem, name: &[u8]) -> Result<bool, SceneError> {
    Ok(soft(fs.open_node(name))?.is_some())
}

/// `!(a >= b)` as the x87 compare answers it: true when `a` is ordered
/// below `b` or either is NaN.
fn not_ge(a: f64, b: f64) -> bool {
    a.partial_cmp(&b).is_none_or(core::cmp::Ordering::is_lt)
}

/// `!(a <= b)`: true when `a` is ordered above `b` or either is NaN.
fn not_le(a: f64, b: f64) -> bool {
    a.partial_cmp(&b).is_none_or(core::cmp::Ordering::is_gt)
}

/// `!(a < b)`: true unless `a` is ordered below `b`.
fn not_lt(a: f64, b: f64) -> bool {
    a.partial_cmp(&b).is_none_or(core::cmp::Ordering::is_ge)
}

/// `!(a > b)`: true unless `a` is ordered above `b`.
fn not_gt(a: f64, b: f64) -> bool {
    a.partial_cmp(&b).is_none_or(core::cmp::Ordering::is_le)
}

/// `FindFirst` / `FindNext`: the entries, none where the find fails.
fn find(fs: &dyn FileSystem, pattern: &[u8]) -> Result<Vec<FindEntry>, SceneError> {
    Ok(soft(fs.find(pattern))?.unwrap_or_default())
}
