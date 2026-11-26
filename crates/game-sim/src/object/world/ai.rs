//! The world as the flight AI sees it (decision record 0001 point 8): the
//! `IObjInspect` slots the behaviours read ([`ObjectView`]), their
//! `IObjAffect` commands turned into the world's typed [`Command`]s, the
//! physics queries, the motion model of a ship (`CShip`'s speed and turn
//! predictions, slots 50 to 63) and the CRT `rand()` the AI shares with
//! the world.

use neuron_core::math::{Matrix, Transform, Vector};

use crate::ai::behavior::DirectiveOp;
use crate::ai::fp::{e, r};
use crate::ai::manager::{BehaviorManager, NotificationOut};
use crate::ai::personality::Personality;
use crate::ai::world::{
    AiCommand, AiRand, AiWorld, DockHardpoints, EngineStatus, LootView, MotionModel, ObjectView,
    RayContact, SpatialQueries, StrafeDir as AiStrafe, ThrustEquipType,
};
use crate::equip::flight::StrafeDir;
use crate::equip::{EquipKind, types};
use crate::object::ship::{self, ThrustKind};
use crate::object::{Command, ObjHandle};

use super::{TickList, World};

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
        let (radius, center) = self.physical_sphere(h);
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
                        .filter_map(|f| id_of(*f))
                        .collect()
                })
                .unwrap_or_default(),
            is_player: ship_data.is_some_and(|s| s.is_player),
            group_id: ship_data.map_or(0, |s| s.group_id),
            is_dying: i.dead,
            is_targetable: i.targetable,
            toughness: self.toughness(h),
            behavior_id: eq.map_or(-1, |e| e.behavior_index),
            // `CShip::get_max_bank_angle`: the ship archetype's.
            max_bank_angle: ship_data
                .and_then(|_| self.data.archetypes.ships.get(&o.archetype))
                .map_or(0.0, |s| s.max_bank_angle),
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
    /// `IObjInspectImpl::get_toughness`: for an equipped object (a ship or
    /// a solar) the rank of its reputation group (`Vibe::GetRank` of
    /// `CEqObj::VibeId`, 0 when the group is unknown) as an `int`, which
    /// is the solar archetype's `toughness` for a solar of a system file
    /// (`SolarReps`), the `CShip::CreateParms::Rank` a ship was made with
    /// (`CShip::init`) or what `pub::Reputation::SetRank` set since; 0 for
    /// anything else.
    #[must_use]
    pub fn toughness(&self, h: ObjHandle) -> i32 {
        let Some(o) = self.get(h) else {
            return 0;
        };
        if !o.kind.is(crate::object::class::EQ_OBJ) {
            return 0;
        }
        let Some(eq) = o.as_equipped() else {
            return 0;
        };
        let (_, rank) = self.vibes.rank(eq.vibe);
        i32::from_ne_bytes(rank.to_ne_bytes())
    }

    /// `PhySys::GetRadiusR(Object, false)` for a body without phantoms:
    /// the core's radius and `PhySys::GetCenterOfMass` (the surface's mass
    /// centre through the body's object frame now, with the reference's
    /// roundings); the object's sphere without a body.
    pub(crate) fn physical_sphere(&self, h: ObjHandle) -> (f32, Vector) {
        let Some(o) = self.get(h) else {
            return (0.0, Vector::ZERO);
        };
        let (Some(body), Some(w)) = (o.physical.body, self.physics(o.physical.system)) else {
            return self.body_sphere(h);
        };
        let (Some(core), Some((m, t))) = (w.core(body), w.object_matrix(body)) else {
            return self.body_sphere(h);
        };
        let center = super::turret::physics_center_of_mass(&m, t, o.physical.mass_center);
        (core.upper_limit_radius, center)
    }

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

impl World {
    /// The ship's motion model with the zone of `pos` (its drag modifier
    /// and immersion, `CSimple::update_zones` there) in place of the zone
    /// the ship is in: a behaviour-tick replay's motion world, whose ships
    /// stand where they were spawned, answers for the recorded position.
    #[must_use]
    pub fn motion_at(&self, h: ObjHandle, pos: Vector) -> Option<Box<dyn MotionModel + '_>> {
        let o = self.get(h)?;
        o.as_ship()?;
        let zone = o
            .simple
            .system
            .and_then(|system| self.physical_zone_at(system, pos))
            .map_or((None, 0.0), |(_, drag, immersion)| (Some(drag), immersion));
        Some(Box::new(ShipMotion {
            world: self,
            h,
            zone: Some(zone),
        }))
    }
}

/// The motion model of one ship (`CShip::get_time_to_accelerate` and the
/// rest of slots 50 to 63), over the world's data.
struct ShipMotion<'a> {
    world: &'a World,
    h: ObjHandle,
    /// The zone's drag modifier and the immersion in place of the
    /// object's own ([`World::motion_at`]).
    zone: Option<(Option<f32>, f32)>,
}

