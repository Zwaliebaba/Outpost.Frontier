//! The world as the flight AI sees it (decision record 0001 point 8): the
//! `IObjInspect` slots the behaviours read ([`ObjectView`]), their
//! `IObjAffect` commands turned into the world's typed [`Command`]s, the
//! physics queries, the motion model of a ship (`CShip`'s speed and turn
//! predictions, slots 50 to 63) and the CRT `rand()` the AI shares with
//! the world.

use neuron_core::math::{Matrix, Vector};

use crate::ai::behavior::DirectiveOp;
use crate::ai::fp::{e, r};
use crate::ai::manager::BehaviorManager;
use crate::ai::personality::Personality;
use crate::ai::world::{
    AiCommand, AiRand, AiWorld, EngineStatus, MotionModel, ObjectView, RayContact, SpatialQueries,
    StrafeDir as AiStrafe, ThrustEquipType,
};
use crate::equip::flight::StrafeDir;
use crate::equip::{EquipKind, types};
use crate::object::ship::{self, ThrustKind};
use crate::object::{Command, ObjHandle};

use super::World;

impl World {
    /// `IObjInspect` of one object for the AI.
    #[must_use]
    pub fn ai_view(&self, h: ObjHandle) -> Option<ObjectView> {
        let i = self.inspect(h)?;
        let o = self.get(h)?;
        let id_of = |x: Option<ObjHandle>| x.and_then(|x| self.get(x)).map(|x| x.simple.id);
        let eq = o.as_equipped();
        let ship_data = o.as_ship();
        let engine = eq.and_then(|eq| {
            let slot = eq.equipment.find_first(types::ENGINE)?;
            let item = eq.equipment.get(slot)?;
            Some((item, item.id))
        });
        let shield = eq
            .and_then(|eq| eq.equipment.first(types::SHIELD))
            .and_then(|e| match &e.kind {
                EquipKind::Shield(s) => Some((s.hit_pts, s.max_hit_pts, s.online)),
                _ => None,
            });
        let cruise = ship_data.map(|_| match engine {
            Some((item, _)) => match &item.kind {
                EquipKind::Engine(s) if s.cruise_active => {
                    (true, s.arch.cruise_charge_time > s.cruise_charge_elapsed)
                }
                _ => (false, false),
            },
            None => (false, false),
        });
        let thrusters = ship_data.map(|_| {
            eq.is_some_and(|eq| {
                eq.equipment
                    .traverse(types::THRUSTER, false)
                    .into_iter()
                    .filter_map(|s| eq.equipment.get(s))
                    .any(|t| t.is_active())
            })
        });
        // `get_physical_radius`: `CSimple`'s cache, which the reference
        // refreshes at every physics sync (`cache_physical_props`:
        // `PhySys::GetRadiusR`, the core's radius and the centre of mass);
        // computed here from the body as it is now.
        let (radius, center) = self.body_sphere(h);
        Some(ObjectView {
            id: i.id,
            class: i.kind.class(),
            type_flags: i.type_bits,
            position: i.position,
            orientation: i.orientation,
            velocity: i.velocity,
            angular_velocity: i.angular_velocity,
            radius,
            center,
            center_of_mass: i.position,
            extents: self.surface_extents(h),
            mass: i.mass,
            hit_points: (i.hit_pts, i.max_hit_pts),
            shield,
            throttle: i.ship.as_ref().map(|s| s.throttle),
            axis_throttle: i.ship.as_ref().map(|s| s.axis_throttle),
            nudge: i.ship.as_ref().map(|s| s.nudge),
            strafe: i
                .ship
                .as_ref()
                .map(|s| AiStrafe::from_value(s.strafe.value())),
            cruise,
            thrusters,
            target: i.ship.as_ref().and_then(|s| id_of(s.target)),
            subtarget: i.ship.as_ref().map(|s| s.sub_target),
            rank: None,
            affiliation: None,
            base: eq.map(|e| e.docking_base).filter(|&b| b != 0),
            dock_target: None,
            power: i.power,
            zone_props: None,
            formation_leader: ship_data.and_then(|s| id_of(s.formation_leader)),
            formation_offset: ship_data.map(|s| s.formation_offset),
            follow_leader: ship_data.and_then(|s| id_of(s.follow_leader)),
            follow_offset: ship_data.map(|s| s.follow_offset),
            formation_followers: ship_data
                .map(|s| {
                    s.formation_followers
                        .iter()
                        .filter_map(|f| id_of(Some(*f)))
                        .collect()
                })
                .unwrap_or_default(),
            is_player: ship_data.is_some_and(|s| s.is_player),
            is_dying: i.dead,
            is_targetable: i.targetable,
            toughness: 0,
            behavior_id: eq.map_or(-1, |e| e.behavior_index),
            max_bank_angle: 0.0,
            scanner_interference: o.simple.scanner_interference,
            atmosphere_range: match &o.data {
                crate::object::KindData::Solar(s) => s.atmosphere_range,
                _ => 0.0,
            },
            engine: engine.map(|(item, id)| EngineStatus {
                id,
                active: item.is_active(),
                destroyed: item.is_destroyed(),
            }),
        })
    }
}

