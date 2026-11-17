//! The typed inspection and command APIs (decision record 0001, point 8)
//! that replace `IObjInspect` (85 slots) and `IObjAffect` (30 slots) of
//! `include/common_iface.h`: [`World::inspect`] reads an object,
//! [`World::command`] commands it. The AI (P2.5), the client, the server
//! and the scenario harness use the same calls, so a replay can record
//! them.

use neuron_core::math::{Matrix, Vector};

use crate::equip::flight::StrafeDir;
use crate::equip::launcher::FireResult;
use crate::equip::{EquipKind, SubObjId, types};
use crate::object::ship::BayState;
use crate::object::{KindData, ObjHandle, ObjectKind};

use super::World;

/// What `IObjInspect` answers about an object, read at one moment.
#[derive(Clone, Debug, PartialEq)]
pub struct Inspect {
    /// The kind.
    pub kind: ObjectKind,
    /// `get_id`.
    pub id: u32,
    /// `get_archetype` (its id).
    pub archetype: u32,
    /// `get_type` (the type bits).
    pub type_bits: u32,
    /// `get_position`.
    pub position: Vector,
    /// `get_orientation`.
    pub orientation: Matrix,
    /// `get_velocity`.
    pub velocity: Vector,
    /// `get_angular_velocity` (world space).
    pub angular_velocity: Vector,
    /// `get_mass`.
    pub mass: f32,
    /// `get_status`: the hit points.
    pub hit_pts: f32,
    /// `get_status`: the maximum.
    pub max_hit_pts: f32,
    /// `is_targetable`.
    pub targetable: bool,
    /// `get_owner_player`.
    pub owner_player: u32,
    /// `get_power` (equipped objects).
    pub power: Option<f32>,
    /// The ship part: throttle, axis throttle, nudge, strafe, cruise,
    /// target, thrust power.
    pub ship: Option<ShipInspect>,
    /// The object is marked dead (still readable until the reap).
    pub dead: bool,
}

/// The ship part of [`Inspect`].
#[derive(Clone, Debug, PartialEq)]
pub struct ShipInspect {
    /// `get_throttle`.
    pub throttle: f32,
    /// `get_axis_throttle`.
    pub axis_throttle: Vector,
    /// `get_nudge_vec`.
    pub nudge: Vector,
    /// `get_strafe_dir`.
    pub strafe: StrafeDir,
    /// `is_cruise_active`.
    pub cruise: bool,
    /// `get_target`.
    pub target: Option<ObjHandle>,
    /// `get_sub_target`.
    pub sub_target: SubObjId,
    /// `get_thrust_power`.
    pub thrust_power: f32,
    /// `get_max_thrust_power`.
    pub max_thrust_power: f32,
    /// `get_bay_state`.
    pub bay_state: BayState,
}

/// What `IObjAffect` does to an object.
#[derive(Clone, Debug, PartialEq)]
pub enum Command {
    /// Slot 0, `set_throttle`.
    SetThrottle(f32),
    /// Slot 1, `set_axis_throttle`.
    SetAxisThrottle(Vector),
    /// Slot 2, `set_nudge_vec`.
    SetNudgeVec(Vector),
    /// Slot 3, `set_strafe_dir`.
    SetStrafeDir(StrafeDir),
    /// `engage_cruise(engage, instant)` on the first engine.
    EngageCruise {
        /// Engage or disengage.
        engage: bool,
        /// Without the charge time.
        instant: bool,
    },
    /// Slot 13, `fire` (`EqObj::slot_34`): the guns at the aim point.
    Fire {
        /// The guns' ids.
        guns: Vec<SubObjId>,
        /// The aim point.
        target: Vector,
    },
    /// `set_target(target, sub)`.
    SetTarget(Option<ObjHandle>, SubObjId),
    /// `set_angular_drag_factor`.
    SetAngularDragFactor(f32),
    /// `set_thrust_power`.
    SetThrustPower(f32),
    /// `activate` an item (`CEquip::Activate`; the engine's and shield's
    /// own versions).
    ActivateEquip(SubObjId, bool),
    /// `set_hit_pts`.
    SetHitPts(f32),
}

