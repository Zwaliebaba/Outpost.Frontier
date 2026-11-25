//! A ship's launch out of a dock: `CShip::launch` with its `LaunchAction`
//! (`Common/common_cship.cpp:2090`, `Common/common_launchaction.cpp:61`).
//! The server's `StarSystem::launch` calls it for an NPC's ship, the ship
//! itself as the launching object (`IObjRW`) and the dock as the base.
//!
//! - **The action.** Made, it keeps the base's type bits; a docking ring
//!   (bit 0x20) launches from its launch point 1. `Start` then chooses the
//!   way out.
//! - **A station or a capital ship** (bits 0x200100, not a ring): the AI
//!   flies the ship out. The action registers as the ship's directive
//!   callback, sets the control exclusion 6 and gives the ship's behaviour
//!   manager the launch directive (`DirectiveLaunchOp`: the base, the
//!   launch point, terminal 1; behaviour 8, [`crate::ai::launch`]). When
//!   the directive ends (`OnDirectiveComplete`) the exclusion is lifted,
//!   the launch is reported to the ship's `IObjAffect::slot_70` with the
//!   base's id ([`super::ServerHooks::launch_complete`]) and the action is
//!   over. The world runs the callback after the behaviour tick (D722).
//! - **A docking ring**: the ship's collisions go off, the control
//!   exclusion is 7 (no behaviour tick) and the throttle 0. The path ends
//!   150 m along the ring's launch orientation's forward axis (its third
//!   column negated) past the launch point (`CEqObj::launch_pos`), shifted
//!   by the ship's `hpmount` offset; each PSI the controller beams the
//!   ship onto the path, `150 f² + 80 (2.5 - t) + 40` metres back from its
//!   end while `t < 2.5` s (`f = (2.5 - t) / 2.5`), then linearly to the
//!   end at 3 s. A ring whose archetype has no docking camera beams the
//!   ship to the end at once. At the ship's first update after 3 s the
//!   throttle is 1, the ship is pushed forward at 80 m/s, a player's ship
//!   on a multiplayer client gets a random heading, the collisions come
//!   back, the exclusion is lifted, the launch is reported (`slot_70`),
//!   the throttle is 1 again and the behaviour hears it
//!   (`update_current_behavior_throttle`).
//! - **Anything else** launches nothing, and the action stays until the
//!   ship has gone.
//!
//! The action ends (`~LaunchAction`) by clearing its directive callback
//! from the ship's behaviour manager (made on the spot, as
//! `GetBehaviorManager` does) and unregistering its controller; it does
//! not turn the collisions back on.

use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::ControllerContext;

use crate::ai::behavior::{DirectiveOp, LaunchOp, ids};
use crate::ai::fp::{e, r};
use crate::ai::manager::PlayerInput;
use crate::object::ship::Launch;
use crate::object::{Command, KindData, ObjHandle, Object};

use super::{ControllerRef, ServerHooks, World, WorldEvent};

/// `LaunchActionVars::launch_coast_time`.
pub const LAUNCH_COAST_TIME: f32 = 0.5;

/// `LaunchActionVars::launch_decel_time`.
pub const LAUNCH_DECEL_TIME: f32 = 2.5;

/// The handle a `LaunchAction` holds in its ship's directive callbacks
/// (`set_directive_callback(this)`; the manager's slots hold handles,
/// D710). Content's handles are its own and must not use this one (D722).
pub const LAUNCH_ACTION_CALLBACK: u32 = u32::MAX;

/// `Start`'s type test for the AI's launch: a station or a capital ship.
const DIRECTIVE_LAUNCH_TYPES: u32 = 0x0020_0100;

/// `rand() * 3.0517578e-05f`'s factor (2^-15).
const RAND_SCALE: f64 = 1.0 / 32_768.0;

/// π/4 through a float, as a double (the random heading's range).
const QUARTER_PI: f64 = core::f32::consts::FRAC_PI_4 as f64;

/// π/8 through a float, as a double (the random heading's offset).
const EIGHTH_PI: f64 = core::f32::consts::FRAC_PI_8 as f64;

impl World {
    /// `CShip::launch(ShipObj, Base, LaunchType)` with the ship as the
    /// launching object, as `StarSystem::launch` passes it: the
    /// `LaunchAction` joins the ship's actions and starts (`Start`). A ship
    /// already launching ends that launch first (the reference keeps both
    /// actions, D720). Nothing for an object that is not a ship or a base
    /// without equipment (D721).
    pub fn launch(&mut self, h: ObjHandle, base: ObjHandle, launch_type: i32) {
        if self.ship_data(h).is_none() {
            return;
        }
        let Some(b) = self.get(base).filter(|o| o.as_equipped().is_some()) else {
            return;
        };
        let flags = b.simple.type_bits;
        let action = Launch {
            base: b.simple.id,
            flags,
            launch_type: if flags & 0x20 != 0 { 1 } else { launch_type },
            orientation: Matrix::IDENTITY,
            ..Launch::default()
        };
        if self.is_launching(h) {
            self.end_launch(h);
        }
        if let Some(ship) = self.ship_data_mut(h) {
            ship.launch = Some(action);
        }
        self.start_launch(h, base);
    }

