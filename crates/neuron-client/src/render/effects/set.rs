//! The live effects (`effect_base.cpp`, `effect_type_list.cpp` and
//! `EffectTypeBucketWalk` of `space_scene_obj_draw.cpp`): the effect head
//! every kind shares ([`EffectBase`]), the 47 per-type buckets the frame
//! walks, newest first ([`EffectSet`]), and the walk itself.
//!
//! The reference links an effect into its type's bucket with a reference
//! of its own and drops that reference when it unlinks it; the effect is
//! deleted when its count falls to zero. [`EffectSet`] holds the effects
//! and keeps the same counts, so an effect lives exactly as long.
//!
//! What the reference reads from the engine (an instance's transform, the
//! velocity of its object, whether the watched object still lives) comes
//! from the caller's [`EffectHost`].

use std::collections::VecDeque;

use neuron_core::math::{Transform, Vector, mul_transform};

use super::batch::EffectVertexBatch;

/// `kEffectTypeBucketCount`: one bucket per effect type.
pub const EFFECT_TYPE_BUCKETS: usize = 0x2f;

/// An effect in an [`EffectSet`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct EffectId(usize);

/// What the effects read from the scene engine.
pub trait EffectHost {
    /// `IEngine::GetInstanceTransform`.
    fn instance_transform(&self, instance: i32) -> Transform;
    /// `GetInstanceUserData` then `CObject::get_velocity`: `None` when the
    /// instance has no object.
    fn instance_velocity(&self, instance: i32) -> Option<Vector>;
    /// `EffectWatchedObject`: whether the watched object still lives.
    fn watched(&self, watch: u32) -> bool;
}

/// The camera as the effects read it (`FSpaceCamera`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EffectCamera {
    /// Slot 0x40, the view transform.
    pub view: Transform,
    /// Slot 0x20.
    pub z_near: f32,
    /// +0x80, `normalise(1, 0, tan(half_fov_x))`.
    pub plane_x: [f32; 3],
    /// +0x8c, `normalise(0, 1, 0.75 tan(half_fov_x))`.
    pub plane_y: [f32; 3],
}

/// An effect type's numbers that the walk and the kinds read
/// (`FEffectTypeInfo`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EffectTypeParams {
    /// +0x00.
    pub priority: f32,
    /// +0x14 `EXIST_OFFSCREEN`: an effect off screen keeps running.
    pub visibility_offscreen: bool,
    /// `pbubble`'s near distance.
    pub pbubble_near: f32,
    /// `pbubble`'s far distance.
    pub pbubble_far: f32,
}

/// The frame's inputs to the walk besides the camera.
pub struct WalkFrame<'a> {
    /// The scene engine.
    pub host: &'a dyn EffectHost,
    /// The type table, indexed by an effect's type.
    pub types: &'a [EffectTypeParams],
    /// `GetPerfGenericEffectPerf()`.
    pub generic_effect_perf: f32,
    /// `GetGameOption(0x26) != 0`.
    pub particles: bool,
    /// `IsFirstPersonCameraCurrent()`.
    pub first_person: bool,
    /// `ShouldSkipLocalServerTick()`: single player with the server paused.
    pub server_paused: bool,
    /// Whether `G_BeamEffectTexture` (`code_beam`) was found.
    pub beam_texture: bool,
}

/// `FEffectBase` (+0x04..+0x6b), the head every kind shares.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EffectBase {
    /// +0x08 bit 0 live (lights), bit 1 dead.
    pub flags: u8,
    /// +0x0c.
    pub priority: f32,
    /// +0x10 the transform in the instance's frame, or in the world.
    pub local: Transform,
    /// +0x40 the engine instance, −1 for none.
    pub instance: i32,
    /// +0x44 the watched object, as the caller's handle.
    pub watch: Option<u32>,
    /// +0x4c the velocity the walk moves a free effect by.
    pub velocity: Vector,
    /// +0x58.
    pub ref_count: i32,
    /// +0x5c.
    pub type_id: usize,
    /// +0x68 non-null: linked into its bucket.
    pub linked: bool,
}

