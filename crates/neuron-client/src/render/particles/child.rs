//! The two appearances that make no billboards (`FxParticleAppearance` and
//! `FxMeshAppearance` in `particlefx_appearances.cpp`, the particle and mesh
//! branches of `ReleaseAppearancePoolItem` in `particlefx_common.cpp`).
//!
//! A particle appearance's particle is a record of the particle pool that
//! carries a child effect, the life effect (`ParticleApp_LifeName`), made
//! at the spawn under the appearance node, so the tree walks compose,
//! update and draw it as the appearance's children. Each frame the record
//! moves and hands the life effect its transform, turned along the
//! velocity when `ParticleApp_UseDynamicRotation` is set (smoothed by a
//! slerp with `ParticleApp_SmoothRotation`). Past its duration the record
//! stops its life effect's emitters; the compose walk then makes its death
//! effect (`ParticleApp_DeathName`) where it is, and the record goes once
//! both effects have finished.
//!
//! A mesh appearance's particle holds a mesh of the client's mesh factory
//! ([`FxMeshFactory`]): asked for at the spawn with the particle's world
//! position and velocity, posed by the compose walk (on the particle's
//! position, or through `MeshApp_ParticleTransform` at the particle's
//! time), updated each frame and drawn by the draw walk.

use std::rc::Rc;

use neuron_core::fx_data::{self as fx, NodeClass};
use neuron_core::math::{
    Quaternion, Transform, Vector, mul_quat, quat_slerp, quaternion_to_matrix, transform_xform,
};

use super::ParticleFx;
use super::bounds::merge_particle_sphere;
use super::draw::GR_GENERIC;
use super::effect::Effect;
use super::emitter::inv_sqrt_guess;
use super::node::{FLAG_DONE, NodeId};
use super::pool::{ParticleRecord, PoolKind};
use super::props::PropsData;
use super::ramp::{IDENTITY, mul, round};
use super::services::{Camera, ParticleDraw, ParticleServices};

/// `0.99f`, the bound past which the velocity's turn takes the other axis.
const AXIS_BOUND: f64 = 0.990_000_009_536_743_2;
/// The twist about z of the other axis's turn: `0xbfe921fb51000000`, about
/// -pi/4.
const TWIST: u64 = 0xBFE9_21FB_5100_0000;
/// `0.01f` (`0x3c23d70a`), the smooth rotation's step for a slow particle.
const SLOW_STEP: u32 = 0x3C23_D70A;

/// `IsNonzeroParticleField`: an ordered comparison with 0 (a NaN is
/// non-zero).
fn nonzero(value: f32) -> bool {
    if value <= 0.0 { value < 0.0 } else { true }
}

/// `IsNonunitParticleField`: an ordered comparison with 1.
fn nonunit(value: f32) -> bool {
    if value <= 1.0 { value < 1.0 } else { true }
}

/// `ParticleInverseSqrt`: the `0x5f3759df` step on the length squared, the
/// seed from its float and the Newton terms on the unrounded value.
fn inverse_sqrt(length_sq: f64) -> f64 {
    let guess = f64::from(inv_sqrt_guess(round(length_sq)));
    (1.5 - ((guess * guess) * length_sq) * 0.5) * guess
}

/// `ParticleAxisLength`: `sqrt((x*x + y*y) + z*z)` unrounded.
fn axis_length(axis: [f32; 3]) -> f64 {
    let [x, y, z] = axis.map(f64::from);
    ((x * x) + (y * y) + (z * z)).sqrt()
}

/// The half-angle quaternion of a turn by `angle` (from `acos`) about
/// `axis`: the sine of the unrounded half angle, the cosine of its float.
fn half_angle_quat(axis: [f32; 3], half: f64) -> [f32; 4] {
    let stored_half = round(half);
    let sine = round(half.sin());
    let scale = round(f64::from(sine) / axis_length(axis));
    let cosine = round(f64::from(stored_half).cos());
    let s = f64::from(scale);
    [
        cosine,
        round(s * f64::from(axis[0])),
        round(s * f64::from(axis[1])),
        round(s * f64::from(axis[2])),
    ]
}