impl ShipMotion<'_> {
    fn parts(&self) -> Option<(&ship::ShipData, &crate::object::Equipped, Option<f32>, f32)> {
        let o = self.world.get(self.h)?;
        let (zone_drag, interference) = self
            .zone
            .unwrap_or((o.simple.zone_drag, o.simple.scanner_interference));
        Some((o.as_ship()?, o.as_equipped()?, zone_drag, interference))
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

    /// `IObjInspectImpl::get_attitude_towards` (`common_iobjinspectimpl.cpp:765`):
    /// the attitude a harness set for the pair ([`World::set_attitude`],
    /// D165), else `CEqObj::attitude_towards` (`common_ceqobj.cpp:598`):
    /// `Vibe::AttitudeTowards` of the two objects' vibes, 0 when either
    /// vibe is unknown. `None` (the C++'s -3) unless both are equipped
    /// objects.
    fn attitude(&self, from: ObjHandle, to: ObjHandle) -> Option<f32> {
        let (a, b) = (self.get(from)?, self.get(to)?);
        if let Some(&value) = self.attitudes.get(&(a.simple.id, b.simple.id)) {
            return Some(value);
        }
        let (va, vb) = (a.equipped.as_ref()?.vibe, b.equipped.as_ref()?.vibe);
        match self.vibes.attitude_towards(va, vb, &self.reputation) {
            (0, value) => Some(value),
            _ => Some(0.0),
        }
    }

    /// `get_attitude_towards_symmetrical` (`:785`): the harness's pair in
    /// both directions when it set either ([`World::set_attitude`], D165),
    /// else `CEqObj::attitude_towards_symmetrical` (`common_ceqobj.cpp:621`):
    /// `Vibe::AttitudeTowardsSymmetrical`, both 0 when it fails.
    fn attitude_symmetrical(&self, from: ObjHandle, to: ObjHandle) -> Option<(f32, f32)> {
        let (a, b) = (self.get(from)?, self.get(to)?);
        let (ia, ib) = (a.simple.id, b.simple.id);
        if self.attitudes.contains_key(&(ia, ib)) || self.attitudes.contains_key(&(ib, ia)) {
            return Some((
                *self.attitudes.get(&(ia, ib))?,
                *self.attitudes.get(&(ib, ia))?,
            ));
        }
        let (va, vb) = (a.equipped.as_ref()?.vibe, b.equipped.as_ref()?.vibe);
        match self
            .vibes
            .attitude_towards_symmetrical(va, vb, &self.reputation)
        {
            (0, toward, Some(reverse)) => Some((toward, reverse)),
            _ => Some((0.0, 0.0)),
        }
    }

    /// `get_hardpoint` (`HardpointPosition`, `common_hierarchy.cpp:55`):
    /// the hardpoint in world space, through the world transform of the
    /// instance it sits on.
    fn hardpoint(&self, h: ObjHandle, name: &[u8]) -> Option<(Vector, Matrix)> {
        self.hardpoint_world(h, name)
    }

    /// `IObjInspectImpl::get_lane_start` ([`World::get_lane_start`]).
    fn lane_start(
        &mut self,
        h: ObjHandle,
        ring: Option<ObjHandle>,
        lane: Option<ObjHandle>,
    ) -> crate::ai::world::LaneStart<ObjHandle> {
        self.get_lane_start(h, ring, lane)
    }

    /// `IObjInspectImpl::is_using_tradelane`.
    fn using_tradelane(&self, h: ObjHandle) -> bool {
        self.is_using_tradelane(h)
    }

    /// `IObjInspectImpl::has_dock_hardpoints` (`common_iobjinspectimpl.cpp:1227`).
    fn has_dock_hardpoints(&self, h: ObjHandle) -> bool {
        self.get(h).and_then(|o| o.as_equipped()).is_some_and(|eq| {
            eq.docking_points
                .iter()
                .any(|d| matches!(d.dock_type, 1 | 6 | 7))
        })
    }

    /// `IObjInspectImpl::get_dock_hardpoints` (`common_iobjinspectimpl.cpp:1252`):
    /// the docking sphere's type and radius, its mount (`hpdockmount<c>`)
    /// and the two dock points (`hpdockpoint<c>01`, `02`; a missing one
    /// copies the mount and makes the result -2).
    fn dock_hardpoints(&mut self, h: ObjHandle, index: i32) -> DockHardpoints {
        let Some(sphere) = self.get(h).and_then(|o| o.as_equipped()).map(|eq| {
            usize::try_from(index)
                .ok()
                .filter(|_| index != -1)
                .and_then(|i| eq.docking_points.get(i).cloned())
        }) else {
            return DockHardpoints::failed(-5);
        };
        let Some(sphere) = sphere else {
            return DockHardpoints::failed(-4);
        };
        let mut out = DockHardpoints {
            result: -5,
            terminal: i32::try_from(sphere.dock_type).ok(),
            mount: None,
            point1: None,
            point2: None,
            radius: Some(sphere.radius),
        };
        let name = &sphere.hardpoint;
        let Some((pos, orient)) = self.hardpoint(h, name) else {
            return out;
        };
        let mount = Transform::new(orient, pos);
        out.mount = Some(mount);
        if !name.windows(11).any(|w| w == b"hpdockmount") {
            return out;
        }
        let suffix = name.last().copied().unwrap_or(0);
        out.result = 0;
        let point = |n: u8, out: &mut DockHardpoints| {
            let mut point_name = b"hpdockpoint".to_vec();
            point_name.push(suffix);
            point_name.extend_from_slice(if n == 1 { b"01" } else { b"02" });
            match self.hardpoint(h, &point_name) {
                Some((p, o)) => Some(Transform::new(o, p)),
                None => {
                    out.result = -2;
                    Some(mount)
                }
            }
        };
        out.point1 = point(1, &mut out);
        out.point2 = point(2, &mut out);
        out
    }

    fn motion(&self, h: ObjHandle) -> Option<Box<dyn MotionModel + '_>> {
        self.get(h)?.as_ship()?;
        Some(Box::new(ShipMotion {
            world: self,
            h,
            zone: None,
        }))
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

    /// The item's state (`get_equipment_status`), the values the system
    /// manager reads (`get_equipment_val`) and a launcher's first barrel.
    /// A destroyed item, whose status call fails, reports
    /// [`EQUIP_DESTROYED`](crate::ai::world::EQUIP_DESTROYED) and no values.
    fn equipment(&self, h: ObjHandle, id: u16) -> Option<crate::ai::world::EquipInfo> {
        use super::{EquipmentVal, equipment_val as v};
        use crate::ai::world::{EQUIP_ACTIVE, EQUIP_DESTROYED, EQUIP_DISABLED, EquipInfo};
        let eq = self.get(h)?.as_equipped()?;
        let item = eq.equipment.by_id(id)?;
        let Ok(status) = self.equipment_status(h, id) else {
            return Some(EquipInfo {
                flags: if item.destroyed { EQUIP_DESTROYED } else { 0 },
                ..EquipInfo::default()
            });
        };
        let mut flags = 0;
        if status.flags & super::EquipStatus::ACTIVE != 0 {
            flags |= EQUIP_ACTIVE;
        }
        if status.flags & super::EquipStatus::DISABLED != 0 {
            flags |= EQUIP_DISABLED;
        }
        let val = |kind: u32| self.equipment_val(h, id, kind, 0.0).ok();
        let float = |kind: u32| match val(kind) {
            Some(EquipmentVal::Float(f)) => Some(f),
            _ => None,
        };
        let int = |kind: u32| match val(kind) {
            Some(EquipmentVal::Int(i)) => Some(i),
            _ => None,
        };
        let byte = |kind: u32| match val(kind) {
            Some(EquipmentVal::Byte(b)) => Some(b),
            _ => None,
        };
        let launcher = match &item.kind {
            EquipKind::Gun(g) => Some(&g.launcher),
            EquipKind::MineDropper(l) | EquipKind::CmDropper(l) => Some(l),
            _ => None,
        };
        let barrel = launcher.and_then(|l| {
            let cx = self.launch_context(h, id)?;
            Some((cx.barrel_pos(&l.barrels, 0), cx.barrel_dir(&l.barrels, 0)))
        });
        Some(EquipInfo {
            flags,
            // The low byte, as the system manager reads `EquipFlags`.
            equip_flags: (status.equip_flags & 0xff) as u8,
            munition_kind: int(v::MUNITION_KIND),
            auto_turret: byte(v::AUTO_TURRET).map(|b| b != 0),
            seeker: byte(v::SEEKER),
            range: float(v::RANGE),
            target_id: int(v::TARGET_ID).map(|t| t as u32),
            barrel,
        })
    }

    /// `IObjInspectImpl::get_scanlist(list, mask, keep)`: -2 (`None`)
    /// without a scanner; otherwise `clear_cache` unless `keep`, then
    /// `scan_for_types(mask)`, and the first scanner's list.
    fn scan_list(&mut self, h: ObjHandle, mask: u32, keep: bool) -> Option<Vec<ObjHandle>> {
        let eq = self.get(h)?.as_equipped()?;
        eq.equipment.first(types::SCANNER)?;
        if !keep {
            self.scanner_clear_cache(h);
        }
        self.scan_for_types(h, mask);
        Some(World::scan_list(self, h))
    }

    fn sub_object_center(&mut self, h: ObjHandle, sub: u16) -> Option<Vector> {
        self.sub_obj_center_of_mass(h, sub)
    }

    fn can_point_at(&self, h: ObjHandle, id: u16, point: Vector, angle: f32) -> bool {
        World::can_point_at(self, h, id, point, angle).unwrap_or(false)
    }

    fn is_pointing_at(&mut self, h: ObjHandle, id: u16, angle: f32) -> bool {
        World::is_pointing_at(self, h, id, angle).unwrap_or(false)
    }

    /// The sub-target form is `get_tgt_lead_fire_pos(id, pos)` (the gun's
    /// lead); without one `CShip::get_tgt_lead_fire_pos`, the ship's own
    /// lead over its gun statistics.
    fn lead_fire_position(&mut self, h: ObjHandle, subtarget: Option<u16>) -> Option<Vector> {
        match subtarget {
            Some(id) => self.tgt_lead_fire_pos(h, id).ok(),
            None => self.ship_lead_fire_position(h),
        }
    }

    fn manager_of(&self, h: ObjHandle) -> Option<(i32, [i32; 3])> {
        self.behavior_managers
            .get(&h)
            .map(|m| (m.state.behavior, m.state.directive))
    }

    /// `CShip::generate_follow_offset(follower, out)`: the follower's slot
    /// in the leader's follow list (cleared slots counted) picks a ring of
    /// four (50 m to the side, 20 m back per ring) turned by a quarter per
    /// slot about the leader's Z, in the leader's frame from its centre.
    /// The reference's `FCOS` and `FSIN` round once from 64 bits; here the
    /// `f64` value is rounded, which agrees for these angles (multiples of
    /// a quarter turn).
    fn generate_follow_offset(
        &self,
        leader: ObjHandle,
        follower: ObjHandle,
    ) -> Option<neuron_core::math::Transform> {
        let fid = self.get(follower)?.simple.id;
        let slots = &self.get(leader)?.as_ship()?.follow_followers;
        let slot = slots.iter().position(|&s| {
            s.and_then(|h| self.get(h))
                .is_some_and(|o| o.simple.id == fid)
        })?;
        let slot = i32::try_from(slot).ok()?;
        let ring = f64::from(slot / 4 + 1);
        let quad = f64::from(slot % 4);
        let offset = Vector::new(r(e(50.0) * ring), r(e(0.0) * ring), r(ring * e(20.0)));
        let angle = quad * e(core::f32::consts::PI) * 0.5;
        let cos = r(angle.cos());
        let sin = r(e(r(angle)).sin());
        let spin = Matrix::from_rows([[cos, -sin, 0.0], [sin, cos, 0.0], [0.0, 0.0, 1.0]]);
        let rotated = neuron_core::math::transform_matrix(&spin, offset);
        let (_, center) = self.physical_sphere(leader);
        let orient = self.inspect(leader)?.orientation;
        let turned = neuron_core::math::transform_matrix(&orient, rotated);
        let translation = Vector::new(
            r(e(turned.x) + e(center.x)),
            r(e(turned.y) + e(center.y)),
            r(e(turned.z) + e(center.z)),
        );
        Some(neuron_core::math::Transform::new(orient, translation))
    }

    fn creation_type_of(&self, h: ObjHandle) -> Option<u32> {
        self.behavior_managers
            .get(&h)
            .map(|m| m.state.creation_type)
    }

    fn personality_of(&self, h: ObjHandle) -> Option<crate::ai::personality::Personality> {
        self.behavior_managers.get(&h).map(|m| m.state.personality)
    }

    /// `CLoot`'s units, AI flag and contents class, and `get_good_id`
    /// (`Arch2Good` of the contents).
    fn loot(&self, h: ObjHandle) -> Option<LootView> {
        let crate::object::KindData::Loot(l) = &self.get(h)?.data else {
            return None;
        };
        let contents = self.data.archetypes.equipment.get(&l.contents_arch);
        Some(LootView {
            units: l.units,
            can_ai_tractor: l.can_ai_tractor,
            contents_class: contents.map_or(0, |a| a.class()),
            good_id: Some(self.data.goods.arch_to_good(l.contents_arch)),
        })
    }

    /// `CShip::get_cargo_hold_remaining` and the ship archetype's
    /// `hold_size`: every item of the hold mask counts its volume (a cargo
    /// stack its count times it), rounded to `float` at each sum; what is
    /// left of the hold, or 0 when the items exceed it.
    fn cargo_hold(&self, h: ObjHandle) -> Option<(f64, f32)> {
        Some((self.cargo_hold_remaining(h)?, self.cargo_hold_size(h)?))
    }

    /// The target of the object's loot behaviour (behaviour 16).
    fn loot_target_of(&self, h: ObjHandle) -> Option<u32> {
        self.behavior_managers
            .get(&h)?
            .behavior(crate::ai::behavior::ids::LOOT)?
            .as_any()?
            .downcast_ref::<crate::ai::loot::LootBehavior>()?
            .target
    }

    /// `Universe::ISystem::GetSphereProperties` over the system's zones
    /// (P2.1's zone tree and `SphereCull`).
    fn sphere_zone_properties(&self, system: u32, center: Vector, radius: f32) -> u32 {
        self.data
            .universe
            .sphere_properties(system, &crate::data::zone::query::Sphere { center, radius })
    }

    /// `SinglePlayer()`.
    fn single_player(&self) -> bool {
        self.config.single_player
    }

    fn content_event(&mut self, h: ObjHandle, handle: u32, event: crate::ai::content::CombatEvent) {
        self.push_server_request(super::ServerRequest::Content {
            ship: h,
            handle,
            event,
        });
    }

    fn content_answer(&self, h: ObjHandle) -> Option<crate::ai::content::FleeAnswer> {
        self.content_answer
            .filter(|(ship, _)| *ship == h)
            .and_then(|(_, answer)| answer)
    }

    fn command(&mut self, h: ObjHandle, command: AiCommand<ObjHandle>) -> i32 {
        let typed = match command {
            AiCommand::SetThrottle(t) => Command::SetThrottle(t),
            AiCommand::SetAxisThrottle(v) => Command::SetAxisThrottle(v),
            AiCommand::SetNudge(v) => Command::SetNudgeVec(v),
            AiCommand::SetStrafe(d) => Command::SetStrafeDir(StrafeDir::from_value(d.value())),
            AiCommand::SetTarget { target, subtarget } => {
                return self.ai_set_target(h, target, subtarget);
            }
            AiCommand::FireGuns { equipment, target } => {
                // `EqObj::slot_34` without an aim point fires forward; the
                // server's part of the shot follows the tick.
                if self.get(h).is_none_or(|o| o.dead) {
                    return -2;
                }
                self.fire_for_ai(h, &equipment, target);
                return 0;
            }
            AiCommand::ActivateEquip { equipment, on } => {
                return self.ai_activate_equip(h, equipment, on);
            }
            AiCommand::EngageCruise { engage, instant } => {
                return self.ai_engage_cruise(h, engage, instant);
            }
            AiCommand::SetThrustersActive(on) => return self.ai_set_thrusters(h, on),
            // `EqObj::slot_40` (the server's own request): -5 for a dead
            // owner, -4 without a target, -2 without the item, -5 when
            // `CETractor::AddTarget` refuses.
            AiCommand::Tractor { equipment, target } => {
                if self.get(h).is_none_or(|o| o.dead) {
                    return -5;
                }
                if self.get(target).is_none() {
                    return -4;
                }
                let is_item = self
                    .get(h)
                    .and_then(|o| o.as_equipped())
                    .and_then(|eq| eq.equipment.by_id(equipment))
                    .is_some();
                if !is_item {
                    return -2;
                }
                if self.tractor_add_target(h, equipment, target)
                    != crate::equip::misc::TractorFailure::Ok
                {
                    return -5;
                }
                return 0;
            }
            // `EqObj::slot_44`: -2 for a missing or destroyed item, -4 for
            // one that is not a gun, else `CEGun::SetTarget` (the target a
            // simple object, else none).
            AiCommand::SetGunTarget {
                equipment,
                target,
                subtarget,
            } => {
                let Some(item) = self
                    .get(h)
                    .and_then(|o| o.as_equipped())
                    .and_then(|eq| eq.equipment.by_id(equipment))
                else {
                    return -2;
                };
                if item.is_destroyed() {
                    return -2;
                }
                if !matches!(item.kind, EquipKind::Gun(_)) {
                    return -4;
                }
                if self.gun_set_target(h, equipment, target, subtarget) {
                    self.push_ai_change(
                        h,
                        super::AiChange::Target {
                            equip: equipment,
                            target,
                            sub_target: subtarget,
                        },
                    );
                }
                return 0;
            }
            AiCommand::SetFormationLeader(_)
            | AiCommand::SetFormationOffset(_)
            | AiCommand::AddFormationFollower(_)
            | AiCommand::RemoveFormationFollower(_)
            | AiCommand::SetFollowLeader(_)
            | AiCommand::SetFollowOffset(_)
            | AiCommand::AddFollowFollower(_)
            | AiCommand::RemoveFollowFollower(_) => return self.formation_command(h, command),
            // `Ship::slot_5c` and `slot_60` are the server's: the request
            // goes to it after the behaviour tick
            // ([`super::ServerHooks::server_requests`]); both answer 0.
            AiCommand::Request {
                kind,
                target,
                second,
                flag,
            } => {
                self.push_server_request(super::ServerRequest::Request {
                    ship: h,
                    kind,
                    target,
                    second,
                    flag,
                });
                return 0;
            }
            AiCommand::CancelRequest { kind, target, arg } => {
                self.push_server_request(super::ServerRequest::Cancel {
                    ship: h,
                    kind,
                    target,
                    arg,
                });
                return 0;
            }
            // `Ship::go_tradelane` and `stop_tradelane` (slots 25 and 26):
            // the `CShip` part at once, the server's after the tick (D730).
            AiCommand::GoTradeLane {
                entrance,
                exit,
                fast_exit,
                speed,
            } => return self.ai_go_tradelane(h, entrance, exit, fast_exit, speed),
            AiCommand::StopTradeLane => {
                if self.ship_data(h).is_none() {
                    return -3;
                }
                self.traveller_stop_tradelane(h);
                return 0;
            }
            // `CShip::request_exit_tradelane`, called on the ship directly.
            AiCommand::RequestExitTradeLane => {
                self.request_exit_tradelane(h);
                return 0;
            }
            // The unnamed slots are not world commands yet.
            _ => return -2,
        };
        match self.command(h, typed) {
            crate::object::CommandResult::Refused => -2,
            _ => 0,
        }
    }
}

