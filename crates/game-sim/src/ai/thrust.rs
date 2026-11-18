//! The system manager's thrust manager (`ThrustManager`,
//! `Common/common_thrustmanager.cpp`): how behaviours and ideas command
//! their ship's throttle, axis throttle, nudge, strafe, thrusters, engine
//! and cruise. Each call reads the ship first and issues the `IObjAffect`
//! command only when the state differs, so a replay sees exactly the
//! commands the reference sends.

use neuron_core::math::{Matrix, Vector, transform_matrix, transpose_transform};

use super::fp::{e, normalize3, r};
use super::raycast::clamped_dot3;
use super::world::{AiCommand, AiWorld, StrafeDir};

/// `ThrustManager` (`SystemManager+0x18`, its own fields).
#[derive(Clone, Debug, PartialEq)]
pub struct ThrustManager {
    /// +0x04 the throttle ceiling (5).
    pub max_throttle: f32,
    /// +0x08 the axis throttle last read or commanded.
    pub axis_throttle: Vector,
    /// +0x2c cruise was commanded on.
    pub cruise_engaged: bool,
    /// +0x2d the pathfinder lets the ship cruise
    /// (`PathfindManager::IsCruiseUnobstructed`).
    pub cruise_unobstructed: bool,
    /// +0x2e the engine was commanded active.
    pub engine_active: bool,
    /// +0x2f the auto-level fallback (`SetSteeringFallbackFlag`).
    pub auto_level: bool,
    /// +0x30 obstacle avoidance on; off for the player's ship.
    pub auto_avoidance: bool,
}

impl ThrustManager {
    /// `InitThrustManager`: avoidance is off for a player's ship.
    #[must_use]
    pub fn new(is_player: bool) -> Self {
        Self {
            max_throttle: 5.0,
            axis_throttle: Vector::ZERO,
            cruise_engaged: false,
            cruise_unobstructed: true,
            engine_active: false,
            auto_level: true,
            auto_avoidance: !is_player,
        }
    }

