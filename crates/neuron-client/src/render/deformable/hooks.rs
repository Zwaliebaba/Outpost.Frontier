//! What the client forwards into a deformable instance (work package P4.7,
//! second increment).
//!
//! In the reference the engine holds each bound bone's runtime as the
//! instance's `IInstanceTransform` (`BindOneBoneRuntime`) and writes every
//! pose into it; the animation system holds each aim event as the owner of
//! the typed channels it made (`CreateTyped(event, ...)`) and calls its
//! `Apply` at every tick. Here the scene engine and the animation system
//! (`neuron_core::scene`, P1.8) keep their own state and the instance owns
//! its bone runtimes and aim events, so the client forwards those calls:
//!
//! * [`TransformCall`] is one `IInstanceTransform` call; the instance finds
//!   the runtime bound to the engine instance and applies it
//!   ([`DeformInstance::forward_transform`]), or takes the engine's world
//!   transforms of all its bound bones at once
//!   ([`DeformInstance::pull_bone_transforms`]).
//! * [`OwnerBind`] is the bind descriptor the channel hands its owner
//!   (`FIkApplyGate`); [`DeformInstance::apply_channel`] finds the aim
//!   event that made the channel and runs `IkApply`, writing the joint's
//!   quaternion into the channel's sample.
//!
//! The engine and the animation system themselves stay with their crate:
//! the engine's calls reach the component through
//! [`DeformHost`](super::DeformHost), and the caller decides when to
//! forward (after the engine updated the instance tree, at the channel's
//! tick).

use super::instance::{DeformInstance, ListPayload};
use super::skeleton::BoneRuntime;
use super::{INSTANCE_NONE, Instance};

/// `GR_OK`.
const GR_OK: u32 = 0;

/// One call the engine makes on an instance's `IInstanceTransform` that a
/// bone runtime answers (slots 3, 5, 7 and 13 of `FBoneRuntime`; the
/// others do nothing).
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum TransformCall {
    /// `SetPosition`.
    Position([f32; 3]),
    /// `SetOrientation`.
    Orientation([f32; 9]),
    /// `SetMatrix`.
    Matrix([f32; 12]),
    /// `MarkWorldDirty`.
    MarkWorldDirty,
}

impl TransformCall {
    /// The call on `bone`: the result the slot returns (`GR_OK` for the
    /// three setters, `MarkWorldDirty`'s answer for the last).
    pub fn apply(&self, bone: &mut BoneRuntime) -> u32 {
        match self {
            Self::Position(p) => {
                bone.set_position(p);
                GR_OK
            }
            Self::Orientation(rows) => {
                bone.set_orientation(rows);
                GR_OK
            }
            Self::Matrix(m) => {
                bone.set_matrix(m);
                GR_OK
            }
            Self::MarkWorldDirty => bone.mark_world_dirty(),
        }
    }
}

/// The bind descriptor a typed channel hands its owner's `Apply`
/// (`FIkApplyGate`): a non-zero first word skips the solve, the word at
/// +0xC is the joint whose relative transform gates it. The aim event's
/// channels are made with the descriptor `{0, 0, 0, joint}`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OwnerBind {
    /// `SkipFlag`.
    pub skip: i32,
    /// `JointAt0C`.
    pub joint: Instance,
}

impl DeformInstance {
    /// The bone runtime bound to the engine instance `instance` (the
    /// transform `SetInstanceTransform` gave it): the first bound bone of
    /// the parts in order whose instance it is.
    pub fn bone_runtime_mut(&mut self, instance: Instance) -> Option<&mut BoneRuntime> {
        if instance == INSTANCE_NONE {
            return None;
        }
        self.parts
            .iter_mut()
            .filter_map(|part| part.lod.as_mut())
            .find_map(|holder| {
                let bound = holder.bound();
                holder.bones[..bound]
                    .iter_mut()
                    .find(|bone| bone.instance == instance)
            })
    }

    /// The engine's call on `instance`'s transform, forwarded to its bone
    /// runtime: the slot's result, `None` when no bound bone is that
    /// instance (the transform is the engine's own).
    pub fn forward_transform(&mut self, instance: Instance, call: TransformCall) -> Option<u32> {
        self.bone_runtime_mut(instance).map(|bone| call.apply(bone))
    }

