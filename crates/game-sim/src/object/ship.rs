//! `CShip` (`CppSource/Common/common_cship.cpp`): the flight state the
//! controllers read, the drag and damping the update recomputes, and the
//! ship-only commands.

use neuron_core::math::Vector;

use crate::data::constants::EngineEquipConsts;
use crate::equip::flight::{EngineState, Nudge, ShipFlightArch, Steering, Strafe};
use crate::equip::{EquipKind, EquipManager, types};

use super::ObjHandle;

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// The flight state of a ship (+0x230..+0x2c0): what the force controllers
/// read and write besides the equipment.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ShipFlight {
    /// The archetype values.
    pub arch: ShipFlightArch,
    /// +0x230 `CSteering`.
    pub steering: Steering,
    /// +0x258 `CNudgeEngine`.
    pub nudge: Nudge,
    /// +0x280 `CStrafeEngine`.
    pub strafe: Strafe,
    /// +0x29c the throttle.
    pub throttle: f32,
    /// +0x2a0 the thrust power (thrusters and strafing draw it).
    pub thrust_power: f32,
    /// +0x2a4 the power plants' thrust capacity.
    pub max_thrust_power: f32,
    /// +0x2c0 the angular drag factor (1).
    pub angular_drag_factor: f32,
}

/// `ShipGunStats` (`get_gun_stats`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct GunStats {
    /// Active guns.
    pub active_guns: i32,
    /// Their mean muzzle velocity.
    pub avg_muzzle_velocity: f32,
    /// The farthest munition range.
    pub max_munition_range: f32,
    /// Counter-measure droppers.
    pub cm_droppers: i32,
    /// Their farthest range.
    pub cm_max_munition_range: f32,
}

/// `BayState`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum BayState {
    /// 0: closed.
    #[default]
    Closed,
    /// 1: closing.
    Closing,
    /// 2: open.
    Open,
    /// 3: opening.
    Opening,
}

/// `CShip`'s own data.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ShipData {
    /// The flight state.
    pub flight: ShipFlight,
    /// +0x1b8 the formation leader (a watcher).
    pub formation_leader: Option<ObjHandle>,
    /// +0x1c0 the formation offset.
    pub formation_offset: Vector,
    /// +0x1d0 the formation followers.
    pub formation_followers: Vec<ObjHandle>,
    /// +0x1e0 the follow leader.
    pub follow_leader: Option<ObjHandle>,
    /// +0x1e8 the follow offset.
    pub follow_offset: Vector,
    /// +0x204 the ships targeting this one.
    pub targeters: Vec<ObjHandle>,
    /// +0x220 the target.
    pub target: Option<ObjHandle>,
    /// +0x228 the sub-target (0 none).
    pub sub_target: u16,
    /// +0x2a8 the bay door requests.
    pub bay_door_counter: i32,
    /// +0x2ac the bay state.
    pub bay_state: BayState,
    /// +0x2b8 the trade lane speed.
    pub tradelane_speed: f32,
    /// +0x2bc in a trade lane.
    pub using_tradelane: bool,
    /// +0x2bd a player's ship.
    pub is_player: bool,
    /// +0x2c4 the gun stats need recomputing.
    pub gun_stats_dirty: bool,
    /// +0x2c8 the gun stats.
    pub gun_stats: GunStats,
    /// The group id.
    pub group_id: u32,
    /// The group name id.
    pub group_name_id: u32,
    /// The atmosphere the ship is in (a solar).
    pub atmosphere_solar: Option<ObjHandle>,
    /// The ship jumped out.
    pub jumped_out: bool,
    /// +0x2b0 the bay door animation (`BayAnim`), `None` (-1) without one.
    pub bay_anim: Option<super::anim::Playback>,
}

impl ShipData {
    /// `CShip::CShip`: angular drag factor 1, gun stats dirty.
    #[must_use]
    pub fn new(arch: ShipFlightArch) -> Self {
        Self {
            flight: ShipFlight {
                arch,
                angular_drag_factor: 1.0,
                ..ShipFlight::default()
            },
            gun_stats_dirty: true,
            ..Self::default()
        }
    }
}

/// The first engine of an equipment list and whether it is active.
fn first_engine(equipment: &EquipManager) -> Option<(&EngineState, bool)> {
    let e = equipment.first(types::ENGINE)?;
    match &e.kind {
        EquipKind::Engine(state) => Some((state, e.is_active())),
        _ => None,
    }
}

/// `CShip::get_linear_drag`: the archetype's drag, plus the first active
/// engine's (cruise drag while cruising), times the zone's drag modifier;
/// unrounded where the reference leaves it on the stack.
#[must_use]
pub fn linear_drag(
    flight: &ShipFlight,
    equipment: &EquipManager,
    zone_drag: Option<f32>,
    consts: &EngineEquipConsts,
) -> f64 {
    let mut total = flight.arch.linear_drag;
    if let Some((engine, true)) = first_engine(equipment) {
        total = if engine.cruise_active {
            r(engine.cruise_drag(consts) + e(total))
        } else {
            r(e(engine.linear_drag()) + e(total))
        };
    }
    match zone_drag {
        Some(m) => e(m) * e(total),
        None => e(total),
    }
}

/// `CShip::get_angular_drag`.
#[must_use]
pub fn angular_drag(flight: &ShipFlight) -> Vector {
    let a = flight.arch.angular_drag;
    let f = flight.angular_drag_factor;
    Vector::new(r(e(f) * e(a[0])), r(e(f) * e(a[1])), r(e(f) * e(a[2])))
}