    /// `SetEngineActive`: the first engine on or off when its state
    /// differs; a player's ship is checked every call, others only when
    /// the cached state changes.
    pub fn set_engine_active<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        active: bool,
    ) {
        let Some(view) = world.inspect(ship) else {
            return;
        };
        if !view.is_player && self.engine_active == active {
            return;
        }
        if let Some(engine) = view.engine
            && !engine.destroyed
            && engine.active != active
        {
            world.command(
                ship,
                AiCommand::ActivateEquip {
                    equipment: engine.id,
                    on: active,
                },
            );
        }
        self.engine_active = active;
    }

    /// `engage_cruise`: refused while the pathfinder blocks cruise;
    /// otherwise the engine is activated and cruise commanded when it is
    /// not already in that state.
    pub fn engage_cruise<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        engage: bool,
    ) {
        if engage && !self.cruise_unobstructed {
            return;
        }
        self.set_engine_active(world, ship, true);
        let (cruise, _) = cruise_of(world, ship);
        if cruise == engage && self.cruise_engaged == engage {
            return;
        }
        world.command(
            ship,
            AiCommand::EngageCruise {
                engage,
                instant: false,
            },
        );
        self.cruise_engaged = engage;
    }

    /// `SetAxisThrottle`: commanded when a component moved by more than
    /// 1e-6 squared from the ship's current axis throttle.
    pub fn set_axis_throttle<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        wanted: Vector,
    ) {
        let Some(view) = world.inspect(ship) else {
            return;
        };
        let Some(current) = view.axis_throttle else {
            return;
        };
        self.axis_throttle = current;
        let epsilon = e(f32::from_bits(0x3586_37be));
        let dx = e(wanted.x) - e(current.x);
        let dy = e(wanted.y) - e(current.y);
        let dz = e(wanted.z) - e(current.z);
        if dx * dx <= epsilon && dy * dy <= epsilon && dz * dz <= epsilon {
            return;
        }
        world.command(ship, AiCommand::SetAxisThrottle(wanted));
        self.axis_throttle = wanted;
    }

    /// `SetStrafeDir`.
    pub fn set_strafe_dir<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        dir: StrafeDir,
    ) {
        self.set_engine_active(world, ship, true);
        let Some(current) = world.inspect(ship).and_then(|v| v.strafe) else {
            return;
        };
        if current == dir {
            return;
        }
        world.command(ship, AiCommand::SetStrafe(dir));
    }

    /// `SetThrustersActive`: nothing while cruising outside a trade lane.
    pub fn set_thrusters_active<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        active: bool,
    ) {
        let (cruise, lane) = cruise_of(world, ship);
        if cruise && !lane {
            return;
        }
        if active {
            self.set_engine_active(world, ship, true);
        }
        let thrusters = world
            .inspect(ship)
            .and_then(|v| v.thrusters)
            .unwrap_or(false);
        if thrusters == active {
            return;
        }
        world.command(ship, AiCommand::SetThrustersActive(active));
    }

    /// `SetThrottle(throttle, mode)`: mode 0 is the engine (clamped to
    /// ±1, cruise left), mode 2 cruise (clamped to 0..1.2, cruise
    /// engaged); any other mode does nothing. `false` without a ship.
    pub fn set_throttle<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        throttle: f32,
        mode: i32,
    ) -> bool {
        if world.inspect(ship).is_none() {
            return false;
        }
        let mut clamped = if throttle > self.max_throttle {
            self.max_throttle
        } else {
            throttle
        };
        let mode = if !self.cruise_unobstructed && mode == 2 {
            0
        } else {
            mode
        };
        if mode != 0 && mode != 2 {
            return true;
        }
        if mode == 2 {
            // `> 1.2` then `< 0`; NaN passes through, as with `clamp`.
            clamped = clamped.clamp(0.0, 1.2);
            let (cruise, lane) = cruise_of(world, ship);
            let defer = cruise && !lane;
            if !defer {
                world.command(ship, AiCommand::SetThrottle(clamped));
            }
            if clamped > 0.0 && self.cruise_unobstructed {
                self.set_engine_active(world, ship, true);
                let (cruise_now, _) = cruise_of(world, ship);
                if !(cruise_now && self.cruise_engaged) {
                    world.command(
                        ship,
                        AiCommand::EngageCruise {
                            engage: true,
                            instant: false,
                        },
                    );
                    self.cruise_engaged = true;
                }
            }
            if defer {
                world.command(ship, AiCommand::SetThrottle(clamped));
            }
            return true;
        }
        clamped = clamped.clamp(-1.0, 1.0);
        self.set_engine_active(world, ship, true);
        let (cruise, _) = cruise_of(world, ship);
        if !(!cruise && !self.cruise_engaged) {
            world.command(
                ship,
                AiCommand::EngageCruise {
                    engage: false,
                    instant: false,
                },
            );
            self.cruise_engaged = false;
        }
        self.set_engine_active(world, ship, true);
        world.command(ship, AiCommand::SetThrottle(clamped));
        true
    }

    /// `SetNudgeDirection`: a world direction (normalised when longer
    /// than 1) into the ship's frame, commanded when it moved by more
    /// than 1e-6 squared.
    pub fn set_nudge_direction<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        world_dir: Vector,
    ) {
        self.set_engine_active(world, ship, true);
        let mut local = world_dir;
        if (e(local.y) * e(local.y) + e(local.z) * e(local.z)) + e(local.x) * e(local.x) > 1.0 {
            local = normalize3(local);
        }
        let Some(view) = world.inspect(ship) else {
            return;
        };
        let rotation: Matrix = view.orientation;
        let nudge = transpose_transform(&rotation, local);
        let Some(current) = view.nudge else {
            return;
        };
        let epsilon = e(1.000_000_1e-6_f32);
        let dx = e(nudge.x) - e(current.x);
        let dy = e(nudge.y) - e(current.y);
        let dz = e(nudge.z) - e(current.z);
        if !(dx * dx <= epsilon) || !(dy * dy <= epsilon) || !(dz * dz <= epsilon) {
            world.command(ship, AiCommand::SetNudge(nudge));
        }
    }

    /// `BlendNudgeDirection(WorldDir, Dt)`: the ship's current nudge (in
    /// the world frame, dropped when it is more than `acos(0.99)` off the
    /// wanted direction) grown toward the wanted direction by `1.5 * Dt`
    /// times its length when the wanted direction is the longer, then
    /// committed through `SetNudgeDirection`.
    pub fn blend_nudge_direction<W: AiWorld + ?Sized>(
        &mut self,
        world: &mut W,
        ship: W::Handle,
        world_dir: Vector,
        dt: f32,
    ) {
        let mut working = world_dir;
        let Some(view) = world.inspect(ship) else {
            return;
        };
        let local = view.nudge.unwrap_or(Vector::ZERO);
        let mut nudge = transform_matrix(&view.orientation, local);
        if clamped_dot3(normalize3(nudge), normalize3(working)) < e(0.99) {
            nudge = Vector::ZERO;
        }
        let len_working = ((e(working.y) * e(working.y) + e(working.x) * e(working.x))
            + e(working.z) * e(working.z))
        .sqrt();
        let len_nudge =
            ((e(nudge.y) * e(nudge.y) + e(nudge.z) * e(nudge.z)) + e(nudge.x) * e(nudge.x)).sqrt();
        if len_working > len_nudge {
            let k = e(1.5);
            let x_scaled = r(k * e(working.x) * e(dt));
            let x_term = r(e(x_scaled) * len_working);
            let y_term = r(k * e(working.y) * e(dt) * len_working);
            let z_scaled = r(k * e(working.z));
            let z_term = e(z_scaled) * e(dt) * len_working;
            working = Vector::new(
                r(e(x_term) + e(nudge.x)),
                r(e(nudge.y) + e(y_term)),
                r(e(nudge.z) + z_term),
            );
        }
        self.set_nudge_direction(world, ship, working);
    }

    /// The idle stop `IdleIdea::Update` and the player's switch to the
    /// idle behaviour make: both throttles to 0, no axis throttle, no
    /// nudge, no strafe, thrusters off.
    pub fn stop<W: AiWorld + ?Sized>(&mut self, world: &mut W, ship: W::Handle) {
        self.set_throttle(world, ship, 0.0, 2);
        self.set_throttle(world, ship, 0.0, 0);
        self.set_axis_throttle(world, ship, Vector::ZERO);
        self.set_nudge_direction(world, ship, Vector::ZERO);
        self.set_strafe_dir(world, ship, StrafeDir::None);
        self.set_thrusters_active(world, ship, false);
    }
}

/// `is_cruise_active(cruise, lane)`, both false when the slot fails.
fn cruise_of<W: AiWorld + ?Sized>(world: &W, ship: W::Handle) -> (bool, bool) {
    world
        .inspect(ship)
        .and_then(|v| v.cruise)
        .unwrap_or((false, false))
}
