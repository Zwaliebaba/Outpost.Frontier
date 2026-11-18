//! What the movement ideas need besides the world: the ship's
//! `ThrustManager` (a `SystemManager` component), the behaviour manager's
//! ship-up queries and the `PathfindManager` the ideas read and mirror
//! their target into. [`NavCx`] bundles them with the world and the ship:
//! the behaviour tick context ([`Cx`]) implements it over the manager's
//! [`ThrustManager`](super::thrust::ThrustManager) and fields, and a test
//! can implement it with a recording fake.
//!
//! The `Behavior_*` motion queries of `common_behavior.cpp` the ideas call
//! are here too, over [`MotionModel`].

use neuron_core::math::Vector;

use super::behavior::{AiEnv, Cx};
use super::fp::e;
use super::pathfind::PathfindManager;
use super::world::{AiCommand, AiWorld, MotionModel, ThrustEquipType};

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

/// What an idea runs against (`BaseIdea`'s `Object`, `Handle` and
/// `Context`): the world, the ship, its thrust manager, its behaviour
/// manager's ship-up state and its pathfinder.
pub trait NavCx: ThrustControl + ShipUpControl {
    /// The world.
    type World: AiWorld + ?Sized;

    /// The world, for reads.
    fn world(&self) -> &Self::World;

    /// The ship.
    fn ship(&self) -> <Self::World as AiWorld>::Handle;

    /// The pathfinder.
    fn pathfind(&self) -> &PathfindManager;

    /// The pathfinder, mutably.
    fn pathfind_mut(&mut self) -> &mut PathfindManager;

    /// One `IObjAffect` call on the ship, outside the thrust manager.
    fn command(&mut self, command: AiCommand<<Self::World as AiWorld>::Handle>) -> i32;
}

/// An object's view by id (a `BaseWatcher`'s target), `None` once it is
/// gone.
pub fn watched<C: NavCx + ?Sized>(cx: &C, id: Option<u32>) -> Option<super::world::ObjectView> {
    let world = cx.world();
    world.handle_of(id?).and_then(|h| world.inspect(h))
}

/// Runs `f` on the motion model of the object `id` (the box borrows the
/// world only for the call, so commands can follow).
pub fn with_motion_of<C: NavCx + ?Sized, T>(
    cx: &C,
    id: Option<u32>,
    f: impl FnOnce(Option<&dyn MotionModel>) -> T,
) -> T {
    let world = cx.world();
    let motion = id
        .and_then(|id| world.handle_of(id))
        .and_then(|h| world.motion(h));
    f(motion.as_deref())
}

/// Runs `f` on the ship's motion model.
pub fn with_own_motion<C: NavCx + ?Sized, T>(
    cx: &C,
    f: impl FnOnce(Option<&dyn MotionModel>) -> T,
) -> T {
    let motion = cx.world().motion(cx.ship());
    f(motion.as_deref())
}

impl<W: AiEnv + ?Sized> ThrustControl for Cx<'_, W> {
    fn set_throttle(&mut self, throttle: f32, mode: i32) -> bool {
        Cx::set_throttle(self, throttle, mode)
    }

    fn engage_cruise(&mut self, engage: bool) {
        Cx::engage_cruise(self, engage);
    }

    fn set_nudge_direction(&mut self, world_dir: Vector) {
        Cx::set_nudge_direction(self, world_dir);
    }

    fn set_engine_active(&mut self, active: bool) {
        Cx::set_engine_active(self, active);
    }

    fn set_axis_throttle(&mut self, wanted: Vector) {
        Cx::set_axis_throttle(self, wanted);
    }

    fn check_steering_gate(&self) -> bool {
        Cx::check_steering_gate(self)
    }

    fn set_thrusters_active(&mut self, active: bool) {
        Cx::set_thrusters_active(self, active);
    }

    fn cruise_engaged(&self) -> bool {
        self.state.thrust.cruise_engaged
    }
}

impl<W: AiEnv + ?Sized> ShipUpControl for Cx<'_, W> {
    fn ship_up_direction(&self) -> Vector {
        self.state.ship_up
    }

    fn camera_level_status(&self) -> bool {
        self.state.camera_level_status == 1 && self.state.level_camera
    }

    fn relevel_ship_up_direction(&mut self, relevel: bool) {
        if !self.state.level_camera {
            return;
        }
        let Some(view) = self.world.inspect(self.ship) else {
            return;
        };
        if !relevel {
            return;
        }
        let m = &view.orientation.m;
        let forward_y = ((e(m[2][2]) + e(m[0][2])) * 0.0 + e(m[1][2])).abs();
        let threshold = e(f32::from_bits(0x3f34_fdf4));
        if !(threshold > forward_y) {
            return;
        }
        self.state.level_camera = false;
        if Cx::check_steering_gate(self) {
            self.state.ship_up = Vector::new(0.0, 1.0, 0.0);
            self.state.ship_up_flag = false;
        }
        self.state.level_camera = true;
    }
}

impl<W: AiEnv + ?Sized> NavCx for Cx<'_, W> {
    type World = W;

    fn world(&self) -> &W {
        self.world
    }

    fn ship(&self) -> W::Handle {
        self.ship
    }

    fn pathfind(&self) -> &PathfindManager {
        &self.state.pathfind
    }

    fn pathfind_mut(&mut self) -> &mut PathfindManager {
        &mut self.state.pathfind
    }

    fn command(&mut self, command: AiCommand<W::Handle>) -> i32 {
        self.world.command(self.ship, command)
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