    /// `CShip::is_launching`: the ship has a launch action.
    #[must_use]
    pub fn is_launching(&self, h: ObjHandle) -> bool {
        self.ship_data(h).is_some_and(|s| s.launch.is_some())
    }

    /// The ship's launch action, while it has one.
    #[must_use]
    pub fn launch_state(&self, h: ObjHandle) -> Option<&Launch> {
        self.ship_data(h).and_then(|s| s.launch.as_ref())
    }

    fn launch_mut(&mut self, h: ObjHandle) -> Option<&mut Launch> {
        self.ship_data_mut(h).and_then(|s| s.launch.as_mut())
    }

    /// `CEqObj::set_control_exclusion`.
    pub fn set_control_exclusion(&mut self, h: ObjHandle, exclusion: u32) {
        if let Some(eq) = self.get_mut(h).and_then(|o| o.equipped.as_deref_mut()) {
            eq.control_exclusion = exclusion;
        }
    }

    /// `LaunchAction::Start`.
    fn start_launch(&mut self, h: ObjHandle, base: ObjHandle) {
        let Some(action) = self.launch_state(h).cloned() else {
            return;
        };
        if !action.is_physical() {
            if action.flags & DIRECTIVE_LAUNCH_TYPES == 0 {
                return;
            }
            let op = DirectiveOp::Launch(LaunchOp {
                launch_object: action.base,
                index: action.launch_type,
                terminal: 1,
            });
            let registered = self
                .with_behavior_manager(h, |m, w| {
                    m.set_directive_callback(LAUNCH_ACTION_CALLBACK);
                    w.set_control_exclusion(h, 6);
                    // `IDirectiveInfo {1, Op.Priority (0), 8}`.
                    m.set_current_directive(w, h, [1, 0, ids::LAUNCH], Some(&op));
                })
                .is_some();
            if let Some(l) = self.launch_mut(h) {
                l.directive = registered;
            }
            return;
        }
        // The immediate launch out of a docking ring.
        self.set_collisions(h, false);
        self.set_control_exclusion(h, 7);
        let _ = self.command(h, Command::SetThrottle(0.0));
        let (_, mut pos, orient) = self.launch_pos(base, action.launch_type);
        // Back along the launch orientation's negated third column.
        let m = &orient.m;
        let forward = [-m[0][2], -m[1][2], -m[2][2]];
        pos = Vector::new(
            r(e(pos.x) + e(forward[0]) * 150.0),
            r(e(pos.y) + e(forward[1]) * 150.0),
            r(e(pos.z) + e(forward[2]) * 150.0),
        );
        // The ship's origin less its `hpmount` (`HardpointPosition`); a
        // model without one adds nothing (D721).
        let at = self.get(h).map_or(Vector::ZERO, |o| o.engine.position);
        let mount = self.hardpoint_world(h, b"hpmount").map_or(at, |(p, _)| p);
        pos = Vector::new(
            r(e(pos.x) + (e(at.x) - e(mount.x))),
            r(e(pos.y) + (e(at.y) - e(mount.y))),
            r(e(pos.z) + (e(at.z) - e(mount.z))),
        );
        let direction = Vector::new(m[0][2], m[1][2], m[2][2]);
        let no_docking_camera = !self.base_docking_camera(base);
        if let Some(l) = self.launch_mut(h) {
            l.position = pos;
            l.orientation = orient;
            l.direction = direction;
            l.coast_timer = r(e(LAUNCH_COAST_TIME) * 80.0);
            l.no_docking_camera = no_docking_camera;
        }
        self.register_controller(h, ControllerRef::Launch);
    }

    /// The base's archetype's `docking_camera` (`Archetype::EqObj::
    /// bDockingCamera`), true for an archetype the data does not have.
    fn base_docking_camera(&self, base: ObjHandle) -> bool {
        let Some(o) = self.get(base) else {
            return true;
        };
        let a = &self.data.archetypes;
        a.solars
            .get(&o.archetype)
            .map(|s| s.eq_obj.docking_camera)
            .or_else(|| a.ships.get(&o.archetype).map(|s| s.eq_obj.docking_camera))
            .unwrap_or(true)
    }

