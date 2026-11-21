//! The mechanisms of a mine's body that the server's `Mine` wrapper
//! (`Server/server_mine.cpp`) drives: its phantom (`arm`'s
//! `CreatePhantom` and `AttachPhantom`, the collision state), its
//! controller (`do_simulation_controller`, the push towards the point it
//! seeks) and the wake-up. When a mine arms, seeks or detonates is the
//! server's (`game-logic`); the controller runs inside the physics step,
//! so it reads the seek state from the mine itself ([`MineData::seeking`],
//! [`MineData::seek_target`]). A world without a server never arms a mine.

use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::{ControllerContext, Geometry, PhantomDesc};

use crate::object::projectile::MineData;
use crate::object::{KindData, ObjHandle, Object};

use super::{ControllerRef, World};

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// `Mine::do_simulation_controller`: while the mine seeks, the speed it
/// lacks along the direction to its target, capped at `top_speed` and
/// (unless `acceleration` is exactly -1) at `acceleration * dt`, plus the
/// drag it will lose, is added to the core's velocity directly. The
/// direction has no zero test, as in the reference.
pub(crate) fn mine_controller(obj: &Object, dt: f32, context: &mut ControllerContext<'_>) {
    let KindData::Mine(m) = &obj.data else {
        return;
    };
    if !m.seeking {
        return;
    }
    let speed = context.core().speed;
    let velocity = Vector::new(speed[0], speed[1], speed[2]);
    let pos = obj.engine.position;
    let t = m.seek_target;
    let dz = r(e(t.z) - e(pos.z));
    let dy = r(e(t.y) - e(pos.y));
    let dx = r(e(t.x) - e(pos.x));
    let length_sq = (e(dz) * e(dz) + e(dy) * e(dy)) + e(dx) * e(dx);
    let inverse = 1.0 / length_sq.sqrt();
    let dir = Vector::new(r(e(dx) * inverse), r(e(dy) * inverse), r(e(dz) * inverse));
    let along = r((e(dir.z) * e(velocity.z) + e(velocity.y) * e(dir.y)) + e(velocity.x) * e(dir.x));
    let delta = mine_push(m, obj.arch_mass, along, dt);
    let Some(k) = delta else {
        return;
    };
    context.set_speed(Vector::new(
        r(e(dir.x) * k + e(velocity.x)),
        r(e(dir.y) * k + e(velocity.y)),
        r(e(dir.z) * k + e(velocity.z)),
    ));
}

/// The push of [`mine_controller`] for a speed `along` the direction to the
/// target: `None` unless the mine is slower than its top speed, otherwise
/// `K = (delta + along) * ((linear_drag / mass) * dt) + delta` with the
/// capped `delta`, unrounded.
#[must_use]
pub fn mine_push(m: &MineData, mass: f32, along: f32, dt: f32) -> Option<f64> {
    let missing = e(m.top_speed) - e(along);
    let mut delta = r(missing);
    if !(missing > 0.0) {
        return None;
    }
    if !(m.top_speed > delta) {
        delta = m.top_speed;
    }
    if m.acceleration != -1.0 {
        let cap = e(m.acceleration) * e(dt);
        if e(delta) > cap {
            delta = r(cap);
        }
    }
    let drag_dt = (e(m.linear_drag) / e(mass)) * e(dt);
    Some((e(delta) + e(along)) * drag_dt + e(delta))
}

impl World {
    /// `Mine::arm`'s phantom: a ball of `radius` at the mine's position
    /// (identity orientation, no collision group, a listener), merged into
    /// its body (`PhySys::CreatePhantom`, `AttachPhantom`). Its collisions
    /// start off; the mine's first update turns them on. False for an
    /// object that is not a mine with a body.
    pub fn attach_mine_phantom(&mut self, h: ObjHandle, radius: f32) -> bool {
        let Some(o) = self.objects.get(h) else {
            return false;
        };
        if !matches!(o.data, KindData::Mine(_)) {
            return false;
        }
        let position = o.engine.position;
        let Some((system, body)) = self.body(h) else {
            return false;
        };
        let Some(world) = self.physics.world_mut(system) else {
            return false;
        };
        let phantom = world.create_phantom(PhantomDesc {
            geometry: Geometry::ball(radius),
            position,
            orientation: Matrix::IDENTITY,
            name: Vec::new(),
            listener: true,
        });
        world.set_phantom_collision_state(phantom, false);
        world.attach_phantom(phantom, body, position);
        if let Some(KindData::Mine(m)) = self.objects.get_mut(h).map(|o| &mut o.data) {
            m.phantom = Some(phantom);
        }
        true
    }

    /// `PhySys::Controller::Register` of a mine's controller
    /// (`Mine::do_simulation_controller`).
    pub fn register_mine_controller(&mut self, h: ObjHandle) {
        if matches!(
            self.objects.get(h).map(|o| &o.data),
            Some(KindData::Mine(_))
        ) {
            self.register_controller(h, ControllerRef::Mine);
        }
    }

    /// `PhySys::SetCollisionStatePhantom` of a mine's phantom; nothing
    /// without one.
    pub fn set_mine_phantom_collisions(&mut self, h: ObjHandle, enable: bool) {
        let Some(o) = self.objects.get(h) else {
            return;
        };
        let KindData::Mine(m) = &o.data else {
            return;
        };
        if let (Some(phantom), Some(world)) = (m.phantom, self.physics.world_mut(o.physical.system))
        {
            world.set_phantom_collision_state(phantom, enable);
        }
    }

    /// `PhySys::Controller::wakeup`: the object's body is woken.
    pub fn wakeup_body(&mut self, h: ObjHandle) {
        if let Some((system, body)) = self.body(h)
            && let Some(world) = self.physics.world_mut(system)
        {
            world.wakeup(body);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn mine() -> MineData {
        MineData {
            linear_drag: 0.4,
            top_speed: 60.0,
            acceleration: 200.0,
            ..MineData::default()
        }
    }

    #[test]
    fn push_is_capped_by_the_acceleration() {
        // 60 missing, capped at 200 * 0.1 = 20; K = (20 + 0) * (4 * 0.1) + 20.
        let m = mine();
        let k = mine_push(&m, 0.1, 0.0, 0.1).unwrap();
        let drag_dt = (e(0.4) / e(0.1)) * e(0.1);
        let delta = r(e(200.0) * e(0.1));
        assert_eq!(k, e(delta) * drag_dt + e(delta));
    }

    #[test]
    fn push_without_acceleration_cap_and_at_top_speed() {
        let m = MineData {
            acceleration: -1.0,
            ..mine()
        };
        let k = mine_push(&m, 0.1, 10.0, 0.1).unwrap();
        let drag_dt = (e(0.4) / e(0.1)) * e(0.1);
        assert_eq!(k, (50.0 + 10.0) * drag_dt + 50.0);
        assert_eq!(mine_push(&m, 0.1, 60.0, 0.1), None);
        assert_eq!(mine_push(&m, 0.1, 70.0, 0.1), None);
    }
}
