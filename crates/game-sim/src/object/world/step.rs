//! The tick: `StarSystem_Update`'s sub-step loop, the per-kind `update`
//! functions, the physics controller hook and the object sync.

use std::cell::RefCell;

use neuron_core::math::Vector;
use neuron_core::physics::{ControllerContext, ControllerHost, PhysicsWorld, StepMode};

use crate::data::constants::EngineEquipConsts;
use crate::equip::flight::{
    EngineShipIo, engine_controller, nudge_controller, steering_controller, strafe_controller,
    thruster_controller,
};
use crate::equip::{EquipKind, types};
use crate::object::{Arena, KindData, ObjHandle, Object, ship, update_flags};

use super::{ControllerRef, TickList, World, WorldEvent};

/// The controller hook: dispatches each registered controller's key to
/// its object's controller (`do_simulation_controller`).
struct Host<'a> {
    objects: &'a RefCell<&'a mut Arena<Object>>,
    controllers: &'a std::collections::BTreeMap<u64, (ObjHandle, ControllerRef)>,
    consts: &'a EngineEquipConsts,
    weapons_offline_ok: bool,
}

/// Runs one controller of an object; the pushes go to `pusher`.
pub(crate) fn run_controller(
    obj: &mut Object,
    which: ControllerRef,
    consts: &EngineEquipConsts,
    dt: f32,
    pusher: &mut dyn crate::equip::Pusher,
) {
    let orientation = obj.engine.orientation;
    let zone_drag = obj.simple_zone_drag();
    let Object { equipped, data, .. } = obj;
    let KindData::Ship(ship) = data else {
        return;
    };
    let flight = &mut ship.flight;
    match which {
        ControllerRef::Ship | ControllerRef::Guided => {}
        ControllerRef::Steering => {
            steering_controller(&flight.steering, &flight.arch, &orientation, dt, pusher);
        }
        ControllerRef::Nudge => {
            nudge_controller(&flight.nudge, &flight.arch, &orientation, dt, pusher);
        }
        ControllerRef::Strafe => {
            let arch = flight.arch;
            strafe_controller(
                &mut flight.strafe,
                &arch,
                &orientation,
                &mut flight.thrust_power,
                dt,
                pusher,
            );
        }
        ControllerRef::Equip(id) => {
            let Some(eq) = equipped.as_deref_mut() else {
                return;
            };
            let weapons_offline = eq.weapons_offline;
            let linear_drag = ship::linear_drag(flight, &eq.equipment, zone_drag, consts);
            let Some(slot) = eq.equipment.find_by_id(id) else {
                return;
            };
            let crate::object::Equipped {
                equipment, power, ..
            } = eq;
            let Some(equip) = equipment.get_mut(slot) else {
                return;
            };
            let functioning = equip.is_functioning(weapons_offline);
            let crate::equip::Equip { kind, events, .. } = equip;
            match kind {
                EquipKind::Engine(engine) => {
                    let io = EngineShipIo {
                        throttle: flight.throttle,
                        orientation: &orientation,
                        linear_drag,
                    };
                    engine_controller(engine, functioning, io, power, consts, dt, pusher);
                }
                EquipKind::Thruster(thruster) => {
                    thruster_controller(
                        thruster,
                        functioning,
                        &orientation,
                        &mut flight.thrust_power,
                        dt,
                        events,
                        pusher,
                    );
                }
                _ => {}
            }
        }
    }
}

impl ControllerHost for Host<'_> {
    fn do_simulation_controller(&mut self, key: u64, context: &mut ControllerContext<'_>) {
        let _ = self.weapons_offline_ok;
        let Some(&(h, which)) = self.controllers.get(&key) else {
            return;
        };
        let mut objects = self.objects.borrow_mut();
        let Some(obj) = objects.get_mut(h) else {
            return;
        };
        let dt = context.event.delta_time;
        if which == ControllerRef::Guided {
            let mass = context.core().mass;
            super::guided::guided_controller(obj, mass, dt, context);
            return;
        }
        run_controller(obj, which, self.consts, dt, context);
    }
}

impl Object {
    /// The drag modifier of the zone the object is in.
    pub(crate) fn simple_zone_drag(&self) -> Option<f32> {
        self.simple.zone_drag
    }
}