impl World {
    /// A change of the server's wrapper that the players are told of
    /// (`StarSystem::notify_*`), queued with the AI's other requests for the
    /// server after the behaviour tick (D857).
    fn push_ai_change(&mut self, h: ObjHandle, change: super::AiChange) {
        self.push_server_request(super::ServerRequest::Changed { ship: h, change });
    }

    /// Whether the wrapper's object is gone (`OwnerSystem` null or
    /// `is_destroyed`: -5).
    fn ai_wrapper_gone(&self, h: ObjHandle) -> bool {
        self.get(h).is_none_or(|o| o.dead)
    }

    /// The first engine's id, cruise flag and activity.
    fn ai_engine(&self, h: ObjHandle) -> Option<(u16, bool, bool)> {
        let eq = self.get(h)?.as_equipped()?;
        let e = eq.equipment.first(types::ENGINE)?;
        let cruise = matches!(&e.kind, EquipKind::Engine(s) if s.cruise_active);
        Some((e.id, cruise, e.is_active()))
    }

    /// `Ship::set_target(Target, SubTarget, 0)` from the AI
    /// (`server_gameobject.cpp:5789-5834`): a target or sub-target that
    /// differs is set, and only then the players hear of it
    /// (`notify_set_target` with `SUBOBJ_ID_NONE`).
    fn ai_set_target(&mut self, h: ObjHandle, target: Option<ObjHandle>, sub: u16) -> i32 {
        if self.ai_wrapper_gone(h) {
            return -5;
        }
        let Some(ship) = self.get(h).and_then(|o| o.as_ship()) else {
            return -2;
        };
        if ship.target == target && ship.sub_target == sub {
            return 0;
        }
        let _ = self.command(h, Command::SetTarget(target, sub));
        self.push_ai_change(
            h,
            super::AiChange::Target {
                equip: crate::equip::SUBOBJ_ID_NONE,
                target,
                sub_target: sub,
            },
        );
        0
    }

