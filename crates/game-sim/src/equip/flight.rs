//! The flight-control equipment and its force controllers
//! (`CppSource/Common/common_ceengine.cpp`, `common_csteering.cpp`,
//! `common_cethruster.cpp`, `common_cepower.cpp`).
//!
//! Each controller's `do_simulation_controller` is a function of the state
//! it reads (the ship's throttle, power pools, orientation and archetype
//! values) and a [`Pusher`] that receives its pushes, so the world calls it
//! from the physics controller hook and the replay test calls it from the
//! oracle's recorded `ctl` inputs (design/sim-records.md).
//!
//! Arithmetic follows decision T11: the reference's x87 expressions in
//! `f64`, rounded to `f32` where it stores a `float`.

use neuron_core::math::{Matrix, Transform, Vector, inverse_rotate, rotate};
use neuron_core::physics::ControllerContext;

use crate::data::archetype::equipment::{Equipment, EquipmentKind};
use crate::data::constants::EngineEquipConsts;

use super::{EquipEvent, SubObjId};

/// The push API of `PhySys::Controller` (`push`, `rotate`), as the
/// controllers call it.
pub trait Pusher {
    /// `push(force, dt)`: `push_core_linear`.
    fn push(&mut self, force: Vector, dt: f32);
    /// `rotate(torque, dt)`: `push_core_angular_ws`.
    fn rotate(&mut self, torque: Vector, dt: f32);
}

impl Pusher for ControllerContext<'_> {
    fn push(&mut self, force: Vector, dt: f32) {
        ControllerContext::push(self, force, dt);
    }

    fn rotate(&mut self, torque: Vector, dt: f32) {
        ControllerContext::rotate(self, torque, dt);
    }
}

/// One push, as the oracle's `pushes` lists it.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum PushRecord {
    /// `push_core_linear(direction, magnitude)`.
    Linear(Vector, f32),
    /// `push_core_angular_ws(axis, magnitude)`.
    AngularWs(Vector, f32),
}

/// A [`Pusher`] that records the pushes in call order.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PushLog(pub Vec<PushRecord>);

impl Pusher for PushLog {
    fn push(&mut self, force: Vector, dt: f32) {
        self.0.push(PushRecord::Linear(force, dt));
    }

    fn rotate(&mut self, torque: Vector, dt: f32) {
        self.0.push(PushRecord::AngularWs(torque, dt));
    }
}

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// The `Archetype::Ship` values the flight code reads.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ShipFlightArch {
    /// +0x104 `linear_drag`.
    pub linear_drag: f32,
    /// +0x108..+0x110 `angular_drag`.
    pub angular_drag: [f32; 3],
    /// +0x114..+0x11c `steering_torque`.
    pub steering_torque: [f32; 3],
    /// +0x120 `nudge_force`.
    pub nudge_force: f32,
    /// +0x124 `strafe_force`.
    pub strafe_force: f32,
    /// +0x128 `strafe_power_usage`.
    pub strafe_power_usage: f32,
}

impl ShipFlightArch {
    /// The values of a ship archetype.
    #[must_use]
    pub fn of(ship: &crate::data::archetype::eq_obj::Ship) -> Self {
        Self {
            linear_drag: ship.linear_drag,
            angular_drag: ship.angular_drag,
            steering_torque: ship.steering_torque,
            nudge_force: ship.nudge_force,
            strafe_force: ship.strafe_force,
            strafe_power_usage: ship.strafe_power_usage,
        }
    }
}

/// `Archetype::Engine`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct EngineArch {
    /// +0x70 `max_force` (`ThrustMultiplier`).
    pub max_force: f32,
    /// +0x74 `linear_drag`.
    pub linear_drag: f32,
    /// +0x78 `power_usage` (`PowerDrawFactor`).
    pub power_usage: f32,
    /// +0x7c `cruise_power_usage`.
    pub cruise_power_usage: f32,
    /// +0x80 `cruise_charge_time`.
    pub cruise_charge_time: f32,
    /// `indestructible`.
    pub indestructible: bool,
    /// +0x88 `reverse_fraction`.
    pub reverse_fraction: f32,
}