/// `ZONE::GetImmersion` (P2.1's `Zone::immersion`), stored to the caller's
/// float.
fn zone_immersion(zone: &crate::data::zone::Zone, pos: Vector) -> f32 {
    zone.immersion(pos) as f32
}

/// `PhySys_SyncObjectFromBody` for every moving body.
fn sync_bodies(objects: &mut Arena<Object>, world: &PhysicsWorld) {
    for &body in world.moving_objects() {
        let Some(key) = world.user_key(body) else {
            continue;
        };
        let h = ObjHandle::from_key(key);
        let Some((position, orientation)) = world.transform(body) else {
            continue;
        };
        if let Some(obj) = objects.get_mut(h) {
            obj.engine.position = position;
            obj.engine.orientation = orientation;
        }
    }
}

impl World {
    /// One server frame of a system (`StarSystem_Update`): the sub-step
    /// loop, the global time, and, with `server_physics`, the physics.
    pub fn step(&mut self, system: u32, dt: f32) {
        // `Server_StepSystems`: outside single player `AnimDB::Update`
        // first, which turns the joints the last frame registered.
        if !self.config.single_player {
            self.update_anim(dt);
        }
        let mut step = dt;
        if !self.config.single_player && f64::from(step) > 0.2 {
            step = 0.2;
        }
        let target = f64::from(dt);
        if target > 0.0 {
            let mut accum = 0.0f64;
            let mut current = step;
            loop {
                accum += f64::from(current);
                self.sub_step(system, current);
                if target - accum < f64::from(current) {
                    current = (target - accum) as f32;
                }
                if !(accum < target) {
                    break;
                }
            }
        }
        self.time += target;
    }

    fn sub_step(&mut self, system: u32, step: f32) {
        for list in [TickList::Ships, TickList::Guided] {
            self.tick_list(system, list, step);
        }
        self.tick_near_solars(system, step);
        for list in [
            TickList::Loot,
            TickList::Beams,
            TickList::Mines,
            TickList::CounterMeasures,
            TickList::Asteroids,
        ] {
            self.tick_list(system, list, step);
        }
        self.reap();
        if self.config.server_physics {
            self.move_beams(system, step);
            self.step_physics(system, step);
            self.guided_phantom_events(system);
            self.process_collisions(system);
            self.reap();
        }
    }

    fn tick_list(&mut self, system: u32, list: TickList, dt: f32) {
        for h in self.list(system, list) {
            self.update_object(h, dt, 0);
        }
    }

    /// `StarSystem_UpdateNearShips`: the solars within 10000 m of a
    /// player's ship, then those kept always updated.
    fn tick_near_solars(&mut self, system: u32, dt: f32) {
        let players: Vec<Vector> = self
            .list(system, TickList::Ships)
            .into_iter()
            .filter_map(|h| self.objects.get(h))
            .filter(|o| o.simple.owner_player != 0)
            .map(|o| o.engine.position)
            .collect();
        let mut chosen = Vec::new();
        if !players.is_empty() {
            for h in self.list(system, TickList::Solars) {
                let Some(o) = self.objects.get(h) else {
                    continue;
                };
                let near = players.iter().any(|p| {
                    let d = [
                        f64::from(p.x) - f64::from(o.engine.position.x),
                        f64::from(p.y) - f64::from(o.engine.position.y),
                        f64::from(p.z) - f64::from(o.engine.position.z),
                    ];
                    (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) <= 10000.0 * 10000.0
                });
                if near {
                    chosen.push(h);
                }
            }
        }
        for &h in self.always_update() {
            if !chosen.contains(&h) {
                chosen.push(h);
            }
        }
        for h in chosen {
            self.update_object(h, dt, 0);
        }
    }

