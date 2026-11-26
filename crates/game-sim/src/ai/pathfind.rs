//! The pathfinder (`PathfindManager`, `include/common_pathfind.h`,
//! `Common/common_pathfind.cpp`): the per-ship path and obstacle state the
//! behaviour manager owns at `+0x0C`. The movement ideas read its override
//! record (`+0x214`) and mirror their target into it; its `Update` runs
//! the obstacle-avoidance probe ([`ObstacleAvoidance`]) on its own clock
//! and hands an avoidance point or nudge to the ideas and the thrust
//! manager.
//!
//! Ported: the constructor (with its three `rand()` phase offsets and the
//! motion queries), `SetCruiseLock`, `IsCruiseUnobstructed`, `Update`,
//! `RefreshAvoidance`, `SelectAvoidancePoint`, `SelectObstacleDirection`
//! with `FindClosestObstacleToAvoid` over the scanner's scan list, the
//! user-zone probe (`GetUserZoneApproachDirection`, the sphere of modes
//! 0 and 2 and the box of mode 1) and `UpdateHeadingBlend`.
//!
//! `ClassifyObstaclePhase` walks the scan list (mask 0xfd58) and the
//! planets, moons and suns of the object database's spatial bucket 7
//! within 20 km (`ObjectDb->GetBucket(7)->QuerySphere`, the server's solar
//! partition, [`AiWorld::bucket_query`](super::world::AiWorld::bucket_query)),
//! in the partition's order.

use neuron_core::math::Quaternion;
use neuron_core::math::{Matrix, Transform, Vector, quaternion_to_matrix, transform_matrix};

use super::avoidance::{
    AvoidIndexTable, AvoidSlot, Nudge, ObstacleAvoidance, nudge_obstacle_path_point,
    resolve_obstacle_candidate_slot,
};
use super::behavior::AiEnv;
use super::fp::{add, column2, inline_fast_length, neg, scale, sub};
use super::fp::{e, r};
use super::intercept::{
    estimate_turn_circle_approach_cost, iface_position_or_zero, negated_iface_direction,
    shape_biased_avoid_direction,
};
use super::nav::max_scalar_attribute;
use super::raycast::{
    point_in_capsule, ray_sphere_intersect, swept_sphere_collision_time,
    transformed_boxes_intersect,
};
use super::thrust::ThrustManager;
use super::world::{
    AVOID_SCAN_MASK, AiWorld, EQUIP_DESTROYED, MotionModel, ObjectView, ThrustEquipType,
};

/// What the obstacle search reads of the system manager: the scanner's
/// id in the timer table and the target list's `bKeepCache`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ScanContext {
    /// `SystemManagerTimerTable::ScannerEquipId`.
    pub scanner: Option<u16>,
    /// `SystemManagerTimerTable::bKeepCache`.
    pub keep: bool,
}

/// `Behavior_QueryChainedRadiusEffect(object, radius, kind)`
/// (`common_behavior.cpp`): the distance covered while accelerating from
/// 1 to `radius` against a falloff of the top speed less 1; 0 without a
/// motion model or when the falloff is below `radius`.
#[must_use]
pub fn chained_radius_effect(
    motion: Option<&dyn MotionModel>,
    radius: f32,
    kind: ThrustEquipType,
) -> f32 {
    let Some(m) = motion else {
        return 0.0;
    };
    let mut value = e(m.speed(1.0, kind)) - 1.0;
    if value < 0.0 {
        value = 0.0;
    }
    if value < e(radius) {
        return 0.0;
    }
    let mut radius = radius;
    if e(radius) < 0.001 {
        radius = 0.0;
    }
    let mut copy = radius;
    let mut falloff = r(value);
    if copy < 0.0 {
        copy = 0.0;
    }
    if value < 0.0 {
        falloff = 0.0;
    }
    if e(copy) < 0.001 {
        copy = 0.0;
    }
    let time = m.time_to_accelerate(1.0, copy, falloff, kind);
    m.distance_travelled(1.0, radius, time, kind)
}

/// `PathfindManager::ObstacleReaction` (`+0x16C`).
#[derive(Clone, Debug, PartialEq)]
pub struct ObstacleReaction {
    /// +0x00 3 when idle.
    pub state: i32,
    /// +0x04.
    pub last_obstacle_tag: u32,
    /// +0x08 the reaction heading.
    pub heading: Vector,
    /// +0x14.
    pub in_reaction: bool,
    /// +0x15.
    pub close_encounter: bool,
}

impl Default for ObstacleReaction {
    fn default() -> Self {
        Self {
            state: 3,
            last_obstacle_tag: 0,
            heading: Vector::ZERO,
            in_reaction: false,
            close_encounter: false,
        }
    }
}

impl ObstacleReaction {
    /// `ObstacleReaction::ResetState`.
    pub fn reset(&mut self) {
        *self = Self::default();
    }
}

/// `PathfindManager::UserZone` (0x58 bytes), modes 0 (sphere) and 2 (a
/// live object's sphere); mode 1 (a box) is kept but not probed.
#[derive(Clone, Debug, PartialEq)]
pub struct UserZone {
    /// +0x00 0 sphere, 1 box, 2 live target.
    pub mode: i32,
    /// +0x04 non-zero flips the probe.
    pub invert: i32,
    /// +0x08 the centre (mode 2 caches the target's position here).
    pub target_point: Vector,
    /// +0x14 the radius.
    pub radius: f32,
    /// +0x18 the box (mode 1).
    pub box_rotation: Matrix,
    /// +0x3C.
    pub box_center: Vector,
    /// +0x48.
    pub box_half_extent: Vector,
    /// +0x54 the object id of mode 2.
    pub target_key: u32,
}

