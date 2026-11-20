//! Moving objects between systems and unloading a system (decision record
//! 0007, points 5 and 6).
//!
//! `GameObject::leave_system` (`Server/server_gameobject.cpp:459`) saves
//! the body's physics parameters (`PhySys::CreateParms`), turns the
//! object's controllers off and destroys its body (`unmake_physical`);
//! `GameObject::enter_system` (`:417`) rebuilds the body from the saved
//! parameters in the new system (`remake_physical`) and turns the
//! controllers back on. Between the two the object is in transit: it is in
//! no system's lists, has no body and is found by id. The server's own
//! parts of the move (the observers, `notify_object_left`, the lookup
//! cache) belong to `game-logic`.
//!
//! `StarSystem::unload` (`Server/server_classes.cpp:11102`) deletes every
//! object of a system's lists, its asteroid fields and its physics world.

use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::CreateParms;

use crate::equip::EquipKind;
use crate::object::{KindData, ObjHandle, ObjectKind};

use super::{ControllerRef, TickList, World};

impl World {
    /// `CObject::get_mass`: the body's mass, or the archetype's without a
    /// body (a solar always reads its archetype's).
    fn object_mass(&self, h: ObjHandle) -> f32 {
        let Some(obj) = self.objects.get(h) else {
            return 0.0;
        };
        let archetype_mass = obj.arch_mass;
        if obj.kind == ObjectKind::Solar {
            return archetype_mass;
        }
        match (obj.physical.body, self.physics.world(obj.physical.system)) {
            (Some(body), Some(world)) => world.mass(body).unwrap_or(archetype_mass),
            _ => archetype_mass,
        }
    }

    /// `disable_controllers`: `CEqObj`'s walk over every item (engines and
    /// thrusters unregister), then a ship's steering, nudge and strafe
    /// engines and the ship itself (`CShip::disable_controllers`).
    fn disable_controllers(&mut self, h: ObjHandle) {
        let items: Vec<_> = self
            .objects
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .map(|eq| {
                eq.equipment
                    .iter()
                    .filter(|e| matches!(e.kind, EquipKind::Engine(_) | EquipKind::Thruster(_)))
                    .map(|e| e.id)
                    .collect()
            })
            .unwrap_or_default();
        for id in items {
            self.unregister(h, ControllerRef::Equip(id));
            if let Some(e) = self
                .objects
                .get_mut(h)
                .and_then(|o| o.equipped.as_deref_mut())
                .and_then(|eq| eq.equipment.by_id_mut(id))
            {
                match &mut e.kind {
                    EquipKind::Engine(s) => s.controller_enabled = false,
                    EquipKind::Thruster(t) => t.controller_enabled = false,
                    _ => {}
                }
            }
        }
        let is_ship = self
            .objects
            .get(h)
            .is_some_and(|o| matches!(o.data, KindData::Ship(_)));
        if !is_ship {
            return;
        }
        for which in [
            ControllerRef::Steering,
            ControllerRef::Nudge,
            ControllerRef::Strafe,
            ControllerRef::Ship,
        ] {
            self.unregister(h, which);
        }
        if let Some(obj) = self.objects.get_mut(h)
            && let KindData::Ship(ship) = &mut obj.data
        {
            ship.flight.steering.controller_enabled = false;
            ship.flight.nudge.controller_enabled = false;
            ship.flight.strafe.controller_enabled = false;
        }
    }

    /// `enable_controllers`: an active engine and every thruster register
    /// (`CEEngine::EnableController` tests `IsActive`), in equipment order,
    /// then a ship's steering, nudge and strafe engines and the ship.
    fn enable_controllers(&mut self, h: ObjHandle) {
        let items: Vec<_> = self
            .objects
            .get(h)
            .and_then(|o| o.equipped.as_deref())
            .map(|eq| {
                eq.equipment
                    .iter()
                    .filter(|e| match e.kind {
                        EquipKind::Engine(_) => e.is_active(),
                        EquipKind::Thruster(_) => true,
                        _ => false,
                    })
                    .map(|e| e.id)
                    .collect()
            })
            .unwrap_or_default();
        for id in items {
            self.register_equip_controller(h, id);
            if let Some(e) = self
                .objects
                .get_mut(h)
                .and_then(|o| o.equipped.as_deref_mut())
                .and_then(|eq| eq.equipment.by_id_mut(id))
            {
                match &mut e.kind {
                    EquipKind::Engine(s) => s.controller_enabled = true,
                    EquipKind::Thruster(t) => t.controller_enabled = true,
                    _ => {}
                }
            }
        }
        let is_ship = self
            .objects
            .get(h)
            .is_some_and(|o| matches!(o.data, KindData::Ship(_)));
        if !is_ship {
            return;
        }
        for which in [
            ControllerRef::Steering,
            ControllerRef::Nudge,
            ControllerRef::Strafe,
            ControllerRef::Ship,
        ] {
            self.register_controller(h, which);
        }
        if let Some(obj) = self.objects.get_mut(h)
            && let KindData::Ship(ship) = &mut obj.data
        {
            ship.flight.steering.controller_enabled = true;
            ship.flight.nudge.controller_enabled = true;
            ship.flight.strafe.controller_enabled = true;
        }
    }