    /// `EqObj::slot_4c(&EquipId, bActivate, 0)` from the AI
    /// (`server_gameobject.cpp:2184-2250`): a missing or destroyed item is
    /// -2; switching an engine off is refused (-5) while the ship strafes or
    /// has a thruster on; an item not as asked is switched, and only then
    /// the players hear of it (`notify_activate_equip`).
    pub(crate) fn ai_activate_equip(&mut self, h: ObjHandle, equip: u16, on: bool) -> i32 {
        if self.ai_wrapper_gone(h) {
            return -5;
        }
        let item = self
            .get(h)
            .and_then(|o| o.as_equipped())
            .and_then(|eq| eq.equipment.by_id(equip))
            .filter(|e| !e.is_destroyed())
            .map(|e| (e.type_bits, e.is_active()));
        let Some((type_bits, active)) = item else {
            return -2;
        };
        if !on && type_bits == types::ENGINE {
            let o = self.get(h);
            let strafing = o
                .and_then(|o| o.as_ship())
                .is_some_and(|s| s.flight.strafe.current != crate::equip::flight::StrafeDir::None);
            let thrusting = o.and_then(|o| o.as_equipped()).is_some_and(|eq| {
                eq.equipment
                    .traverse(types::THRUSTER, false)
                    .into_iter()
                    .filter_map(|slot| eq.equipment.get(slot))
                    .any(crate::equip::Equip::is_active)
            });
            if strafing || thrusting {
                return -5;
            }
        }
        if active == on {
            return 0;
        }
        let _ = self.command(h, Command::ActivateEquip(equip, on));
        self.push_ai_change(h, super::AiChange::Equip { equip, on });
        0
    }