/// `PathfindManager`, the fields the port reads and writes, under the
/// reference's names (`field_NNN` for the unnamed ones).
#[derive(Clone, Debug, PartialEq)]
pub struct PathfindManager {
    /// +0x000 1 inside a zone with any of 0x7000, | 8 with a probe
    /// contact.
    pub field_000: i32,
    /// +0x004 the zone properties around the ship
    /// ([`AiWorld::sphere_zone_properties`]).
    pub field_004: i32,
    /// +0x008 / +0x00C the user zone (`submit_user_zone`).
    pub user_zone: Option<UserZone>,
    /// +0x064 the id of the object an idea watches
    /// (`BaseIdea::PublishTargetId`); the obstacle search skips it.
    pub target_id: u32,
    /// +0x16C the reaction.
    pub reaction: ObstacleReaction,
    /// +0x1A0.
    pub blend_timer: f32,
    /// +0x1A4 (0, 1, 0).
    pub blend_start: Vector,
    /// +0x1B0.
    pub blend_enabled: bool,
    /// `RecentObstacleIds` (eight ids `ClassifyObstaclePhase` looks its
    /// candidates up in; nothing in the reference writes them, so they
    /// stay 0).
    pub recent_obstacle_ids: [u32; 8],
    /// `DeflectionSlots`: the candidates' capsules, refilled in turn.
    pub deflection_slots: [AvoidSlot; 8],
    /// `NextDeflectionSlot`: the slot the next candidate refills.
    pub next_deflection_slot: u32,
    /// +0x1B4 (`ClassifyObstaclePhase` copies the cached target here).
    pub target_position: Vector,
    /// +0x1C4 the owner's `get_type`.
    pub owner_type: u32,
    /// +0x1E0 / +0x1E4 the zone probe's clock.
    pub timer_1e0: f32,
    /// +0x1E4.
    pub period_1e4: f32,
    /// +0x1E8 / +0x1EC the avoidance probe's clock (0.5, 0.1 while it
    /// steers).
    pub timer_1e8: f32,
    /// +0x1EC.
    pub period_1ec: f32,
    /// +0x1F4 `get_speed(1, engine)`.
    pub max_engine_speed: f32,
    /// +0x1F8 the distance covered from rest in 1000 s at that speed.
    pub field_1f8: f32,
    /// +0x1FC the same at 300.
    pub field_1fc: f32,
    /// +0x200 the cached target position the ideas mirror.
    pub cached_target_position: Vector,
    /// +0x20C / +0x210 the override record's clock (0.1).
    pub timer_20c: f32,
    /// +0x210.
    pub period_210: f32,
    /// +0x214 the override record: bit 0 a nudge direction (`+0x220`),
    /// bit 1 a goal point (`+0x22c`); any bit caps the engine throttle.
    pub field_214: i32,
    /// +0x218 the obstacle the nudge record steers around (its id).
    pub watcher_218: Option<u32>,
    /// +0x220 the nudge direction.
    pub field_220: Vector,
    /// +0x22C the override goal.
    pub field_22c: Vector,
    /// +0x238 the probe's contact flag when the goal was set.
    pub field_238: bool,
    /// +0x23C the override throttle cap (1).
    pub field_23c: f32,
    /// +0x240 the override kind (1 the probe, 2 the user zone, 3 the
    /// reaction, 4 an obstacle).
    pub field_240: i32,
    /// +0x244 the nudge record is blended (not cruising, or an obstacle
    /// of the 0x7f0000 types).
    pub field_244: bool,
    /// +0x245 (1).
    pub flag_245: u8,
    /// +0x248 the obstacle search's type mask (-1); `SplineMoveIdea`
    /// stores `0x30038010`.
    pub field_248: i32,
    /// +0x24C the cruise lock's obstacle bit (`SetCruiseLock` mask 1).
    pub field_24c: u8,
    /// +0x250 (0x7f0000).
    pub field_250: i32,
    /// +0x254 the cruise lock's avoidance bit (mask 2).
    pub field_254: u8,
    /// +0x255 an obstacle direction was found.
    pub field_255: bool,
    /// +0x258 its object id.
    pub field_258: u32,
    /// +0x25C the kind of the last avoidance point.
    pub field_25c: i32,
    /// +0x260 the obstacle `ClassifyObstaclePhase` reacts to.
    pub cached_obstacle_id: u32,
    /// +0x264.
    pub has_cached_obstacle: bool,
    /// +0x268 the probe.
    pub avoidance: ObstacleAvoidance,
}

impl Default for PathfindManager {
    /// The fields after the constructor's stores, without its draws and
    /// queries (the timers at 0, the speeds at their seeds).
    fn default() -> Self {
        Self {
            field_000: 0,
            field_004: 0,
            user_zone: None,
            target_id: 0,
            reaction: ObstacleReaction::default(),
            blend_timer: 0.0,
            blend_start: Vector::new(0.0, 1.0, 0.0),
            blend_enabled: false,
            recent_obstacle_ids: [0; 8],
            deflection_slots: [AvoidSlot::default(); 8],
            next_deflection_slot: 0,
            target_position: Vector::ZERO,
            owner_type: 0,
            timer_1e0: 0.0,
            period_1e4: 1.0,
            timer_1e8: 0.0,
            period_1ec: 0.5,
            max_engine_speed: 80.0,
            field_1f8: 0.0,
            field_1fc: 0.0,
            cached_target_position: Vector::ZERO,
            timer_20c: 0.0,
            period_210: 0.1,
            field_214: 0,
            watcher_218: None,
            field_220: Vector::ZERO,
            field_22c: Vector::ZERO,
            field_238: false,
            field_23c: 1.0,
            field_240: 0,
            field_244: false,
            flag_245: 1,
            field_248: -1,
            field_24c: 1,
            field_250: 0x7f_0000,
            field_254: 1,
            field_255: false,
            field_258: 0,
            field_25c: 0,
            cached_obstacle_id: 0,
            has_cached_obstacle: false,
            avoidance: ObstacleAvoidance::new(),
        }
    }
}

/// `(float)rand() * (1/32768) * period`, the constructor's phase offset:
/// the product of the converted draw and the float constant is a float
/// expression, each step on the 53-bit stack, one rounding at the store.
fn phase(rand: i32, period: f32) -> f32 {
    r(f64::from(rand) * e(3.051_757_8e-5) * e(period))
}

impl PathfindManager {
    /// `PathfindManager::PathfindManager(Owner)` for `ship`: the owner's
    /// type, the probe attached to the owner's box, the three phase offsets
    /// (`rand()` in this order) and the engine speeds. The physics system
    /// (`SetPathfindObjectDb`) is the ship's.
    pub fn new<W: AiEnv + ?Sized>(world: &mut W, ship: W::Handle) -> Self {
        let mut p = Self::default();
        let view = world.inspect(ship);
        p.owner_type = view.as_ref().map_or(0, |v| v.type_flags);
        if let Some(v) = &view {
            p.avoidance.attach_owner(v);
        }
        p.avoidance.phys_system_id = 0;
        p.period_1e4 = 1.0;
        p.timer_1e0 = phase(world.rand(), p.period_1e4);
        p.period_1ec = 0.5;
        p.timer_1e8 = phase(world.rand(), p.period_1ec);
        p.period_210 = 0.1;
        p.timer_20c = phase(world.rand(), p.period_210);
        let motion = world.motion(ship);
        p.init_engine_distances(motion.as_deref());
        drop(motion);
        p.blend_start = Vector::new(0.0, 1.0, 0.0);
        p.avoidance.phys_system_id = world.system_of(ship).unwrap_or(0);
        p
    }