impl ParticleFx {
    // --- The pools' item teardown --------------------------------------------------------

    /// `ReleaseAppearancePoolItem(pool, item)`: a mesh item's mesh lets go
    /// (`IFxMesh::Slot7`) and is released; a particle record's life and
    /// death effects are stopped and released (`Slot13(7)`, death first);
    /// then the item goes back to its pool.
    pub(crate) fn release_pool_item(
        &mut self,
        kind: PoolKind,
        index: u32,
        svc: &mut dyn ParticleServices,
    ) {
        match kind {
            PoolKind::Mesh => {
                if let Some(mesh) = self.pools.mesh_mut(index).take()
                    && let Some(factory) = svc.mesh_factory()
                {
                    factory.drop_mesh(mesh);
                    factory.release_mesh(mesh);
                }
            }
            PoolKind::Particle => {
                let record = self.pools.record_mut(index);
                let mut life = record.life.take();
                let mut death = record.death.take();
                if let Some(life) = life.as_mut() {
                    life.dispatch(self, 7, svc);
                }
                if let Some(death) = death.as_mut() {
                    death.dispatch(self, 7, svc);
                }
                if let Some(death) = death {
                    death.release(self, svc);
                }
                if let Some(life) = life {
                    life.release(self, svc);
                }
            }
            _ => {}
        }
        self.pools.get_mut(kind).free(index);
    }

    /// Unlinks a pool item from its owner's list.
    fn unlink_item(&mut self, owner: NodeId, kind: PoolKind, index: u32) {
        let app = self.app(owner);
        let (mut head, mut tail) = (app.head, app.tail);
        self.pools
            .get_mut(kind)
            .unlink_particle(&mut head, &mut tail, index);
        let app = self.app_mut(owner);
        app.head = head;
        app.tail = tail;
    }

    /// The owner of a used item when it is a live appearance.
    fn item_owner(&self, kind: PoolKind, index: u32) -> Option<NodeId> {
        self.pools
            .get(kind)
            .item(index)
            .owner
            .filter(|&o| self.node(o).is_some_and(|n| n.appearance().is_some()))
    }

    /// Runs `f` on a record's life and death effects (taken out of the
    /// record while it runs).
    fn with_children(&mut self, index: u32, mut f: impl FnMut(&mut Self, &mut Effect)) {
        let record = self.pools.record_mut(index);
        let mut life = record.life.take();
        let mut death = record.death.take();
        if let Some(effect) = life.as_mut() {
            f(self, effect);
        }
        if let Some(effect) = death.as_mut() {
            f(self, effect);
        }
        let record = self.pools.record_mut(index);
        record.life = life;
        record.death = death;
    }

    /// Runs `f` on a record's life effect alone.
    fn with_life(&mut self, index: u32, f: impl FnOnce(&mut Self, &mut Effect)) {
        let mut life = self.pools.record_mut(index).life.take();
        if let Some(effect) = life.as_mut() {
            f(self, effect);
        }
        self.pools.record_mut(index).life = life;
    }

    /// The particle records of an appearance's list, in list order.
    fn list_items(&self, id: NodeId, kind: PoolKind) -> Vec<u32> {
        let mut out = Vec::new();
        let mut cursor = self.app(id).head;
        while let Some(index) = cursor {
            out.push(index);
            cursor = self.pools.get(kind).item(index).next;
        }
        out
    }

    // --- The outer time and LOD input ----------------------------------------------------

    /// The node's `SetNodeOuterTime`: a particle appearance also forwards
    /// it to each record's life and death effects (`Slot6`).
    pub(crate) fn set_node_outer_time(&mut self, id: NodeId, value: f32) {
        self.n_mut(id).outer_time = value;
        if self.n(id).class() == NodeClass::ParticleAppearance {
            for index in self.list_items(id, PoolKind::Particle) {
                self.with_children(index, |fx, e| e.set_outer_time(fx, value));
            }
        }
    }