impl EngineArch {
    /// The values of an engine archetype; `None` for another kind.
    #[must_use]
    pub fn of(arch: &Equipment) -> Option<Self> {
        match arch.kind {
            EquipmentKind::Engine {
                max_force,
                linear_drag,
                power_usage,
                cruise_power_usage,
                cruise_charge_time,
                indestructible,
                reverse_fraction,
            } => Some(Self {
                max_force,
                linear_drag,
                power_usage,
                cruise_power_usage,
                cruise_charge_time,
                indestructible,
                reverse_fraction,
            }),
            _ => None,
        }
    }
}

/// An exhaust nozzle the engine found (`ExhaustNozzleInfo`): the
/// `HpEngine0<N>` hardpoints of the owner.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Nozzle {
    /// The collision group or equipment that carries it
    /// (`inst_to_subobj_id` of the hardpoint's instance).
    pub sub_obj_id: SubObjId,
    /// N.
    pub index: i32,
    /// The hardpoint's position in its instance.
    pub position: Vector,
    /// Its orientation.
    pub orientation: Matrix,
}

/// `CEEngine`'s own state (+0x20..+0x54).
#[derive(Clone, Debug, PartialEq)]
pub struct EngineState {
    /// The archetype values.
    pub arch: EngineArch,
    /// +0x30 the controller is registered (`bControllerEnabled`).
    pub controller_enabled: bool,
    /// +0x34 the nozzles.
    pub nozzles: Vec<Nozzle>,
    /// +0x44 the throttle at the last wake-up.
    pub last_wake_throttle: f32,
    /// +0x4c the cruise charge rate (also the ramp accumulator once
    /// charged, as the reference reuses it).
    pub cruise_charge_rate: f32,
    /// +0x50 the time since cruise was engaged (negative while disrupted).
    pub cruise_charge_elapsed: f32,
    /// +0x54 cruise engaged.
    pub cruise_active: bool,
    /// A `wakeup()` was asked for since the world last looked.
    pub wake_requested: bool,
}

impl EngineState {
    /// `CEEngine::CEEngine` before the nozzle search and activation.
    #[must_use]
    pub fn new(arch: EngineArch) -> Self {
        Self {
            arch,
            controller_enabled: false,
            nozzles: Vec::new(),
            last_wake_throttle: 0.0,
            cruise_charge_rate: 0.0,
            cruise_charge_elapsed: 0.0,
            cruise_active: false,
            wake_requested: false,
        }
    }

    /// The nozzle part of `IsFunctioning`: nozzles, or an indestructible
    /// archetype.
    #[must_use]
    pub fn has_nozzle_or_indestructible(&self) -> bool {
        !self.nozzles.is_empty() || self.arch.indestructible
    }

    /// `GetLinearDrag`.
    #[must_use]
    pub fn linear_drag(&self) -> f32 {
        self.arch.linear_drag
    }

    /// `GetCruiseDrag`: left unrounded, as its callers keep it.
    #[must_use]
    pub fn cruise_drag(&self, consts: &EngineEquipConsts) -> f64 {
        e(self.arch.linear_drag) + e(consts.cruise_drag)
    }

    /// `GetThrust(throttle)`.
    #[must_use]
    pub fn thrust(&self, throttle: f32) -> f32 {
        let mut result = e(throttle);
        if throttle < 0.0 {
            result *= e(self.arch.reverse_fraction);
        }
        r(result * e(self.arch.max_force))
    }

    /// `GetPowerDraw(throttle)`: zero unless functioning.
    #[must_use]
    pub fn power_draw(&self, throttle: f32, functioning: bool) -> f32 {
        if functioning {
            r(e(throttle.abs()) * e(self.arch.power_usage))
        } else {
            0.0
        }
    }

    /// `GetCruiseThrust(throttle)`, with the ship's `get_linear_drag()`.
    #[must_use]
    pub fn cruise_thrust(&self, ship_linear_drag: f64, throttle: f32, consts: &EngineEquipConsts) -> f32 {
        r(ship_linear_drag * e(consts.cruising_speed) * e(throttle))
    }

