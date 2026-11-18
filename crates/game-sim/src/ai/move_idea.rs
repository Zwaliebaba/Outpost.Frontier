//! `MoveIdea` (`Common/common_moveidea.cpp`, class in `include/common_idea.h`):
//! the idea that flies a ship to a watched object, a point or along a
//! direction, in engine (mode 0), trade-lane nudge (1) or cruise (2) mode,
//! slowing down inside its arrival range.
//!
//! The state is the C++ object's own fields; the `BaseIdea` plumbing
//! (`Object`, `Handle`, `Context`) is the [`NavCx`] each call takes: the
//! world and the ship, the ship's `ThrustManager` and its
//! `PathfindManager`. The watched object is kept by id (the reference's
//! `BaseWatcher` lets go of an object that leaves; the id stops resolving).
//! The embedded `StopIdea`'s `Update` returns 0 without doing anything in
//! the reference port (`common_idea.cpp:92`), so `StopStyle` 1 only skips
//! the other arms.

use neuron_core::math::{Matrix, Vector, transform_matrix};

use super::behavior::{AiEnv, Cx};
use super::fp::{add, column2, e, normalize3, r, scale, sub};
use super::idea::{Idea, IdeaBase};
use super::intercept::{
    estimate_turn_circle_approach_cost, get_intercept_clearance_of_iface, iface_position_or_zero,
};
use super::nav::{
    NavCx, projected_throttle, query_falloff_effect, query_fixed_falloff_effect, watched,
    with_own_motion,
};
use super::raycast::bearing_angle_to_point;
use super::world::{AiWorld, ThrustEquipType};

/// `MoveIdea::GetKind`.
pub const MOVE_IDEA_KIND: i16 = 6;

/// What [`MoveIdea::update`] returns (the reference's 0 to 3).
pub mod status {
    /// Still moving.
    pub const MOVING: i32 = 0;
    /// Arrived; `Commit` ran.
    pub const ARRIVED: i32 = 1;
    /// The thrust manager refused the engine throttle.
    pub const REFUSED: i32 = 2;
    /// No object.
    pub const NO_OBJECT: i32 = 3;
}

/// `MoveIdea` (`common_idea.h`), its fields from `+0x14`.
#[derive(Clone, Debug, PartialEq)]
pub struct MoveIdea {
    /// `BaseIdea`'s fields.
    pub base: IdeaBase,
    /// +0x14 the watched object's id (`BaseWatcher`).
    pub watcher: Option<u32>,
    /// +0x1c the target point (`SetPosition`).
    pub position: Vector,
    /// +0x28 the unit direction (`SetDirection`, `SetDirectionFrom`).
    pub direction: Vector,
    /// +0x34 0 engine, 1 trade-lane nudge, 2 cruise.
    pub mode: i32,
    /// +0x38 thrust only while the goal's bearing is below this (π).
    pub max_thrust_bearing: f32,
    /// +0x3c arrival range (200).
    pub arrival_range: f32,
    /// +0x40 the throttle (1).
    pub throttle: f32,
    /// +0x44 how to slow down: 0 cut the throttle, 1 the stop idea.
    pub stop_style: i32,
    /// +0x48 0 none, 1 watched object, 2 point, 3 direction.
    pub target_kind: i32,
    /// +0x4c an offset in the ship's frame added to its centre (the lead
    /// point).
    pub offset_4c: Vector,
    /// +0x58 stored by `SetEnabled`, never read.
    pub field_58: u8,
    /// +0x59 off bearing, switch the engine off when nonzero (1).
    pub kill_engine_off_bearing: u8,
    /// +0x5c `get_speed(1, engine)` at bind time.
    pub max_speed: f32,
    /// +0x60 `get_angular_speed_XY(0, 1)` at bind time.
    pub max_angular_speed_xy: f32,
}

impl Default for MoveIdea {
    fn default() -> Self {
        Self::new()
    }
}

/// `NormalizeInPlace` (`common_moveidea.cpp:31`): no zero-length guard.
fn normalize_in_place(v: Vector) -> Vector {
    let len = ((e(v.x) * e(v.x) + e(v.y) * e(v.y)) + e(v.z) * e(v.z)).sqrt();
    let inv = 1.0 / len;
    Vector::new(r(inv * e(v.x)), r(inv * e(v.y)), r(inv * e(v.z)))
}