/// `CShip::update`'s damping (`NO_DYNAMICS` clear): the linear drag over
/// the mass and the angular drag over the moment of inertia, per axis, the
/// values `PhySys::SetDamping` receives.
#[must_use]
pub fn damping(
    flight: &ShipFlight,
    equipment: &EquipManager,
    zone_drag: Option<f32>,
    consts: &EngineEquipConsts,
    mass: f32,
    inertia: Vector,
) -> (f32, Vector) {
    let mut drag = flight.arch.linear_drag;
    if let Some((engine, true)) = first_engine(equipment) {
        let engine_drag = if engine.cruise_active {
            engine.cruise_drag(consts)
        } else {
            e(engine.linear_drag())
        };
        drag = r(engine_drag + e(drag));
    }
    if let Some(m) = zone_drag {
        drag = r(e(m) * e(drag));
    }
    let linear = r(e(drag) / e(mass));
    let ad = angular_drag(flight);
    let angular = Vector::new(
        r(e(ad.x) / e(inertia.x)),
        r(e(ad.y) / e(inertia.y)),
        r(e(ad.z) / e(inertia.z)),
    );
    (linear, angular)
}

/// The thrust kinds `collect_force_components` sums (`ThrustEquipType`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ThrustKind {
    /// The engine.
    Engine,
    /// The engine and the thrusters.
    Thruster,
    /// Cruise.
    Cruise,
    /// A trade lane.
    Tradelane,
}

/// `CShip::collect_force_components`: the force and the drag the AI's
/// speed predictions use.
#[must_use]
pub fn collect_force_components(
    flight: &ShipFlight,
    equipment: &EquipManager,
    throttle: f32,
    kind: ThrustKind,
    zone: Option<(f32, f32)>,
    consts: &EngineEquipConsts,
    ship_linear_drag: f64,
) -> (f32, f32) {
    let (mut force, mut drag) = (0.0f32, 0.0f32);
    let engine = equipment.first(types::ENGINE).and_then(|e| match &e.kind {
        EquipKind::Engine(s) => Some(s),
        _ => None,
    });
    match kind {
        ThrustKind::Engine | ThrustKind::Thruster => {
            if let Some(engine) = engine {
                force = r(e(engine.thrust(throttle)) + e(force));
                drag = r(e(engine.linear_drag()) + e(drag));
            }
            if kind == ThrustKind::Thruster {
                for slot in equipment.traverse(types::THRUSTER, false) {
                    if let Some(EquipKind::Thruster(t)) = equipment.get(slot).map(|e| &e.kind) {
                        force = r(e(t.max_force) + e(force));
                    }
                }
            }
        }
        ThrustKind::Cruise => {
            if let Some(engine) = engine {
                force = r(e(engine.cruise_thrust(ship_linear_drag, throttle, consts)) + e(force));
                drag = r(engine.cruise_drag(consts) + e(drag));
            }
        }
        ThrustKind::Tradelane => {
            force = r(e(throttle) * e(flight.arch.nudge_force) + e(force));
            if let Some(engine) = engine {
                drag = r(e(engine.linear_drag()) + e(drag));
            }
        }
    }
    drag = r(e(flight.arch.linear_drag) + e(drag));
    let base = drag;
    if let Some((modifier, interference)) = zone {
        drag = r(e(modifier) * e(interference) * e(base) + e(drag));
    }
    (force, drag)
}

/// `CShip::get_thrust_power_ratio`.
#[must_use]
pub fn thrust_power_ratio(flight: &ShipFlight) -> f32 {
    if flight.max_thrust_power <= 0.0 {
        return 0.0;
    }
    flight.thrust_power / flight.max_thrust_power
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::archetype::equipment::Equipment;
    use crate::equip::{Equip, classes};

    fn ship() -> ShipFlight {
        ShipData::new(ShipFlightArch {
            linear_drag: 1.0,
            angular_drag: [100.0, 200.0, 300.0],
            steering_torque: [1.0; 3],
            ..ShipFlightArch::default()
        })
        .flight
    }

    fn engine_list(active: bool) -> EquipManager {
        let mut arch = Equipment::new(classes::ENGINE);
        if let crate::data::archetype::equipment::EquipmentKind::Engine { linear_drag, .. } =
            &mut arch.kind
        {
            *linear_drag = 5.0;
        }
        let mut e = Equip::alloc(0x22, &arch, false, true, None).unwrap();
        e.active = active;
        let mut m = EquipManager::default();
        m.add(e);
        m
    }

    #[test]
    fn damping_is_drag_over_mass_and_inertia() {
        let consts = crate::data::constants::Constants::default().engine;
        let (lin, ang) = damping(
            &ship(),
            &engine_list(true),
            None,
            &consts,
            2.0,
            Vector::new(10.0, 20.0, 30.0),
        );
        assert_eq!(lin, 3.0);
        assert_eq!(ang, Vector::new(10.0, 10.0, 10.0));
        let (lin, _) = damping(
            &ship(),
            &engine_list(false),
            Some(0.5),
            &consts,
            2.0,
            Vector::new(1.0, 1.0, 1.0),
        );
        assert_eq!(lin, 0.25);
    }

    #[test]
    fn linear_drag_counts_an_active_engine() {
        let consts = crate::data::constants::Constants::default().engine;
        assert_eq!(linear_drag(&ship(), &engine_list(true), None, &consts), 6.0);
        assert_eq!(
            linear_drag(&ship(), &engine_list(false), Some(2.0), &consts),
            2.0
        );
    }

    #[test]
    fn force_components_sum_engine_and_drag() {
        let consts = crate::data::constants::Constants::default().engine;
        let (f, d) = collect_force_components(
            &ship(),
            &engine_list(true),
            1.0,
            ThrustKind::Engine,
            None,
            &consts,
            6.0,
        );
        assert_eq!(f, 0.0);
        assert_eq!(d, 6.0);
    }
}