    /// `NotifyArchGroupDestroyed`: the nozzles on that group go, one event
    /// each.
    pub fn arch_group_destroyed(&mut self, sub_obj_id: SubObjId, events: &mut Vec<EquipEvent>) {
        let before = self.nozzles.len();
        self.nozzles.retain(|n| n.sub_obj_id != sub_obj_id);
        for _ in self.nozzles.len()..before {
            events.push(EquipEvent::NozzleLost(sub_obj_id));
        }
    }

    /// `CEEngine::Update`'s own part: the cruise charge clock and the
    /// throttle wake-up. `throttle` is the ship's (read unless the update
    /// has `NO_DYNAMICS`).
    pub fn update(
        &mut self,
        functioning: bool,
        dt: f32,
        throttle: Option<f32>,
        events: &mut Vec<EquipEvent>,
    ) {
        if !functioning {
            return;
        }
        let charge_time = self.arch.cruise_charge_time;
        if self.cruise_active {
            if self.cruise_charge_elapsed < charge_time {
                let prev = self.cruise_charge_elapsed;
                let next = e(dt) + e(prev);
                self.cruise_charge_elapsed = r(next);
                if prev < 0.0 {
                    if next >= 0.0 {
                        let code = if charge_time > 0.0 { 2 } else { 3 };
                        events.push(EquipEvent::CruiseStateChanged(code));
                    }
                } else if prev < charge_time && next >= e(charge_time) {
                    events.push(EquipEvent::CruiseStateChanged(3));
                }
            }
        } else if self.cruise_charge_elapsed < 0.0 {
            let next = e(dt) + e(self.cruise_charge_elapsed);
            self.cruise_charge_elapsed = if next < 0.0 { r(next) } else { 0.0 };
        }
        if let Some(throttle) = throttle
            && throttle != self.last_wake_throttle
            && throttle != 0.0
        {
            self.wake_requested = true;
            self.last_wake_throttle = throttle;
        }
    }

    /// `CEEngine::EngageCruise`'s engine part; returns the new state and
    /// whether the ship's throttle is to be set to 1 (both branches set
    /// it). `active` is `IsActive()`; `ship_linear_drag` is the ship's
    /// `get_linear_drag()`, read after the flag changed, as the reference
    /// reads it.
    pub fn engage_cruise(
        &mut self,
        engage: bool,
        instant: bool,
        active: bool,
        consts: &EngineEquipConsts,
        ship_linear_drag: impl FnOnce(&Self) -> f64,
        events: &mut Vec<EquipEvent>,
    ) -> (bool, bool) {
        if !active || self.cruise_active == engage {
            return (self.cruise_active, false);
        }
        self.cruise_active = engage;
        if engage {
            if instant {
                self.cruise_charge_elapsed = self.arch.cruise_charge_time;
                events.push(EquipEvent::CruiseStateChanged(3));
                self.wake_requested = true;
                return (self.cruise_active, true);
            }
            let drag = ship_linear_drag(self) * e(consts.cruising_speed);
            self.cruise_charge_rate =
                r(e(consts.cruise_accel_time) * e(self.arch.max_force) / drag);
            if self.cruise_charge_elapsed >= 0.0 {
                events.push(EquipEvent::CruiseStateChanged(2));
            }
            self.wake_requested = true;
            return (self.cruise_active, true);
        }
        let code = if instant {
            self.cruise_charge_elapsed = -consts.cruise_disrupt_time;
            1
        } else {
            if self.cruise_charge_elapsed > 0.0 {
                self.cruise_charge_elapsed = 0.0;
            }
            0
        };
        events.push(EquipEvent::CruiseStateChanged(code));
        self.cruise_charge_rate = 0.0;
        self.wake_requested = true;
        (self.cruise_active, true)
    }
}

/// What the engine controller reads and writes on its ship.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EngineShipIo<'a> {
    /// +0x29c the throttle.
    pub throttle: f32,
    /// The ship's orientation (`CObject::Orientation`).
    pub orientation: &'a Matrix,
    /// The ship's `get_linear_drag()`, unrounded.
    pub linear_drag: f64,
}