    /// `CSimple::update_zones`: the smallest zone with a physical effect at
    /// the object's position, its drag modifier and the immersion.
    pub(crate) fn update_zones(&mut self, h: ObjHandle) {
        let Some(obj) = self.objects.get(h) else {
            return;
        };
        let Some(system) = obj.simple.system else {
            if let Some(o) = self.objects.get_mut(h) {
                o.simple.zone = None;
                o.simple.zone_drag = None;
                o.simple.scanner_interference = 0.0;
            }
            return;
        };
        let pos = obj.engine.position;
        let mut best: Option<(&crate::data::zone::Zone, f32)> = None;
        for zone in self.data.universe.system_zones(system) {
            if !zone.has_physical_effect() {
                continue;
            }
            let radius = zone.radius();
            if best.is_some_and(|(_, r)| !(radius < r)) {
                continue;
            }
            if zone.in_zone(pos, 0.0) {
                best = Some((zone, radius));
            }
        }
        let found = best.map(|(z, _)| (z.nickname, z.drag_modifier, zone_immersion(z, pos)));
        if let Some(o) = self.objects.get_mut(h) {
            match found {
                Some((zone, drag, immersion)) => {
                    o.simple.zone = Some(zone);
                    o.simple.zone_drag = Some(drag);
                    o.simple.scanner_interference = immersion;
                }
                None => {
                    o.simple.zone = None;
                    o.simple.zone_drag = None;
                    o.simple.scanner_interference = 0.0;
                }
            }
        }
    }

    /// The object's `update(dt, flags)`.
    pub fn update_object(&mut self, h: ObjHandle, dt: f32, flags: u32) {
        let Some(obj) = self.objects.get(h) else {
            return;
        };
        if obj.dead {
            return;
        }
        match obj.kind {
            crate::object::ObjectKind::Ship => self.update_ship(h, dt, flags),
            crate::object::ObjectKind::Solar => {
                self.update_equipped(h, dt, flags);
                self.update_solar_gate(h, dt);
            }
            crate::object::ObjectKind::Loot => {
                self.update_zones(h);
                let renamed = match self.objects.get_mut(h) {
                    Some(o) => {
                        let id = o.simple.id;
                        match &mut o.data {
                            KindData::Loot(l) => l.update(dt, id),
                            _ => None,
                        }
                    }
                    None => None,
                };
                if let Some(name) = renamed
                    && let Some((system, body)) = self.body(h)
                    && let Some(world) = self.physics.world_mut(system)
                {
                    world.set_collision_group(body, &name);
                }
            }
            crate::object::ObjectKind::Guided => self.update_guided(h, dt),
            crate::object::ObjectKind::Beam
            | crate::object::ObjectKind::Mine
            | crate::object::ObjectKind::CounterMeasure => self.update_projectile(h, dt),
            _ => {}
        }
    }

    fn update_projectile(&mut self, h: ObjHandle, dt: f32) {
        let Some(o) = self.objects.get_mut(h) else {
            return;
        };
        let Some(p) = o.as_projectile_mut() else {
            return;
        };
        let expired = p.update(dt);
        let alive = p.is_alive();
        if expired {
            // `expire_safe_time`: the owner can be hit now.
            match &mut o.data {
                KindData::Beam(b) => b.collision_group.clear(),
                _ => {
                    if let Some((system, body)) = o.physical.body.map(|b| (o.physical.system, b))
                        && let Some(world) = self.physics.world_mut(system)
                    {
                        world.set_collision_group(body, b"");
                    }
                }
            }
        }
        if !alive {
            // The server's projectile wrappers answer false once the
            // lifetime ran out; the step deletes them.
            self.destroy(h, 0);
        }
    }

