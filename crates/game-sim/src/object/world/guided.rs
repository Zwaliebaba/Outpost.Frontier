//! The server's guided-missile wrapper (`Server/server_guided.cpp`,
//! `Guided`): the motor's controller (`do_simulation_controller`), the
//! update (lifetime, proximity fuse, seeker and steering: `update`,
//! `seek`), the fuse's phantom (`arm`, `added`), the hit
//! (`on_collision`) and the detonation (`destroy` through
//! `StarSystem::explode`). Moved into Phase 2 from P3.3 (owner,
//! 2026-11-18). Mines and counter-measures are plain bodies without a
//! server (D125); the server's wrappers (`game-logic::munitions`) arm a
//! mine through `world::mine`. `set_target` is the server's.

use neuron_core::math::{Matrix, Vector, transform_matrix};

use crate::equip::{Pusher, SUBOBJ_ID_ROOT};
use crate::object::damage::{self, CAUSE_EXPLOSION, DamageCause};
use crate::object::{KindData, ObjHandle, Object, ObjectKind, class};

use super::{ServerHooks, TickList, World};

/// One seeker step of a guided munition (diagnostic,
/// [`World::set_guided_trace`]): its id, the aim point and velocity, the
/// munition's position and velocity, whether the seeker saw the aim point,
/// and the angular and linear velocity changes `seek` made (`None` when it
/// made none).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct GuidedTrace {
    /// The munition's id.
    pub id: u32,
    /// The aim point.
    pub target_pos: Vector,
    /// The target's velocity.
    pub target_vel: Vector,
    /// The munition's position.
    pub pos: Vector,
    /// The munition's velocity.
    pub vel: Vector,
    /// The seeker saw the aim point.
    pub sees: bool,
    /// The change of the object-space angular velocity.
    pub turn: Option<Vector>,
    /// The change of the velocity.
    pub align: Option<Vector>,
}

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// `Guided::do_simulation_controller`: while the motor burns
/// (`motor_on`), a push along the third column of the object's
/// transform of `-((mass * accel) * dt)`, stored as a float.
pub(crate) fn guided_controller(obj: &Object, mass: f32, dt: f32, pusher: &mut dyn Pusher) {
    let KindData::Guided(g) = &obj.data else {
        return;
    };
    let Some(motor) = &g.motor else {
        return;
    };
    if !g.motor_on() {
        return;
    }
    let m = &obj.engine.orientation.m;
    let axis = Vector::new(m[0][2], m[1][2], m[2][2]);
    let amount = r(-((e(mass) * e(motor.accel)) * e(dt)));
    pusher.push(axis, amount);
}

impl World {
    /// Starts (or stops) keeping every guided munition's seeker step
    /// (diagnostic).
    pub fn set_guided_trace(&mut self, on: bool) {
        self.guided_trace = on.then(Vec::new);
    }

    /// The seeker steps kept since the last call.
    pub fn take_guided_trace(&mut self) -> Vec<GuidedTrace> {
        self.guided_trace
            .as_mut()
            .map(std::mem::take)
            .unwrap_or_default()
    }