impl World {
    /// `get_archetype_extents` (`CObject::get_surface_extents`): the
    /// `exts` corners of the physics part the object was built from (its
    /// root part, or the single part of a simple model); `None` without
    /// one.
    fn surface_extents(&self, h: ObjHandle) -> Option<(Vector, Vector)> {
        let o = self.get(h)?;
        let root_part = o.physical.part_map.first().map_or(0, |p| p.part_id);
        let parts = self.surfaces.get(&o.archetype)?.as_ref()?;
        let part = parts
            .get(&root_part)
            .or_else(|| (root_part == 0 && parts.len() == 1).then(|| parts.values().next())?)?;
        part.extents.map(|x| (x.min, x.max))
    }
}

/// The motion model of one ship (`CShip::get_time_to_accelerate` and the
/// rest of slots 50 to 63), over the world's data.
struct ShipMotion<'a> {
    world: &'a World,
    h: ObjHandle,
}

impl ShipMotion<'_> {
    fn parts(&self) -> Option<(&ship::ShipData, &crate::object::Equipped, Option<f32>, f32)> {
        let o = self.world.get(self.h)?;
        Some((
            o.as_ship()?,
            o.as_equipped()?,
            o.simple.zone_drag,
            o.simple.scanner_interference,
        ))
    }

    /// `collect_force_components(throttle, type)`.
    fn forces(&self, throttle: f32, kind: ThrustEquipType) -> (f32, f32) {
        let Some((s, eq, zone, interference)) = self.parts() else {
            return (0.0, 0.0);
        };
        let consts = &self.world.data.constants.engine;
        let kind = match kind {
            // `collect_force_components` has no case for 0 or another
            // value: the archetype's drag only.
            ThrustEquipType::Normal | ThrustEquipType::Other(_) => None,
            ThrustEquipType::Engine => Some(ThrustKind::Engine),
            ThrustEquipType::Thrusters => Some(ThrustKind::Thruster),
            ThrustEquipType::Tradelane => Some(ThrustKind::Tradelane),
            ThrustEquipType::Cruise => Some(ThrustKind::Cruise),
        };
        let zone = zone.map(|m| (m, interference));
        let Some(kind) = kind else {
            // No engine term: the archetype's drag and the zone's share.
            let mut drag = s.flight.arch.linear_drag;
            let base = drag;
            if let Some((m, i)) = zone {
                drag = r(e(m) * e(i) * e(base) + e(drag));
            }
            return (0.0, drag);
        };
        let linear = ship::linear_drag(&s.flight, &eq.equipment, zone.map(|z| z.0), consts);
        ship::collect_force_components(
            &s.flight,
            &eq.equipment,
            throttle,
            kind,
            zone,
            consts,
            linear,
        )
    }

    fn mass(&self) -> f32 {
        self.world.mass(self.h)
    }

    fn inertia(&self) -> Vector {
        self.world
            .body(self.h)
            .and_then(|(system, body)| self.world.physics(system)?.moment_of_inertia(body))
            .unwrap_or(Vector::ZERO)
    }

    /// `GetAxisSteeringTorque(axis, factor)` rounded, and the angular drag
    /// factor with the archetype's drags.
    fn torque(&self, axis: u32, factor: f32) -> f32 {
        self.parts().map_or(0.0, |(s, ..)| {
            r(s.flight
                .steering
                .axis_steering_torque(&s.flight.arch, axis, factor))
        })
    }

    fn angular_drag(&self, axis: usize) -> f32 {
        self.parts().map_or(0.0, |(s, ..)| {
            r(e(s.flight.angular_drag_factor) * e(s.flight.arch.angular_drag[axis]))
        })
    }
}