    /// The node's `SetNodeLodInput`: a particle appearance also forwards it
    /// to each record's life and death effects (`Slot14`).
    pub(crate) fn set_node_lod_input(&mut self, id: NodeId, value: f32) {
        self.n_mut(id).lod_input = value;
        if self.n(id).class() == NodeClass::ParticleAppearance {
            for index in self.list_items(id, PoolKind::Particle) {
                self.with_children(index, |fx, e| e.set_lod_input(fx, value));
            }
        }
    }

    // --- FxParticleAppearance ------------------------------------------------------------

    fn particle_props(&self, id: NodeId) -> Option<Rc<super::props::NodeProps>> {
        let props = Rc::clone(&self.n(id).props);
        matches!(props.data, PropsData::Particle(_)).then_some(props)
    }

    /// `FxParticleAppearance::EraseRecord`: the record leaves its owner's
    /// list (the live count is not kept) and is torn down.
    fn erase_record(&mut self, owner: NodeId, index: u32, svc: &mut dyn ParticleServices) {
        self.unlink_item(owner, PoolKind::Particle, index);
        self.release_pool_item(PoolKind::Particle, index, svc);
    }

    /// `FxParticleAppearance::Pad47(six, age, duration)`: a full pool first
    /// gives up its oldest record; without a life name nothing spawns;
    /// otherwise the life effect is made under the appearance and a record
    /// carries it, moved once with no time (its first transform), with the
    /// appearance's outer time and LOD input handed on when they are not 0
    /// and 1.
    pub(crate) fn particle_spawn(
        &mut self,
        id: NodeId,
        six: &[f32; 6],
        age: f32,
        duration: f32,
        svc: &mut dyn ParticleServices,
    ) {
        let kind = PoolKind::Particle;
        if self.pools.get(kind).capacity() == 0 {
            // The reference reads the owner of a null item (D371).
            return;
        }
        if self.pools.get(kind).free_count() == 0
            && let Some(oldest) = self.pools.get(kind).oldest()
        {
            match self.item_owner(kind, oldest) {
                Some(owner) => self.erase_record(owner, oldest, svc),
                None => self.release_pool_item(kind, oldest, svc),
            }
        }
        let Some(props) = self.particle_props(id) else {
            return;
        };
        let PropsData::Particle(p) = &props.data else {
            return;
        };
        if p.life_name.is_empty() {
            return;
        }
        // The system is its own effect library, always bound (D494).
        let Ok(child) = self.create_effect(id, fx::name_hash(&p.life_name), svc) else {
            return;
        };
        let Some(index) = self.pools.get_mut(kind).alloc() else {
            child.release(self, svc);
            return;
        };
        let item = self.pools.get_mut(kind).item_mut(index);
        item.owner = Some(id);
        item.pos = [six[0], six[1], six[2]];
        item.vel = [six[3], six[4], six[5]];
        item.duration = duration;
        item.age = age;
        item.inv_duration = round(1.0 / f64::from(duration));
        *self.pools.record_mut(index) = ParticleRecord {
            life: Some(child),
            ..ParticleRecord::default()
        };
        let app = self.app(id);
        let (mut head, mut tail) = (app.head, app.tail);
        self.pools
            .get_mut(kind)
            .append_particle(&mut head, &mut tail, index);
        let app = self.app_mut(id);
        app.head = head;
        app.tail = tail;
        self.integrate_record(id, index, 0.0);
        let (outer, lod) = (self.n(id).outer_time, self.n(id).lod_input);
        if nonzero(outer) {
            self.with_life(index, |fx, e| e.set_outer_time(fx, outer));
        }
        if nonunit(lod) {
            self.with_life(index, |fx, e| e.set_lod_input(fx, lod));
        }
    }