    /// `Guided::update`: `CGuided::update` (the seeker clock, the lifetime
    /// and the owner-safe time); a spent lifetime detonates; the fuse's
    /// collisions go on at the first update, and a touch latched since
    /// detonates; the wake-up; and a lock-on seeker with a target it can
    /// see steers onto it (`seek`), one that lost it drops it.
    pub(crate) fn update_guided(&mut self, h: ObjHandle, dt: f32, hooks: &mut dyn ServerHooks) {
        let Some(o) = self.objects.get_mut(h) else {
            return;
        };
        let (system, body) = (o.physical.system, o.physical.body);
        let KindData::Guided(g) = &mut o.data else {
            return;
        };
        g.seeker_elapsed += dt;
        let expired = g.projectile.update(dt);
        let alive = g.projectile.is_alive();
        let (phantom, triggered, trigger_obj) = (g.phantom, g.triggered, g.trigger_obj);
        if expired && let (Some(body), Some(world)) = (body, self.physics.world_mut(system)) {
            // `CGuided::expire_safe_time`.
            world.set_collision_group(body, b"");
        }
        if !alive {
            self.kill(h, 0, hooks);
            return;
        }
        if let (Some(phantom), Some(world)) = (phantom, self.physics.world_mut(system)) {
            if !world.phantom_collision_state(phantom) {
                world.set_phantom_collision_state(phantom, true);
            } else if triggered {
                self.kill(h, trigger_obj, hooks);
                return;
            }
        }
        // `PhySys::Controller::wakeup`.
        if let (Some(body), Some(world)) = (body, self.physics.world_mut(system)) {
            world.wakeup(body);
        }
        let Some(o) = self.objects.get(h) else {
            return;
        };
        let KindData::Guided(g) = &o.data else {
            return;
        };
        if g.seeker != 2 {
            return;
        }
        let (position, orientation) = (o.engine.position, o.engine.orientation);
        let sub = g.sub_target;
        let Some(target) = g.target.filter(|t| self.objects.get(*t).is_some()) else {
            if let Some(KindData::Guided(g)) = self.objects.get_mut(h).map(|o| &mut o.data) {
                g.target = None;
            }
            return;
        };
        // The aim point: a sub-targeted equipped object's sub-object
        // centre of mass, otherwise (or when that fails) the target's.
        let equipped = self
            .objects
            .get(target)
            .is_some_and(|t| t.is(class::EQ_OBJ));
        let sub_center = if equipped && sub != SUBOBJ_ID_ROOT {
            self.sub_obj_center_of_mass(target, sub)
        } else {
            None
        };
        let target_pos = sub_center.unwrap_or_else(|| self.center_of_mass(target));
        let sees = match self.objects.get(h).map(|o| &o.data) {
            Some(KindData::Guided(g)) => g.seeker_can_see(position, &orientation, target_pos),
            _ => false,
        };
        if self.guided_trace.is_some() {
            let id = self.objects.get(h).map_or(0, |o| o.simple.id);
            let (target_vel, vel) = (self.velocity(target), self.velocity(h));
            if let Some(t) = self.guided_trace.as_mut() {
                t.push(GuidedTrace {
                    id,
                    target_pos,
                    target_vel,
                    pos: position,
                    vel,
                    sees,
                    ..GuidedTrace::default()
                });
            }
        }
        if sees {
            // A sub-object's velocity is its physics owner's, which for
            // the ports' single-body objects is the target's.
            let target_vel = self.velocity(target);
            self.seek(h, target_pos, target_vel);
        } else if let Some(KindData::Guided(g)) = self.objects.get_mut(h).map(|o| &mut o.data) {
            // `set_target(nullptr, SUBOBJ_ID_ROOT, 0)`.
            g.target = None;
            g.sub_target = SUBOBJ_ID_ROOT;
        }
    }