/// `CShip::get_time_to_accelerate(ForceOverMass, Mass, Drag, From, To)`.
fn time_to_accelerate_raw(force_over_mass: f32, mass: f32, drag: f32, from: f32, to: f32) -> f32 {
    let mut to = to;
    if e(from) * e(to) < 0.0 || from < 0.0 || to < 0.0 {
        return 0.0;
    }
    let force = r(e(force_over_mass) * e(mass));
    if drag == 0.0 {
        return r((e(to) - e(from)) * (e(mass) / e(force)));
    }
    let terminal = e(force) / e(drag);
    let terminal_f = r(terminal);
    if terminal > 0.0 {
        let cap = e(terminal_f) * e(0.99);
        if e(to) > cap {
            to = r(cap);
        }
    } else if terminal_f <= 0.0 && e(to) < 0.01 {
        to = 0.01;
    }
    let gap = e(from) - e(terminal_f);
    if gap == 0.0 {
        return 0.0;
    }
    let gap_f = r(gap);
    let ratio = (e(to) - e(terminal_f)) / e(gap_f);
    if !(ratio > 0.0) {
        return 0.0;
    }
    r(-(ratio.ln() * e(mass) / e(drag)))
}

/// The distance travelled under drag from `from` for `time`
/// (`get_distance_travelled`'s closed form).
fn distance_raw(accel: f32, mass: f32, drag: f32, from: f32, time: f32) -> f32 {
    if time < 0.0 || drag < 0.01 {
        return 0.0;
    }
    let start = if from < 0.0 { 0.0 } else { e(from) };
    let t = if time > 1000.0 { 1000.0 } else { e(time) };
    let terminal = e(mass) / e(drag) * e(accel);
    let decay = (-1.0 / e(mass) * t * e(drag)).exp();
    r(((1.0 - decay) * ((start - terminal) * e(mass))) / e(drag) + terminal * t)
}