    /// `FxParticleAppearance::IntegrateRecord(record, dt)`: the record moves
    /// by its velocity; the life effect gets the record's position, turned
    /// along the velocity with `UseDynamicRotation` (bit 0) and smoothed
    /// with `SmoothRotation` (bit 1), as its raw transform (`Slot3(1)`).
    /// `acos`, the sine and the cosine are Rust's (D495).
    #[allow(clippy::many_single_char_names)]
    fn integrate_record(&mut self, owner: NodeId, index: u32, dt: f32) {
        let flags = self
            .particle_props(owner)
            .map_or(0, |props| match &props.data {
                PropsData::Particle(p) => p.flags,
                _ => 0,
            });
        let dt64 = f64::from(dt);
        let item = self.pools.get_mut(PoolKind::Particle).item_mut(index);
        let step_z = round(dt64 * f64::from(item.vel[2]));
        let step_y = round(dt64 * f64::from(item.vel[1]));
        let step_x = round(dt64 * f64::from(item.vel[0]));
        item.pos[0] = round(f64::from(step_x) + f64::from(item.pos[0]));
        item.pos[1] = round(f64::from(step_y) + f64::from(item.pos[1]));
        item.pos[2] = round(f64::from(step_z) + f64::from(item.pos[2]));
        let (pos, vel) = (item.pos, item.vel);
        let mut m = IDENTITY;
        m[9..12].copy_from_slice(&pos);
        if flags & 1 != 0 {
            let [vx, vy, vz] = vel.map(f64::from);
            let length_sq_a = round((vz * vz + vx * vx) + vy * vy);
            let inv_len = inverse_sqrt(f64::from(length_sq_a));
            let n0 = round(vx * inv_len);
            let n1 = round(vy * inv_len);
            let nz = inv_len * vz;
            let length_sq_b = (vx * vx + vy * vy) + vz * vz;
            let speed = round(inverse_sqrt(length_sq_b) * length_sq_b);
            let q = if f64::from(n1).abs() < AXIS_BOUND {
                let axis = [round(nz), 0.0, -n0];
                half_angle_quat(axis, f64::from(n1).acos() * 0.5)
            } else {
                let axis = [0.0, round(-nz), n1];
                let q1 = half_angle_quat(axis, f64::from(n0).acos() * 0.5);
                let twist = f64::from_bits(TWIST);
                let twist_axis = [0.0, 0.0, 1.0];
                let twist_sine = round(twist.sin());
                let twist_scale = round(f64::from(twist_sine) / axis_length(twist_axis));
                let ts = f64::from(twist_scale);
                let q2 = [
                    round(twist.cos()),
                    round(ts * 0.0),
                    round(ts * 0.0),
                    round(ts * 1.0),
                ];
                mul_quat(&Quaternion::from_words(q1), &Quaternion::from_words(q2)).to_words()
            };
            let record = self.pools.record_mut(index);
            record.vel_quat = q;
            record.blend = 0.0;
            let rotation = if flags & 2 != 0 {
                if speed < 0.5 {
                    record.vel_quat = [1.0, 0.0, 0.0, 0.0];
                    record.blend = round(
                        (dt64 + f64::from(record.blend)) + f64::from(f32::from_bits(SLOW_STEP)),
                    );
                } else {
                    record.blend = round(f64::from(speed) * dt64);
                }
                if record.blend > 1.0 {
                    record.blend = 1.0;
                }
                record.blend_quat = quat_slerp(
                    &Quaternion::from_words(record.blend_quat),
                    &Quaternion::from_words(record.vel_quat),
                    record.blend,
                )
                .to_words();
                quaternion_to_matrix(&Quaternion::from_words(record.blend_quat))
            } else {
                quaternion_to_matrix(&Quaternion::from_words(record.vel_quat))
            };
            m[..9].copy_from_slice(&rotation.to_words());
        }
        self.with_life(index, |fx, e| {
            e.set_transform(fx, 1, &m);
        });
    }