    /// `GameObject::leave_system` with `OutParms`: the body's parameters
    /// (mass, inertia, collision state, damping, collision group), the
    /// controllers off, the body destroyed (`unmake_physical`), and the
    /// object out of its system's lists. The object keeps the parameters
    /// ([`World::transit_parms`]) and its id; it is in transit until
    /// [`World::enter_system`]. Returns the parameters, or `None` for an
    /// unknown object or one already in transit.
    pub fn leave_system(&mut self, h: ObjHandle) -> Option<CreateParms> {
        let obj = self.objects.get(h)?;
        if obj.transit.is_some() {
            return None;
        }
        let system = obj.physical.system;
        let body = obj.physical.body;
        let mass = self.object_mass(h);
        let world = self.physics.world(system);
        let (inertia, enable, damping, name) = match (body, world) {
            (Some(b), Some(w)) => (
                w.moment_of_inertia(b).unwrap_or(Vector::ZERO),
                w.collision_state(b),
                w.damping(b).unwrap_or((0.0, Vector::ZERO)),
                w.collision_group(b),
            ),
            // `PhySys::GetCollisionState`, `GetDamping` and
            // `GetCollisionGroup` of an object with no body read zeros
            // and an empty name.
            _ => (Vector::ZERO, false, (0.0, Vector::ZERO), Vec::new()),
        };
        let parms = CreateParms {
            mass,
            linear_damping: damping.0,
            angular_damping: damping.1,
            name,
            enable_collisions: enable,
            inertia: Some(inertia),
        };
        // `unmake_physical`: the controllers, then the body and the bodies
        // of the parts below it that are objects of their own.
        self.disable_controllers(h);
        if let Some(obj) = self.objects.get_mut(h) {
            let body = obj.physical.body.take();
            let free: Vec<_> = obj.physical.free_parts.drain(..).map(|p| p.body).collect();
            obj.transit = Some(parms.clone());
            if let Some(world) = self.physics.world_mut(system) {
                if let Some(body) = body {
                    world.destroy_body(body);
                }
                for part in free {
                    world.destroy_body(part);
                }
            }
        }
        if let Some(sys) = self.systems.get_mut(&system) {
            for list in sys.lists.values_mut() {
                list.retain(|x| *x != h);
            }
        }
        self.always_update.retain(|x| *x != h);
        if let Some(obj) = self.objects.get_mut(h) {
            obj.physical.system = 0;
        }
        Some(parms)
    }

    /// The parameters an object in transit keeps; `None` when it is in a
    /// system.
    #[must_use]
    pub fn transit_parms(&self, h: ObjHandle) -> Option<&CreateParms> {
        self.objects.get(h).and_then(|o| o.transit.as_ref())
    }

    /// Whether an object is in transit (in no system, with no body).
    #[must_use]
    pub fn in_transit(&self, h: ObjHandle) -> bool {
        self.transit_parms(h).is_some()
    }

    /// `StarSystem::add_ship`'s object part and `GameObject::enter_system`
    /// with the saved parameters: the object at `position` and
    /// `orientation` in `system` (which must be loaded), its instance tree
    /// updated, its body rebuilt from the parameters in that system's
    /// physics world (`remake_physical`, collisions on when they were on),
    /// the controllers on, and the object appended to its list. Returns
    /// false for an unknown object, one not in transit, or a system with
    /// no physics world; nothing changes then.
    pub fn enter_system(
        &mut self,
        h: ObjHandle,
        system: u32,
        position: Vector,
        orientation: Matrix,
    ) -> bool {
        if self.physics.world(system).is_none() {
            return false;
        }
        let Some(obj) = self.objects.get_mut(h) else {
            return false;
        };
        let Some(parms) = obj.transit.take() else {
            return false;
        };
        obj.engine.position = position;
        obj.engine.orientation = orientation;
        obj.physical.system = system;
        let kind = obj.kind;
        self.sync_instance(h);
        let enable = parms.enable_collisions;
        // `ReMakePhysicalR`: the template from the parameters, then
        // `enable_collisions_r` when they had collisions on.
        let parms = CreateParms {
            enable_collisions: false,
            ..parms
        };
        self.rebuild_body(h, parms);
        if enable {
            self.set_collisions(h, true);
        }
        self.enable_controllers(h);
        self.systems
            .entry(system)
            .or_default()
            .lists
            .entry(TickList::of(kind))
            .or_default()
            .push(h);
        true
    }

    /// `StarSystem::unload`'s world part: every object in the system's
    /// lists removed at once (`StarSystem_DeleteBlock` for each list), the
    /// asteroid fields dropped (`SrvAsteroid_UnloadSystem`,
    /// `CmnAsteroid::Unload`), and the physics world shut down
    /// (`PhySys::Shutdown`). Objects in transit are not in the lists and
    /// stay. Returns the number of objects removed.
    pub fn unload_system(&mut self, system: u32) -> usize {
        let Some(sys) = self.systems.get(&system) else {
            return 0;
        };
        let handles: Vec<ObjHandle> = sys.lists.values().flatten().copied().collect();
        let count = handles.len();
        for h in handles {
            self.remove_object(h);
        }
        self.asteroid_fields.remove(&system);
        self.systems.remove(&system);
        self.physics.shutdown(system);
        count
    }

    /// Whether a system is loaded ([`World::load_system`]).
    #[must_use]
    pub fn system_loaded(&self, system: u32) -> bool {
        self.systems.contains_key(&system)
    }
}