    /// The engine's world transform of every bound bone, as the engine
    /// writes it after an update: `SetMatrix` with the transform `world`
    /// answers, then `MarkWorldDirty`. Bones `world` answers `None` for
    /// keep their pose. The number of bones written.
    pub fn pull_bone_transforms(
        &mut self,
        mut world: impl FnMut(Instance) -> Option<[f32; 12]>,
    ) -> usize {
        let mut written = 0;
        for holder in self.parts.iter_mut().filter_map(|part| part.lod.as_mut()) {
            let bound = holder.bound();
            for bone in &mut holder.bones[..bound] {
                if bone.instance == INSTANCE_NONE {
                    continue;
                }
                if let Some(m) = world(bone.instance) {
                    TransformCall::Matrix(m).apply(bone);
                    TransformCall::MarkWorldDirty.apply(bone);
                    written += 1;
                }
            }
        }
        written
    }

    /// The owner callback of the typed channel `key` (`FIkEvent::Apply`,
    /// `IkApply`): the aim event of the instance's list that made the
    /// channel solves (on the first tick of a round, unless `bind` skips)
    /// and writes the channel's joint quaternion into `sampled`. The
    /// event's result, `None` when no aim event of this instance owns the
    /// channel.
    pub fn apply_channel<S: super::services::AimHost + ?Sized>(
        &mut self,
        sampled: &mut [f32],
        key: u32,
        bind: &OwnerBind,
        services: &mut S,
    ) -> Option<i32> {
        let event = self
            .list
            .iter_mut()
            .find_map(|node| match &mut node.payload {
                ListPayload::Aim(Some(event)) if event.channels.contains(&key) => Some(event),
                _ => None,
            })?;
        Some(event.apply(sampled, key, bind.skip != 0, bind.joint, services))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::render::deformable::part::DeformPart;
    use crate::render::deformable::skeleton::LodHolder;

    fn instance_with_bones(instances: &[Instance], bound: i32) -> DeformInstance {
        let holder = LodHolder {
            bones: instances
                .iter()
                .map(|&i| BoneRuntime {
                    instance: i,
                    ..BoneRuntime::default()
                })
                .collect(),
            bind_cursor: bound,
            ..LodHolder::default()
        };
        let part = DeformPart {
            lod: Some(holder),
            ..DeformPart::default()
        };
        DeformInstance::with_parts(vec![part], 0, 0)
    }

    #[test]
    fn calls_reach_the_bound_runtime_only() {
        let mut instance = instance_with_bones(&[10, 11, 12], 2);
        instance.parts[0].lod.as_mut().expect("a holder").bones[0].flags = 1;
        let m = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 4.0, 5.0, 6.0];
        assert_eq!(
            instance.forward_transform(10, TransformCall::Matrix(m)),
            Some(0)
        );
        let bones = &instance.parts[0].lod.as_ref().expect("a holder").bones;
        assert_eq!(bones[0].local, m);
        assert_eq!(bones[0].flags, 3, "a root's move marks it dirty");
        // Bone 12 is past the bound count; -1 is no instance.
        assert_eq!(
            instance.forward_transform(12, TransformCall::MarkWorldDirty),
            None
        );
        assert_eq!(
            instance.forward_transform(-1, TransformCall::MarkWorldDirty),
            None
        );
        assert_eq!(
            instance.forward_transform(11, TransformCall::Position([1.0, 2.0, 3.0])),
            Some(0)
        );
        let bones = &instance.parts[0].lod.as_ref().expect("a holder").bones;
        assert_eq!(bones[1].local[9..], [1.0, 2.0, 3.0]);
        assert_eq!(bones[1].flags, 0, "no root: not dirty until marked");
        assert_eq!(
            instance.forward_transform(11, TransformCall::MarkWorldDirty),
            Some(1)
        );
        assert_eq!(
            instance.forward_transform(11, TransformCall::MarkWorldDirty),
            Some(0)
        );
    }

    #[test]
    fn pulled_transforms_mark_every_bound_bone() {
        let mut instance = instance_with_bones(&[10, -1, 12], 3);
        let n = instance.pull_bone_transforms(|i| {
            (i != 12).then_some([0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0])
        });
        assert_eq!(n, 1);
        let bones = &instance.parts[0].lod.as_ref().expect("a holder").bones;
        assert_eq!(bones[0].flags & 2, 2);
        assert_eq!(bones[2].flags & 2, 0);
    }
}
