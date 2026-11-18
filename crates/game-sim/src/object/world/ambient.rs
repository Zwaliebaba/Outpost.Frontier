//! The ambient animations of equipment (`CEInternalFX`'s `use_animation`,
//! such as a station's turning tower) and their part of `AnimDB::Update`
//! (`CppSource/Common/common_animdb.cpp`): the playback-map entries,
//! `MixerWalk` of their roots, the instance update and the golem targets
//! of the free parts the animation moves (Phase E, mode 1).

use std::sync::Arc;

use neuron_core::anim::AnimScript;
use neuron_core::math::{Vector, matrix_to_quaternion};
use neuron_core::scene::InstanceId;
use neuron_core::scene::typed::MixerKey;
use neuron_core::vfs::{FsDesc, UtfFileSystem};

use crate::object::ObjHandle;

use super::World;

/// An `AnimDB` playback-map entry (`FAnimSub`): the root object of the
/// playback and its members, the objects whose own root instance a joint
/// channel of the playback drives.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct AmbientSub {
    /// `Root`: `GetRoot` of the last instance a joint channel resolved.
    pub root: ObjHandle,
    /// `Members`: each a free part of `root`, by its root instance.
    pub members: Vec<InstanceId>,
}

/// The object an instance belongs to (`GetInstanceUserData`), as far as
/// the ambient animations need it: a free part of the owner (by its index)
/// or the owner itself.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum InstanceOwner {
    Owner,
    FreePart(usize),
}

impl World {
    /// The animation script of an archetype file (`get_script_index`),
    /// loaded once.
    fn anim_script(&mut self, path: &[u8]) -> Option<Arc<AnimScript>> {
        if let Some(cached) = self.anim_scripts.get(path) {
            return cached.clone();
        }
        let script = self.assets.as_ref().and_then(|assets| {
            if path.is_empty() {
                return None;
            }
            let mut fs = UtfFileSystem::open(
                &assets.host,
                &FsDesc::read(path).with_implementation(b"UTF"),
            )
            .ok()?;
            AnimScript::open(&mut fs).ok().flatten().map(Arc::new)
        });
        self.anim_scripts.insert(path.to_vec(), script.clone());
        script
    }

    /// Which of the owner's objects an instance belongs to.
    fn instance_owner(&self, owner: ObjHandle, instance: InstanceId) -> InstanceOwner {
        let Some(o) = self.objects.get(owner) else {
            return InstanceOwner::Owner;
        };
        o.physical
            .free_parts
            .iter()
            .position(|p| p.part_map.iter().any(|e| e.instance == Some(instance)))
            .map_or(InstanceOwner::Owner, InstanceOwner::FreePart)
    }

    /// `CEInternalFX::CEInternalFX`'s ambient playback: `PlayScript` of
    /// the animation on the owner's instance, `EvalPlayback(id, 2, -2, 1,
    /// 0, 1, 1, 0)` and `AnimDB::Add(id)`.
    pub(crate) fn start_ambient(&mut self, owner: ObjHandle, name: &[u8]) {
        let Some(o) = self.objects.get(owner) else {
            return;
        };
        let (kind, archetype, root) = (o.kind, o.archetype, o.engine.instance);
        let path = self.archetype_path(kind, archetype);
        let Some(script) = self.anim_script(&path) else {
            return;
        };
        let Some(id) = self
            .animation
            .play_script(&self.scene, &script, root, name, 0, 0)
        else {
            return;
        };
        self.animation
            .eval_playback(&self.scene, id, 2, -2.0, 1.0, 0.0, 1.0, 1.0, None);
        self.ambient_add(owner, id);
    }

    /// `AnimDB::Add(PlaybackId)`: the entry once per id, its members the
    /// objects whose root instance a joint channel drives, its root the
    /// owner.
    fn ambient_add(&mut self, owner: ObjHandle, id: u32) {
        let key = i32::from_ne_bytes(id.to_ne_bytes());
        if self.ambient.contains_key(&key) {
            return;
        }
        let typed = self
            .animation
            .playback(id)
            .map(|p| p.typed.clone())
            .unwrap_or_default();
        let mut members = Vec::new();
        for t in typed {
            let Some(channel) = self.animation.channel(t) else {
                continue;
            };
            if channel.bind.kind != 0 {
                continue;
            }
            let Some(instance) = channel.bind.instance else {
                continue;
            };
            // A member is an object whose own root instance is the
            // channel's: here a free part whose part instance it is.
            if let InstanceOwner::FreePart(k) = self.instance_owner(owner, instance)
                && self
                    .objects
                    .get(owner)
                    .and_then(|o| o.physical.free_parts.get(k))
                    .and_then(|p| p.part_map.first())
                    .is_some_and(|e| e.instance == Some(instance))
            {
                members.push(instance);
            }
        }
        self.ambient.insert(
            key,
            AmbientSub {
                root: owner,
                members,
            },
        );
    }