    /// The constructor's motion queries (`common_pathfind.cpp:132-152`):
    /// `get_speed(1, engine)` and, unless it is exactly -1, the distances
    /// `get_distance_travelled(0, speed, 1000, engine)` and
    /// `get_distance_travelled(0, 300, 1000, engine)`, which pads
    /// `ClassifyObstaclePhase`'s candidate capsules (`Field_1FC * 0.5`).
    /// Without a motion model the speed is 0 and the distances stay.
    pub fn init_engine_distances(&mut self, motion: Option<&dyn MotionModel>) {
        let speed = motion.map_or(0.0, |m| m.speed(1.0, ThrustEquipType::Engine));
        self.max_engine_speed = speed;
        if speed == -1.0 {
            self.field_1f8 = 0.0;
            self.field_1fc = 0.0;
        } else if let Some(m) = motion {
            self.field_1f8 = m.distance_travelled(0.0, speed, 1000.0, ThrustEquipType::Engine);
            self.field_1fc = m.distance_travelled(0.0, 300.0, 1000.0, ThrustEquipType::Engine);
        }
    }

    /// `IsCruiseUnobstructed`: false while the probe holds a contact or a
    /// reaction is under way.
    #[must_use]
    pub fn is_cruise_unobstructed(&self) -> bool {
        let mut obstructed = self.avoidance.has_contact;
        if self.reaction.state != 3 && self.reaction.in_reaction {
            obstructed = true;
        }
        !obstructed
    }

    /// The override record cleared (the default-constructed temporary
    /// `SetCruiseLock` assigns, and `RefreshAvoidance`'s first stores).
    fn clear_record(&mut self) {
        self.field_214 = 0;
        self.watcher_218 = None;
        self.field_220 = Vector::ZERO;
        self.field_22c = Vector::ZERO;
        self.field_238 = false;
        self.field_23c = 1.0;
        self.field_240 = 0;
    }

    /// `SetCruiseLock(Value, Mask)`: mask bit 1 sets the avoidance flag,
    /// bit 0 the obstacle flag; a zero value with bit 0 drops a live nudge
    /// (zero nudge to the thrust manager) and, for any mask, clears the
    /// override record.
    pub fn set_cruise_lock<W: AiWorld + ?Sized>(
        &mut self,
        thrust: &mut ThrustManager,
        world: &mut W,
        ship: W::Handle,
        value: u8,
        mask: u32,
    ) {
        if mask & 2 != 0 {
            self.field_254 = value;
        }
        let bit0 = mask & 1;
        if bit0 != 0 {
            self.field_24c = value;
        }
        if value != 0 {
            return;
        }
        if bit0 != 0 && self.field_214 & 1 != 0 {
            thrust.set_nudge_direction(world, ship, Vector::ZERO);
        }
        self.clear_record();
    }

    /// `PathfindManager::Update(Dt)`, with `behavior` the manager's current
    /// behaviour: the zone clock, the reset for the idle behaviours, the
    /// override record every 0.1 s, the nudge blend, the heading blend and
    /// the obstacle probe on its own clock.
    pub fn update<W: AiEnv + ?Sized>(
        &mut self,
        thrust: &mut ThrustManager,
        world: &mut W,
        ship: W::Handle,
        behavior: i32,
        dt: f32,
        scan: ScanContext,
    ) {
        if self.owner_type & 0x7f_0000 == 0 {
            return;
        }
        let zone_clock = e(dt) + e(self.timer_1e0);
        self.timer_1e0 = r(zone_clock);
        if zone_clock > e(self.period_1e4) {
            self.timer_1e0 = r(e(self.timer_1e0) % e(self.period_1e4));
            // The zone properties of a 20 m sphere around the ship.
            let props = match (world.system_of(ship), world.inspect(ship)) {
                (Some(system), Some(v)) => world.sphere_zone_properties(system, v.position, 20.0),
                _ => 0,
            };
            self.field_004 = i32::from_ne_bytes(props.to_ne_bytes());
            self.field_000 = i32::from(props & 0x7000 != 0);
            if self.avoidance.has_contact {
                self.field_000 |= 8;
            }
        }

        if matches!(behavior, 6 | 8 | 0x13 | -1) {
            self.avoidance.reset_contact();
            self.reaction.reset();
            return;
        }

        let previous = self.field_214;
        let record_clock = e(dt) + e(self.timer_20c);
        self.timer_20c = r(record_clock);
        if record_clock > e(self.period_210) {
            self.timer_20c = r(e(self.timer_20c) % e(self.period_210));
            self.refresh_avoidance(thrust, world, ship, scan);
        }

        if self.field_214 & 1 != 0 {
            if self.field_244 {
                thrust.blend_nudge_direction(world, ship, self.field_220, dt);
            }
        } else if previous & 1 != 0 {
            thrust.set_nudge_direction(world, ship, Vector::ZERO);
        }

        if self.flag_245 == 0 || self.field_254 == 0 {
            return;
        }

        self.update_heading_blend(world, ship, behavior, dt, scan);

        let avoid_clock = e(dt) + e(self.timer_1e8);
        self.timer_1e8 = r(avoid_clock);
        if avoid_clock > e(self.period_1ec) {
            self.timer_1e8 = r(e(self.timer_1e8) % e(self.period_1ec));
            let target = self.cached_target_position;
            let period = self.period_1ec;
            let mut table = AvoidIndexTable {
                shuffled: world.avoid_table_shuffled(),
            };
            self.avoidance
                .update(world, ship, target, period, &mut table);
            if table.shuffled {
                world.set_avoid_table_shuffled();
            }
            self.period_1ec = if self.avoidance.active || self.avoidance.has_contact {
                0.1
            } else {
                0.5
            };
        }
    }