    /// `CEqObj::update`: every item of the first list updates, then the
    /// behaviour tick (unless destroyed with weapons offline, excluded or
    /// flagged off).
    fn update_equipped(&mut self, h: ObjHandle, dt: f32, flags: u32) {
        let now = self.time;
        let Some(o) = self.objects.get_mut(h) else {
            return;
        };
        let owner_id = o.simple.id;
        let rel_health = o.simple.relative_health();
        let is_ship = o.kind == crate::object::ObjectKind::Ship;
        let Object { equipped, data, .. } = o;
        let Some(eq) = equipped.as_deref_mut() else {
            return;
        };
        let weapons_offline_destroyed = eq.weapons_offline && rel_health <= 0.0;
        let mut ship = match data {
            KindData::Ship(s) => Some(s.as_mut()),
            _ => None,
        };
        let throttle = ship.as_ref().map(|s| s.flight.throttle);
        let mut cloak = None;
        let mut regen: Vec<(crate::equip::SubObjId, f32)> = Vec::new();
        let mut guns: Vec<crate::equip::SubObjId> = Vec::new();
        let mut scanners: Vec<crate::equip::SubObjId> = Vec::new();
        let mut tractors: Vec<crate::equip::SubObjId> = Vec::new();
        let slots = eq.equipment.traverse(types::EXTERNAL_LIST, false);
        for slot in slots {
            let weapons_offline = eq.weapons_offline;
            let max_power = eq.max_power;
            let crate::object::Equipped {
                equipment, power, ..
            } = &mut *eq;
            let shield_view: Vec<(crate::equip::SubObjId, f64, f32)> = equipment
                .iter()
                .filter_map(|e| match &e.kind {
                    EquipKind::Shield(s) => Some((e.id, s.relative_health(), s.hit_pts)),
                    _ => None,
                })
                .collect();
            let Some(equip) = equipment.get_mut(slot) else {
                continue;
            };
            let functioning = equip.is_functioning(weapons_offline);
            let destroyed = equip.is_destroyed();
            let status = super::spawn::shield_status(equip);
            let id = equip.id;
            // `CAttachedEquip::Update`: a destroyed item with debris left
            // still updates.
            let updates = !destroyed || equip.attached.as_ref().is_some_and(|a| a.debris.is_some());
            let crate::equip::Equip { kind, events, .. } = equip;
            match kind {
                EquipKind::Engine(engine) => {
                    let t = if flags & update_flags::NO_DYNAMICS == 0 {
                        throttle
                    } else {
                        None
                    };
                    engine.update(functioning, dt, t, events);
                }
                EquipKind::Power(plant) => {
                    let thrust = ship
                        .as_deref_mut()
                        .map(|s| (&mut s.flight.thrust_power, s.flight.max_thrust_power));
                    plant.recharge(functioning, destroyed, dt, (power, max_power), thrust);
                }
                EquipKind::Gun(g) => {
                    g.launcher.update(dt);
                    if updates {
                        guns.push(id);
                    }
                }
                EquipKind::MineDropper(l) | EquipKind::CmDropper(l) => l.update(dt),
                EquipKind::Shield(s) => s.update(status, now, events),
                EquipKind::ShieldGenerator(g) => {
                    let real_dt = (now - g.last_update_time) as f32;
                    g.last_update_time = now;
                    if let Some(shield_id) = g.linked_shield
                        && let Some(&(_, rel, hit)) =
                            shield_view.iter().find(|(id, _, _)| *id == shield_id)
                        && let Some(new_hit) = g.regenerate(functioning, real_dt, rel, hit, power)
                    {
                        regen.push((shield_id, new_hit));
                    }
                }
                EquipKind::Scanner(_) => scanners.push(id),
                EquipKind::Tractor(_) => tractors.push(id),
                EquipKind::Cloak(c) => {
                    if let Some(p) = c.update(destroyed, dt) {
                        cloak = Some(p);
                    }
                }
                _ => {}
            }
        }
        for (shield_id, hit) in regen {
            if let Some(shield) = eq.equipment.by_id_mut(shield_id) {
                let status = super::spawn::shield_status(shield);
                let crate::equip::Equip { kind, events, .. } = shield;
                if let EquipKind::Shield(s) = kind {
                    s.set_hit_points(hit, status, now, events);
                }
            }
        }
        if let Some(c) = cloak {
            eq.cloak_percent = c;
        }
        let behavior = eq.has_behavior
            && !weapons_offline_destroyed
            && flags & update_flags::NO_BEHAVIOR == 0
            && eq.control_exclusion & 1 == 0;
        let _ = is_ship;
        // `CEGun::Update`'s turret aim and lock clock (each gun's own
        // update; nothing else in the list reads what it changes).
        for id in guns {
            self.update_gun_turret(h, id, dt);
        }
        for id in scanners {
            self.update_scanner(h, id, flags);
        }
        for id in tractors {
            self.update_tractor(h, id, dt);
        }
        // Equipment events go to the listeners.
        if let Some(eq) = self
            .objects
            .get_mut(h)
            .and_then(|o| o.equipped.as_deref_mut())
        {
            for e in eq.equipment.iter_mut() {
                for event in e.events.drain(..) {
                    self.events.push(WorldEvent::Equip {
                        owner: owner_id,
                        equip: e.id,
                        event,
                    });
                }
            }
        }
        self.sync_shield_collisions(h);
        if behavior && let Some(mut hook) = self.take_behavior() {
            hook.update_behavior_tick(self, h, dt);
            self.put_behavior(Some(hook));
        }
    }