    /// The ambient playbacks of a removed object end (`AnimDB::Rem`,
    /// `DestroyScript`).
    pub(crate) fn end_ambient(&mut self, h: ObjHandle) {
        let ids: Vec<i32> = self
            .ambient
            .iter()
            .filter(|(_, s)| s.root == h)
            .map(|(k, _)| *k)
            .collect();
        for key in ids {
            self.ambient.remove(&key);
            self.animation
                .destroy_script(&self.scene, u32::from_ne_bytes(key.to_ne_bytes()));
        }
    }

    /// `AnimDB::Update` Phase A for the ambient playbacks: `MixerWalk` of
    /// each entry's root instance once. Returns the roots walked, for the
    /// instance update (Phase C).
    pub(crate) fn walk_ambient(&mut self, dt: f32) -> Vec<InstanceId> {
        let mut seen: Vec<InstanceId> = Vec::new();
        let roots: Vec<Option<InstanceId>> = self
            .ambient
            .values()
            .map(|s| self.objects.get(s.root).and_then(|o| o.engine.instance))
            .collect();
        for root in roots.into_iter().flatten() {
            if seen.contains(&root) {
                continue;
            }
            seen.push(root);
            let _ = self
                .animation
                .mixer_walk(&mut self.scene, MixerKey::of(Some(root)), dt);
        }
        seen
    }

    /// `AnimDB::Update` Phases D and E for the ambient playbacks whose root
    /// has a body: every member and the objects below it
    /// (`CollectInstanceChildren`), and for each free part (control
    /// mode one) its golem's target set to the part's frame now, at rest,
    /// stamped with the physics time. The attachments below a member
    /// (control mode two, `BeamBodyToWorldPose`) are not moved (D167).
    pub(crate) fn push_ambient_targets(&mut self) {
        let mut parts: Vec<(ObjHandle, usize)> = Vec::new();
        let subs: Vec<AmbientSub> = self.ambient.values().cloned().collect();
        for sub in subs {
            let Some(o) = self.objects.get(sub.root) else {
                continue;
            };
            if o.physical.body.is_none() {
                continue;
            }
            for member in &sub.members {
                if let InstanceOwner::FreePart(k) = self.instance_owner(sub.root, *member)
                    && !parts.contains(&(sub.root, k))
                {
                    parts.push((sub.root, k));
                }
                self.collect_part_children(sub.root, *member, &mut parts);
            }
        }
        for (owner, k) in parts {
            let Some(o) = self.objects.get(owner) else {
                continue;
            };
            let system = o.physical.system;
            let Some(part) = o.physical.free_parts.get(k) else {
                continue;
            };
            let body = part.body;
            let instance = part.part_map.first().and_then(|e| e.instance);
            let (orientation, position) = self.instance_frame(instance);
            let quat = matrix_to_quaternion(&orientation);
            if let Some(world) = self.physics.world_mut(system) {
                let now = world.current_time();
                world.golem_set_target_position(body, position, Vector::ZERO, now);
                world.golem_set_target_quaternion(body, quat, now, None, 0.0);
            }
        }
    }

    /// `CollectInstanceChildren`: the free parts below an instance, depth
    /// first, each once.
    fn collect_part_children(
        &self,
        owner: ObjHandle,
        instance: InstanceId,
        out: &mut Vec<(ObjHandle, usize)>,
    ) {
        let this = self.instance_owner(owner, instance);
        let mut cursor = None;
        while let Some(child) = self.scene.related_instance(Some(instance), 1, cursor) {
            cursor = Some(child);
            let child_owner = self.instance_owner(owner, child);
            if child_owner != this
                && let InstanceOwner::FreePart(k) = child_owner
            {
                if !out.contains(&(owner, k)) {
                    out.push((owner, k));
                }
            }
            self.collect_part_children(owner, child, out);
        }
    }
}