    /// `Ship::slot_54(bActivate, 0)` from the AI
    /// (`server_gameobject.cpp:5888-5949`): switching them on is refused
    /// (-5) while cruising and first switches an inactive engine on
    /// (`slot_4c`); every thruster not as asked is switched, and only when
    /// one was the players hear of it (`notify_thrusters`). A ship without
    /// an engine reads as not cruising with its engine on (D650).
    fn ai_set_thrusters(&mut self, h: ObjHandle, on: bool) -> i32 {
        if self.ai_wrapper_gone(h) {
            return -5;
        }
        if on {
            let engine = self.ai_engine(h);
            if engine.is_some_and(|(_, cruise, _)| cruise) {
                return -5;
            }
            if let Some((id, _, false)) = engine {
                let _ = self.ai_activate_equip(h, id, true);
            }
        }
        let thrusters: Vec<(u16, bool)> = self
            .get(h)
            .and_then(|o| o.as_equipped())
            .map(|eq| {
                eq.equipment
                    .traverse(types::THRUSTER, false)
                    .into_iter()
                    .filter_map(|slot| eq.equipment.get(slot))
                    .map(|t| (t.id, t.is_active()))
                    .collect()
            })
            .unwrap_or_default();
        let mut changed = false;
        for (id, active) in thrusters {
            if active != on {
                changed = true;
                let _ = self.command(h, Command::ActivateEquip(id, on));
            }
        }
        if changed {
            self.push_ai_change(h, super::AiChange::Thrusters(on));
        }
        0
    }