    /// `CShip::update`: zones, `CEqObj::update`, the wake-up and the
    /// damping (drag over mass and inertia), the target check.
    fn update_ship(&mut self, h: ObjHandle, dt: f32, flags: u32) {
        self.update_zones(h);
        self.update_equipped(h, dt, flags);
        if flags & update_flags::NO_DYNAMICS == 0 {
            let consts = self.data.constants.engine.clone();
            let Some((system, body)) = self.body(h) else {
                return;
            };
            let Some(world) = self.physics.world_mut(system) else {
                return;
            };
            world.wakeup(body);
            let mass = world.mass(body).unwrap_or(1.0);
            let inertia = world.moment_of_inertia(body).unwrap_or(Vector::ZERO);
            if let Some(o) = self.objects.get(h)
                && let (Some(eq), Some(ship)) = (o.equipped.as_deref(), o.as_ship())
            {
                let (linear, angular) = ship::damping(
                    &ship.flight,
                    &eq.equipment,
                    o.simple.zone_drag,
                    &consts,
                    mass,
                    inertia,
                );
                world.set_damping(body, linear, angular);
            }
        }
        self.update_bay(h);
        // A target that cannot be targeted any more is dropped.
        let target = self
            .objects
            .get(h)
            .and_then(|o| o.as_ship())
            .and_then(|s| s.target);
        if let Some(t) = target {
            let keep = self
                .objects
                .get(t)
                .is_some_and(|o| !o.dead && o.simple.targetable);
            if !keep && let Some(s) = self.objects.get_mut(h).and_then(|o| o.as_ship_mut()) {
                s.target = None;
                s.sub_target = 0;
            }
        }
        self.apply_wakeups(h);
    }

    /// The `wakeup()` calls the ship's controllers asked for.
    pub(crate) fn apply_wakeups(&mut self, h: ObjHandle) {
        let Some(o) = self.objects.get_mut(h) else {
            return;
        };
        let mut wake = false;
        if let KindData::Ship(s) = &mut o.data {
            for flag in [
                &mut s.flight.steering.wake_requested,
                &mut s.flight.nudge.wake_requested,
                &mut s.flight.strafe.wake_requested,
            ] {
                wake |= std::mem::take(flag);
            }
        }
        if let Some(eq) = o.equipped.as_deref_mut() {
            for e in eq.equipment.iter_mut() {
                match &mut e.kind {
                    EquipKind::Engine(s) => wake |= std::mem::take(&mut s.wake_requested),
                    EquipKind::Thruster(t) => wake |= std::mem::take(&mut t.wake_requested),
                    _ => {}
                }
            }
        }
        if wake
            && let Some((system, body)) = self.body(h)
            && let Some(world) = self.physics.world_mut(system)
        {
            world.wakeup(body);
        }
    }

    /// `PhySys::Update(system, dt)` with the controller hook and the sync,
    /// then the instance trees of the moved objects.
    pub fn step_physics(&mut self, system: u32, dt: f32) {
        let mode = self.config.step_mode;
        let consts = self.data.constants.engine.clone();
        let objects = RefCell::new(&mut self.objects);
        let mut host = Host {
            objects: &objects,
            controllers: &self.controllers,
            consts: &consts,
            weapons_offline_ok: true,
        };
        let mut sync = |world: &PhysicsWorld| {
            sync_bodies(&mut objects.borrow_mut(), world);
        };
        self.physics.update(system, dt, mode, &mut host, &mut sync);
        let moved: Vec<ObjHandle> = self
            .physics
            .world(system)
            .map(|w| {
                w.moving_objects()
                    .iter()
                    .filter_map(|b| w.user_key(*b))
                    .map(ObjHandle::from_key)
                    .collect()
            })
            .unwrap_or_default();
        for h in moved {
            self.sync_instance(h);
        }
        let _ = StepMode::SinglePlayer;
    }
}