    /// `Guided::seek`: while the target closes in, the angular velocity
    /// becomes three times the line-of-sight rate (clamped to the
    /// munition's `max_angular_velocity`); then, flying nose first, the
    /// whole speed is put back on the nose.
    fn seek(&mut self, h: ObjHandle, target_pos: Vector, target_vel: Vector) {
        let Some(o) = self.objects.get(h) else {
            return;
        };
        let KindData::Guided(g) = &o.data else {
            return;
        };
        let max_rate = g.max_angular_velocity;
        let (system, Some(body)) = (o.physical.system, o.physical.body) else {
            return;
        };
        let my_pos = o.engine.position;
        let orientation = o.engine.orientation;
        let my_vel = self.velocity(h);
        let rel_pos = Vector::new(
            r(e(target_pos.x) - e(my_pos.x)),
            r(e(target_pos.y) - e(my_pos.y)),
            r(e(target_pos.z) - e(my_pos.z)),
        );
        let rel_vel = Vector::new(
            r(e(target_vel.x) - e(my_vel.x)),
            r(e(target_vel.y) - e(my_vel.y)),
            r(e(target_vel.z) - e(my_vel.z)),
        );
        let closing = (e(rel_vel.z) * e(rel_pos.z) + e(rel_vel.y) * e(rel_pos.y))
            + e(rel_vel.x) * e(rel_pos.x);
        if closing < 0.0 {
            let cross = Vector::new(
                r(e(rel_vel.z) * e(rel_pos.y) - e(rel_pos.z) * e(rel_vel.y)),
                r(e(rel_pos.z) * e(rel_vel.x) - e(rel_vel.z) * e(rel_pos.x)),
                r(e(rel_vel.y) * e(rel_pos.x) - e(rel_pos.y) * e(rel_vel.x)),
            );
            let dist_sq = (e(rel_pos.z) * e(rel_pos.z) + e(rel_pos.y) * e(rel_pos.y))
                + e(rel_pos.x) * e(rel_pos.x);
            if dist_sq >= e(0.0001f32) {
                let inv = r(1.0 / dist_sq);
                let omega = Vector::new(
                    r(e(inv) * e(cross.x)),
                    r(e(inv) * e(cross.y)),
                    r(e(inv) * e(cross.z)),
                );
                let mut desired = Vector::new(
                    r(3.0 * e(omega.x)),
                    r(3.0 * e(omega.y)),
                    r(3.0 * e(omega.z)),
                );
                let magnitude = r(((e(desired.y) * e(desired.y) + e(desired.z) * e(desired.z))
                    + e(desired.x) * e(desired.x))
                .sqrt());
                if magnitude > max_rate {
                    let length = ((e(desired.x) * e(desired.x) + e(desired.y) * e(desired.y))
                        + e(desired.z) * e(desired.z))
                    .sqrt();
                    let inverse = 1.0 / length;
                    desired = Vector::new(
                        r(e(desired.x) * inverse),
                        r(e(desired.y) * inverse),
                        r(e(desired.z) * inverse),
                    );
                    let rate = e(max_rate);
                    desired = Vector::new(
                        r(rate * e(desired.x)),
                        r(rate * e(desired.y)),
                        r(rate * e(desired.z)),
                    );
                }
                let Some(world) = self.physics.world_mut(system) else {
                    return;
                };
                let angular = world.angular_velocity_os(body);
                let m = &orientation.m;
                let transposed = Matrix::from_rows([
                    [m[0][0], m[1][0], m[2][0]],
                    [m[0][1], m[1][1], m[2][1]],
                    [m[0][2], m[1][2], m[2][2]],
                ]);
                let local = transform_matrix(&transposed, desired);
                let change = Vector::new(
                    r(e(-angular.x) + e(local.x)),
                    r(e(-angular.y) + e(local.y)),
                    r(e(-angular.z) + e(local.z)),
                );
                world.add_to_angular_velocity_os(body, change);
                if let Some(t) = self.guided_trace.as_mut().and_then(|t| t.last_mut()) {
                    t.turn = Some(change);
                }
            }
        }
        let velocity = self.velocity(h);
        let m = &orientation.m;
        let nose = Vector::new(-m[0][2], -m[1][2], -m[2][2]);
        let along =
            (e(nose.z) * e(velocity.z) + e(nose.y) * e(velocity.y)) + e(velocity.x) * e(nose.x);
        if !(along > 0.0) {
            return;
        }
        let speed = r(
            ((e(velocity.z) * e(velocity.z) + e(velocity.y) * e(velocity.y))
                + e(velocity.x) * e(velocity.x))
            .sqrt(),
        );
        let wanted = Vector::new(
            r(e(nose.x) * e(speed)),
            r(e(nose.y) * e(speed)),
            r(e(nose.z) * e(speed)),
        );
        let change = Vector::new(
            r(e(wanted.x) + e(-velocity.x)),
            r(e(wanted.y) + e(-velocity.y)),
            r(e(wanted.z) + e(-velocity.z)),
        );
        if let Some(world) = self.physics.world_mut(system) {
            world.add_to_velocity(body, change);
        }
        if let Some(t) = self.guided_trace.as_mut().and_then(|t| t.last_mut()) {
            t.align = Some(change);
        }
    }

    /// The phantom events of the last physics step: those of a field
    /// mine's phantom go to the mine ([`World::mine_phantom_event`]); an
    /// object entering a missile's fuse (`Guided::added`) latches the
    /// trigger with the id of its root (0 without one), unless it is the
    /// missile's owner while the owner-safe time runs; the others go to the
    /// server ([`ServerHooks::phantom_event`]).
    pub(crate) fn phantom_events(&mut self, system: u32, hooks: &mut dyn ServerHooks) {
        let events = match self.physics.world_mut(system) {
            Some(w) => w.take_phantom_events(),
            None => return,
        };
        if events.is_empty() {
            return;
        }
        let missiles = self.list(system, TickList::Guided);
        for ev in events {
            // A field mine's phantom (`MedMine::added`, `deleted`).
            if self.mine_phantom_event(system, &ev) {
                continue;
            }
            let Some(&missile) = missiles.iter().find(|m| {
                matches!(
                    self.objects.get(**m).map(|o| &o.data),
                    Some(KindData::Guided(g)) if g.phantom == Some(ev.phantom)
                )
            }) else {
                hooks.phantom_event(self, system, &ev);
                continue;
            };
            if !ev.entered {
                continue;
            }
            let intruder = ObjHandle::from_key(ev.user_key);
            let root = match self.objects.get(intruder).map(|o| &o.data) {
                Some(KindData::EquipmentObject {
                    owner: Some(owner), ..
                }) => *owner,
                _ => intruder,
            };
            let id = self.objects.get(root).map_or(0, |o| o.simple.id);
            if let Some(KindData::Guided(g)) = self.objects.get_mut(missile).map(|o| &mut o.data) {
                if id != 0 && id == g.projectile.owner_id && g.projectile.is_owner_safe() {
                    continue;
                }
                g.triggered = true;
                g.trigger_obj = id;
            }
        }
    }