/// `CEEngine::do_simulation_controller` (`common_ceengine.cpp:469-553`).
/// `power` is the owner's `CEqObj::Power` (+0x198).
pub fn engine_controller(
    engine: &mut EngineState,
    functioning: bool,
    ship: EngineShipIo<'_>,
    power: &mut f32,
    consts: &EngineEquipConsts,
    dt: f32,
    pusher: &mut dyn Pusher,
) {
    if !functioning {
        return;
    }
    let arch = engine.arch;
    let force: f32;
    let power_rate: f64;
    if engine.cruise_active {
        if engine.cruise_charge_elapsed < arch.cruise_charge_time {
            let mut thrust = e(ship.throttle);
            if ship.throttle < 0.0 {
                thrust *= e(arch.reverse_fraction);
            }
            force = r(thrust * e(arch.max_force));
        } else {
            let mut ramp = 1.0f32;
            let accel = e(engine.cruise_charge_rate) + e(dt);
            engine.cruise_charge_rate = r(accel);
            if accel < e(consts.cruise_accel_time) {
                ramp = r(accel / e(consts.cruise_accel_time));
            }
            let throttle = ship.throttle;
            force = r(ship.linear_drag * e(consts.cruising_speed) * e(throttle) * e(ramp));
        }
        power_rate = e(arch.cruise_power_usage);
    } else {
        let throttle = ship.throttle;
        power_rate = e(throttle.abs()) * e(arch.power_usage);
        let mut thrust = e(throttle);
        if throttle < 0.0 {
            thrust *= e(arch.reverse_fraction);
        }
        force = r(thrust * e(arch.max_force));
    }
    let new_power = e(*power) - power_rate * e(dt);
    if new_power < 0.0 || new_power.is_nan() {
        return;
    }
    *power = r(new_power);
    if force == 0.0 {
        return;
    }
    let m = &ship.orientation.m;
    let dir = Vector::new(m[0][2], m[1][2], m[2][2]);
    pusher.push(dir, -r(e(force) * e(dt)));
}

/// `CEThruster`'s state and archetype values.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Thruster {
    /// +0x88 `power_usage`.
    pub power_usage: f32,
    /// +0x8c `max_force`.
    pub max_force: f32,
    /// +0x5c the controller is registered.
    pub controller_enabled: bool,
    /// +0x60 it pushed in the last PSI.
    pub could_thrust: bool,
    /// A `wakeup()` was asked for.
    pub wake_requested: bool,
}

impl Thruster {
    /// The values of a thruster archetype.
    #[must_use]
    pub fn of(arch: &Equipment) -> Option<Self> {
        match arch.kind {
            EquipmentKind::Thruster {
                power_usage,
                max_force,
                ..
            } => Some(Self {
                power_usage,
                max_force,
                ..Self::default()
            }),
            _ => None,
        }
    }
}

/// `CEThruster::do_simulation_controller` (`common_cethruster.cpp`).
/// `thrust_power` is the ship's `ThrustPower` (+0x2a0).
pub fn thruster_controller(
    thruster: &mut Thruster,
    functioning: bool,
    orientation: &Matrix,
    thrust_power: &mut f32,
    dt: f32,
    events: &mut Vec<EquipEvent>,
    pusher: &mut dyn Pusher,
) {
    thruster.could_thrust = false;
    if !functioning {
        return;
    }
    let cost = e(thruster.power_usage) * e(dt);
    let pool = e(*thrust_power);
    if !(cost <= pool) {
        events.push(EquipEvent::ThrusterStarved);
        return;
    }
    *thrust_power = r(pool - cost);
    let m = &orientation.m;
    let dir = Vector::new(-m[0][2], -m[1][2], -m[2][2]);
    pusher.push(dir, r(e(thruster.max_force) * e(dt)));
    thruster.could_thrust = true;
}

/// `CSteering` (+0x230 of `CShip`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Steering {
    /// +0x10 the controller is registered.
    pub controller_enabled: bool,
    /// +0x18 `bActive`.
    pub active: bool,
    /// +0x1c the axis throttle.
    pub axis_throttle: Vector,
    /// A `wakeup()` was asked for.
    pub wake_requested: bool,
}