/// `VelocityMagnitude` (`common_moveidea.cpp:44`): `sqrt((x*x + z*z) +
/// y*y)`, unrounded.
#[must_use]
pub fn velocity_magnitude(v: Vector) -> f64 {
    ((e(v.x) * e(v.x) + e(v.z) * e(v.z)) + e(v.y) * e(v.y)).sqrt()
}

impl MoveIdea {
    /// `MoveIdea::MoveIdea` (`common_moveidea.cpp:57`).
    #[must_use]
    pub fn new() -> Self {
        Self {
            base: IdeaBase::default(),
            watcher: None,
            position: Vector::ZERO,
            direction: Vector::ZERO,
            mode: 0,
            max_thrust_bearing: core::f32::consts::PI,
            arrival_range: 200.0,
            throttle: 1.0,
            stop_style: 0,
            target_kind: 0,
            offset_4c: Vector::ZERO,
            field_58: 0,
            kill_engine_off_bearing: 1,
            max_speed: 0.0,
            max_angular_speed_xy: 0.0,
        }
    }

    /// `MoveIdea::SetContext` (`common_moveidea.cpp:81`): the bind-time
    /// speeds from the ship's motion model; false when the ship is gone.
    pub fn set_context<C: NavCx + ?Sized>(&mut self, cx: &C) -> bool {
        if cx.world().inspect(cx.ship()).is_none() {
            return false;
        }
        self.base.owner = cx.world().inspect(cx.ship()).map(|v| v.id);
        self.base.attached = true;
        let (angular, speed) = with_own_motion(cx, |m| {
            (
                m.map_or(0.0, |m| m.angular_speed_xy(0.0, 1.0)),
                m.map_or(0.0, |m| m.speed(1.0, ThrustEquipType::Engine)),
            )
        });
        self.max_angular_speed_xy = angular;
        self.max_speed = speed;
        true
    }

    /// `MoveIdea::Commit` (`common_moveidea.cpp:485`): zero nudge, both
    /// throttles cut unless in nudge mode (for stop styles 0 and 1), and
    /// the pathfinder's mirrored target cleared.
    pub fn commit<C: NavCx + ?Sized>(&self, cx: &mut C) {
        cx.set_nudge_direction(Vector::ZERO);
        if self.mode != 1 && (0..=1).contains(&self.stop_style) {
            cx.set_throttle(0.0, 0);
            cx.set_throttle(0.0, 2);
        }
        let pathfind = cx.pathfind_mut();
        pathfind.target_id = 0;
        pathfind.cached_target_position = Vector::ZERO;
    }

    /// `MoveIdea::SetPosition` (`common_moveidea.cpp:523`): kind 2, mirrored
    /// into the pathfinder.
    pub fn set_position<C: NavCx + ?Sized>(&mut self, cx: &mut C, pos: Vector) {
        self.target_kind = 2;
        self.position = pos;
        cx.pathfind_mut().cached_target_position = self.position;
    }

    /// `MoveIdea::SetDirection` (`common_moveidea.cpp:539`): kind 3, the
    /// direction normalised (no zero guard).
    pub fn set_direction(&mut self, dir: Vector) {
        self.target_kind = 3;
        self.direction = normalize_in_place(dir);
    }

    /// `MoveIdea::SetDirectionFrom` (`common_moveidea.cpp:549`): kind 3,
    /// the negated column 2 of `basis`, normalised.
    pub fn set_direction_from(&mut self, basis: &Matrix) {
        self.target_kind = 3;
        let c = column2(basis);
        self.direction = normalize_in_place(Vector::new(-c.x, -c.y, -c.z));
    }