impl EffectBase {
    /// `EffectBaseConstruct`: priority 1, the identity, no instance, one
    /// reference.
    #[must_use]
    pub fn new(type_id: usize) -> Self {
        Self {
            flags: 0,
            priority: 1.0,
            local: Transform::IDENTITY,
            instance: -1,
            watch: None,
            velocity: Vector::new(0.0, 0.0, 0.0),
            ref_count: 1,
            type_id,
            linked: false,
        }
    }

    /// Bit 1 of +0x08.
    #[must_use]
    pub fn dead(&self) -> bool {
        self.flags & 2 != 0
    }

    /// `FEffectBase::Attach`: the watched object, the instance and the
    /// local transform; the velocity zeroed.
    pub fn attach(&mut self, watch: Option<u32>, instance: i32, local: Transform) {
        self.watch = watch;
        self.instance = instance;
        self.local = local;
        self.velocity = Vector::new(0.0, 0.0, 0.0);
    }

    /// `EffectWatchedObject` is non-null.
    #[must_use]
    pub fn watching(&self, host: &dyn EffectHost) -> bool {
        self.watch.is_some_and(|w| host.watched(w))
    }

    /// `EffectGetWorldTransform`: the instance's transform times the local
    /// one while the watched object lives and the instance is set,
    /// otherwise the local one.
    #[must_use]
    pub fn world_transform(&self, host: &dyn EffectHost) -> Transform {
        if self.watching(host) && self.instance != -1 {
            mul_transform(&host.instance_transform(self.instance), &self.local)
        } else {
            self.local
        }
    }

    /// `EffectBakeInstance`: the object's velocity, then the world
    /// transform kept as the local one, and the instance dropped.
    fn bake_instance(&mut self, host: &dyn EffectHost) {
        self.velocity = host
            .instance_velocity(self.instance)
            .unwrap_or(Vector::new(0.0, 0.0, 0.0));
        self.local = self.world_transform(host);
        self.instance = -1;
    }

    /// `EffectFreezeToWorld` for a linked effect: an effect on an instance
    /// leaves it, keeping its world pose and its object's velocity.
    ///
    /// # Panics
    ///
    /// On an instance whose watched object is gone, where the reference
    /// calls itself until its stack runs out (D1404).
    pub fn freeze_to_world(&mut self, host: &dyn EffectHost) {
        if self.instance == -1 {
            return;
        }
        assert!(
            self.watching(host),
            "EffectFreezeToWorld on instance {} without its object: the reference recurses without end",
            self.instance
        );
        self.bake_instance(host);
    }

    /// `FEffectBase::Kill` for a linked effect: frozen to the world and
    /// marked dead.
    ///
    /// # Panics
    ///
    /// As [`Self::freeze_to_world`] (D1404).
    pub fn kill(&mut self, host: &dyn EffectHost) {
        self.freeze_to_world(host);
        self.flags |= 2;
    }
}