/// `CSteering::AXIS_CHANGE_THRESHOLD`.
pub const AXIS_CHANGE_THRESHOLD: f32 = 1e-6;
/// `CNudgeEngine::NUDGE_CHANGE_THRESHOLD`.
pub const NUDGE_CHANGE_THRESHOLD: f32 = 9.999_999_7e-5;

fn clamp_unit(v: f32) -> f32 {
    if v < -1.0 {
        -1.0
    } else if v > 1.0 {
        1.0
    } else {
        v
    }
}

impl Steering {
    /// `IsActive`.
    #[must_use]
    pub fn is_active(&self) -> bool {
        self.controller_enabled && self.active
    }

    /// `SetAxisThrottle`: each axis clamped to [-1, 1] (a NaN passes);
    /// nothing happens unless an axis moves by more than the threshold.
    pub fn set_axis_throttle(&mut self, throttle: Vector) {
        let cz = clamp_unit(throttle.z);
        let cy = clamp_unit(throttle.y);
        let cx = clamp_unit(throttle.x);
        let threshold_sq = AXIS_CHANGE_THRESHOLD * AXIS_CHANGE_THRESHOLD;
        let dx = e(self.axis_throttle.x) - e(cx);
        let dy = e(self.axis_throttle.y) - e(cy);
        let dz = e(self.axis_throttle.z) - e(cz);
        let t = e(threshold_sq);
        if dx * dx <= t && dy * dy <= t && dz * dz <= t {
            return;
        }
        self.axis_throttle = Vector::new(cx, cy, cz);
        self.wake_requested = true;
    }

    /// `GetAxisSteeringTorque(axis, factor)`, unrounded.
    #[must_use]
    pub fn axis_steering_torque(&self, arch: &ShipFlightArch, axis: u32, factor: f32) -> f64 {
        if !self.controller_enabled || !self.active {
            return 0.0;
        }
        match axis {
            0..=2 => e(arch.steering_torque[axis as usize]) * e(factor),
            _ => 0.0 * e(factor),
        }
    }
}

/// `CSteering::do_simulation_controller`.
pub fn steering_controller(
    steering: &Steering,
    arch: &ShipFlightArch,
    orientation: &Matrix,
    dt: f32,
    pusher: &mut dyn Pusher,
) {
    if !steering.controller_enabled || !steering.active {
        return;
    }
    let a = steering.axis_throttle;
    let len_sq = (e(a.x) * e(a.x) + e(a.y) * e(a.y)) + e(a.z) * e(a.z);
    if len_sq == 0.0 {
        return;
    }
    let xform = Transform::new(*orientation, Vector::ZERO);
    let local = inverse_rotate(&xform, a);
    let scaled = Vector::new(
        r(e(local.x) * e(arch.steering_torque[0])),
        r(e(local.y) * e(arch.steering_torque[1])),
        r(e(local.z) * e(arch.steering_torque[2])),
    );
    let torque = rotate(&xform, scaled);
    pusher.rotate(torque, dt);
}

/// `CNudgeEngine` (+0x258 of `CShip`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Nudge {
    /// +0x10 the controller is registered.
    pub controller_enabled: bool,
    /// +0x18 `bActive`.
    pub active: bool,
    /// +0x1c the nudge vector.
    pub current: Vector,
    /// A `wakeup()` was asked for.
    pub wake_requested: bool,
}

impl Nudge {
    /// `IsActive`.
    #[must_use]
    pub fn is_active(&self) -> bool {
        self.controller_enabled && self.active
    }

    /// `SetNudgeVec`: unchanged within the threshold; otherwise wakes,
    /// stores, and normalises a vector longer than 1.
    pub fn set_nudge_vec(&mut self, v: Vector) {
        let threshold_sq = e(NUDGE_CHANGE_THRESHOLD * NUDGE_CHANGE_THRESHOLD);
        let dx = e(self.current.x) - e(v.x);
        let dy = e(self.current.y) - e(v.y);
        let dz = e(self.current.z) - e(v.z);
        if dx * dx <= threshold_sq && dy * dy <= threshold_sq && dz * dz <= threshold_sq {
            return;
        }
        self.wake_requested = true;
        let len = ((e(v.z) * e(v.z) + e(v.y) * e(v.y)) + e(v.x) * e(v.x)).sqrt();
        self.current = v;
        if len > 1.0 {
            let inv = 1.0 / len;
            self.current = Vector::new(r(inv * e(v.x)), r(inv * e(v.y)), r(inv * e(v.z)));
        }
    }
}