impl MotionModel for ShipMotion<'_> {
    fn time_to_accelerate(&self, throttle: f32, from: f32, to: f32, kind: ThrustEquipType) -> f32 {
        let from = if from < 0.0 { 0.0 } else { from };
        let to = if to < 0.0 { 0.0 } else { to };
        let throttle = throttle.clamp(-1.0, 1.0);
        if from == 0.0 && throttle == 0.0 {
            return 0.0;
        }
        let mass = self.mass();
        if !(e(mass) > 0.01) {
            return 0.0;
        }
        let (force, drag) = self.forces(throttle, kind);
        time_to_accelerate_raw(r(e(force) / e(mass)), mass, drag, from, to)
    }

    fn distance_travelled(
        &self,
        throttle: f32,
        from: f32,
        time: f32,
        kind: ThrustEquipType,
    ) -> f32 {
        if time < 0.0 {
            return 0.0;
        }
        let from = if from < 0.0 { 0.0 } else { from };
        let throttle = throttle.clamp(-1.0, 1.0);
        let mass = self.mass();
        if !(e(mass) > 0.01) {
            return 0.0;
        }
        let (force, drag) = self.forces(throttle, kind);
        distance_raw(r(e(force) / e(mass)), mass, drag, from, time)
    }

    fn projected_throttle(&self, speed: f32, kind: ThrustEquipType) -> f32 {
        let direction = if speed > 0.0 { 1.0 } else { -1.0 };
        let (force, drag) = self.forces(direction, kind);
        if !(drag >= 0.0) {
            return 0.0;
        }
        if !(e(force).abs() > e(0.01)) {
            return 0.0;
        }
        let result = e(speed).abs() * e(drag) / e(force);
        if result > 1.0 { 1.0 } else { r(result) }
    }

    fn speed(&self, throttle: f32, kind: ThrustEquipType) -> f32 {
        let throttle = throttle.clamp(-1.0, 1.0);
        if !(e(self.mass()) > 0.01) {
            return 0.0;
        }
        let (force, drag) = self.forces(throttle, kind);
        if !(drag > 0.001) {
            return 0.0;
        }
        r(e(force) / e(drag))
    }

    fn initial_speed_to_coast_distance(&self, speed: f32, stop: bool) -> f32 {
        if speed <= 0.0 {
            return 0.0;
        }
        let mass = self.mass();
        if !(e(mass) > 0.01) {
            return 0.0;
        }
        let kind = if stop {
            ThrustEquipType::Engine
        } else {
            ThrustEquipType::Normal
        };
        let (_, drag) = self.forces(0.0, kind);
        if drag < 0.01 || mass < 0.0 {
            return 0.0;
        }
        r(e(drag) / e(mass) * e(speed))
    }

    fn time_to_accelerate_angularly(&self, throttle: f32, from: f32, to: f32) -> f32 {
        let inertia = self.inertia();
        let accel = r(e(self.torque(2, throttle)) / e(inertia.z));
        time_to_accelerate_raw(accel, inertia.z, self.angular_drag(2), from, to)
    }

    fn time_to_accelerate_angularly_xy(&self, pitch: f32, yaw: f32, from: f32, to: f32) -> f32 {
        let inertia = self.inertia();
        let pitch_accel = r(e(self.torque(0, pitch)) / e(inertia.x));
        let yaw_accel = r(e(self.torque(1, yaw)) / e(inertia.y));
        let drag = self.angular_drag(1);
        let combined_inertia =
            r((e(inertia.y) * e(inertia.y) + e(inertia.x) * e(inertia.x)).sqrt());
        let combined_accel =
            r((e(yaw_accel) * e(yaw_accel) + e(pitch_accel) * e(pitch_accel)).sqrt());
        time_to_accelerate_raw(combined_accel, combined_inertia, drag, from, to)
    }

    fn angular_distance_travelled(&self, throttle: f32, from: f32, time: f32) -> f32 {
        let inertia = self.inertia();
        let accel = r(e(self.torque(2, throttle)) / e(inertia.z));
        distance_raw(accel, inertia.z, self.angular_drag(2), from, time)
    }

    fn angular_distance_travelled_xy(&self, pitch: f32, yaw: f32, from: f32, time: f32) -> f32 {
        let inertia = self.inertia();
        let pitch_accel = r(e(self.torque(0, pitch)) / e(inertia.x));
        let yaw_accel = e(self.torque(1, yaw)) / e(inertia.y);
        let drag = self.angular_drag(1);
        let combined_accel = r((yaw_accel * yaw_accel + e(pitch_accel) * e(pitch_accel)).sqrt());
        let combined_inertia =
            r((e(inertia.y) * e(inertia.y) + e(inertia.x) * e(inertia.x)).sqrt());
        distance_raw(combined_accel, combined_inertia, drag, from, time)
    }

    fn angular_speed_xy(&self, pitch_rate: f32, yaw_rate: f32) -> f32 {
        let Some((s, ..)) = self.parts() else {
            return 0.0;
        };
        let f = e(s.flight.angular_drag_factor);
        let a = s.flight.arch.angular_drag;
        let pitch = r(e(self.torque(0, pitch_rate)) / (f * e(a[0])));
        let yaw = e(self.torque(1, yaw_rate)) / (f * e(a[1]));
        r((yaw * yaw + e(pitch) * e(pitch)).sqrt())
    }

    fn angular_speed_z(&self, rate: f32) -> f32 {
        let Some((s, ..)) = self.parts() else {
            return 0.0;
        };
        let f = e(s.flight.angular_drag_factor);
        r(e(self.torque(2, rate)) / (f * e(s.flight.arch.angular_drag[2])))
    }

    fn projected_axis_throttle_xy(&self, speed: f32) -> f32 {
        let inertia = self.inertia();
        let pitch = r(e(self.torque(0, 1.0)) / e(inertia.x));
        let yaw = e(self.torque(1, 1.0)) / e(inertia.y);
        let drag = self.angular_drag(1);
        let combined = (yaw * yaw + e(pitch) * e(pitch)).sqrt();
        if !(combined.abs() > e(0.01)) {
            return 0.0;
        }
        r(e(drag) * e(speed) / (combined * e(inertia.y)))
    }

    fn projected_axis_throttle_z(&self, speed: f32) -> f32 {
        let inertia = self.inertia();
        let roll = e(self.torque(2, 1.0)) / e(inertia.z);
        let drag = self.angular_drag(2);
        if !(roll.abs() > e(0.01)) {
            return 0.0;
        }
        r(e(drag) * e(speed) / (e(inertia.z) * roll))
    }
}