/// What a command answered.
#[derive(Clone, Debug, PartialEq)]
pub enum CommandResult {
    /// Done (`IObjAffect`'s 0).
    Ok,
    /// The guns' results.
    Fired(Vec<(SubObjId, FireResult)>),
    /// The object cannot take the command (wrong kind, gone).
    Refused,
}

impl World {
    /// Reads an object; `None` for a reaped handle.
    #[must_use]
    pub fn inspect(&self, h: ObjHandle) -> Option<Inspect> {
        let o = self.get(h)?;
        let angular = match (o.physical.body, self.physics(o.physical.system)) {
            (Some(body), Some(w))
                if !matches!(o.data, KindData::Solar(_) | KindData::Asteroid(_)) =>
            {
                neuron_core::math::transform_matrix(
                    &o.engine.orientation,
                    w.angular_velocity_os(body),
                )
            }
            _ => Vector::ZERO,
        };
        let ship = o.as_ship().map(|s| ShipInspect {
            throttle: s.flight.throttle,
            axis_throttle: s.flight.steering.axis_throttle,
            nudge: s.flight.nudge.current,
            strafe: s.flight.strafe.current,
            cruise: o
                .equipped
                .as_deref()
                .and_then(|e| e.equipment.first(types::ENGINE))
                .is_some_and(|e| matches!(&e.kind, EquipKind::Engine(s) if s.cruise_active)),
            target: s.target.filter(|t| self.get(*t).is_some()),
            sub_target: s.sub_target,
            thrust_power: s.flight.thrust_power,
            max_thrust_power: s.flight.max_thrust_power,
            bay_state: s.bay_state,
        });
        Some(Inspect {
            kind: o.kind,
            id: o.simple.id,
            archetype: o.archetype,
            type_bits: o.simple.type_bits,
            position: o.engine.position,
            orientation: o.engine.orientation,
            velocity: self.velocity(h),
            angular_velocity: angular,
            mass: self.mass(h),
            hit_pts: o.simple.hit_pts,
            max_hit_pts: o.simple.max_hit_pts,
            targetable: o.simple.targetable,
            owner_player: o.simple.owner_player,
            power: o.equipped.as_deref().map(|e| e.power),
            ship,
            dead: o.dead,
        })
    }