    /// `Ship::slot_50(bEngage, bArg2, 0)` from the AI
    /// (`server_gameobject.cpp:5848-5884`): when the first engine's cruise
    /// is not as asked, engaging first switches the thrusters off
    /// (`slot_54(false, 0)`), the engine engages or leaves cruise, and the
    /// players hear of it (`notify_cruise`). A ship without an engine, which
    /// the C++ reads through null, changes nothing (D708).
    fn ai_engage_cruise(&mut self, h: ObjHandle, engage: bool, instant: bool) -> i32 {
        if self.ai_wrapper_gone(h) {
            return -5;
        }
        let Some((_, cruise, _)) = self.ai_engine(h) else {
            return 0;
        };
        if cruise == engage {
            return 0;
        }
        if engage {
            let _ = self.ai_set_thrusters(h, false);
        }
        let _ = self.command(h, Command::EngageCruise { engage, instant });
        self.push_ai_change(h, super::AiChange::Cruise { engage, instant });
        0
    }

    /// The formation and follow slots of `IObjAffect` (4 to 7, 9 to 12) on a
    /// ship (`Ship::set_formation_leader` and the `CShip` bodies they
    /// forward to); `GameObject`'s -8 for any other object. The player's
    /// client reports of `set_formation_leader` and `set_formation_offset`
    /// are the server's (P3).
    fn formation_command(&mut self, h: ObjHandle, command: AiCommand<ObjHandle>) -> i32 {
        if self.ship_data(h).is_none() {
            return -8;
        }
        let own_id = self.id_of_handle(h);
        match command {
            AiCommand::SetFormationLeader(leader) => {
                if let Some(s) = self.ship_data_mut(h) {
                    s.formation_leader = leader;
                }
                0
            }
            AiCommand::SetFormationOffset(offset) => {
                if let Some(s) = self.ship_data_mut(h) {
                    s.formation_offset = offset;
                }
                0
            }
            AiCommand::SetFollowLeader(leader) => {
                if let Some(s) = self.ship_data_mut(h) {
                    s.follow_leader = leader;
                }
                0
            }
            AiCommand::SetFollowOffset(offset) => {
                if let Some(s) = self.ship_data_mut(h) {
                    s.follow_offset = offset;
                }
                0
            }
            // `CShip::add_formation_follower`: only a ship that leads
            // itself takes followers; the first cleared slot, else a new
            // one; then the formation speed.
            AiCommand::AddFormationFollower(f) => {
                let leader = self
                    .ship_data(h)
                    .and_then(|s| s.formation_leader)
                    .filter(|&l| self.get(l).is_some());
                if leader.is_none_or(|l| self.id_of_handle(l) != own_id) {
                    return -2;
                }
                let follower_id = self.id_of_handle(f);
                let slots = self
                    .ship_data(h)
                    .map(|s| s.formation_followers.clone())
                    .unwrap_or_default();
                if let Some(i) = self.free_or_same_slot(&slots, follower_id) {
                    if self.live_slot(slots[i]).is_none()
                        && let Some(s) = self.ship_data_mut(h)
                    {
                        s.formation_followers[i] = Some(f);
                    }
                } else if let Some(s) = self.ship_data_mut(h) {
                    s.formation_followers.push(Some(f));
                }
                self.recalculate_formation_speed(h);
                0
            }
            AiCommand::RemoveFormationFollower(f) => {
                let follower_id = self.id_of_handle(f);
                let slots = self
                    .ship_data(h)
                    .map(|s| s.formation_followers.clone())
                    .unwrap_or_default();
                let Some(i) = self.matching_slot(&slots, follower_id) else {
                    return -3;
                };
                if let Some(s) = self.ship_data_mut(h) {
                    s.formation_followers[i] = None;
                }
                self.recalculate_formation_speed(h);
                0
            }
            // `CShip::add_follow_follower`: not the ship itself; the first
            // cleared slot, else a new one.
            AiCommand::AddFollowFollower(f) => {
                let follower_id = self.id_of_handle(f);
                if follower_id == own_id {
                    return -1;
                }
                let slots = self
                    .ship_data(h)
                    .map(|s| s.follow_followers.clone())
                    .unwrap_or_default();
                if let Some(i) = self.free_or_same_slot(&slots, follower_id) {
                    if self.live_slot(slots[i]).is_none()
                        && let Some(s) = self.ship_data_mut(h)
                    {
                        s.follow_followers[i] = Some(f);
                    }
                } else if let Some(s) = self.ship_data_mut(h) {
                    s.follow_followers.push(Some(f));
                }
                0
            }
            AiCommand::RemoveFollowFollower(f) => {
                let follower_id = self.id_of_handle(f);
                let slots = self
                    .ship_data(h)
                    .map(|s| s.follow_followers.clone())
                    .unwrap_or_default();
                let Some(i) = self.matching_slot(&slots, follower_id) else {
                    return -3;
                };
                if let Some(s) = self.ship_data_mut(h) {
                    s.follow_followers[i] = None;
                }
                0
            }
            _ => -2,
        }
    }