impl AiWorld for World {
    type Handle = ObjHandle;

    fn inspect(&self, h: ObjHandle) -> Option<ObjectView> {
        self.ai_view(h)
    }

    fn handle_of(&self, id: u32) -> Option<ObjHandle> {
        self.find(id)
    }

    fn system_objects(&self, system: u32, class_mask: u32) -> Vec<ObjHandle> {
        self.objects_in(system)
            .into_iter()
            .filter(|h| {
                self.get(*h)
                    .is_some_and(|o| o.kind.class() & class_mask == class_mask)
            })
            .collect()
    }

    fn attitude(&self, _from: ObjHandle, _to: ObjHandle) -> Option<f32> {
        // Reputation is not ported (the server's groups).
        None
    }

    fn attitude_symmetrical(&self, _from: ObjHandle, _to: ObjHandle) -> Option<(f32, f32)> {
        None
    }

    fn hardpoint(&self, h: ObjHandle, name: &[u8]) -> Option<(Vector, Matrix)> {
        let o = self.get(h)?;
        let (_, data) = self.find_hardpoint(o.engine.instance, name)?;
        Some((data.position(), data.orientation()))
    }

    fn motion(&self, h: ObjHandle) -> Option<Box<dyn MotionModel + '_>> {
        self.get(h)?.as_ship()?;
        Some(Box::new(ShipMotion { world: self, h }))
    }

    fn system_of(&self, h: ObjHandle) -> Option<u32> {
        Some(self.get(h)?.physical.system)
    }

    fn find_equipment(&self, h: ObjHandle, type_mask: u32) -> Option<Vec<u16>> {
        let eq = self.get(h)?.as_equipped()?;
        Some(
            eq.equipment
                .traverse(type_mask, false)
                .into_iter()
                .filter_map(|s| eq.equipment.get(s).map(|x| x.id))
                .collect(),
        )
    }

    /// The item's state and the gun values the system manager reads. A
    /// gun's munition kind is read from its projectile's seeker only (the
    /// cruise-disruptor and hardpoint-type kinds need the munition
    /// archetype the launcher does not keep), and the launcher status
    /// (`EquipFlags`) and barrels are not exposed yet, so fire control
    /// does not fire in the Rust world.
    fn equipment(&self, h: ObjHandle, id: u16) -> Option<crate::ai::world::EquipInfo> {
        let eq = self.get(h)?.as_equipped()?;
        let item = eq.equipment.by_id(id)?;
        let mut flags = 0;
        if item.active {
            flags |= crate::ai::world::EQUIP_ACTIVE;
        }
        if item.destroyed {
            flags |= crate::ai::world::EQUIP_DESTROYED;
        }
        if item.is_disabled(eq.weapons_offline) {
            flags |= crate::ai::world::EQUIP_DISABLED;
        }
        let mut info = crate::ai::world::EquipInfo {
            flags,
            ..crate::ai::world::EquipInfo::default()
        };
        if let EquipKind::Gun(gun) = &item.kind
            && !item.destroyed
        {
            let seeker = gun.launcher.projectile.is_some_and(|p| p.seeker != 0);
            info.munition_kind = Some(i32::from(seeker));
            info.auto_turret = Some(gun.auto_turret);
            info.seeker = Some(u8::from(seeker));
            info.target_id = Some(
                gun.target
                    .and_then(|(t, _)| self.get(t))
                    .map_or(0, |o| o.simple.id),
            );
        }
        Some(info)
    }

    fn manager_of(&self, h: ObjHandle) -> Option<(i32, [i32; 3])> {
        self.behavior_managers
            .get(&h)
            .map(|m| (m.state.behavior, m.state.directive))
    }

    fn command(&mut self, h: ObjHandle, command: AiCommand<ObjHandle>) -> i32 {
        let typed = match command {
            AiCommand::SetThrottle(t) => Command::SetThrottle(t),
            AiCommand::SetAxisThrottle(v) => Command::SetAxisThrottle(v),
            AiCommand::SetNudge(v) => Command::SetNudgeVec(v),
            AiCommand::SetStrafe(d) => Command::SetStrafeDir(StrafeDir::from_value(d.value())),
            AiCommand::SetTarget { target, subtarget } => Command::SetTarget(target, subtarget),
            AiCommand::FireGuns { equipment, target } => {
                let Some(target) = target else {
                    return -2;
                };
                Command::Fire {
                    guns: equipment,
                    target,
                }
            }
            AiCommand::ActivateEquip { equipment, on } => Command::ActivateEquip(equipment, on),
            AiCommand::EngageCruise { engage, instant } => {
                Command::EngageCruise { engage, instant }
            }
            AiCommand::SetThrustersActive(on) => {
                // `slot_54`: every thruster on or off.
                let ids: Vec<u16> = self
                    .get(h)
                    .and_then(|o| o.as_equipped())
                    .map(|eq| {
                        eq.equipment
                            .traverse(types::THRUSTER, false)
                            .into_iter()
                            .filter_map(|s| eq.equipment.get(s).map(|x| x.id))
                            .collect()
                    })
                    .unwrap_or_default();
                for id in ids {
                    self.command(h, Command::ActivateEquip(id, on));
                }
                return 0;
            }
            // Formation, follow, trade lane and the unnamed slots are not
            // world commands yet.
            _ => return -2,
        };
        match self.command(h, typed) {
            crate::object::CommandResult::Refused => -2,
            _ => 0,
        }
    }
}