    /// `RefreshAvoidance`: the override record rebuilt from the avoidance
    /// point, else (for the owner types in 0x130000) from an obstacle
    /// direction.
    fn refresh_avoidance<W: AiEnv + ?Sized>(
        &mut self,
        thrust: &mut ThrustManager,
        world: &mut W,
        ship: W::Handle,
        scan: ScanContext,
    ) {
        self.clear_record();
        if let Some((point, scale, kind)) = self.select_avoidance_point(&*world, ship) {
            self.field_22c = point;
            self.field_23c = scale;
            self.field_238 = self.avoidance.has_contact;
            self.watcher_218 = None;
            self.field_214 = 2;
            self.field_240 = kind;
            return;
        }
        if self.owner_type & 0x13_0000 == 0 {
            return;
        }
        let Some((direction, obstacle)) = self.select_obstacle_direction(world, ship, scan) else {
            return;
        };
        self.watcher_218 = obstacle;
        self.field_240 = 4;
        let cruise = world
            .inspect(ship)
            .and_then(|v| v.cruise)
            .is_some_and(|c| c.0);
        let obstacle_type = obstacle
            .and_then(|id| world.handle_of(id))
            .and_then(|h| world.inspect(h))
            .map_or(0, |v| v.type_flags);
        let blend = !cruise || obstacle_type & 0x7f_0000 != 0;
        self.field_244 = blend;
        if !blend {
            thrust.set_nudge_direction(world, ship, direction);
        }
        self.field_220 = direction;
        self.field_214 = 1;
    }

    /// `SelectAvoidancePoint`: the user zone's exit, the reaction's
    /// heading, or the probe's avoid point; the point, the throttle scale
    /// and the kind (the scale is 1 where the reference leaves it
    /// untouched: the record was just reset to 1).
    fn select_avoidance_point<W: AiWorld + ?Sized>(
        &mut self,
        world: &W,
        ship: W::Handle,
    ) -> Option<(Vector, f32, i32)> {
        if self.flag_245 == 0 || self.field_254 == 0 {
            return None;
        }
        let view = world.inspect(ship)?;
        if let Some(exit) = self.user_zone_approach_direction(world, ship) {
            self.field_25c = 2;
            let t = view.position;
            let reach = Vector::new(
                r(e(exit.x) * 100.0),
                r(e(exit.y) * 100.0),
                r(e(exit.z) * 100.0),
            );
            let point = Vector::new(
                r(e(reach.x) + e(t.x)),
                r(e(reach.y) + e(t.y)),
                r(e(reach.z) + e(t.z)),
            );
            return Some((point, 1.0, 2));
        }
        if self.reaction.state != 3 {
            let h = self.reaction.heading;
            let t = view.position;
            let reach_z = r(e(h.z) * 100.0);
            let point = Vector::new(
                r(e(h.x) * 100.0 + e(t.x)),
                r(e(h.y) * 100.0 + e(t.y)),
                r(e(reach_z) + e(t.z)),
            );
            let motion = world.motion(ship);
            let mut scale = r(estimate_turn_circle_approach_cost(
                &view,
                motion.as_deref(),
                point,
                0.0,
                0.0,
            ));
            if self.reaction.close_encounter {
                let m = &view.orientation.m;
                let forward_x = -m[0][2];
                let dot = (e(-m[2][2]) * e(h.z) + e(-m[1][2]) * e(h.y)) + e(forward_x) * e(h.x);
                if dot < e(0.93) {
                    scale = 0.0;
                }
            }
            self.field_25c = 3;
            let len_sq =
                (e(point.x) * e(point.x) + e(point.y) * e(point.y)) + e(point.z) * e(point.z);
            if !(len_sq > 1.0) {
                return None;
            }
            return Some((point, scale, 3));
        }
        if let Some((point, id)) = self.avoidance.get_avoid_point()
            && id != self.target_id
        {
            let owner_has_ship = true;
            self.field_25c = 1;
            return Some((point, self.avoidance.close_rate_scale(owner_has_ship), 1));
        }
        self.field_25c = 0;
        None
    }