    /// `~LaunchAction`: the directive callback cleared from the ship's
    /// behaviour manager (made when it has none, `GetBehaviorManager`), the
    /// controller unregistered, the action gone. The collisions stay as
    /// they are.
    fn end_launch(&mut self, h: ObjHandle) {
        if self
            .ship_data_mut(h)
            .and_then(|s| s.launch.take())
            .is_none()
        {
            return;
        }
        let _ = self.with_behavior_manager(h, |m, _| {
            m.clear_directive_callback(LAUNCH_ACTION_CALLBACK);
        });
        self.unregister(h, ControllerRef::Launch);
    }

    /// `LaunchAction::update` from `ActionDB::update`: the AI's launch
    /// waits for its directive; the ring's launch completes at the first
    /// update after 3 s of PSIs (or once the controller beamed the ship to
    /// the end, without a docking camera) and otherwise wakes the
    /// controller. A completed action is removed.
    pub(crate) fn update_launch(&mut self, h: ObjHandle, hooks: &mut dyn ServerHooks) {
        let Some(action) = self.launch_state(h).cloned() else {
            return;
        };
        if !action.is_physical() {
            if action.complete {
                self.end_launch(h);
            }
            return;
        }
        let mut complete = action.complete;
        if !action.no_docking_camera {
            complete = e(action.coast_elapsed) >= e(LAUNCH_COAST_TIME) + e(LAUNCH_DECEL_TIME);
            if let Some(l) = self.launch_mut(h) {
                l.complete = complete;
            }
            if complete {
                let _ = self.command(h, Command::SetThrottle(1.0));
                // The ship's own forward axis now, not the launch's.
                let m = self
                    .get(h)
                    .map_or(Matrix::IDENTITY, |o| o.engine.orientation)
                    .m;
                let delta = Vector::new(
                    r(e(m[0][2]) * -80.0),
                    r(e(m[1][2]) * -80.0),
                    r(e(m[2][2]) * -80.0),
                );
                if let Some((system, body)) = self.body(h)
                    && let Some(world) = self.physics.world_mut(system)
                {
                    world.add_to_velocity(body, delta);
                }
            }
        }
        if !complete {
            let registered = self.objects.get(h).is_some_and(|o| {
                o.physical
                    .controllers
                    .iter()
                    .any(|(w, _, _)| *w == ControllerRef::Launch)
            });
            if registered
                && let Some((system, body)) = self.body(h)
                && let Some(world) = self.physics.world_mut(system)
            {
                world.wakeup(body);
            }
            return;
        }
        let is_player = self.ship_data(h).is_some_and(|s| s.is_player);
        if !self.config.single_player && is_player {
            self.random_launch_heading(h);
        }
        self.set_collisions(h, true);
        self.set_control_exclusion(h, 0);
        self.report_launch(h, action.base, hooks);
        let _ = self.command(h, Command::SetThrottle(1.0));
        let _ = self.with_behavior_manager(h, |m, w| {
            m.update_current_behavior(w, h, PlayerInput::Throttle(1.0));
        });
        self.end_launch(h);
    }

    /// `IObjAffect::slot_70(base)` of the launching ship: the world's event
    /// and the server's hook.
    fn report_launch(&mut self, h: ObjHandle, base: u32, hooks: &mut dyn ServerHooks) {
        if let Some(o) = self.get(h) {
            let ship = o.simple.id;
            self.events.push(WorldEvent::LaunchComplete { ship, base });
        }
        hooks.launch_complete(self, h, base);
    }

    /// A player's ship leaving a ring on a multiplayer client: its own
    /// orientation turned by two random angles in [-π/8, π/8), first in
    /// the x and z columns, then in the y and z columns, and the ship
    /// beamed to its centre of mass with it. The reference's `FCOS` is
    /// stored to a float and its `FSIN` kept as a double; here `f64::cos`
    /// and `f64::sin` of the same double (D723).
    fn random_launch_heading(&mut self, h: ObjHandle) {
        let Some(mut orient) = self.get(h).map(|o| o.engine.orientation) else {
            return;
        };
        let angle = self.launch_heading_angle();
        let (cos, sin) = (angle.cos() as f32, angle.sin());
        for row in &mut orient.m {
            let (x, z) = (e(row[0]), e(row[2]));
            row[0] = r(e(cos) * x - sin * z);
            row[2] = r(e(cos) * z + x * sin);
        }
        let angle = self.launch_heading_angle();
        let (cos, sin) = (angle.cos() as f32, angle.sin());
        for row in &mut orient.m {
            let (y, z) = (e(row[1]), e(row[2]));
            row[1] = r(sin * z + e(cos) * y);
            row[2] = r(e(cos) * z - y * sin);
        }
        let center = self.center_of_mass(h);
        self.beam_object(h, center, orient);
    }