impl World {
    /// The object id of a physics user key (the world keys bodies by
    /// handle; the AI keys them by object id, as the reference does).
    fn id_of_key(&self, key: u64) -> u32 {
        self.get(ObjHandle::from_key(key))
            .map_or(0, |o| o.simple.id)
    }

    fn contact(&self, h: &neuron_core::physics::RayHit) -> RayContact {
        RayContact {
            object: self.id_of_key(h.user_key),
            point: h.point,
            normal: h.normal,
            distance: h.distance,
        }
    }
}

impl SpatialQueries for World {
    fn find_ray_collisions(
        &self,
        system: u32,
        from: Vector,
        to: Vector,
        max_hits: usize,
    ) -> Vec<RayContact> {
        self.physics(system)
            .map(|w| {
                w.find_ray_collisions(from, to, max_hits)
                    .iter()
                    .map(|h| self.contact(h))
                    .collect()
            })
            .unwrap_or_default()
    }

    fn find_ray_intersect(
        &self,
        system: u32,
        object: u32,
        from: Vector,
        to: Vector,
        max_hits: usize,
    ) -> Vec<RayContact> {
        let (Some(w), Some(body)) = (
            self.physics(system),
            self.find(object).and_then(|h| self.get(h)?.physical.body),
        ) else {
            return Vec::new();
        };
        w.find_ray_intersect(body, from, to, max_hits)
            .iter()
            .map(|h| self.contact(h))
            .collect()
    }

    fn find_sphere_collisions(
        &self,
        system: u32,
        center: Vector,
        radius: f32,
        max_objects: usize,
    ) -> Vec<u32> {
        self.physics(system)
            .map(|w| {
                w.find_sphere_collisions(center, radius, max_objects)
                    .into_iter()
                    .map(|k| self.id_of_key(k))
                    .collect()
            })
            .unwrap_or_default()
    }
}

impl AiRand for World {
    fn rand(&mut self) -> i32 {
        self.rng.rand()
    }

    fn avoid_table_shuffled(&self) -> bool {
        self.avoid_table_shuffled
    }

    fn set_avoid_table_shuffled(&mut self) {
        self.avoid_table_shuffled = true;
    }
}