    /// Commands an object.
    pub fn command(&mut self, h: ObjHandle, command: Command) -> CommandResult {
        let Some(o) = self.get(h) else {
            return CommandResult::Refused;
        };
        if o.dead {
            return CommandResult::Refused;
        }
        let consts = self.data.constants.engine.clone();
        let result = match command {
            Command::Fire { guns, target } => {
                return CommandResult::Fired(self.fire(h, &guns, target));
            }
            Command::SetHitPts(hp) => {
                if let Some(o) = self.get_mut(h) {
                    o.simple.hit_pts = hp;
                }
                CommandResult::Ok
            }
            Command::ActivateEquip(id, on) => self.activate_equip(h, id, on),
            other => {
                let Some(o) = self.get_mut(h) else {
                    return CommandResult::Refused;
                };
                let zone_drag = o.simple.zone_drag;
                let crate::object::Object { equipped, data, .. } = o;
                let KindData::Ship(ship) = data else {
                    return CommandResult::Refused;
                };
                match other {
                    Command::SetThrottle(t) => {
                        // `CShip::set_throttle`: an inactive first engine
                        // is activated.
                        ship.flight.throttle = t;
                        if let Some(eq) = equipped.as_deref_mut()
                            && let Some(slot) = eq.equipment.find_first(types::ENGINE)
                            && let Some(e) = eq.equipment.get_mut(slot)
                            && !e.is_active()
                        {
                            e.activate_base(true);
                        }
                        CommandResult::Ok
                    }
                    Command::SetAxisThrottle(v) => {
                        ship.flight.steering.set_axis_throttle(v);
                        CommandResult::Ok
                    }
                    Command::SetNudgeVec(v) => {
                        ship.flight.nudge.set_nudge_vec(v);
                        CommandResult::Ok
                    }
                    Command::SetStrafeDir(d) => {
                        ship.flight.strafe.set_strafe(d);
                        CommandResult::Ok
                    }
                    Command::SetTarget(t, sub) => {
                        ship.target = t;
                        ship.sub_target = sub;
                        CommandResult::Ok
                    }
                    Command::SetAngularDragFactor(f) => {
                        ship.flight.angular_drag_factor = f;
                        CommandResult::Ok
                    }
                    Command::SetThrustPower(p) => {
                        ship.flight.thrust_power = p;
                        CommandResult::Ok
                    }
                    Command::EngageCruise { engage, instant } => {
                        let Some(eq) = equipped.as_deref_mut() else {
                            return CommandResult::Refused;
                        };
                        let flight_arch = ship.flight.clone();
                        let Some(slot) = eq.equipment.find_first(types::ENGINE) else {
                            return CommandResult::Refused;
                        };
                        let snapshot = eq.equipment.clone();
                        let Some(e) = eq.equipment.get_mut(slot) else {
                            return CommandResult::Refused;
                        };
                        let active = e.is_active();
                        let crate::equip::Equip { kind, events, .. } = e;
                        if let EquipKind::Engine(engine) = kind {
                            let (_, set_throttle) = engine.engage_cruise(
                                engage,
                                instant,
                                active,
                                &consts,
                                |state| {
                                    // `get_linear_drag` with the engine as it
                                    // now is.
                                    let mut list = snapshot.clone();
                                    if let Some(EquipKind::Engine(s)) =
                                        list.get_mut(slot).map(|x| &mut x.kind)
                                    {
                                        s.cruise_active = state.cruise_active;
                                    }
                                    crate::object::ship::linear_drag(
                                        &flight_arch,
                                        &list,
                                        zone_drag,
                                        &consts,
                                    )
                                },
                                events,
                            );
                            if set_throttle {
                                ship.flight.throttle = 1.0;
                            }
                        }
                        CommandResult::Ok
                    }
                    _ => CommandResult::Refused,
                }
            }
        };
        self.apply_wakeups(h);
        result
    }

    /// `Activate` on an item, with the engine's and the shield's own
    /// versions and the controller registration they make.
    fn activate_equip(&mut self, h: ObjHandle, id: SubObjId, on: bool) -> CommandResult {
        let now = self.time;
        let Some(eq) = self.get_mut(h).and_then(|o| o.equipped.as_deref_mut()) else {
            return CommandResult::Refused;
        };
        let Some(e) = eq.equipment.by_id_mut(id) else {
            return CommandResult::Refused;
        };
        let active = e.activate_base(on);
        let mut register = None;
        match &mut e.kind {
            EquipKind::Engine(s) => {
                if !on {
                    s.cruise_active = false;
                }
                if active != s.controller_enabled {
                    s.controller_enabled = active;
                    register = Some(active);
                }
            }
            EquipKind::Thruster(t) => {
                if active {
                    t.wake_requested = true;
                } else {
                    t.could_thrust = false;
                }
            }
            EquipKind::Cloak(c) => c.activate(active),
            _ => {}
        }
        if matches!(e.kind, EquipKind::Shield(_)) {
            let status = super::world::shield_status(e);
            let crate::equip::Equip { kind, events, .. } = e;
            if let EquipKind::Shield(s) = kind {
                s.activate(status, now, events);
            }
        }
        match register {
            Some(true) => self.register_equip_controller(h, id),
            Some(false) => self.unregister(h, super::world::ControllerRef::Equip(id)),
            None => {}
        }
        self.apply_wakeups(h);
        CommandResult::Ok
    }
}