/// `CNudgeEngine::do_simulation_controller`.
pub fn nudge_controller(
    nudge: &Nudge,
    arch: &ShipFlightArch,
    orientation: &Matrix,
    dt: f32,
    pusher: &mut dyn Pusher,
) {
    if !nudge.controller_enabled || !nudge.active {
        return;
    }
    let n = nudge.current;
    let len_sq = (e(n.x) * e(n.x) + e(n.y) * e(n.y)) + e(n.z) * e(n.z);
    if len_sq == 0.0 {
        return;
    }
    let xform = Transform::new(*orientation, Vector::ZERO);
    let world = rotate(&xform, n);
    let scale = e(arch.nudge_force);
    let force = Vector::new(
        r(e(world.x) * scale),
        r(e(world.y) * scale),
        r(e(world.z) * scale),
    );
    pusher.push(force, dt);
}

/// `StrafeDir` (`include/common_cequip.h:66`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum StrafeDir {
    /// 0.
    #[default]
    None,
    /// 1: −X.
    Left,
    /// 2: +X.
    Right,
    /// 3: +Y.
    Up,
    /// 4: −Y.
    Down,
}

impl StrafeDir {
    /// The wire and record value.
    #[must_use]
    pub fn value(self) -> i32 {
        match self {
            Self::None => 0,
            Self::Left => 1,
            Self::Right => 2,
            Self::Up => 3,
            Self::Down => 4,
        }
    }

    /// The direction of a record value; other values read as `None`.
    #[must_use]
    pub fn from_value(value: i32) -> Self {
        match value {
            1 => Self::Left,
            2 => Self::Right,
            3 => Self::Up,
            4 => Self::Down,
            _ => Self::None,
        }
    }
}

/// `CStrafeEngine` (+0x280 of `CShip`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Strafe {
    /// +0x10 the controller is registered.
    pub controller_enabled: bool,
    /// +0x18 the direction.
    pub current: StrafeDir,
    /// A `wakeup()` was asked for.
    pub wake_requested: bool,
}

impl Strafe {
    /// `SetStrafe`: wakes for a new direction other than none.
    pub fn set_strafe(&mut self, dir: StrafeDir) {
        if dir != self.current {
            self.current = dir;
            if dir != StrafeDir::None {
                self.wake_requested = true;
            }
        }
    }
}

/// `CStrafeEngine::do_simulation_controller`. `thrust_power` is the
/// ship's `ThrustPower`.
pub fn strafe_controller(
    strafe: &mut Strafe,
    arch: &ShipFlightArch,
    orientation: &Matrix,
    thrust_power: &mut f32,
    dt: f32,
    pusher: &mut dyn Pusher,
) {
    if strafe.current == StrafeDir::None {
        return;
    }
    let cost = e(arch.strafe_power_usage) * e(dt);
    let pool = e(*thrust_power);
    if !(cost <= pool) {
        strafe.current = StrafeDir::None;
        return;
    }
    *thrust_power = r(pool - cost);
    let m = &orientation.m;
    let dir = match strafe.current {
        StrafeDir::Left => Vector::new(-m[0][0], -m[1][0], -m[2][0]),
        StrafeDir::Right => Vector::new(m[0][0], m[1][0], m[2][0]),
        StrafeDir::Up => Vector::new(m[0][1], m[1][1], m[2][1]),
        StrafeDir::Down => Vector::new(-m[0][1], -m[1][1], -m[2][1]),
        StrafeDir::None => return,
    };
    let f = e(arch.strafe_force);
    let force = Vector::new(r(e(dir.x) * f), r(e(dir.y) * f), r(e(dir.z) * f));
    pusher.push(force, dt);
}

