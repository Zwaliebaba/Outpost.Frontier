//! A ship's arrival through a jump gate, a jump hole or an airlock:
//! `CShip::jump_in` with its `JumpInAction`
//! (`Common/common_cship.cpp:1066`, `Common/common_launchaction.cpp:490`)
//! and `CShip::airlock_in` (`common_cship.cpp:1000`). The server's
//! `StarSystem::arrive_ship` calls them for an NPC's ship.
//!
//! - **The jump-in action.** Made, it turns the ship's collisions off,
//!   zeroes its throttle, activates its engine and beams it to the arrival
//!   pose, then registers its physics controller. Each PSI the controller
//!   beams the ship along the arrival orientation's third column, at the
//!   squared remaining fraction of the approach times `speed * 2000`, onto
//!   the arrival point; once `speed` seconds have run it beams the ship
//!   there, zeroes the throttle, activates the engine and reports the
//!   arrival with the ship's system (`IObjAffect::slot_74`,
//!   [`super::ServerHooks::jump_in_complete`]). The ship's update
//!   (`ActionDB::update`) wakes the controller until then and removes the
//!   completed action, which turns the collisions back on.
//! - **The airlock.** Full throttle, the engine on and the ship beamed to
//!   the pose.

use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::ControllerContext;

use crate::equip::types;
use crate::object::ship::JumpIn;
use crate::object::{Command, KindData, ObjHandle, Object};

use super::{ControllerRef, ServerHooks, TickList, World, WorldEvent};

impl World {
    /// `CEquip::Activate(true)` on the ship's first engine when it is not
    /// active (`EquipManager.FindFirst(EquipTypeEngine)`).
    fn activate_first_engine(&mut self, h: ObjHandle) {
        let engine = self
            .objects
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .and_then(|eq| eq.equipment.first(types::ENGINE))
            .filter(|e| !e.is_active())
            .map(|e| e.id);
        if let Some(id) = engine {
            let _ = self.command(h, Command::ActivateEquip(id, true));
        }
    }

    /// `CShip::airlock_in(Pos, Orient)`: the throttle at 1, the first
    /// engine activated when it is not active, and the ship beamed to the
    /// pose (`beam_object`). Nothing for an object that is not a ship.
    pub fn airlock_in(&mut self, h: ObjHandle, position: Vector, orientation: Matrix) {
        let Some(ship) = self.ship_data_mut(h) else {
            return;
        };
        ship.flight.throttle = 1.0;
        self.activate_first_engine(h);
        self.beam_object(h, position, orientation);
    }

    /// `CShip::jump_in(JumpObj, Pos, Orient, Speed)`, the ship itself as the
    /// jumping object (as `StarSystem::arrive_ship` passes it): the
    /// `JumpInAction` joins the ship's actions. Its constructor turns the
    /// ship's collisions off, zeroes the throttle, activates the first
    /// engine when it is not active, beams the ship to the pose and
    /// registers the action's controller. A ship already jumping in ends
    /// that jump first (the reference keeps both actions, D681). Nothing for
    /// an object that is not a ship.
    pub fn jump_in(&mut self, h: ObjHandle, position: Vector, orientation: Matrix, speed: f32) {
        if self.ship_data(h).is_none() {
            return;
        }
        if self.is_jumping(h) {
            self.end_jump_in(h);
        }
        let action = JumpIn {
            speed,
            approach_budget: (f64::from(speed) * 2000.0) as f32,
            orientation,
            position,
            ..JumpIn::default()
        };
        self.set_collisions(h, false);
        if let Some(ship) = self.ship_data_mut(h) {
            ship.flight.throttle = 0.0;
        }
        self.activate_first_engine(h);
        self.beam_object(h, position, orientation);
        if let Some(ship) = self.ship_data_mut(h) {
            ship.jump_in = Some(action);
        }
        self.register_controller(h, ControllerRef::JumpIn);
    }

    /// `CShip::is_jumping`: the ship has a jump-in action.
    #[must_use]
    pub fn is_jumping(&self, h: ObjHandle) -> bool {
        self.ship_data(h).is_some_and(|s| s.jump_in.is_some())
    }

    /// The ship's jump-in action, while it has one.
    #[must_use]
    pub fn jump_in_state(&self, h: ObjHandle) -> Option<&JumpIn> {
        self.ship_data(h).and_then(|s| s.jump_in.as_ref())
    }

    /// `~JumpInAction`: the ship's collisions back on, the controller
    /// unregistered, the action gone.
    fn end_jump_in(&mut self, h: ObjHandle) {
        if self
            .ship_data_mut(h)
            .and_then(|s| s.jump_in.take())
            .is_none()
        {
            return;
        }
        self.set_collisions(h, true);
        self.unregister(h, ControllerRef::JumpIn);
    }