/// One kind of effect (the virtual slots of `FEffectBase`).
pub trait SpaceEffect {
    /// The shared head.
    fn base(&self) -> &EffectBase;
    /// The shared head.
    fn base_mut(&mut self) -> &mut EffectBase;
    /// Slot +0x04, after the set has linked the effect.
    fn kill(&mut self, frame: &WalkFrame<'_>);
    /// Slot +0x10.
    fn is_alive(&self) -> bool;
    /// Slot +0x14: whether the effect is visible this frame.
    fn update_visibility(&mut self, dt: f32, cam: &EffectCamera, frame: &WalkFrame<'_>) -> bool;
    /// Slot +0x18: false when the effect has finished.
    fn tick_draw(&mut self, dt: f32, cam: &EffectCamera, frame: &WalkFrame<'_>) -> bool;
    /// Slot +0x1c.
    fn build_geometry(
        &mut self,
        cam: &EffectCamera,
        frame: &WalkFrame<'_>,
        batch: &mut EffectVertexBatch,
    );
}

/// The live effects and `G_EffectTypeBuckets`.
pub struct EffectSet<K> {
    effects: Vec<Option<K>>,
    free: Vec<usize>,
    buckets: Vec<VecDeque<EffectId>>,
}

impl<K> Default for EffectSet<K> {
    fn default() -> Self {
        Self {
            effects: Vec::new(),
            free: Vec::new(),
            buckets: vec![VecDeque::new(); EFFECT_TYPE_BUCKETS],
        }
    }
}

impl<K: SpaceEffect> EffectSet<K> {
    /// A new effect with the caller's reference (its count as made).
    pub fn insert(&mut self, effect: K) -> EffectId {
        if let Some(i) = self.free.pop() {
            self.effects[i] = Some(effect);
            EffectId(i)
        } else {
            self.effects.push(Some(effect));
            EffectId(self.effects.len() - 1)
        }
    }

    /// The effect, while it lives.
    #[must_use]
    pub fn get(&self, id: EffectId) -> Option<&K> {
        self.effects.get(id.0).and_then(Option::as_ref)
    }

    /// The effect, while it lives.
    pub fn get_mut(&mut self, id: EffectId) -> Option<&mut K> {
        self.effects.get_mut(id.0).and_then(Option::as_mut)
    }

    /// The live effects.
    pub fn iter(&self) -> impl Iterator<Item = (EffectId, &K)> {
        self.effects
            .iter()
            .enumerate()
            .filter_map(|(i, e)| e.as_ref().map(|e| (EffectId(i), e)))
    }

    /// A bucket's effects, head first.
    #[must_use]
    pub fn bucket(&self, type_id: usize) -> &VecDeque<EffectId> {
        &self.buckets[type_id]
    }

    fn live(&mut self, id: EffectId) -> &mut K {
        self.get_mut(id)
            .unwrap_or_else(|| panic!("effect {} used after its release", id.0))
    }

    /// `FEffectBase::AddRef`.
    pub fn add_ref(&mut self, id: EffectId) {
        self.live(id).base_mut().ref_count += 1;
    }

    /// `FEffectBase::Release`: the effect is deleted when no reference is
    /// left; the answer is the count after.
    pub fn release(&mut self, id: EffectId) -> i32 {
        let base = self.live(id).base_mut();
        base.ref_count -= 1;
        let left = base.ref_count;
        if left < 1 {
            self.effects[id.0] = None;
            self.free.push(id.0);
        }
        left
    }

    /// `EffectTypeListLink`: a live, unlinked effect goes to the front of
    /// its type's bucket with a reference.
    ///
    /// # Panics
    ///
    /// On a type beyond the buckets, which the reference does not test.
    pub fn link(&mut self, id: EffectId) {
        let base = self.live(id).base_mut();
        if base.linked || base.dead() {
            return;
        }
        base.linked = true;
        base.ref_count += 1;
        let type_id = base.type_id;
        self.buckets[type_id].push_front(id);
    }

    /// `EffectTypeListUnlinkAndRelease`: a linked effect leaves its bucket
    /// and the link's reference is dropped.
    pub fn unlink_and_release(&mut self, id: EffectId) {
        let base = self.live(id).base_mut();
        if !base.linked {
            return;
        }
        base.linked = false;
        let type_id = base.type_id;
        let bucket = &mut self.buckets[type_id];
        if let Some(at) = bucket.iter().position(|&e| e == id) {
            bucket.remove(at);
        }
        self.release(id);
    }

    /// Slot +0x04 from outside a walk: the effect is linked first, as each
    /// kind's kill does.
    pub fn kill(&mut self, id: EffectId, frame: &WalkFrame<'_>) {
        self.link(id);
        self.live(id).kill(frame);
    }

    /// `EffectTypeListKillAll`: every bucket emptied, each head killed then
    /// unlinked.
    pub fn kill_all(&mut self, frame: &WalkFrame<'_>) {
        for type_id in 0..EFFECT_TYPE_BUCKETS {
            while let Some(&id) = self.buckets[type_id].front() {
                self.live(id).kill(frame);
                self.unlink_and_release(id);
            }
        }
    }

    /// `EffectTypeBucketWalk`: each effect of a type, head first. A dead one
    /// is unlinked. A free one moves by its velocity. Then its visibility:
    /// one that is not visible, of a type that does not run off screen, is
    /// frozen and marked dead; any other ticks, and builds its geometry when
    /// it is visible and still drawing. Last, a dead or finished effect is
    /// unlinked.
    ///
    /// # Panics
    ///
    /// On a type beyond `frame.types`, which the reference does not test.
    pub fn walk(
        &mut self,
        type_id: usize,
        dt: f32,
        cam: &EffectCamera,
        frame: &WalkFrame<'_>,
        batch: &mut EffectVertexBatch,
    ) {
        let order: Vec<EffectId> = self.buckets[type_id].iter().copied().collect();
        for id in order {
            let effect = self.live(id);
            if effect.base().dead() {
                self.unlink_and_release(id);
                continue;
            }
            let base = effect.base_mut();
            if base.instance == -1 {
                // The x and y products stay on the FPU; z is stored first.
                let (v, p) = (base.velocity, &mut base.local.translation);
                let step_z = (f64::from(dt) * f64::from(v.z)) as f32;
                p.x = (f64::from(dt) * f64::from(v.x) + f64::from(p.x)) as f32;
                p.y = (f64::from(dt) * f64::from(v.y) + f64::from(p.y)) as f32;
                p.z = (f64::from(step_z) + f64::from(p.z)) as f32;
            }
            let visible = effect.update_visibility(dt, cam, frame);
            let offscreen = frame.types[effect.base().type_id].visibility_offscreen;
            if !offscreen && !visible {
                // The link is a no-op: the effect is in the bucket.
                let base = effect.base_mut();
                base.freeze_to_world(frame.host);
                base.flags |= 2;
            } else if effect.tick_draw(dt, cam, frame) && visible {
                effect.build_geometry(cam, frame, batch);
            }
            if !effect.base().dead() && effect.is_alive() {
                continue;
            }
            self.unlink_and_release(id);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::render::effects::beam::{BEAM_EFFECT_TYPE, BeamEffect, BeamShape};
    use neuron_core::math::Matrix;

    struct Host {
        alive: bool,
    }

    impl EffectHost for Host {
        fn instance_transform(&self, _instance: i32) -> Transform {
            Transform::new(Matrix::IDENTITY, Vector::new(10.0, 0.0, 0.0))
        }
        fn instance_velocity(&self, _instance: i32) -> Option<Vector> {
            Some(Vector::new(0.0, 0.0, 5.0))
        }
        fn watched(&self, _watch: u32) -> bool {
            self.alive
        }
    }

    const TYPES: [EffectTypeParams; EFFECT_TYPE_BUCKETS] = [EffectTypeParams {
        priority: 1.0,
        visibility_offscreen: false,
        pbubble_near: 1000.0,
        pbubble_far: 2000.0,
    }; EFFECT_TYPE_BUCKETS];

    fn frame(host: &Host) -> WalkFrame<'_> {
        WalkFrame {
            host,
            types: &TYPES,
            generic_effect_perf: 1.0,
            particles: true,
            first_person: false,
            server_paused: false,
            beam_texture: true,
        }
    }

    /// Looking down −z from the origin, 90° across.
    const CAM: EffectCamera = EffectCamera {
        view: Transform::IDENTITY,
        z_near: 1.0,
        plane_x: [0.70710677, 0.0, 0.70710677],
        plane_y: [0.0, 0.8, 0.6],
    };

    fn beam_at(z: f32) -> BeamEffect {
        let mut beam = BeamEffect::new(BEAM_EFFECT_TYPE);
        beam.shape = Some(BeamShape {
            segment_mode: 0,
            head_texture: 0,
            trail_texture: 0,
            tip_color: [1.0; 3],
            tail_color: [1.0; 3],
            core_color: [1.0; 3],
            sec_core_color: [1.0; 3],
            outer_color: [1.0; 3],
            sec_outer_color: [1.0; 3],
            head_brightness: 1.0,
            trail_brightness: 1.0,
            tip_length: 2.0,
            tail_length: 8.0,
            core_length: 1.0,
            core_width: 1.0,
            head_width: 1.0,
            flash_size: 2.0,
        });
        beam.base.local.translation = Vector::new(0.0, 0.0, z);
        beam
    }

    #[test]
    fn links_hold_a_reference_and_the_newest_comes_first() {
        let mut set = EffectSet::default();
        let a = set.insert(beam_at(-10.0));
        let b = set.insert(beam_at(-20.0));
        set.link(a);
        set.link(b);
        set.link(a);
        assert_eq!(
            set.bucket(BEAM_EFFECT_TYPE)
                .iter()
                .copied()
                .collect::<Vec<_>>(),
            [b, a]
        );
        assert_eq!(set.get(a).map(|e| e.base.ref_count), Some(2));
        // The caller lets go; the link keeps the effect until it is unlinked.
        assert_eq!(set.release(a), 1);
        set.unlink_and_release(a);
        assert!(set.get(a).is_none());
        assert_eq!(set.bucket(BEAM_EFFECT_TYPE).len(), 1);
    }

    #[test]
    fn the_walk_draws_the_visible_and_drops_the_hidden() {
        let host = Host { alive: false };
        let mut set = EffectSet::default();
        let near = set.insert(beam_at(-10.0));
        // Behind the camera: not visible, and the type does not run off
        // screen, so it is frozen, marked dead and unlinked.
        let behind = set.insert(beam_at(100.0));
        for id in [near, behind] {
            set.link(id);
            set.release(id);
        }
        let mut batch = EffectVertexBatch::default();
        set.walk(BEAM_EFFECT_TYPE, 0.05, &CAM, &frame(&host), &mut batch);
        assert!(set.get(behind).is_none());
        // Visible, so drawn; BuildGeometry clears the bit it was given.
        let beam = set.get(near).expect("the visible beam");
        assert_eq!((beam.state.flags, beam.state.progress), (0, 0.0));
        assert_eq!(batch.vertices.len(), 12);
        // The second frame grows it.
        batch.reset();
        set.walk(BEAM_EFFECT_TYPE, 0.05, &CAM, &frame(&host), &mut batch);
        assert_eq!(set.get(near).map(|b| b.state.progress), Some(0.05));
    }

    #[test]
    fn a_killed_beam_fades_then_dies() {
        let host = Host { alive: true };
        let mut set = EffectSet::default();
        let mut beam = beam_at(-10.0);
        beam.base.attach(Some(7), 3, beam.base.local);
        let id = set.insert(beam);
        set.kill(id, &frame(&host));
        // Linked by the kill, frozen at the instance's pose with its
        // object's velocity, and fading.
        let beam = set.get(id).expect("the beam");
        assert!(beam.base.linked && !beam.base.dead());
        assert_eq!(beam.base.instance, -1);
        assert_eq!(beam.base.local.translation, Vector::new(10.0, 0.0, -10.0));
        assert_eq!(beam.state.lifetime, 0.3);
        set.release(id);
        let mut batch = EffectVertexBatch::default();
        let mut frames = 0;
        while set.get(id).is_some() {
            set.walk(BEAM_EFFECT_TYPE, 0.1, &CAM, &frame(&host), &mut batch);
            frames += 1;
        }
        // 0.3 - 0.1 is a tie in f32 that rounds up, so a residue is left
        // after three frames of 0.1 s.
        assert_eq!(frames, 4);
    }

    #[test]
    #[should_panic(expected = "recurses without end")]
    fn freezing_an_orphaned_instance_is_the_reference_s_endless_recursion() {
        let host = Host { alive: false };
        let mut base = EffectBase::new(0);
        base.attach(Some(1), 3, Transform::IDENTITY);
        base.freeze_to_world(&host);
    }
}