    /// `rand() * 3.0517578e-05f * π/4 - π/8` on the x87 stack.
    fn launch_heading_angle(&mut self) -> f64 {
        // The product of two floats is exact: the draw is below 2^15.
        let unit = f64::from(self.rng.rand()) * RAND_SCALE;
        unit * QUARTER_PI - EIGHTH_PI
    }

    /// `NotifyDirectiveCallbacks` after the behaviour tick: for each
    /// directive result the ship's manager reported in it
    /// (`ManagerState::completed` past `before`), every non-empty slot of
    /// its five directive callbacks in slot order, each read again before
    /// its call (a callback may change them): the launch action's
    /// ([`Self::launch_directive_complete`], D722) or the server's
    /// ([`ServerHooks::directive_complete`], D194).
    pub(crate) fn directive_callbacks(
        &mut self,
        h: ObjHandle,
        before: usize,
        hooks: &mut dyn ServerHooks,
    ) {
        let results: Vec<(i32, i32)> = self
            .behavior_managers
            .get(&h)
            .map(|m| m.state.completed.get(before..).unwrap_or_default().to_vec())
            .unwrap_or_default();
        for result in results {
            for slot in 0..5 {
                let Some(handle) = self
                    .behavior_managers
                    .get(&h)
                    .map(|m| m.state.directive_callbacks[slot])
                else {
                    return;
                };
                if handle == 0 {
                    continue;
                }
                if handle == LAUNCH_ACTION_CALLBACK {
                    self.launch_directive_complete(h, hooks);
                } else {
                    hooks.directive_complete(self, h, handle, result);
                }
            }
        }
    }

    /// `LaunchAction::OnDirectiveComplete`: while the launch action's
    /// callback is registered and its launch not physical, the control
    /// exclusion is lifted, the launch is reported (`slot_70` with the
    /// base) and the action is over; the ship's next update removes it
    /// (D722).
    fn launch_directive_complete(&mut self, h: ObjHandle, hooks: &mut dyn ServerHooks) {
        let Some(base) = self
            .launch_state(h)
            .filter(|l| !l.is_physical() && l.directive && !l.complete)
            .map(|l| l.base)
        else {
            return;
        };
        self.set_control_exclusion(h, 0);
        self.report_launch(h, base, hooks);
        if let Some(l) = self.launch_mut(h) {
            l.complete = true;
        }
    }

    /// The number of directive results the ship's manager has reported so
    /// far (`ManagerState::completed`), 0 without one.
    pub(crate) fn directive_results(&self, h: ObjHandle) -> usize {
        self.behavior_managers
            .get(&h)
            .map_or(0, |m| m.state.completed.len())
    }
}

/// `LaunchAction::do_simulation_controller`: the coast time advances by
/// the PSI when the base's type bits are exactly a docking ring's (0x20).
/// Without a docking camera the ship is beamed to the path's end and the
/// action completes; otherwise it is beamed `X` metres back along the
/// launch direction from the end, `X = 150 f² + 80 (2.5 - t) + 40` while
/// `t < 2.5` (`f = (2.5 - t) / 2.5`), else `(3 - t) / 0.5 · 40`, with the
/// launch orientation. `X` stays unrounded; the y and z steps are stored
/// as floats before the add, the x step joins the position unrounded.
pub(crate) fn launch_controller(obj: &mut Object, dt: f32, context: &mut ControllerContext<'_>) {
    let KindData::Ship(ship) = &mut obj.data else {
        return;
    };
    let Some(l) = ship.launch.as_mut() else {
        return;
    };
    if l.flags == 0x20 {
        l.coast_elapsed = r(e(l.coast_elapsed) + e(dt));
    }
    if l.no_docking_camera {
        context.beam(l.position, l.orientation);
        l.complete = true;
        return;
    }
    let (coast, decel) = (e(LAUNCH_COAST_TIME), e(LAUNCH_DECEL_TIME));
    let elapsed = e(l.coast_elapsed);
    let x = if l.coast_elapsed < LAUNCH_DECEL_TIME {
        let remaining = decel - elapsed;
        let fraction = remaining / decel;
        (150.0 * fraction * fraction + 80.0 * remaining) + e(l.coast_timer)
    } else {
        ((coast + decel - elapsed) / coast) * e(l.coast_timer)
    };
    let (p, d) = (l.position, l.direction);
    let step_y = r(x * e(d.y));
    let step_z = r(x * e(d.z));
    let at = Vector::new(
        r(x * e(d.x) + e(p.x)),
        r(e(step_y) + e(p.y)),
        r(e(step_z) + e(p.z)),
    );
    context.beam(at, l.orientation);
}