    /// `ActionDB::update` of a ship (`CShip::update` without `NO_ACTIONS`):
    /// `JumpInAction::update` wakes the controller while the jump is not
    /// complete (`PhySys::Controller::wakeup`, nothing once the body that
    /// held it is gone) and answers whether the action stays; a completed
    /// one is removed. An arrival the server has not heard of yet (in
    /// single player, where the client side steps the physics) is
    /// reported first.
    pub(crate) fn update_actions(&mut self, h: ObjHandle, hooks: &mut dyn ServerHooks) {
        let Some(complete) = self.jump_in_state(h).map(|j| j.complete) else {
            return;
        };
        if !complete {
            let registered = self.objects.get(h).is_some_and(|o| {
                o.physical
                    .controllers
                    .iter()
                    .any(|(w, _, _)| *w == ControllerRef::JumpIn)
            });
            if registered
                && let Some((system, body)) = self.body(h)
                && let Some(world) = self.physics.world_mut(system)
            {
                world.wakeup(body);
            }
            return;
        }
        self.deliver_jump_in(h, hooks);
        self.end_jump_in(h);
    }

    /// After a physics step: each ship of the system whose jump-in arrived
    /// during it activates its first engine when it is not active (the
    /// controller's `Activate(true)`, which cannot register the engine's
    /// controller while the step holds the objects, D682) and the arrival
    /// joins the world's events.
    pub(crate) fn finish_jump_ins(&mut self, system: u32) {
        for h in self.list(system, TickList::Ships) {
            let Some(o) = self.objects.get_mut(h) else {
                continue;
            };
            let id = o.simple.id;
            let KindData::Ship(ship) = &mut o.data else {
                continue;
            };
            let Some(j) = ship.jump_in.as_mut() else {
                continue;
            };
            if !std::mem::take(&mut j.activate_engine) {
                continue;
            }
            let arrived = j.arrived;
            self.activate_first_engine(h);
            if let Some(system) = arrived {
                self.events
                    .push(WorldEvent::JumpInComplete { ship: id, system });
            }
        }
    }

    /// A jump-in's arrival to the server ([`ServerHooks::jump_in_complete`]),
    /// once.
    fn deliver_jump_in(&mut self, h: ObjHandle, hooks: &mut dyn ServerHooks) {
        let arrived = self
            .ship_data_mut(h)
            .and_then(|s| s.jump_in.as_mut())
            .and_then(|j| j.arrived.take());
        if let Some(system) = arrived {
            hooks.jump_in_complete(self, h, system);
        }
    }

    /// The arrivals of a physics step to the server, in the ship list's
    /// order.
    pub(crate) fn deliver_jump_ins(&mut self, system: u32, hooks: &mut dyn ServerHooks) {
        for h in self.list(system, TickList::Ships) {
            self.deliver_jump_in(h, hooks);
        }
    }
}

/// `JumpInAction::do_simulation_controller`: the elapsed time (stored as a
/// float; the unrounded sum compares and eases). Before `speed` the ship
/// is beamed to the arrival point plus the orientation's third column
/// times the squared remaining fraction (clamped to 0..1) times the
/// approach budget; at `speed` the action completes, the ship is beamed
/// to the arrival pose, its throttle zeroed, its engine activation queued
/// and the arrival reported with the system of the ship's object database
/// (none without one).
pub(crate) fn jump_in_controller(obj: &mut Object, dt: f32, context: &mut ControllerContext<'_>) {
    let system = obj.simple.system;
    let KindData::Ship(ship) = &mut obj.data else {
        return;
    };
    let Some(j) = ship.jump_in.as_mut() else {
        return;
    };
    let elapsed = f64::from(j.elapsed) + f64::from(dt);
    j.elapsed = elapsed as f32;
    let speed = f64::from(j.speed);
    if !(elapsed >= speed) {
        let fraction = (speed - elapsed) / speed;
        // Above 1 is 1, below 0 is 0, NaN stays.
        let blend = (fraction * fraction).clamp(0.0, 1.0) * f64::from(j.approach_budget);
        let m = &j.orientation.m;
        let p = j.position;
        // The x and z offsets are stored as floats before the add; the y
        // offset joins the position unrounded.
        let off_x = (f64::from(m[0][2]) * blend) as f32;
        let off_z = (f64::from(m[2][2]) * blend) as f32;
        let at = Vector::new(
            (f64::from(off_x) + f64::from(p.x)) as f32,
            (f64::from(m[1][2]) * blend + f64::from(p.y)) as f32,
            (f64::from(off_z) + f64::from(p.z)) as f32,
        );
        context.beam(at, j.orientation);
        return;
    }
    j.complete = true;
    j.activate_engine = true;
    j.arrived = system;
    context.beam(j.position, j.orientation);
    ship.flight.throttle = 0.0;
}