    /// `FxParticleAppearance::Pad5(dt)`: the node ages (done with no
    /// emitter and no record), the fields act on the records not past their
    /// duration, and each record ages: one still within its duration moves,
    /// one that just passed it stops its life effect's emitters
    /// (`Slot13(1)`) unless the effect has finished; a record past its
    /// duration goes once its life effect and its death effect (if any)
    /// have finished.
    pub(crate) fn particle_update(&mut self, id: NodeId, dt: f32, svc: &mut dyn ParticleServices) {
        let node = self.n_mut(id);
        node.age = round(f64::from(dt) + f64::from(node.age));
        let app = self.app(id);
        if app.emitter_refs == 0 && app.head.is_none() {
            self.n_mut(id).flags |= FLAG_DONE;
        }
        self.apply_fields_to_pool(id, true, svc);
        let kind = PoolKind::Particle;
        let mut cursor = self.app(id).head;
        while let Some(index) = cursor {
            let item = *self.pools.get(kind).item(index);
            if item.age <= item.duration {
                let age = round(f64::from(dt) + f64::from(item.age));
                self.pools.get_mut(kind).item_mut(index).age = age;
                if !(age > item.duration) {
                    self.integrate_record(id, index, dt);
                } else {
                    self.with_life(index, |fx, e| {
                        if !e.finished(fx, 7) {
                            e.dispatch(fx, 1, svc);
                        }
                    });
                }
                cursor = self.pools.get(kind).item(index).next;
                continue;
            }
            let record = self.pools.record(index);
            let life_done = record.life.as_ref().is_none_or(|e| e.finished(self, 7));
            if !life_done {
                cursor = item.next;
                continue;
            }
            let death_done = record.death.as_ref().is_none_or(|e| e.finished(self, 7));
            if !death_done {
                cursor = item.next;
                continue;
            }
            cursor = item.next;
            self.erase_record(id, index, svc);
        }
    }

    /// The death effects `FxParticleAppearance::ComposeNodeTransform` makes
    /// before the node's own compose: with a death name, each record past
    /// its duration (ordered) without one gets it, placed at the record's
    /// position (`Slot3(1)`), with the outer time and LOD input handed on
    /// when they are not 0 and 1.
    pub(crate) fn particle_death_effects(&mut self, id: NodeId, svc: &mut dyn ParticleServices) {
        let Some(props) = self.particle_props(id) else {
            return;
        };
        let PropsData::Particle(p) = &props.data else {
            return;
        };
        if p.death_name.is_empty() {
            return;
        }
        let hash = fx::name_hash(&p.death_name);
        for index in self.list_items(id, PoolKind::Particle) {
            let item = *self.pools.get(PoolKind::Particle).item(index);
            if self.pools.record(index).death.is_some() || !(item.age > item.duration) {
                continue;
            }
            // A death effect whose creation fails is not kept (D491).
            let Ok(death) = self.create_effect(id, hash, svc) else {
                continue;
            };
            let mut m = IDENTITY;
            m[9..12].copy_from_slice(&item.pos);
            death.set_transform(self, 1, &m);
            let (outer, lod) = (self.n(id).outer_time, self.n(id).lod_input);
            if nonzero(outer) {
                death.set_outer_time(self, outer);
            }
            if nonunit(lod) {
                death.set_lod_input(self, lod);
            }
            self.pools.record_mut(index).death = Some(death);
        }
    }

    /// `FxParticleAppearance::GetBoundingSphere`: every record's life and
    /// death effect sphere (`Slot15`) that answers, the first copied and
    /// the others merged; `GR_GENERIC` when none answers.
    pub(crate) fn particle_bounds(&self, id: NodeId, out: &mut [f32; 4]) -> i32 {
        let mut merged = 0_u32;
        for index in self.list_items(id, PoolKind::Particle) {
            let record = self.pools.record(index);
            for (effect, inline) in [(&record.life, false), (&record.death, true)] {
                let Some(effect) = effect else {
                    continue;
                };
                let mut child = [0.0_f32; 4];
                if !effect.bounds(self, &mut child) {
                    continue;
                }
                if merged == 0 {
                    *out = child;
                } else {
                    merge_particle_sphere(out, &child, inline);
                }
                merged += 1;
            }
        }
        if merged == 0 { GR_GENERIC } else { 0 }
    }

    // --- FxMeshAppearance ----------------------------------------------------------------