/// `CEPower`'s archetype values (`Archetype::Power`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PowerPlant {
    /// +0x70 `capacity`.
    pub capacity: f32,
    /// +0x74 `charge_rate`.
    pub charge_rate: f32,
    /// +0x78 `thrust_capacity`.
    pub thrust_capacity: f32,
    /// +0x7c `thrust_charge_rate`.
    pub thrust_charge_rate: f32,
}

impl PowerPlant {
    /// The values of a power archetype.
    #[must_use]
    pub fn of(arch: &Equipment) -> Option<Self> {
        match arch.kind {
            EquipmentKind::Power {
                capacity,
                charge_rate,
                thrust_capacity,
                thrust_charge_rate,
            } => Some(Self {
                capacity,
                charge_rate,
                thrust_capacity,
                thrust_charge_rate,
            }),
            _ => None,
        }
    }

    /// `CEPower::Update`'s recharge: the owner's power and, on a ship, its
    /// thrust power, each capped at its maximum.
    pub fn recharge(
        &self,
        functioning: bool,
        destroyed: bool,
        dt: f32,
        power: (&mut f32, f32),
        thrust: Option<(&mut f32, f32)>,
    ) {
        if !functioning {
            return;
        }
        let rate = if destroyed { 0.0 } else { self.charge_rate };
        let (current, max) = power;
        let next = e(rate) * e(dt) + e(*current);
        *current = if next < e(max) { r(next) } else { max };
        if let Some((current, max)) = thrust {
            let rate = if destroyed { 0.0 } else { self.thrust_charge_rate };
            let next = e(rate) * e(dt) + e(*current);
            *current = if next < e(max) { r(next) } else { max };
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn consts() -> EngineEquipConsts {
        crate::data::constants::Constants::default().engine
    }

    fn engine() -> EngineState {
        let mut s = EngineState::new(EngineArch {
            max_force: 48000.0,
            linear_drag: 600.0,
            power_usage: 1.0,
            cruise_power_usage: 0.0,
            cruise_charge_time: 5.0,
            indestructible: true,
            reverse_fraction: 0.15,
        });
        s.controller_enabled = true;
        s
    }

    #[test]
    fn engine_pushes_along_minus_z() {
        let mut s = engine();
        let mut power = 100.0f32;
        let mut log = PushLog::default();
        let io = EngineShipIo {
            throttle: 1.0,
            orientation: &Matrix::IDENTITY,
            linear_drag: 600.0,
        };
        engine_controller(&mut s, true, io, &mut power, &consts(), 0.1, &mut log);
        assert_eq!(power, 99.9);
        assert_eq!(
            log.0,
            vec![PushRecord::Linear(Vector::new(0.0, 0.0, 1.0), -4800.0)]
        );
    }

    #[test]
    fn engine_without_power_stays_quiet() {
        let mut s = engine();
        let mut power = 0.05f32;
        let mut log = PushLog::default();
        let io = EngineShipIo {
            throttle: 1.0,
            orientation: &Matrix::IDENTITY,
            linear_drag: 600.0,
        };
        engine_controller(&mut s, true, io, &mut power, &consts(), 0.1, &mut log);
        assert_eq!(power, 0.05);
        assert!(log.0.is_empty());
    }

    #[test]
    fn reverse_thrust_uses_the_fraction() {
        let s = engine();
        assert_eq!(s.thrust(-1.0), r(-0.15f32 as f64 * 48000.0));
    }

    #[test]
    fn steering_rotates_through_the_ship_frame() {
        let steering = Steering {
            controller_enabled: true,
            active: true,
            axis_throttle: Vector::new(0.0, 0.5, 0.0),
            wake_requested: false,
        };
        let arch = ShipFlightArch {
            steering_torque: [10.0, 20.0, 30.0],
            ..ShipFlightArch::default()
        };
        let mut log = PushLog::default();
        steering_controller(&steering, &arch, &Matrix::IDENTITY, 0.05, &mut log);
        assert_eq!(
            log.0,
            vec![PushRecord::AngularWs(Vector::new(0.0, 10.0, 0.0), 0.05)]
        );
        let mut idle = PushLog::default();
        let off = Steering {
            axis_throttle: Vector::ZERO,
            ..steering
        };
        steering_controller(&off, &arch, &Matrix::IDENTITY, 0.05, &mut idle);
        assert!(idle.0.is_empty());
    }

    #[test]
    fn axis_throttle_is_clamped_and_thresholded() {
        let mut s = Steering::default();
        s.set_axis_throttle(Vector::new(2.0, -3.0, 0.5));
        assert_eq!(s.axis_throttle, Vector::new(1.0, -1.0, 0.5));
        assert!(s.wake_requested);
        s.wake_requested = false;
        s.set_axis_throttle(Vector::new(1.0, -1.0, 0.500_000_1));
        assert!(!s.wake_requested);
    }

    #[test]
    fn nudge_is_normalised() {
        let mut n = Nudge::default();
        n.set_nudge_vec(Vector::new(3.0, 4.0, 0.0));
        assert_eq!(n.current, Vector::new(0.6, 0.8, 0.0));
    }

    #[test]
    fn strafe_drains_thrust_power_or_stops() {
        let arch = ShipFlightArch {
            strafe_force: 100.0,
            strafe_power_usage: 10.0,
            ..ShipFlightArch::default()
        };
        let mut s = Strafe {
            current: StrafeDir::Up,
            ..Strafe::default()
        };
        let mut pool = 1.5f32;
        let mut log = PushLog::default();
        strafe_controller(&mut s, &arch, &Matrix::IDENTITY, &mut pool, 0.1, &mut log);
        assert_eq!(pool, 0.5);
        assert_eq!(
            log.0,
            vec![PushRecord::Linear(Vector::new(0.0, 100.0, 0.0), 0.1)]
        );
        strafe_controller(&mut s, &arch, &Matrix::IDENTITY, &mut pool, 0.1, &mut log);
        assert_eq!(s.current, StrafeDir::None);
        assert_eq!(log.0.len(), 1);
    }

    #[test]
    fn thruster_pushes_forward_and_reports_starvation() {
        let mut t = Thruster {
            power_usage: 100.0,
            max_force: 1000.0,
            ..Thruster::default()
        };
        let mut pool = 15.0f32;
        let mut events = Vec::new();
        let mut log = PushLog::default();
        thruster_controller(&mut t, true, &Matrix::IDENTITY, &mut pool, 0.1, &mut events, &mut log);
        assert!(t.could_thrust);
        assert_eq!(pool, 5.0);
        assert_eq!(
            log.0,
            vec![PushRecord::Linear(Vector::new(-0.0, -0.0, -1.0), 100.0)]
        );
        thruster_controller(&mut t, true, &Matrix::IDENTITY, &mut pool, 0.1, &mut events, &mut log);
        assert_eq!(events, vec![EquipEvent::ThrusterStarved]);
        assert!(!t.could_thrust);
    }

    #[test]
    fn cruise_charges_and_reports() {
        let mut s = engine();
        let c = consts();
        let mut events = Vec::new();
        let (on, set_throttle) = s.engage_cruise(true, false, true, &c, |_| 600.0, &mut events);
        assert!(on && set_throttle);
        assert_eq!(events, vec![EquipEvent::CruiseStateChanged(2)]);
        for _ in 0..60 {
            s.update(true, 0.1, Some(1.0), &mut events);
        }
        assert_eq!(events.last(), Some(&EquipEvent::CruiseStateChanged(3)));
        let (off, _) = s.engage_cruise(false, true, true, &c, |_| 600.0, &mut events);
        assert!(!off);
        assert_eq!(s.cruise_charge_elapsed, -c.cruise_disrupt_time);
    }

    #[test]
    fn power_recharges_to_its_caps() {
        let p = PowerPlant {
            capacity: 100.0,
            charge_rate: 50.0,
            thrust_capacity: 20.0,
            thrust_charge_rate: 10.0,
        };
        let (mut power, mut thrust) = (90.0f32, 19.5f32);
        p.recharge(true, false, 0.1, (&mut power, 100.0), Some((&mut thrust, 20.0)));
        assert_eq!(power, 95.0);
        assert_eq!(thrust, 20.0);
    }
}