    /// `MoveIdea::SetWatchObject` (`common_moveidea.cpp:573`): kind 1 and
    /// the watched object's id and position mirrored, or kind 0 and both
    /// mirrors cleared. A watched object the world cannot inspect mirrors
    /// 0 and the origin (the reference calls through it).
    pub fn set_watch_object<C: NavCx + ?Sized>(&mut self, cx: &mut C, object: Option<u32>) {
        let Some(id) = object else {
            self.target_kind = 0;
            self.watcher = None;
            let pathfind = cx.pathfind_mut();
            pathfind.target_id = 0;
            pathfind.cached_target_position = Vector::ZERO;
            return;
        };
        self.target_kind = 1;
        self.watcher = Some(id);
        let view = watched(&*cx, Some(id));
        let pathfind = cx.pathfind_mut();
        pathfind.target_id = view.as_ref().map_or(0, |v| v.id);
        pathfind.cached_target_position = view.map_or(Vector::ZERO, |v| v.position);
    }

    /// `MoveIdea::SetMode` (`common_moveidea.cpp:619`): leaving cruise cuts
    /// the cruise throttle, leaving engine mode for nudge mode the engine
    /// throttle, leaving nudge mode zeroes the nudge.
    pub fn set_mode<C: NavCx + ?Sized>(&mut self, cx: &mut C, new_mode: i32) {
        let old = self.mode;
        if old != new_mode {
            if old == 2 {
                cx.set_throttle(0.0, 2);
            } else if old == 0 && new_mode != 2 {
                cx.set_throttle(0.0, 0);
            } else if old == 1 {
                cx.set_nudge_direction(Vector::ZERO);
            }
        }
        self.mode = new_mode;
    }

    /// `MoveIdea::SetAngle38`.
    pub fn set_angle_38(&mut self, value: f32) {
        self.max_thrust_bearing = value;
    }

    /// `MoveIdea::SetRange3c`.
    pub fn set_range_3c(&mut self, value: f32) {
        self.arrival_range = value;
    }

    /// `MoveIdea::SetScale40`.
    pub fn set_scale_40(&mut self, value: f32) {
        self.throttle = value;
    }

    /// `MoveIdea::SetThrottleScale` (`common_moveidea.cpp:695`): the
    /// throttle that gives `value` with the mode's thrust source (engine,
    /// trade lane, cruise; any other mode passes `value`'s bits as the
    /// source, as the reference does).
    pub fn set_throttle_scale<C: NavCx + ?Sized>(&mut self, cx: &C, value: f32) {
        let kind = match self.mode {
            0 => ThrustEquipType::Engine,
            1 => ThrustEquipType::Tradelane,
            2 => ThrustEquipType::Cruise,
            _ => ThrustEquipType::from_value(i32::from_ne_bytes(value.to_bits().to_ne_bytes())),
        };
        self.throttle = with_own_motion(cx, |m| projected_throttle(m, value, kind));
    }

    /// `MoveIdea::SetField44`.
    pub fn set_field_44(&mut self, value: i32) {
        self.stop_style = value;
    }

    /// `MoveIdea::SetEnabled`.
    pub fn set_enabled(&mut self, value: u8) {
        self.field_58 = value;
    }

    /// `MoveIdea::SetField59`.
    pub fn set_field_59(&mut self, value: u8) {
        self.kill_engine_off_bearing = value;
    }

    /// The goal point `Update` steers at (`common_moveidea.cpp:226`): the
    /// pathfinder's override goal when its bit 1 is set, else by target
    /// kind the watched object's centre, the point, or 10 km along the
    /// direction from `lead`; the origin for kind 0.
    #[must_use]
    pub fn goal(
        &self,
        override_goal: Option<Vector>,
        watched_center: Option<Vector>,
        lead: Vector,
    ) -> Vector {
        if let Some(goal) = override_goal {
            return goal;
        }
        match self.target_kind {
            1 => watched_center.unwrap_or(Vector::ZERO),
            2 => self.position,
            3 => far_goal(self.direction, lead),
            _ => Vector::ZERO,
        }
    }