    /// A watcher slot's object while it lives.
    fn live_slot(&self, slot: Option<ObjHandle>) -> Option<ObjHandle> {
        slot.filter(|&f| self.get(f).is_some())
    }

    /// An object's id, 0 once it is gone.
    fn id_of_handle(&self, h: ObjHandle) -> u32 {
        self.get(h).map_or(0, |o| o.simple.id)
    }

    /// The first slot that is cleared or holds the object `id`.
    fn free_or_same_slot(&self, slots: &[Option<ObjHandle>], id: u32) -> Option<usize> {
        slots.iter().position(|&slot| {
            self.live_slot(slot)
                .is_none_or(|e| self.id_of_handle(e) == id)
        })
    }

    /// The slot that holds the live object `id`.
    fn matching_slot(&self, slots: &[Option<ObjHandle>], id: u32) -> Option<usize> {
        slots.iter().position(|&slot| {
            self.live_slot(slot)
                .is_some_and(|e| self.id_of_handle(e) == id)
        })
    }

    /// `CShip::recalculate_formation_speed`: the slowest live follower's
    /// `get_speed(1, engine)`, 1e8 without one.
    fn recalculate_formation_speed(&mut self, h: ObjHandle) {
        let slots = self
            .ship_data(h)
            .map(|s| s.formation_followers.clone())
            .unwrap_or_default();
        let mut min = 1.0e8_f32;
        for f in slots.into_iter().filter_map(|s| self.live_slot(s)) {
            let speed = <Self as AiWorld>::motion(self, f)
                .map_or(0.0, |m| m.speed(1.0, ThrustEquipType::Engine));
            if !(min < speed) {
                min = speed;
            }
        }
        if let Some(s) = self.ship_data_mut(h) {
            s.formation_speed = min;
        }
    }

    pub(crate) fn ship_data(&self, h: ObjHandle) -> Option<&crate::object::ship::ShipData> {
        self.get(h).and_then(|o| o.as_ship())
    }

    pub(crate) fn ship_data_mut(
        &mut self,
        h: ObjHandle,
    ) -> Option<&mut crate::object::ship::ShipData> {
        self.get_mut(h).and_then(|o| o.as_ship_mut())
    }

    /// `pub::AI::update_formation_state(ship, leader, offset)` (Server):
    /// the ship leaves its old leader's formation (unless it led itself),
    /// takes the new leader and offset, and joins the new leader's
    /// followers (unless it is the leader). -3 for a ship that is not
    /// there, else 0; in multiplayer an unchanged leader changes nothing.
    /// (The player clients of the leaders and followers are told by the
    /// server, P3.)
    pub fn update_formation_state(
        &mut self,
        ship: ObjHandle,
        leader: Option<ObjHandle>,
        offset: Vector,
    ) -> i32 {
        if self.get(ship).is_none() {
            return -3;
        }
        let leader = self.live_slot(leader);
        let old = self.live_slot(self.ship_data(ship).and_then(|s| s.formation_leader));
        if !self.config.single_player && leader == old {
            return 0;
        }
        let own_id = self.id_of_handle(ship);
        if let Some(old) = old
            && self.id_of_handle(old) != own_id
        {
            self.command_ai(old, AiCommand::RemoveFormationFollower(ship));
        }
        self.command_ai(ship, AiCommand::SetFormationLeader(leader));
        self.command_ai(ship, AiCommand::SetFormationOffset(offset));
        if let Some(l) = leader
            && l != ship
        {
            self.command_ai(l, AiCommand::AddFormationFollower(ship));
        }
        0
    }

    fn command_ai(&mut self, h: ObjHandle, command: AiCommand<ObjHandle>) -> i32 {
        <Self as AiWorld>::command(self, h, command)
    }

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
        let outbox = std::mem::take(&mut manager.state.outbox);
        world.finish_manager_call(object, manager);
        world.deliver_notifications(outbox);
    }
}