/// The world's own behaviour hook: runs the behaviour manager the world
/// keeps for an object (`IBehaviorManager::UpdateBehaviorTick` from
/// `CEqObj::update`) and copies the current behaviour into the object
/// (`CEqObj+0x1a0`, which `SetCurrentBehavior` writes). [`World`] installs
/// it with the first manager unless another hook is installed.
#[derive(Clone, Copy, Debug, Default)]
pub struct ManagerHook;

impl super::BehaviorHook for ManagerHook {
    fn update_behavior_tick(&mut self, world: &mut World, object: ObjHandle, dt: f32) {
        let Some(mut manager) = world.behavior_managers.remove(&object) else {
            return;
        };
        manager.update_behavior_tick(world, object, dt);
        world.finish_manager_call(object, manager);
    }
}

impl World {
    /// Puts a manager back after a call and copies its current behaviour
    /// into the object.
    fn finish_manager_call(&mut self, h: ObjHandle, manager: BehaviorManager<World>) {
        let index = manager.state.behavior;
        if let Some(eq) = self.get_mut(h).and_then(|o| o.equipped.as_deref_mut()) {
            eq.behavior_index = index;
        }
        if self.get(h).is_some() {
            self.behavior_managers.insert(h, manager);
        }
    }

    /// `GetBehaviorManager(Obj)`: the object's manager, created on the
    /// spot for an equipped object that has none (`CEqObj::
    /// create_behavior_interface(Obj, 0)`: the manager's constructor, then
    /// state graph 0). `false` for an object that cannot have one.
    pub fn ensure_behavior_manager(&mut self, h: ObjHandle) -> bool {
        if self.behavior_managers.contains_key(&h) {
            return true;
        }
        if self.get(h).and_then(|o| o.as_equipped()).is_none() {
            return false;
        }
        let mut manager = BehaviorManager::new(self, h);
        let data = self.data.clone();
        manager.set_state_graph(self, h, &data.state_graphs, 0, false);
        if let Some(eq) = self.get_mut(h).and_then(|o| o.equipped.as_deref_mut()) {
            eq.has_behavior = true;
        }
        if self.behavior.is_none() {
            self.behavior = Some(Box::new(ManagerHook));
        }
        self.finish_manager_call(h, manager);
        true
    }

    /// An object's behaviour manager.
    #[must_use]
    pub fn behavior_manager(&self, h: ObjHandle) -> Option<&BehaviorManager<World>> {
        self.behavior_managers.get(&h)
    }

    /// Runs `f` on an object's manager (created when missing, as
    /// `GetBehaviorManager` does).
    pub fn with_behavior_manager<T>(
        &mut self,
        h: ObjHandle,
        f: impl FnOnce(&mut BehaviorManager<World>, &mut World) -> T,
    ) -> Option<T> {
        if !self.ensure_behavior_manager(h) {
            return None;
        }
        let mut manager = self.behavior_managers.remove(&h)?;
        let out = f(&mut manager, self);
        self.finish_manager_call(h, manager);
        Some(out)
    }

    /// `pub::AI::SubmitState` with `SetPersonalityParams`: the state graph
    /// (by id; `pub::StateGraph::get_state_graph(name, LEADER)` gives it)
    /// and, when given, the personality. 0, or 4 for an object without a
    /// manager.
    pub fn submit_state(
        &mut self,
        h: ObjHandle,
        state_graph: i32,
        personality: Option<Personality>,
    ) -> i32 {
        let data = self.data.clone();
        self.with_behavior_manager(h, |m, w| {
            m.set_state_graph(w, h, &data.state_graphs, state_graph, false);
            if let Some(p) = personality {
                m.set_personality(w, h, p);
            }
            0
        })
        .unwrap_or(4)
    }

    /// `pub::AI::SubmitDirective` for a ship no player owns: the
    /// `OP_RTYPE` (1 done, 2 another directive is current, 5 refused).
    pub fn submit_directive(&mut self, h: ObjHandle, op: &DirectiveOp) -> i32 {
        self.with_behavior_manager(h, |m, w| m.submit_directive(w, h, op, 0))
            .unwrap_or(5)
    }
}