    /// `MoveIdea::Update` (`common_moveidea.cpp:145`): one tick toward the
    /// goal. Returns a [`status`] value.
    ///
    /// With target kind 1 and the watched object gone the squared distance
    /// stays at its sentinel (the reference calls through the missing
    /// object; D162).
    pub fn update<C: NavCx + ?Sized>(&mut self, cx: &mut C, _dt: f32) -> i32 {
        let Some(view) = cx.world().inspect(cx.ship()) else {
            return status::NO_OBJECT;
        };
        let mut best_dist_sq = f32::from_bits(0x7eff_ffff);

        let rotated = transform_matrix(&view.orientation, self.offset_4c);
        let lead = add(rotated, view.center);
        let watched = watched(&*cx, self.watcher);

        if self.target_kind == 1 {
            if let Some(w) = &watched {
                let clearance = get_intercept_clearance_of_iface(&view, w);
                best_dist_sq = r(clearance * clearance);
            }
        } else if self.target_kind == 2 {
            best_dist_sq = r(dist_sq_zyx(lead, self.position));
        }

        let range = self.arrival_range;
        if e(best_dist_sq) < e(range) * e(range) {
            MoveIdea::commit(self, cx);
            return status::ARRIVED;
        }

        let (field_214, field_22c, field_23c, field_240) = {
            let p = cx.pathfind();
            (p.field_214, p.field_22c, p.field_23c, p.field_240)
        };
        let slot_override = (field_214 >> 1) & 1 != 0;
        let goal = self.goal(
            slot_override.then_some(field_22c),
            Some(iface_position_or_zero(watched.as_ref())),
            lead,
        );

        if self.mode == 2 && self.stop_style != 2 {
            let threshold = 3.0_f64 * 300.0;
            if threshold * threshold > e(best_dist_sq) {
                self.mode = 0;
            }
        }

        let mut speed = 0.0_f32;
        if self.stop_style == 0 || self.stop_style == 1 {
            let magnitude = r(velocity_magnitude(view.velocity));
            let kind = if self.mode == 1 {
                ThrustEquipType::Tradelane
            } else {
                ThrustEquipType::Engine
            };
            let stop_style = self.stop_style;
            speed = with_own_motion(&*cx, |motion| {
                if stop_style == 0 {
                    query_fixed_falloff_effect(motion, magnitude)
                } else {
                    query_falloff_effect(motion, -1.0, magnitude, 0.0, kind)
                }
            });
        }
        let scaled_speed = e(speed) * e(0.8);

        let mut mode_eff = self.mode;
        if slot_override && (field_240 == 1 || field_23c < 1.0) {
            mode_eff = 0;
        }

        let gate = scaled_speed + e(self.arrival_range);
        let close = e(best_dist_sq) < gate * gate;
        let moving = velocity_magnitude(view.velocity);
        let bearing = || bearing_angle_to_point(view.center, &view.orientation, goal);

        match mode_eff {
            2 => {
                if close && moving > 0.1 {
                    cx.engage_cruise(false);
                    if self.stop_style == 0 {
                        cx.set_throttle(0.0, 2);
                    }
                    return status::MOVING;
                }
                if bearing() < e(self.max_thrust_bearing) {
                    cx.set_throttle(self.throttle, 2);
                } else {
                    cx.engage_cruise(false);
                }
                status::MOVING
            }
            1 => {
                if close && moving > 0.1 {
                    cx.set_nudge_direction(Vector::ZERO);
                    return status::MOVING;
                }
                let dir = normalize3(sub(goal, lead));
                cx.set_nudge_direction(scale(dir, self.throttle));
                status::MOVING
            }
            0 => {
                if close && moving > e(0.1) {
                    if self.stop_style == 0 {
                        cx.set_throttle(0.0, 0);
                        cx.set_engine_active(true);
                    }
                    return status::MOVING;
                }
                if !(bearing() < e(self.max_thrust_bearing)) {
                    cx.set_throttle(0.0, 0);
                    cx.set_engine_active(self.kill_engine_off_bearing == 0);
                    return status::MOVING;
                }
                let mut throttle = self.throttle;
                if field_214 != 0 {
                    if field_23c < throttle {
                        throttle = field_23c;
                    }
                } else {
                    let (angular, linear) = (self.max_angular_speed_xy, self.max_speed);
                    let cost = with_own_motion(&*cx, |motion| {
                        estimate_turn_circle_approach_cost(&view, motion, goal, angular, linear)
                    });
                    if cost < e(throttle) {
                        throttle = r(cost);
                    }
                }
                if cx.set_throttle(throttle, 0) {
                    status::MOVING
                } else {
                    status::REFUSED
                }
            }
            _ => status::MOVING,
        }
    }
}

impl<W: AiEnv + ?Sized> Idea<W> for MoveIdea {
    fn base(&self) -> &IdeaBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut IdeaBase {
        &mut self.base
    }