    /// `FxMeshAppearance::EmitOne(six, age, duration)`: a full mesh pool
    /// first gives up its oldest item; without a bound factory nothing is
    /// emitted; otherwise the factory is asked for a mesh with the
    /// particle's position and velocity through the appearance's world
    /// transform (both as points), the mesh posed through the particle
    /// transform when `MeshApp_UseParticleTransform` is set, and a particle
    /// carries it.
    pub(crate) fn mesh_spawn(
        &mut self,
        id: NodeId,
        six: &[f32; 6],
        age: f32,
        duration: f32,
        svc: &mut dyn ParticleServices,
    ) {
        let kind = PoolKind::Mesh;
        if self.pools.get(kind).capacity() == 0 {
            // The reference reads the owner of a null item (D371).
            return;
        }
        if self.pools.get(kind).free_count() == 0
            && let Some(oldest) = self.pools.get(kind).oldest()
        {
            if let Some(owner) = self.item_owner(kind, oldest) {
                self.unlink_item(owner, kind, oldest);
                let app = self.app_mut(owner);
                if app.live != 0 {
                    app.live -= 1;
                }
            }
            self.release_pool_item(kind, oldest, svc);
        }
        let props = Rc::clone(&self.n(id).props);
        let PropsData::Mesh(m) = &props.data else {
            return;
        };
        let Some(factory) = svc.mesh_factory() else {
            return;
        };
        let world = Transform::from_words(self.n(id).world);
        let pos = transform_xform(&world, Vector::new(six[0], six[1], six[2])).to_words();
        let vel = transform_xform(&world, Vector::new(six[3], six[4], six[5])).to_words();
        let world_six = [pos[0], pos[1], pos[2], vel[0], vel[1], vel[2]];
        let (result, mesh) = factory.create_mesh(m.mesh_id, &m.mesh_name, &world_six);
        if result != 0 {
            if let Some(mesh) = mesh {
                factory.release_mesh(mesh);
            }
            return;
        }
        if m.use_particle_transform != 0
            && let Some(mesh) = mesh
        {
            // A null mesh is not posed (D492).
            let phase = round(f64::from(age) / f64::from(duration));
            let anim = m.particle_transform.evaluate(self.n(id).outer_time, phase);
            let mut t = IDENTITY;
            t[9..12].copy_from_slice(&six[..3]);
            let a = mul(&self.n(id).world, &t);
            factory.set_mesh_transform(mesh, &mul(&a, &anim));
        }
        let Some(index) = self.pools.get_mut(kind).alloc() else {
            if let Some(mesh) = mesh {
                factory.release_mesh(mesh);
            }
            return;
        };
        let item = self.pools.get_mut(kind).item_mut(index);
        item.pos = [six[0], six[1], six[2]];
        item.vel = [six[3], six[4], six[5]];
        item.owner = Some(id);
        item.duration = duration;
        item.age = age;
        item.inv_duration = round(1.0 / f64::from(duration));
        *self.pools.mesh_mut(index) = mesh;
        let app = self.app(id);
        let (mut head, mut tail) = (app.head, app.tail);
        self.pools
            .get_mut(kind)
            .append_particle(&mut head, &mut tail, index);
        let app = self.app_mut(id);
        app.head = head;
        app.tail = tail;
        app.live = app.live.wrapping_add(1);
    }

    /// `FxMeshAppearance::Pad5(dt)`: the node ages (done with no emitter and
    /// no particle), the fields act on every particle, and each particle
    /// ages: within its duration it moves (all three steps rounded) and its
    /// mesh updates (`IFxMesh::Slot5`); past it the particle is torn down.
    pub(crate) fn mesh_update(&mut self, id: NodeId, dt: f32, svc: &mut dyn ParticleServices) {
        let node = self.n_mut(id);
        node.age = round(f64::from(node.age) + f64::from(dt));
        let app = self.app(id);
        if app.emitter_refs == 0 && app.head.is_none() {
            self.n_mut(id).flags |= FLAG_DONE;
        }
        self.apply_fields_to_pool(id, false, svc);
        let kind = PoolKind::Mesh;
        let dt64 = f64::from(dt);
        let mut cursor = self.app(id).head;
        while let Some(index) = cursor {
            let item = self.pools.get_mut(kind).item_mut(index);
            item.age = round(f64::from(item.age) + dt64);
            let next = item.next;
            if !(item.age > item.duration) {
                let step = item.vel.map(|v| round(dt64 * f64::from(v)));
                for (p, s) in item.pos.iter_mut().zip(step) {
                    *p = round(f64::from(*p) + f64::from(s));
                }
                // A null mesh is not updated (D492).
                if let Some(mesh) = self.pools.mesh(index)
                    && let Some(factory) = svc.mesh_factory()
                {
                    factory.update_mesh(mesh, dt);
                }
            } else {
                self.unlink_item(id, kind, index);
                let app = self.app_mut(id);
                app.live = app.live.wrapping_sub(1);
                self.release_pool_item(kind, index, svc);
            }
            cursor = next;
        }
    }

