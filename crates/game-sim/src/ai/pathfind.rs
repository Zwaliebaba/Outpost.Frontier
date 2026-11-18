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
//! `RefreshAvoidance`, `SelectAvoidancePoint`, `SelectObstacleDirection`,
//! the user-zone probe of modes 0 and 2 (`GetUserZoneApproachDirection`)
//! and `UpdateHeadingBlend`.
//!
//! Not ported (P2.5 gap, `design/library-ledger.md`): the box user zone
//! (mode 1),
//! `ClassifyObstaclePhase` (it walks the scanner's scan list and the
//! system's spatial partition, which the AI world does not expose yet; the
//! port answers phase 4, "nothing to react to", which is the reference's
//! answer when the scan list holds no obstacle within its capsules) and
//! `FindClosestObstacleToAvoid` (the scan list again; the port finds none,
//! so `SelectObstacleDirection` falls back to the probe's avoid direction
//! as the reference does when the list is empty).

use neuron_core::math::Quaternion;
use neuron_core::math::{Matrix, Vector, quaternion_to_matrix, transform_matrix};

use super::avoidance::{AvoidIndexTable, ObstacleAvoidance};
use super::behavior::AiEnv;
use super::fp::{e, r};
use super::intercept::{
    estimate_turn_circle_approach_cost, iface_position_or_zero, shape_biased_avoid_direction,
};
use super::thrust::ThrustManager;
use super::world::{AiWorld, ThrustEquipType};

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
        let speed = motion
            .as_deref()
            .map_or(0.0, |m| m.speed(1.0, ThrustEquipType::Engine));
        p.max_engine_speed = speed;
        if speed == -1.0 {
            p.field_1f8 = 0.0;
            p.field_1fc = 0.0;
        } else if let Some(m) = motion.as_deref() {
            p.field_1f8 = m.distance_travelled(0.0, speed, 1000.0, ThrustEquipType::Engine);
            p.field_1fc = m.distance_travelled(0.0, 300.0, 1000.0, ThrustEquipType::Engine);
        }
        drop(motion);
        p.blend_start = Vector::new(0.0, 1.0, 0.0);
        p.avoidance.phys_system_id = world.system_of(ship).unwrap_or(0);
        p
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
            self.refresh_avoidance(thrust, world, ship);
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

        self.update_heading_blend(world, ship, behavior, dt);

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
        let Some((direction, obstacle)) = self.select_obstacle_direction(&*world, ship) else {
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
    /// obstacle on the scan list (none: `FindClosestObstacleToAvoid` is not
    /// ported), else the probe's avoid direction and its contact.
    fn select_obstacle_direction<W: AiWorld + ?Sized>(
        &mut self,
        world: &W,
        ship: W::Handle,
    ) -> Option<(Vector, Option<u32>)> {
        if self.flag_245 == 0 || self.field_24c == 0 {
            return None;
        }
        self.field_255 = false;
        if let Some(obstacle) = self.find_closest_obstacle_to_avoid(world, ship) {
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

    /// `FindClosestObstacleToAvoid` over the scan list: not ported (the
    /// scan list is not part of the AI world yet), so no obstacle.
    fn find_closest_obstacle_to_avoid<W: AiWorld + ?Sized>(
        &self,
        _world: &W,
        _ship: W::Handle,
    ) -> Option<u32> {
        None
    }

    /// `GetUserZoneApproachDirection`: modes 0 and 2 (a sphere; the box of
    /// mode 1 is not ported and answers nothing).
    fn user_zone_approach_direction<W: AiWorld + ?Sized>(
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

    /// `ClassifyObstaclePhase`: the reaction reset, the cached target
    /// copied; the scan-list and spatial-partition walk is not ported, so
    /// the phase is 4 (no obstacle) on every path the reference reaches
    /// before it, and 4 after it too.
    fn classify_obstacle_phase<W: AiWorld + ?Sized>(
        &mut self,
        world: &W,
        ship: W::Handle,
        behavior: i32,
    ) -> i32 {
        self.reaction.reset();
        self.cached_obstacle_id = 0;
        self.has_cached_obstacle = false;
        let target = self.cached_target_position;
        self.target_position = target;
        if behavior == 10 {
            return 4;
        }
        let look_ahead = matches!(behavior, 1 | 3 | 5 | 0xb | 0xd | 0x12);
        let len_sq =
            (e(target.x) * e(target.x) + e(target.y) * e(target.y)) + e(target.z) * e(target.z);
        if len_sq < 1.0 {
            if !look_ahead {
                self.blend_enabled = false;
                return 4;
            }
            self.blend_enabled = true;
        } else {
            self.blend_enabled = true;
        }
        let throttle = world.inspect(ship).and_then(|v| v.throttle).unwrap_or(1.0);
        if e(throttle) < 0.1 {
            return 4;
        }
        // The scan list (`GetOwnScanList(0xfd58)`) and the planets of the
        // spatial partition within 20 km: not ported.
        4
    }

    /// `UpdateHeadingBlend(Dt)`: phase 0 starts a blend from the reaction
    /// heading; another phase but 1 turns the heading toward the target
    /// while the blend timer runs; otherwise the reaction is dropped.
    fn update_heading_blend<W: AiWorld + ?Sized>(
        &mut self,
        world: &W,
        ship: W::Handle,
        behavior: i32,
        dt: f32,
    ) {
        let phase = self.classify_obstacle_phase(world, ship, behavior);
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

#[cfg(test)]
mod tests {
    use super::*;

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