    /// `Guided::on_collision`: a hit on an object with an id that no
    /// player owns goes to `StarSystem::munition_collision` with the
    /// missile's munition and owner (the hit position is never written in
    /// the reference; zero here).
    pub(crate) fn guided_collision(
        &mut self,
        h: ObjHandle,
        other: ObjHandle,
        other_sub: crate::equip::SubObjId,
        hooks: &mut dyn ServerHooks,
    ) {
        let (Some(o), Some(other_obj)) = (self.objects.get(h), self.objects.get(other)) else {
            return;
        };
        let KindData::Guided(g) = &o.data else {
            return;
        };
        if other_obj.simple.id == 0 || other_obj.simple.owner_player != 0 {
            return;
        }
        let (owner, munition) = (g.projectile.owner_id, g.munition);
        self.munition_collision_hooked(other, owner, munition, other_sub, Vector::ZERO, hooks);
    }

    /// `destroy(1, killer)`: the object is marked destroyed; a guided
    /// munition then detonates (`Guided::destroy`).
    pub(crate) fn kill(&mut self, h: ObjHandle, killer: u32, hooks: &mut dyn ServerHooks) {
        let Some(o) = self.objects.get(h) else {
            return;
        };
        if o.dead {
            return;
        }
        let guided = o.kind == ObjectKind::Guided;
        self.destroy_hooked(h, 1, killer, hooks);
        // A server that keeps the object (a death fuse) does not detonate it.
        let dead = self.objects.get(h).is_none_or(|o| o.dead);
        if guided && dead {
            self.guided_detonate(h, killer, hooks);
        }
    }

    /// `Guided::destroy`'s detonation: unless a counter-measure killed it,
    /// a missile whose explosion has a radius and some effect explodes at
    /// its position (`StarSystem::explode`), inflicted by its owner, cause
    /// 6 for a cruise disruptor else 5.
    fn guided_detonate(&mut self, h: ObjHandle, killer: u32, hooks: &mut dyn ServerHooks) {
        if self
            .find(killer)
            .and_then(|k| self.objects.get(k))
            .is_some_and(|k| k.kind == ObjectKind::CounterMeasure)
        {
            return;
        }
        let Some(o) = self.objects.get(h) else {
            return;
        };
        let KindData::Guided(g) = &o.data else {
            return;
        };
        let (system, position, owner) =
            (o.physical.system, o.engine.position, g.projectile.owner_id);
        let Some(arch) = self.data.archetypes.equipment.get(&o.archetype) else {
            return;
        };
        let cruise_disruptor = match &arch.kind {
            crate::data::archetype::equipment::EquipmentKind::Munition(m) => m.cruise_disruptor,
            _ => false,
        };
        if arch.root.explosion_arch == 0 {
            return;
        }
        let Some(explosion) = self
            .data
            .archetypes
            .explosions
            .get(&arch.root.explosion_arch)
            .cloned()
        else {
            return;
        };
        let epsilon = 0.0001f64;
        if !(e(explosion.radius) > epsilon) {
            return;
        }
        if !(e(explosion.impulse) > epsilon)
            && !(e(explosion.hull_damage) > epsilon)
            && !(e(explosion.energy_damage) > epsilon)
        {
            return;
        }
        let cause: DamageCause = if cruise_disruptor { 6 } else { CAUSE_EXPLOSION };
        let blast = damage::Blast {
            position,
            radius: explosion.radius,
            hull_damage: explosion.hull_damage,
            energy_damage: explosion.energy_damage,
        };
        self.explode_caused_hooked(
            system,
            &blast,
            cause,
            owner,
            Some(h),
            explosion.impulse,
            hooks,
        );
    }
}