    fn update(&mut self, cx: &mut Cx<'_, W>, dt: f32) -> i32 {
        MoveIdea::update(self, cx, dt)
    }

    fn kind(&self) -> i16 {
        MOVE_IDEA_KIND
    }

    fn commit(&mut self, cx: &mut Cx<'_, W>) {
        MoveIdea::commit(self, cx);
    }
}

/// `(dz*dz + dy*dy) + dx*dx` of `a - b`, unrounded.
fn dist_sq_zyx(a: Vector, b: Vector) -> f64 {
    let dx = e(a.x) - e(b.x);
    let dy = e(a.y) - e(b.y);
    let dz = e(a.z) - e(b.z);
    (dz * dz + dy * dy) + dx * dx
}

/// Target kind 3's goal (`common_moveidea.cpp:239`): `lead + direction *
/// 10000`, the z product rounded before its sum, x and y not.
#[must_use]
pub fn far_goal(direction: Vector, lead: Vector) -> Vector {
    let far = 10000.0_f64;
    let far_z = r(e(direction.z) * far);
    Vector::new(
        r(e(direction.x) * far + e(lead.x)),
        r(e(direction.y) * far + e(lead.y)),
        r(e(far_z) + e(lead.z)),
    )
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::ai::nav::{ShipUpControl, ThrustControl};
    use crate::ai::pathfind::PathfindManager;
    use crate::ai::world::{AiCommand, AiWorld, MotionModel, ObjectView, class};

    /// Records the thrust calls; `set_throttle` answers `accept`.
    #[derive(Debug, Default)]
    pub(crate) struct ThrustLog {
        pub calls: Vec<String>,
        pub accept: bool,
        pub gate: bool,
        pub engaged: bool,
    }

    pub(crate) struct Motion;

    impl MotionModel for Motion {
        fn time_to_accelerate(&self, _: f32, _: f32, _: f32, _: ThrustEquipType) -> f32 {
            0.0
        }
        fn distance_travelled(&self, _: f32, speed: f32, _: f32, _: ThrustEquipType) -> f32 {
            speed * 2.0
        }
        fn projected_throttle(&self, speed: f32, _: ThrustEquipType) -> f32 {
            speed / 100.0
        }
        fn speed(&self, throttle: f32, _: ThrustEquipType) -> f32 {
            throttle * 100.0
        }
        fn initial_speed_to_coast_distance(&self, _: f32, _: bool) -> f32 {
            0.0
        }
        fn time_to_accelerate_angularly(&self, _: f32, _: f32, _: f32) -> f32 {
            0.0
        }
        fn time_to_accelerate_angularly_xy(&self, _: f32, _: f32, _: f32, _: f32) -> f32 {
            0.0
        }
        fn angular_distance_travelled_xy(&self, _: f32, _: f32, _: f32, _: f32) -> f32 {
            0.0
        }
        fn angular_distance_travelled(&self, _: f32, _: f32, _: f32) -> f32 {
            0.0
        }
        fn angular_speed_xy(&self, _: f32, _: f32) -> f32 {
            1.0
        }
        fn angular_speed_z(&self, _: f32) -> f32 {
            0.0
        }
        fn projected_axis_throttle_xy(&self, speed: f32) -> f32 {
            speed
        }
        fn projected_axis_throttle_z(&self, _: f32) -> f32 {
            0.0
        }
    }

    /// Ship 1 at the origin facing -Z; object 2 wherever the test puts it.
    pub(crate) struct Two {
        pub ship: ObjectView,
        pub other: ObjectView,
        pub commands: Vec<AiCommand<u32>>,
    }

    impl Two {
        pub(crate) fn new(ship_velocity: Vector, other_at: Vector) -> Self {
            Self {
                ship: ObjectView {
                    id: 1,
                    class: class::SHIP,
                    orientation: Matrix::IDENTITY,
                    velocity: ship_velocity,
                    radius: 10.0,
                    ..ObjectView::default()
                },
                other: ObjectView {
                    id: 2,
                    class: class::SHIP,
                    position: other_at,
                    center: other_at,
                    orientation: Matrix::IDENTITY,
                    radius: 10.0,
                    ..ObjectView::default()
                },
                commands: Vec::new(),
            }
        }
    }

    impl AiWorld for Two {
        type Handle = u32;
        fn inspect(&self, h: u32) -> Option<ObjectView> {
            match h {
                1 => Some(self.ship.clone()),
                2 => Some(self.other.clone()),
                _ => None,
            }
        }
        fn handle_of(&self, id: u32) -> Option<u32> {
            Some(id)
        }
        fn system_objects(&self, _: u32, _: u32) -> Vec<u32> {
            vec![1, 2]
        }
        fn attitude(&self, _: u32, _: u32) -> Option<f32> {
            None
        }
        fn attitude_symmetrical(&self, _: u32, _: u32) -> Option<(f32, f32)> {
            None
        }
        fn hardpoint(&self, _: u32, _: &[u8]) -> Option<(Vector, Matrix)> {
            None
        }
        fn motion(&self, h: u32) -> Option<Box<dyn MotionModel + '_>> {
            (h == 1).then(|| Box::new(Motion) as Box<dyn MotionModel>)
        }
        fn system_of(&self, _: u32) -> Option<u32> {
            Some(1)
        }
        fn command(&mut self, _: u32, c: AiCommand<u32>) -> i32 {
            self.commands.push(c);
            0
        }
    }

    /// An idea's context over [`Two`]: the thrust calls recorded, the
    /// ship-up state and the pathfinder plain fields.
    pub(crate) struct TestCx {
        pub world: Two,
        pub ship: u32,
        pub thrust: ThrustLog,
        pub pathfind: PathfindManager,
        pub up: Vector,
        pub camera: bool,
        pub relevels: usize,
    }

    impl TestCx {
        pub(crate) fn new(world: Two) -> Self {
            Self {
                world,
                ship: 1,
                thrust: ThrustLog::default(),
                pathfind: PathfindManager::default(),
                up: Vector::new(0.0, 1.0, 0.0),
                camera: false,
                relevels: 0,
            }
        }
    }

    impl ThrustControl for TestCx {
        fn set_throttle(&mut self, throttle: f32, mode: i32) -> bool {
            self.thrust
                .calls
                .push(format!("throttle {throttle} {mode}"));
            self.thrust.accept
        }
        fn engage_cruise(&mut self, engage: bool) {
            self.thrust.calls.push(format!("cruise {engage}"));
        }
        fn set_nudge_direction(&mut self, d: Vector) {
            self.thrust
                .calls
                .push(format!("nudge {} {} {}", d.x, d.y, d.z));
        }
        fn set_engine_active(&mut self, active: bool) {
            self.thrust.calls.push(format!("engine {active}"));
        }
        fn set_axis_throttle(&mut self, d: Vector) {
            self.thrust
                .calls
                .push(format!("axis {} {} {}", d.x, d.y, d.z));
        }
        fn check_steering_gate(&self) -> bool {
            self.thrust.gate
        }
        fn set_thrusters_active(&mut self, active: bool) {
            self.thrust.calls.push(format!("thrusters {active}"));
        }
        fn cruise_engaged(&self) -> bool {
            self.thrust.engaged
        }
    }

    impl ShipUpControl for TestCx {
        fn ship_up_direction(&self) -> Vector {
            self.up
        }
        fn camera_level_status(&self) -> bool {
            self.camera
        }
        fn relevel_ship_up_direction(&mut self, _: bool) {
            self.relevels += 1;
        }
    }

    impl NavCx for TestCx {
        type World = Two;
        fn world(&self) -> &Two {
            &self.world
        }
        fn ship(&self) -> u32 {
            self.ship
        }
        fn pathfind(&self) -> &PathfindManager {
            &self.pathfind
        }
        fn pathfind_mut(&mut self) -> &mut PathfindManager {
            &mut self.pathfind
        }
        fn command(&mut self, c: AiCommand<u32>) -> i32 {
            self.world.command(self.ship, c)
        }
    }

    #[test]
    fn arrives_inside_range() {
        let mut cx = TestCx::new(Two::new(Vector::ZERO, Vector::ZERO));
        let mut idea = MoveIdea::new();
        assert!(idea.set_context(&cx));
        assert_eq!(idea.max_speed, 100.0);
        idea.set_position(&mut cx, Vector::new(0.0, 0.0, -50.0));
        assert_eq!(
            cx.pathfind.cached_target_position,
            Vector::new(0.0, 0.0, -50.0)
        );
        assert_eq!(idea.update(&mut cx, 0.1), status::ARRIVED);
        assert_eq!(
            cx.thrust.calls,
            ["nudge 0 0 0", "throttle 0 0", "throttle 0 2"]
        );
        assert_eq!(cx.pathfind.cached_target_position, Vector::ZERO);
        cx.ship = 9;
        assert_eq!(idea.update(&mut cx, 0.1), status::NO_OBJECT);
    }

    #[test]
    fn engine_mode_thrusts_toward_a_point_ahead() {
        let mut cx = TestCx::new(Two::new(Vector::ZERO, Vector::ZERO));
        let mut idea = MoveIdea::new();
        idea.set_context(&cx);
        idea.set_position(&mut cx, Vector::new(0.0, 0.0, -5000.0));
        cx.thrust.accept = true;
        assert_eq!(idea.update(&mut cx, 0.1), status::MOVING);
        assert_eq!(cx.thrust.calls, ["throttle 1 0"]);
        cx.thrust.accept = false;
        cx.thrust.calls.clear();
        assert_eq!(idea.update(&mut cx, 0.1), status::REFUSED);
        // Off bearing (behind the ship) with a narrow cone: cut and kill.
        idea.set_angle_38(0.5);
        idea.set_position(&mut cx, Vector::new(0.0, 0.0, 5000.0));
        cx.thrust.calls.clear();
        idea.update(&mut cx, 0.1);
        assert_eq!(cx.thrust.calls, ["throttle 0 0", "engine false"]);
    }

    #[test]
    fn closing_fast_cuts_the_throttle() {
        let mut cx = TestCx::new(Two::new(Vector::new(0.0, 0.0, -100.0), Vector::ZERO));
        let mut idea = MoveIdea::new();
        idea.set_context(&cx);
        // 300 away; the stopping distance 2 * 100 * 0.8 + 200 covers it.
        idea.set_position(&mut cx, Vector::new(0.0, 0.0, -300.0));
        idea.update(&mut cx, 0.1);
        assert_eq!(cx.thrust.calls, ["throttle 0 0", "engine true"]);
    }

    #[test]
    fn nudge_and_cruise_modes() {
        let mut cx = TestCx::new(Two::new(Vector::ZERO, Vector::new(0.0, 0.0, -5000.0)));
        let mut idea = MoveIdea::new();
        idea.set_context(&cx);
        idea.set_watch_object(&mut cx, Some(2));
        assert_eq!(cx.pathfind.target_id, 2);
        idea.set_mode(&mut cx, 1);
        assert_eq!(cx.thrust.calls, ["throttle 0 0"]);
        cx.thrust.calls.clear();
        idea.update(&mut cx, 0.1);
        assert_eq!(cx.thrust.calls, ["nudge 0 0 -1"]);
        idea.set_mode(&mut cx, 2);
        cx.thrust.calls.clear();
        idea.update(&mut cx, 0.1);
        assert_eq!(cx.thrust.calls, ["throttle 1 2"]);
        idea.set_throttle_scale(&cx, 50.0);
        assert_eq!(idea.throttle, 0.5);
        idea.set_watch_object(&mut cx, None);
        assert_eq!(idea.target_kind, 0);
        assert_eq!(cx.pathfind.target_id, 0);
    }

    #[test]
    fn direction_goal_and_override() {
        let mut idea = MoveIdea::new();
        idea.set_direction(Vector::new(0.0, 3.0, 4.0));
        assert_eq!(idea.direction, Vector::new(0.0, 0.6, 0.8));
        assert_eq!(
            idea.goal(None, None, Vector::ZERO),
            Vector::new(0.0, 6000.0, 8000.0)
        );
        let o = Vector::new(1.0, 2.0, 3.0);
        assert_eq!(idea.goal(Some(o), None, Vector::ZERO), o);
        idea.set_direction_from(&Matrix::IDENTITY);
        assert_eq!(idea.direction, Vector::new(0.0, 0.0, -1.0));
    }
}
