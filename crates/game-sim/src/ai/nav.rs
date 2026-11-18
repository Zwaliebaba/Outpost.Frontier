//! What the movement ideas need besides the world: the ship's
//! `ThrustManager` (a `SystemManager` component), the behaviour manager's
//! ship-up queries, and the `PathfindManager` fields the ideas read and
//! mirror their target into. Each is a trait or a plain struct here, so an
//! idea is a function of its own state and these (decision T10); the
//! owners implement them when their components are ported.
//!
//! The `Behavior_*` motion queries of `common_behavior.cpp` the ideas call
//! are here too, over [`MotionModel`].

use neuron_core::math::Vector;

use super::fp::e;
use super::world::{MotionModel, ThrustEquipType};

/// The `ThrustManager` calls the ideas make (`common_thrustmanager.cpp`,
/// reached as `Handle + 0x18`).
pub trait ThrustControl {
    /// `ThrustManager::SetThrottle(throttle, mode)`: mode 0 the engine,
    /// 2 cruise; whether the manager took it.
    fn set_throttle(&mut self, throttle: f32, mode: i32) -> bool;
    /// `ThrustManager::engage_cruise`.
    fn engage_cruise(&mut self, engage: bool);
    /// `ThrustManager::SetNudgeDirection`, a world direction.
    fn set_nudge_direction(&mut self, world_dir: Vector);
    /// `ThrustManager::SetEngineActive`.
    fn set_engine_active(&mut self, active: bool);
    /// `ThrustManager::SetAxisThrottle`.
    fn set_axis_throttle(&mut self, wanted: Vector);
    /// `ThrustManager::CheckSteeringGate`.
    fn check_steering_gate(&self) -> bool;
    /// `ThrustManager::SetThrustersActive`.
    fn set_thrusters_active(&mut self, active: bool);
    /// `ThrustManager::bCruiseEngaged` (`+0x2c`).
    fn cruise_engaged(&self) -> bool;
}

/// The `IBehaviorManager` calls `FaceIdea` makes on its context.
pub trait ShipUpControl {
    /// `IBehaviorManager::get_ship_up_direction`.
    fn ship_up_direction(&self) -> Vector;
    /// `IBehaviorManager::get_camera_level_status`.
    fn camera_level_status(&self) -> bool;
    /// `IBehaviorManager::RelevelShipUpDirection`.
    fn relevel_ship_up_direction(&mut self, relevel: bool);
}

/// The `PathfindManager` fields the movement ideas touch
/// (`include/common_pathfind.h`, reached as `[Context + 0xc]`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PathfindFields {
    /// +0x064 the id of the object an idea watches (`BaseIdea::PublishTargetId`).
    pub field_064: i32,
    /// +0x200 the cached target position (`ObstacleAvoidance::Update`'s
    /// target).
    pub cached_target_position: Vector,
    /// +0x214 the override record's flags: bit 1 replaces the goal, any
    /// bit caps the engine throttle.
    pub field_214: i32,
    /// +0x22c the override goal.
    pub field_22c: Vector,
    /// +0x23c the override throttle cap (constructor 1.0).
    pub field_23c: f32,
    /// +0x240 the override mode; 1 forces engine mode.
    pub field_240: i32,
    /// +0x248 (constructor -1); `SplineMoveIdea::Update` stores
    /// `0x30038010`.
    pub field_248: i32,
}

impl PathfindFields {
    /// The constructor's values of these fields.
    #[must_use]
    pub fn new() -> Self {
        Self {
            field_23c: 1.0,
            field_248: -1,
            ..Self::default()
        }
    }
}

/// `Behavior_InvokeManagerSlotD4` (`common_behavior.cpp:28`):
/// `get_projected_throttle`, 0 without a motion model.
#[must_use]
pub fn projected_throttle(
    motion: Option<&dyn MotionModel>,
    value: f32,
    kind: ThrustEquipType,
) -> f32 {
    motion.map_or(0.0, |m| m.projected_throttle(value, kind))
}

/// `Behavior_InvokeManagerSlotD8` (`common_behavior.cpp:49`): `get_speed`,
/// 0 without a motion model.
#[must_use]
pub fn speed(motion: Option<&dyn MotionModel>, value: f32, kind: ThrustEquipType) -> f32 {
    motion.map_or(0.0, |m| m.speed(value, kind))
}

/// `Behavior_QueryFalloffEffect` (`common_behavior.cpp:119`):
/// `get_distance_travelled(speed, distance, falloff, kind)` with a
/// distance below 0.001 snapped to 0; 0 for a negative falloff or without
/// a motion model.
#[must_use]
pub fn query_falloff_effect(
    motion: Option<&dyn MotionModel>,
    speed: f32,
    distance: f32,
    falloff: f32,
    kind: ThrustEquipType,
) -> f32 {
    let Some(m) = motion else {
        return 0.0;
    };
    if falloff < 0.0 {
        return 0.0;
    }
    let distance = if e(distance) < 0.001 { 0.0 } else { distance };
    m.distance_travelled(speed, distance, falloff, kind)
}

/// `Behavior_QueryFixedFalloffEffect` (`common_behavior.cpp:180`): the
/// falloff query at 1000 with the engine; 0 below 0.001.
#[must_use]
pub fn query_fixed_falloff_effect(motion: Option<&dyn MotionModel>, distance: f32) -> f32 {
    if motion.is_none() || distance < 0.001 {
        return 0.0;
    }
    query_falloff_effect(motion, 0.0, distance, 1000.0, ThrustEquipType::Engine)
}

/// `Behavior_QueryRadiusEffect` (`common_behavior.cpp:96`):
/// `get_time_to_accelerate(from, to, dt, kind)` with negative `to` and
/// `dt` clamped to 0 and `to` below 0.001 snapped to 0; 0 when `to` and
/// `from` are both 0 or without a motion model.
#[must_use]
pub fn query_radius_effect(
    motion: Option<&dyn MotionModel>,
    from: f32,
    to: f32,
    dt: f32,
    kind: ThrustEquipType,
) -> f32 {
    let Some(m) = motion else {
        return 0.0;
    };
    let mut to = if to < 0.0 { 0.0 } else { to };
    let dt = if dt < 0.0 { 0.0 } else { dt };
    if to == 0.0 && from == 0.0 {
        return 0.0;
    }
    if e(to) < 0.001 {
        to = 0.0;
    }
    m.time_to_accelerate(from, to, dt, kind)
}

/// `Behavior_GetIfaceMaxScalarAttribute` (`common_behavior.cpp:76`): the
/// larger of the physical radius and the atmosphere range.
#[must_use]
pub fn max_scalar_attribute(view: &super::world::ObjectView) -> f32 {
    if view.atmosphere_range > view.radius {
        view.atmosphere_range
    } else {
        view.radius
    }
}

/// `Behavior_InvokeManagerSlotF8`:`get_projected_axis_throttle_XY`, 0
/// without a motion model.
#[must_use]
pub fn projected_axis_throttle_xy(motion: Option<&dyn MotionModel>, value: f32) -> f32 {
    motion.map_or(0.0, |m| m.projected_axis_throttle_xy(value))
}