    /// The poses `FxMeshAppearance::ComposeNodeTransform` gives after the
    /// node's own compose: each particle's mesh goes to the particle's
    /// place in the node's world, through the particle transform at the
    /// particle's time when `MeshApp_UseParticleTransform` is set, else on
    /// the mesh's own rotation (`IFxMesh::Slot4`).
    pub(crate) fn mesh_pose(&mut self, id: NodeId, svc: &mut dyn ParticleServices) {
        let props = Rc::clone(&self.n(id).props);
        let PropsData::Mesh(m) = &props.data else {
            return;
        };
        let world = self.n(id).world;
        let outer = self.n(id).outer_time;
        for index in self.list_items(id, PoolKind::Mesh) {
            let Some(mesh) = self.pools.mesh(index) else {
                continue;
            };
            let Some(factory) = svc.mesh_factory() else {
                return;
            };
            let item = self.pools.get(PoolKind::Mesh).item(index);
            if m.use_particle_transform != 0 {
                let phase = round(f64::from(item.age) / f64::from(item.duration));
                let anim = m.particle_transform.evaluate(outer, phase);
                let mut t = IDENTITY;
                t[9..12].copy_from_slice(&item.pos);
                let x = mul(&world, &t);
                factory.set_mesh_transform(mesh, &mul(&x, &anim));
            } else {
                let mut pose = factory.mesh_transform(mesh);
                let at =
                    transform_xform(&Transform::from_words(world), Vector::from_words(item.pos));
                pose[9..12].copy_from_slice(&at.to_words());
                factory.set_mesh_transform(mesh, &pose);
            }
        }
    }

    /// `FxMeshAppearance::DrawOrEnqueue`: each particle's mesh draws itself
    /// with the walk's camera (`IFxMesh::Slot6`).
    pub(crate) fn mesh_draw(
        &self,
        id: NodeId,
        camera: &mut dyn Camera,
        svc: &mut dyn ParticleDraw,
    ) {
        let Some(factory) = svc.mesh_factory() else {
            return;
        };
        for index in self.list_items(id, PoolKind::Mesh) {
            // A null mesh does not draw (D492).
            if let Some(mesh) = self.pools.mesh(index) {
                factory.draw_mesh(mesh, camera);
            }
        }
    }

    /// `FxMeshAppearance::GetBoundingSphere`: the particles' positions in
    /// list order (the first a point sphere, the others merged as points),
    /// then the centre through the node's world transform, also for an
    /// empty list; `GR_GENERIC` with no particle.
    pub(crate) fn mesh_bounds(&self, id: NodeId, out: &mut [f32; 4]) -> i32 {
        let items = self.list_items(id, PoolKind::Mesh);
        let pool = self.pools.get(PoolKind::Mesh);
        for (count, &index) in items.iter().enumerate() {
            let p = pool.item(index).pos;
            if count == 0 {
                *out = [p[0], p[1], p[2], 0.0];
            } else {
                super::bounds::merge_point(out, p);
            }
        }
        let world = Transform::from_words(self.n(id).world);
        let c = transform_xform(&world, Vector::new(out[0], out[1], out[2])).to_words();
        out[..3].copy_from_slice(&c);
        if items.is_empty() { GR_GENERIC } else { 0 }
    }
}