    /// `SelectObstacleDirection`: the shape-biased way around the closest
    /// obstacle on the scan list other than the published target, else the
    /// probe's avoid direction and its contact.
    fn select_obstacle_direction<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        scan: ScanContext,
    ) -> Option<(Vector, Option<u32>)> {
        if self.flag_245 == 0 || self.field_24c == 0 {
            return None;
        }
        self.field_255 = false;
        if let Some(obstacle) = self.find_closest_obstacle_to_avoid(world, ship, scan) {
            let view = world.inspect(ship)?;
            let target = world.handle_of(obstacle).and_then(|h| world.inspect(h));
            let direction = shape_biased_avoid_direction(&view, target.as_ref());
            self.field_255 = true;
            self.field_258 = obstacle;
            return Some((direction, Some(obstacle)));
        }
        let (direction, id) = self.avoidance.get_avoid_direction()?;
        self.field_255 = true;
        self.field_258 = id;
        Some((direction, world.handle_of(id).map(|_| id)))
    }

    /// `FindClosestObstacleToAvoid(&Sys->Timers, target id, Field_248)`
    /// (`common_obstacleavoidance.cpp`): of the scan list (mask
    /// [`AVOID_SCAN_MASK`]), the nearest object ahead within the look-ahead
    /// reach that the ship would meet over the look-ahead time (a swept
    /// sphere test, or a swept box test for the type bit 29 and for a ship
    /// of the same formation over 0.3 s); none below 5 m/s or when the
    /// scanner is destroyed.
    #[allow(clippy::too_many_lines)]
    fn find_closest_obstacle_to_avoid<W: AiWorld + ?Sized>(
        &self,
        world: &mut W,
        ship: W::Handle,
        scan: ScanContext,
    ) -> Option<u32> {
        let exclude = self.target_id;
        let mask = u32::from_ne_bytes(self.field_248.to_ne_bytes());
        let own = world.inspect(ship)?;
        let v = own.velocity;
        let speed_exact = ((e(v.x) * e(v.x) + e(v.y) * e(v.y)) + e(v.z) * e(v.z)).sqrt();
        let speed = r(speed_exact);
        if speed_exact < 5.0 {
            return None;
        }
        let owner_pos = own.position;
        let owner_xform = own.transform();
        let owner_radius = own.radius;
        let heading = negated_iface_direction(Some(&own));
        let (readiness, chained) = {
            let motion = world.motion(ship);
            let m = motion.as_deref();
            (
                e(m.map_or(0.0, |m| m.speed(1.0, ThrustEquipType::Tradelane))) * 0.5,
                chained_radius_effect(m, 0.0, ThrustEquipType::Tradelane),
            )
        };
        if readiness <= 1.0 {
            return None;
        }
        let look_exact = e(chained) / e(speed) + 6.0;
        let look = r(look_exact);
        let reach = look_exact * e(speed);
        let reach_sq = r(reach * reach);
        let mut owner_box = [0.0_f32; 6];
        if let Some((mn, mx)) = own.extents {
            owner_box = [
                r(e(mn.x) * 1.5),
                r(e(mx.x) * 1.5),
                r(e(mn.y) * 1.5),
                r(e(mx.y) * 1.5),
                r(e(mn.z) * 1.5),
                r(e(mx.z) * 1.5),
            ];
        }
        let live = |world: &W, id: Option<u32>| id.filter(|&id| world.handle_of(id).is_some());
        let owner_leader = live(world, own.formation_leader);
        let destroyed = scan
            .scanner
            .and_then(|id| world.equipment(ship, id))
            .is_some_and(|i| i.flags & EQUIP_DESTROYED != 0);
        if destroyed {
            return None;
        }
        let list = world.scan_list(ship, AVOID_SCAN_MASK, scan.keep)?;
        let mut result = None;
        let mut best = 1.0e12_f32;
        for h in list {
            let Some(c) = world.inspect(h) else {
                continue;
            };
            if c.type_flags & mask == 0 {
                continue;
            }
            let to = sub(c.center, owner_pos);
            let ahead = (e(heading.x) * e(to.x) + e(heading.z) * e(to.z)) + e(heading.y) * e(to.y);
            if ahead < 0.0 {
                continue;
            }
            let len_sq = (e(to.x) * e(to.x) + e(to.z) * e(to.z)) + e(to.y) * e(to.y);
            if len_sq > e(reach_sq) {
                continue;
            }
            if c.id == exclude || c.id == own.id {
                continue;
            }
            if c.type_flags & AVOID_SCAN_MASK == 0 {
                continue;
            }
            if c.radius > 1000.0 {
                continue;
            }
            let mut time = look;
            let mut use_box = (c.type_flags >> 29) & 1 != 0;
            if let (Some(cl), Some(ol)) = (live(world, c.formation_leader), owner_leader)
                && cl == ol
            {
                use_box = true;
                time = 0.3;
            }
            let cand_pos = c.center;
            let cv = c.velocity;
            let owner_travel = r(e(time) * e(speed));
            let cand_speed = ((e(cv.x) * e(cv.x) + e(cv.y) * e(cv.y)) + e(cv.z) * e(cv.z)).sqrt();
            let cand_travel = r(cand_speed * e(time));
            let collide = if use_box {
                let Some((mn, mx)) = c.extents else {
                    continue;
                };
                let cand_box = [mn.x, mx.x, mn.y, mx.y, r(e(mn.z) - e(cand_travel)), mx.z];
                let mut swept = owner_box;
                swept[4] = r(e(owner_box[4]) - e(owner_travel));
                transformed_boxes_intersect(&owner_xform, &swept, &c.transform(), &cand_box)
            } else {
                let cand_radius = max_scalar_attribute(&c);
                let forward = neg(column2(&c.orientation));
                let cand_end = add(cand_pos, scale(forward, cand_travel));
                let owner_end = Vector::new(
                    r(e(heading.x) * e(owner_travel) + e(owner_pos.x)),
                    r(e(heading.y) * e(owner_travel) + e(owner_pos.y)),
                    r(e(heading.z) * e(owner_travel) + e(owner_pos.z)),
                );
                swept_sphere_collision_time(
                    owner_radius,
                    owner_pos,
                    owner_end,
                    cand_radius,
                    cand_pos,
                    cand_end,
                )
                .hit
            };
            if !collide {
                continue;
            }
            let dx = e(cand_pos.x) - e(owner_pos.x);
            let dy = e(cand_pos.y) - e(owner_pos.y);
            let dz = e(cand_pos.z) - e(owner_pos.z);
            let distance = inline_fast_length((dz * dz + dy * dy) + dx * dx);
            if distance < e(best) {
                best = r(distance);
                result = Some(c.id);
            }
        }
        result
    }

    /// `GetUserZoneApproachDirection`: the direction toward the user zone
    /// (a sphere for modes 0 and 2, a box for mode 1) while the ship is
    /// outside it, or away from it while inside when the zone is inverted.
    pub fn user_zone_approach_direction<W: AiWorld + ?Sized>(
        &mut self,
        world: &W,
        ship: W::Handle,
    ) -> Option<Vector> {
        let zone = self.user_zone.as_mut()?;
        let own = iface_position_or_zero(world.inspect(ship).as_ref());
        if zone.mode == 2 {
            let found = world
                .handle_of(zone.target_key)
                .and_then(|h| world.inspect(h))?;
            zone.target_point = found.position;
        }
        if zone.mode == 1 {
            let (gate, dir) = box_zone_gate(zone, own);
            return if zone.invert == 0 {
                (gate > 0.0).then_some(dir)
            } else {
                (gate < 0.0).then(|| Vector::new(-dir.x, -dir.y, -dir.z))
            };
        }
        if zone.mode != 0 && zone.mode != 2 {
            return None;
        }
        let t = zone.target_point;
        let bx = r(e(own.x) - e(t.x));
        let by = r(e(own.y) - e(t.y));
        let bz = r(e(own.z) - e(t.z));
        let gate = ((e(by) * e(by) + e(bz) * e(bz)) + e(bx) * e(bx)).sqrt() - e(zone.radius);
        let dx = r(e(t.x) - e(own.x));
        let dy = r(e(t.y) - e(own.y));
        let dz = r(e(t.z) - e(own.z));
        let inv = 1.0 / ((e(dz) * e(dz) + e(dy) * e(dy)) + e(dx) * e(dx)).sqrt();
        let dir = Vector::new(r(e(dx) * inv), r(e(dy) * inv), r(e(dz) * inv));
        if zone.invert == 0 {
            if !(gate > 0.0) {
                return None;
            }
            return Some(dir);
        }
        if !(gate < 0.0) {
            return None;
        }
        Some(Vector::new(-dir.x, -dir.y, -dir.z))
    }

    /// `ClassifyObstaclePhase` (`common_obstacleavoidance.cpp:1062`): of
    /// the scan list (mask 0xfd58) and the planets, moons and suns within
    /// 20 km, the obstacle whose inflated capsule holds the ship and lies
    /// nearest the target; its reaction heading committed (0), or 4 for
    /// nothing to react to (or `NudgeObstaclePathPoint`'s refusal).
    #[allow(clippy::too_many_lines)]
    fn classify_obstacle_phase<W: AiEnv + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        behavior: i32,
        scan: ScanContext,
    ) -> i32 {
        self.reaction.reset();
        self.cached_obstacle_id = 0;
        self.has_cached_obstacle = false;
        let mut target = self.cached_target_position;
        self.target_position = target;
        if behavior == 10 {
            return 4;
        }
        let look_ahead = matches!(behavior, 1 | 3 | 5 | 0xb | 0xd | 0x12);
        let Some(own) = world.inspect(ship) else {
            return 4;
        };
        let ship_xform = own.transform();
        let len_sq =
            (e(target.x) * e(target.x) + e(target.y) * e(target.y)) + e(target.z) * e(target.z);
        if len_sq < 1.0 {
            if !look_ahead {
                self.blend_enabled = false;
                return 4;
            }
            self.blend_enabled = true;
            target =
                neuron_core::math::transform_xform(&ship_xform, Vector::new(0.0, 0.0, -2000.0));
        } else {
            self.blend_enabled = true;
        }
        let throttle = own.throttle.unwrap_or(1.0);
        if e(throttle) < 0.1 {
            return 4;
        }
        let Some(list) = world.scan_list(ship, 0xfd58, scan.keep) else {
            return 4;
        };
        // `ObjectDb->GetBucket(7)` and, when it is there, its
        // `QuerySphere` of 20000 around the ship's position
        // (`common_obstacleavoidance.cpp:1140-1153`); the pass below keeps
        // the planets, moons and suns (mask 7).
        let origin = ship_xform.translation;
        let nearby: Vec<W::Handle> = world
            .system_of(ship)
            .and_then(|system| world.bucket_query(system, origin, 20000.0))
            .unwrap_or_default();
        let tracing = world.tracing();
        if tracing {
            world.trace(format!(
                "classify: behaviour {behavior} look_ahead {look_ahead} ship {:?} target {:?} throttle {:08x} field_1fc {:08x} ({}) scan {:?} bucket {:?}",
                origin,
                target,
                throttle.to_bits(),
                self.field_1fc.to_bits(),
                self.field_1fc,
                list,
                nearby
            ));
        }

        let mut best_distance = f32::MAX;
        let mut best: Option<(ObjectView, AvoidSlot)> = None;
        for (pass, candidates) in [(0, list), (1, nearby)] {
            let type_mask = if pass == 0 { 0xfd58 } else { 7 };
            for h in candidates {
                let Some(c) = world.inspect(h) else {
                    if tracing {
                        world.trace(format!("  pass {pass} {h:?}: not inspected"));
                    }
                    continue;
                };
                if c.type_flags & type_mask == 0 {
                    if tracing {
                        world.trace(format!(
                            "  pass {pass} id {}: type {:#x} not in {type_mask:#x}",
                            c.id, c.type_flags
                        ));
                    }
                    continue;
                }
                let close = c.type_flags & 7 != 0;
                let cached = self.recent_obstacle_ids.iter().position(|&id| id == c.id);
                let index = cached.unwrap_or_else(|| {
                    let next = (self.next_deflection_slot + 1) % 8;
                    self.next_deflection_slot = next;
                    let k = usize::try_from(next).unwrap_or(0);
                    self.deflection_slots[k] = resolve_obstacle_candidate_slot(&c);
                    k
                });
                let slot = &mut self.deflection_slots[index];
                let resolved = *slot;
                if slot.timer < 100.0 || slot.timer > 20000.0 {
                    if tracing {
                        world.trace(format!(
                            "  pass {pass} id {} type {:#x} pos {:?} radius {} center {:?} atmosphere {} extents {:?}: slot {:?} ({}) outside 100..20000",
                            c.id,
                            c.type_flags,
                            c.position,
                            c.radius,
                            c.center,
                            c.atmosphere_range,
                            c.extents,
                            resolved,
                            if cached.is_some() { "cached" } else { "resolved" }
                        ));
                    }
                    continue;
                }
                let scaled = e(slot.timer) * e(0.15f32);
                let floor: f32 = if close && look_ahead { 750.0 } else { 250.0 };
                let margin = if e(floor) > scaled { floor } else { r(scaled) };
                slot.timer = r((e(margin) + e(slot.timer)) + e(self.field_1fc) * 0.5);
                let holds_ship = point_in_capsule(origin, &slot.capsule());
                if tracing {
                    let mid = Vector::new(
                        r(e(slot.direction.x) * 0.5 + e(slot.point.x)),
                        r(e(slot.direction.y) * 0.5 + e(slot.point.y)),
                        r(e(slot.direction.z) * 0.5 + e(slot.point.z)),
                    );
                    let (dx, dy, dz) = (
                        e(mid.x) - e(target.x),
                        e(mid.y) - e(target.y),
                        e(mid.z) - e(target.z),
                    );
                    world.trace(format!(
                        "  pass {pass} id {} type {:#x} pos {:?} radius {} center {:?} atmosphere {} extents {:?}: slot {:?} ({}), margin {margin}, padded radius {:08x} ({}), holds the ship {holds_ship}, distance to target {}, best so far {best_distance}",
                        c.id,
                        c.type_flags,
                        c.position,
                        c.radius,
                        c.center,
                        c.atmosphere_range,
                        c.extents,
                        resolved,
                        if cached.is_some() { "cached" } else { "resolved" },
                        slot.timer.to_bits(),
                        slot.timer,
                        inline_fast_length((dx * dx + dz * dz) + dy * dy)
                    ));
                }
                if holds_ship {
                    let mid = Vector::new(
                        r(e(slot.direction.x) * 0.5 + e(slot.point.x)),
                        r(e(slot.direction.y) * 0.5 + e(slot.point.y)),
                        r(e(slot.direction.z) * 0.5 + e(slot.point.z)),
                    );
                    let dx = e(mid.x) - e(target.x);
                    let dy = e(mid.y) - e(target.y);
                    let dz = e(mid.z) - e(target.z);
                    let distance = inline_fast_length((dx * dx + dz * dz) + dy * dy);
                    if distance < e(best_distance) {
                        best_distance = r(distance);
                        best = Some((c.clone(), *slot));
                    }
                }
                slot.timer = r(e(slot.timer) - e(margin));
            }
        }

        let Some((winner, best_slot)) = best else {
            if tracing {
                world.trace("classify: no candidate holds the ship: 4".to_owned());
            }
            return 4;
        };
        if tracing {
            world.trace(format!(
                "classify: winner id {} type {:#x} slot {:?}, target in its capsule {}",
                winner.id,
                winner.type_flags,
                best_slot,
                point_in_capsule(target, &best_slot.capsule())
            ));
        }
        if behavior == 2 {
            let p = winner.position;
            let dx = e(p.x) - e(target.x);
            let dy = e(p.y) - e(target.y);
            let dz = e(p.z) - e(target.z);
            if inline_fast_length((dx * dx + dz * dz) + dy * dy) <= 1.0 {
                if tracing {
                    world.trace("classify: the winner stands on the target: 4".to_owned());
                }
                return 4;
            }
        }
        self.cached_obstacle_id = winner.id;
        self.has_cached_obstacle = true;
        let has_close_flags = winner.type_flags & 7 != 0;
        if has_close_flags {
            let p = winner.position;
            let dx = e(p.x) - e(origin.x);
            let dy = e(p.y) - e(origin.y);
            let dz = e(p.z) - e(origin.z);
            let len_sq_f = r((dz * dz + dy * dy) + dx * dx);
            if inline_fast_length(e(len_sq_f)) <= e(winner.atmosphere_range) {
                self.reaction.close_encounter = true;
            }
            if look_ahead
                && let Some(t) = own
                    .target
                    .and_then(|id| world.handle_of(id))
                    .and_then(|h| world.inspect(h))
            {
                let tp = t.position;
                let tx = e(tp.x) - e(origin.x);
                let ty = e(tp.y) - e(origin.y);
                let tz = e(tp.z) - e(origin.z);
                if inline_fast_length((tz * tz + ty * ty) + tx * tx) > 500.0 {
                    target = tp;
                } else {
                    self.reaction.state = 1;
                    self.reaction.last_obstacle_tag = winner.id;
                    self.reaction.in_reaction = true;
                    let mid = Vector::new(
                        r(e(best_slot.direction.x) * 0.5 + e(best_slot.point.x)),
                        r(e(best_slot.direction.y) * 0.5 + e(best_slot.point.y)),
                        r(e(best_slot.direction.z) * 0.5 + e(best_slot.point.z)),
                    );
                    let mx = e(mid.x) - e(origin.x);
                    let my = e(mid.y) - e(origin.y);
                    let mz = e(mid.z) - e(origin.z);
                    let mzf = r(mz);
                    let inv = r(1.0 / ((mz * e(mzf) + my * my) + mx * mx).sqrt());
                    self.reaction.heading =
                        Vector::new(-r(mx * e(inv)), -r(my * e(inv)), -r(e(mzf) * e(inv)));
                    return 0;
                }
            }
        }

        if point_in_capsule(target, &best_slot.capsule()) {
            let nudge = nudge_obstacle_path_point(
                &best_slot,
                target,
                &ship_xform,
                self.reaction.close_encounter,
            );
            if tracing {
                world.trace(format!("classify: nudge of the target {nudge:?}"));
            }
            return match nudge {
                Nudge::Point(out) => self.commit_reaction(&ship_xform, winner.id, out),
                other => other.code(),
            };
        }

        if has_close_flags {
            let mut radius = winner.radius;
            if winner.atmosphere_range > radius {
                radius = winner.atmosphere_range;
            }
            let sphere_radius = r(e(radius) + 200.0);
            let rx = e(target.x) - e(origin.x);
            let ry = e(target.y) - e(origin.y);
            let rz = e(target.z) - e(origin.z);
            let rzf = r(rz);
            let inv = r(1.0 / ((rz * e(rzf) + ry * ry) + rx * rx).sqrt());
            let dir = Vector::new(r(rx * e(inv)), r(ry * e(inv)), r(e(rzf) * e(inv)));
            let hit = ray_sphere_intersect(origin, dir, winner.center, sphere_radius);
            if tracing {
                world.trace(format!(
                    "classify: ray {dir:?} against the sphere {:?} radius {sphere_radius}: {hit:?}",
                    winner.center
                ));
            }
            if hit.is_none() {
                return 4;
            }
        } else {
            // `PhySys::FindRayIntersect` against the winner's own body.
            let system = world.system_of(ship).unwrap_or(0);
            let hits = world.find_ray_intersect(system, winner.id, origin, target, 1);
            if tracing {
                world.trace(format!(
                    "classify: FindRayIntersect({}, {origin:?}, {target:?}): {} hits",
                    winner.id,
                    hits.len()
                ));
            }
            if hits.is_empty() {
                return 4;
            }
        }
        let nudge = nudge_obstacle_path_point(&best_slot, target, &ship_xform, false);
        if tracing {
            world.trace(format!("classify: nudge {nudge:?}"));
        }
        match nudge {
            Nudge::Point(out) => self.commit_reaction(&ship_xform, winner.id, out),
            other => other.code(),
        }
    }

    /// `ClassifyObstaclePhase`'s commit: the reaction heading toward the
    /// path point, in reaction when it is more than about 60 degrees off
    /// the nose.
    fn commit_reaction(&mut self, ship: &Transform, winner: u32, out: Vector) -> i32 {
        self.reaction.state = 0;
        self.reaction.last_obstacle_tag = winner;
        self.reaction.in_reaction = false;
        let h = sub(out, ship.translation);
        let inv = 1.0 / ((e(h.x) * e(h.x) + e(h.y) * e(h.y)) + e(h.z) * e(h.z)).sqrt();
        let h = Vector::new(r(inv * e(h.x)), r(inv * e(h.y)), r(inv * e(h.z)));
        self.reaction.heading = h;
        let m = &ship.rotation.m;
        let facing = (-e(m[2][2]) * e(h.z) + -e(m[1][2]) * e(h.y)) + -e(m[0][2]) * e(h.x);
        if facing < 0.5 {
            self.reaction.in_reaction = true;
        }
        0
    }

    /// `UpdateHeadingBlend(Dt)`: phase 0 starts a blend from the reaction
    /// heading; another phase but 1 turns the heading toward the target
    /// while the blend timer runs; otherwise the reaction is dropped.
    fn update_heading_blend<W: AiEnv + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        behavior: i32,
        dt: f32,
        scan: ScanContext,
    ) {
        let phase = self.classify_obstacle_phase(world, ship, behavior, scan);
        let translation = world.inspect(ship).map_or(Vector::ZERO, |v| v.position);
        if phase == 0 {
            self.blend_start = self.reaction.heading;
            self.blend_timer = 4.0;
            return;
        }
        if phase != 1 {
            let new_timer = e(self.blend_timer) - e(dt);
            let local_timer = r(new_timer);
            self.blend_timer = local_timer;
            if !(new_timer < 0.0) && self.blend_enabled {
                let tp = self.target_position;
                let dx = e(tp.x) - e(translation.x);
                let dy = e(tp.y) - e(translation.y);
                let dz = e(tp.z) - e(translation.z);
                let dzf = r(dz);
                let inv = r(1.0 / ((dz * e(dzf) + dx * dx) + dy * dy).sqrt());
                let dir = Vector::new(r(dx * e(inv)), r(dy * e(inv)), r(e(dzf) * e(inv)));
                let bs = self.blend_start;
                let dot = (e(dir.x) * e(bs.x) + e(dir.z) * e(bs.z)) + e(dir.y) * e(bs.y);
                let dot_f = r(dot);
                let heading = if dot > 0.999 {
                    dir
                } else {
                    let axis = Vector::new(
                        r(e(dir.z) * e(bs.y) - e(dir.y) * e(bs.z)),
                        r(e(dir.x) * e(bs.z) - e(dir.z) * e(bs.x)),
                        r(e(dir.y) * e(bs.x) - e(dir.x) * e(bs.y)),
                    );
                    let half = e(dot_f).acos() * ((e(4.0) - e(local_timer)) * e(0.25)) * 0.5;
                    let axis_len = ((e(axis.z) * e(axis.z) + e(axis.x) * e(axis.x))
                        + e(axis.y) * e(axis.y))
                    .sqrt();
                    let k = r(half.sin() / axis_len);
                    let quat = Quaternion::new(
                        r(half.cos()),
                        r(e(axis.x) * e(k)),
                        r(e(axis.y) * e(k)),
                        r(e(axis.z) * e(k)),
                    );
                    transform_matrix(&quaternion_to_matrix(&quat), bs)
                };
                self.reaction.last_obstacle_tag = 0;
                self.reaction.in_reaction = false;
                self.reaction.state = 2;
                self.reaction.heading = heading;
                return;
            }
        }
        self.blend_timer = 0.0;
        self.reaction.last_obstacle_tag = 0;
        self.reaction.state = 3;
        self.reaction.heading = Vector::ZERO;
        self.reaction.in_reaction = false;
        self.reaction.close_encounter = false;
    }
}