impl World {
    /// Delivers the notifications a manager queued for other ships'
    /// managers during a call (D195), in the order it queued them, each
    /// with the notifications its delivery queues in turn handed on before
    /// the next (the reference runs each delivery inside the call, depth
    /// first). A ship without a manager gets nothing
    /// (`get_behavior_interface` answers null).
    pub fn deliver_notifications(&mut self, outbox: Vec<NotificationOut>) {
        for n in outbox {
            let Some(h) = self.find(n.target) else {
                continue;
            };
            let Some(mut manager) = self.behavior_managers.remove(&h) else {
                continue;
            };
            manager.receive_notification(self, h, n.extra);
            let more = std::mem::take(&mut manager.state.outbox);
            self.finish_manager_call(h, manager);
            self.deliver_notifications(more);
        }
    }

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

    /// `Ship::request_returned`'s callback arm: the server's answer to the
    /// request the ship's behaviour made, to the requesting behaviour's
    /// `OnResult` (`BehaviorManager::request_returned`). `None` when the
    /// ship has no manager with a pending request (the answer reaches no
    /// behaviour); otherwise whether the behaviour keeps the request.
    pub fn request_returned(
        &mut self,
        h: ObjHandle,
        kind: i32,
        response: i32,
        arg: i32,
    ) -> Option<bool> {
        if self
            .behavior_managers
            .get(&h)
            .is_none_or(|m| m.state.pending_request.is_none())
        {
            return None;
        }
        let mut manager = self.behavior_managers.remove(&h)?;
        manager.request_returned(self, h, kind, response, arg);
        let kept = manager.state.pending_request.is_some();
        let outbox = std::mem::take(&mut manager.state.outbox);
        self.finish_manager_call(h, manager);
        self.deliver_notifications(outbox);
        Some(kept)
    }

    /// `CEqObj::disconnect(IObjRW*)` with the object's own wrapper: its
    /// behaviour manager is deleted (a dying ship's death fuse lit). A
    /// later `GetBehaviorManager` makes a new one. False when it had none.
    pub fn drop_behavior_manager(&mut self, h: ObjHandle) -> bool {
        let had = self.behavior_managers.remove(&h).is_some();
        if had && let Some(eq) = self.get_mut(h).and_then(|o| o.equipped.as_deref_mut()) {
            eq.has_behavior = false;
        }
        had
    }

    /// Sets the attitude of `from` toward `to`, which the AI reads as
    /// `get_attitude_towards` (the reputation groups are not ported).
    pub fn set_attitude(&mut self, from: ObjHandle, to: ObjHandle, value: f32) {
        let (Some(a), Some(b)) = (self.get(from), self.get(to)) else {
            return;
        };
        let key = (a.simple.id, b.simple.id);
        self.attitudes.insert(key, value);
    }

    /// An object's behaviour manager.
    #[must_use]
    pub fn behavior_manager(&self, h: ObjHandle) -> Option<&BehaviorManager<World>> {
        self.behavior_managers.get(&h)
    }

    /// The manager's type-2 event for the object `h` (a player's manoeuvre,
    /// `IBehaviorManager::Slot00({2, &XSetManeuver})`,
    /// [`BehaviorManager::maneuver_event`]): nothing for an object without
    /// a manager (`get_behavior_interface` answers null).
    pub fn maneuver_event(&mut self, h: ObjHandle, behavior: i32) {
        if let Some(manager) = self.behavior_managers.get_mut(&h) {
            manager.maneuver_event(behavior);
        }
    }

    /// `StarSystem::complete_dock`'s type-3 event
    /// (`Server/server_classes.cpp:10183-10223`): every ship of `system`
    /// that no player owns and that has a manager hears that the ship
    /// `event.ship` docked at `event.dock`, in the system's ship order. Its
    /// own ship lets go of the docked ship (`CShip::leaving_system_update`:
    /// the formation and follow followers' slots and the targeters; the
    /// reference does it after the guns' targets, which it does not touch),
    /// then the manager runs case 3 ([`BehaviorManager::dock_event`]).
    pub fn dock_event(&mut self, system: u32, event: &crate::ai::behavior::DockEvent) {
        for h in self.list(system, TickList::Ships) {
            if self.get(h).is_none_or(|o| o.simple.owner_player != 0) {
                continue;
            }
            // `Slot00` returns at once without a current behaviour.
            if self
                .behavior_managers
                .get(&h)
                .is_none_or(|m| m.state.behavior == -1)
            {
                continue;
            }
            self.leaving_system_update(h, event.ship);
            let _ = self.with_behavior_manager(h, |m, w| m.dock_event(w, h, event));
        }
    }

    /// `CShip::leaving_system_update(&Id)` (`Common/common_cship.cpp:2584-2630`):
    /// every formation follower, follow follower and targeter of the ship
    /// `h` that is the object `id` is let go; true when one was.
    pub fn leaving_system_update(&mut self, h: ObjHandle, id: u32) -> bool {
        let gone = self.find(id);
        let Some(gone) = gone else {
            return false;
        };
        let Some(ship) = self.ship_data_mut(h) else {
            return false;
        };
        let mut found = false;
        for slot in ship
            .formation_followers
            .iter_mut()
            .chain(ship.follow_followers.iter_mut())
        {
            if *slot == Some(gone) {
                *slot = None;
                found = true;
            }
        }
        let before = ship.targeters.len();
        ship.targeters.retain(|&t| t != gone);
        found || ship.targeters.len() != before
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
        let outbox = std::mem::take(&mut manager.state.outbox);
        self.finish_manager_call(h, manager);
        self.deliver_notifications(outbox);
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