/// `ClampPointToBoxSurface(Box, Src, Dst)` (`common_pathfind.cpp:700`):
/// the point on the box's surface nearest `local` (box space) and whether
/// `local` was outside; a point inside goes to the face its smallest
/// overshoot points at.
fn clamp_point_to_box_surface(half: Vector, local: Vector) -> (bool, Vector) {
    let half = [half.x, half.y, half.z];
    let input = [local.x, local.y, local.z];
    let mut out = input;
    let mut overshoot = [0.0f32; 3];
    let mut outside = false;
    for axis in 0..3 {
        overshoot[axis] = r(e(input[axis]).abs() - e(half[axis]));
        if overshoot[axis] >= 0.0 {
            out[axis] = half[axis].copysign(input[axis]);
            outside = true;
        }
    }
    if !outside {
        let nearest = if overshoot[0] >= overshoot[1] {
            if overshoot[0] >= overshoot[2] { 0 } else { 2 }
        } else if overshoot[1] >= overshoot[2] {
            1
        } else {
            2
        };
        out[nearest] = half[nearest].copysign(input[nearest]);
    }
    (outside, Vector::new(out[0], out[1], out[2]))
}

/// Mode 1 of `GetUserZoneApproachDirection`: the signed distance from the
/// box's surface (negative inside) and the unit direction to its centre.
fn box_zone_gate(zone: &UserZone, own: Vector) -> (f64, Vector) {
    let delta = sub(own, zone.box_center);
    let local = neuron_core::math::transpose_transform(&zone.box_rotation, delta);
    let (outside, clamped) = clamp_point_to_box_surface(zone.box_half_extent, local);
    let d = sub(local, clamped);
    let mut signed_sq = (e(d.y) * e(d.y) + e(d.z) * e(d.z)) + e(d.x) * e(d.x);
    if !outside {
        signed_sq = -signed_sq;
    }
    let gate = if signed_sq > 0.0 {
        e(r(signed_sq.sqrt()))
    } else {
        0.0
    };
    let to = sub(zone.box_center, own);
    let inv = 1.0 / ((e(to.x) * e(to.x) + e(to.y) * e(to.y)) + e(to.z) * e(to.z)).sqrt();
    let dir = Vector::new(r(e(to.x) * inv), r(e(to.y) * inv), r(e(to.z) * inv));
    (gate, dir)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_box_zone_points_inward_from_outside_and_out_from_inside() {
        let zone = UserZone {
            mode: 1,
            invert: 0,
            target_point: Vector::ZERO,
            radius: 0.0,
            box_rotation: Matrix::IDENTITY,
            box_center: Vector::new(100.0, 0.0, 0.0),
            box_half_extent: Vector::new(10.0, 10.0, 10.0),
            target_key: 0,
        };
        let (gate, dir) = box_zone_gate(&zone, Vector::ZERO);
        assert_eq!(gate, 90.0);
        assert_eq!((dir.x, dir.y, dir.z), (1.0, 0.0, 0.0));
        let (inside, _) = box_zone_gate(&zone, Vector::new(98.0, 0.0, 0.0));
        assert_eq!(inside, 0.0);
        let (out, p) =
            clamp_point_to_box_surface(Vector::new(10.0, 10.0, 10.0), Vector::new(-2.0, 9.0, 1.0));
        assert!(!out);
        assert_eq!((p.x, p.y, p.z), (-2.0, 10.0, 1.0));
    }

    #[test]
    fn cruise_is_blocked_by_a_contact_or_a_reaction() {
        let mut p = PathfindManager::default();
        assert!(p.is_cruise_unobstructed());
        p.avoidance.has_contact = true;
        assert!(!p.is_cruise_unobstructed());
        p.avoidance.has_contact = false;
        p.reaction.state = 2;
        p.reaction.in_reaction = true;
        assert!(!p.is_cruise_unobstructed());
    }

    #[test]
    fn the_phase_offsets_scale_the_draw() {
        assert_eq!(phase(0, 0.5), 0.0);
        assert_eq!(phase(16384, 1.0), 0.5);
        assert_eq!(phase(16384, 0.1), r(0.5 * e(0.1)));
    }
}
