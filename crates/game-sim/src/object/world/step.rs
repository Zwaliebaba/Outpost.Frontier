//! The tick: `StarSystem_Update`'s sub-step loop, the per-kind `update`
//! functions, the physics controller hook and the object sync.

use std::cell::RefCell;

use neuron_core::math::Vector;
use neuron_core::physics::{ControllerContext, ControllerHost, PhysicsWorld, StepMode};

use crate::data::constants::EngineEquipConsts;
use crate::equip::flight::{
    EngineShipIo, engine_controller, nudge_controller, steering_controller, strafe_controller,
    thruster_controller,
};
use crate::equip::{EquipKind, types};
use crate::object::{Arena, KindData, ObjHandle, Object, ship, update_flags};

use super::{ControllerRef, TickList, World, WorldEvent};

/// The controller hook: dispatches each registered controller's key to
/// its object's controller (`do_simulation_controller`).
struct Host<'a> {
    objects: &'a RefCell<&'a mut Arena<Object>>,
    controllers: &'a std::collections::BTreeMap<u64, (ObjHandle, ControllerRef)>,
    consts: &'a EngineEquipConsts,
    weapons_offline_ok: bool,
}

/// Runs one controller of an object; the pushes go to `pusher`.
pub(crate) fn run_controller(
    obj: &mut Object,
    which: ControllerRef,
    consts: &EngineEquipConsts,
    dt: f32,
    pusher: &mut dyn crate::equip::Pusher,
) {
    let orientation = obj.engine.orientation;
    let zone_drag = obj.simple_zone_drag();
    let Object { equipped, data, .. } = obj;
    let KindData::Ship(ship) = data else {
        return;
    };
    let flight = &mut ship.flight;
    match which {
        ControllerRef::Ship
        | ControllerRef::Guided
        | ControllerRef::Mine
        | ControllerRef::JumpIn
        | ControllerRef::Launch
        | ControllerRef::Lane => {}
        ControllerRef::Steering => {
            steering_controller(&flight.steering, &flight.arch, &orientation, dt, pusher);
        }
        ControllerRef::Nudge => {
            nudge_controller(&flight.nudge, &flight.arch, &orientation, dt, pusher);
        }
        ControllerRef::Strafe => {
            let arch = flight.arch;
            strafe_controller(
                &mut flight.strafe,
                &arch,
                &orientation,
                &mut flight.thrust_power,
                dt,
                pusher,
            );
        }
        ControllerRef::Equip(id) => {
            let Some(eq) = equipped.as_deref_mut() else {
                return;
            };
            let weapons_offline = eq.weapons_offline;
            let linear_drag = ship::linear_drag(flight, &eq.equipment, zone_drag, consts);
            let Some(slot) = eq.equipment.find_by_id(id) else {
                return;
            };
            let crate::object::Equipped {
                equipment, power, ..
            } = eq;
            let Some(equip) = equipment.get_mut(slot) else {
                return;
            };
            let functioning = equip.is_functioning(weapons_offline);
            let crate::equip::Equip { kind, events, .. } = equip;
            match kind {
                EquipKind::Engine(engine) => {
                    let io = EngineShipIo {
                        throttle: flight.throttle,
                        orientation: &orientation,
                        linear_drag,
                    };
                    engine_controller(engine, functioning, io, power, consts, dt, pusher);
                }
                EquipKind::Thruster(thruster) => {
                    thruster_controller(
                        thruster,
                        functioning,
                        &orientation,
                        &mut flight.thrust_power,
                        dt,
                        events,
                        pusher,
                    );
                }
                _ => {}
            }
        }
    }
}

impl ControllerHost for Host<'_> {
    fn do_simulation_controller(&mut self, key: u64, context: &mut ControllerContext<'_>) {
        let _ = self.weapons_offline_ok;
        let Some(&(h, which)) = self.controllers.get(&key) else {
            return;
        };
        let mut objects = self.objects.borrow_mut();
        let Some(obj) = objects.get_mut(h) else {
            return;
        };
        let dt = context.event.delta_time;
        if which == ControllerRef::Guided {
            let mass = context.core().mass;
            super::guided::guided_controller(obj, mass, dt, context);
            return;
        }
        if which == ControllerRef::Mine {
            super::mine::mine_controller(obj, dt, context);
            return;
        }
        if which == ControllerRef::JumpIn {
            super::jump::jump_in_controller(obj, dt, context);
            return;
        }
        if which == ControllerRef::Launch {
            super::launch_action::launch_controller(obj, dt, context);
            return;
        }
        if which == ControllerRef::Lane {
            super::lane_action::lane_controller(obj, dt, context);
            return;
        }
        run_controller(obj, which, self.consts, dt, context);
    }
}

impl Object {
    /// The drag modifier of the zone the object is in.
    pub(crate) fn simple_zone_drag(&self) -> Option<f32> {
        self.simple.zone_drag
    }
}

/// The server's state of an object that the damage computation reads
/// (`GameObject::is_damage_blocked`'s invincibility, the difficulty scale,
/// `is_death_fuse_burning`): none without a server.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DamageState {
    /// The largest fraction of its hit points the object may lose
    /// (`InvincibleTimer`), when it is invincible.
    pub invincible: Option<f32>,
    /// The difficulty scale of damage to it.
    pub difficulty_scale: Option<f32>,
    /// Its death fuse burns.
    pub death_fuse_burning: bool,
    /// Its root takes no damage (set from [`HitRule::RootImmune`]).
    pub root_immune: bool,
}

/// A hit the server's per-kind rule is asked about ([`ServerHooks::hit_rule`]).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Hit {
    /// `damage_sub_obj` (a collision).
    SubObj,
    /// `munition_hit`; `seeker` when the munition seeks.
    Munition {
        /// The munition has a seeker.
        seeker: bool,
    },
    /// `explosion_hit`.
    Explosion,
}

/// The server's answer for a hit (decision O46).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HitRule {
    /// The world's computation (`EqObj`'s and `GameObject`'s).
    Common,
    /// No damage (the asteroid fields' rocks and mines).
    Ignore,
    /// The object is destroyed (a counter-measure; a mine hit by a
    /// collision or a seeker).
    Destroy,
    /// The computation without root damage (an indestructible solar).
    RootImmune,
}

/// A request the AI made of the server through `IObjAffect`, which the
/// world hands to [`ServerHooks::server_requests`] after the behaviour
/// tick that made it (the reference runs it inside the tick, D622).
#[derive(Clone, Debug, PartialEq)]
pub enum ServerRequest {
    /// `EqObj::slot_34(Ids, Count, Target, -1)` from the AI: the world
    /// fired the guns inside the tick ([`World::fire`]); the server's part
    /// of the shot (the wrappers' fire counts, the missiles' wrappers and
    /// creates, the fire notice) follows.
    Fired {
        /// The shooter.
        ship: ObjHandle,
        /// The aim point (`None`: each gun fired forward).
        aim: Option<Vector>,
        /// Each gun's result, in the request's order.
        results: Vec<(crate::equip::SubObjId, crate::equip::launcher::FireResult)>,
        /// The guided munitions the shot made, with their gun.
        guided: Vec<(crate::equip::SubObjId, ObjHandle)>,
    },
    /// `slot_5c(Kind, &Target, &Second, Flag, Callback)` (`Ship::slot_5c`):
    /// a dock (0), formation (1) or trade-lane (2) request with the
    /// requesting behaviour as the callback.
    Request {
        /// The requesting ship.
        ship: ObjHandle,
        /// The request type.
        kind: i32,
        /// The target's id.
        target: u32,
        /// The second id (the lane's far ring).
        second: u32,
        /// `bFlag`.
        flag: i32,
    },
    /// `slot_60(Kind, &Target, Arg3)` (`Ship::slot_60`): the request
    /// cancelled.
    Cancel {
        /// The ship.
        ship: ObjHandle,
        /// The request type.
        kind: i32,
        /// The target's id.
        target: u32,
        /// `Arg3`.
        arg: i32,
    },
    /// `go_tradelane(Entrance, Exit, bFastExit, Speed)` (`Ship::
    /// go_tradelane`, IObjAffect slot 25) from the AI: the world made the
    /// ship's `LaneAction` (`CShip::go_tradelane`); the server's
    /// `notify_go_tradelane` follows (D730).
    GoTradeLane {
        /// The ship.
        ship: ObjHandle,
        /// The entrance ring's id.
        entrance: u32,
        /// The exit ring's id (0 for none).
        exit: u32,
        /// `bFastExit`.
        fast_exit: bool,
        /// The speed.
        speed: f32,
    },
    /// A change the AI made through the server's wrapper that the
    /// players are told of (`Ship::set_target`, `EqObj::slot_44`,
    /// `Ship::slot_50`, `slot_54`, `EqObj::slot_4c`): the world made it
    /// inside the tick; the server's `notify_*` follows (D857).
    Changed {
        /// The ship.
        ship: ObjHandle,
        /// The change.
        change: AiChange,
    },
    /// A report of the ship's behaviour manager to its content callback
    /// (`pub::AI::ContentCallback`, by the owner's handle, D710), in the
    /// order the tick made it among the requests (record 0008 point 4).
    Content {
        /// The ship.
        ship: ObjHandle,
        /// The content callback's handle.
        handle: u32,
        /// The report.
        event: crate::ai::content::CombatEvent,
    },
    /// `stop_tradelane()` (`Ship::stop_tradelane`, IObjAffect slot 26) of
    /// the ship, from the AI or from its `LaneAction`'s end: the world ran
    /// `CShip::stop_tradelane` and read the rings
    /// (`get_tradelane_entrance_ring`, `get_tradelane_exit_ring`); the
    /// server's `notify_stop_tradelane` follows (D730).
    StopTradeLane {
        /// The ship.
        ship: ObjHandle,
        /// The entrance ring's id.
        entrance: u32,
        /// What `get_tradelane_exit_ring` answers (the current leg's first
        /// ring, as the reference reads it).
        exit: u32,
    },
}

/// A change of [`ServerRequest::Changed`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AiChange {
    /// `notify_set_target(Id, Equip, Target, SubTarget, 0)`: the ship's
    /// target (`equip` `SUBOBJ_ID_NONE`) or a gun's.
    Target {
        /// The sub-object (`SUBOBJ_ID_NONE` for the ship).
        equip: crate::equip::SubObjId,
        /// The target.
        target: Option<ObjHandle>,
        /// The sub-target.
        sub_target: crate::equip::SubObjId,
    },
    /// `notify_cruise(Id, bEngage, bArg2, 0)`.
    Cruise {
        /// `bEngage`.
        engage: bool,
        /// `bArg2`.
        instant: bool,
    },
    /// `notify_thrusters(Id, bActivate, 0)`.
    Thrusters(bool),
    /// `notify_activate_equip(Id, Equip, bActivate, 0)`.
    Equip {
        /// The item.
        equip: crate::equip::SubObjId,
        /// `bActivate`.
        on: bool,
    },
}

/// The server's parts of a system frame (decision record 0007, point 2,
/// amended 2026-11-21): the wrapper's update around each object's own,
/// the server's side of applying a damage list and of destroying an
/// object, the dock queue after the deferred deletes, and whether the
/// frame steps the physics. `game-logic` implements it; the world alone
/// uses [`NoServerHooks`].
pub trait ServerHooks {
    /// `GameObject::update`'s server part before the object's own update.
    /// False skips the own update (a ship in its death sequence).
    fn object_update(&mut self, world: &mut World, object: ObjHandle, dt: f32) -> bool {
        let _ = (world, object, dt);
        true
    }

    /// The flags of the object's own update (`CObject::update(dt,
    /// flags)`), asked when [`ServerHooks::object_update`] lets it run:
    /// `Ship::update` passes `NO_SCANNER` for an NPC's ship and
    /// `UPDATE_DUMB` for a player's on a multiplayer server. 0
    /// (`UPDATE_DEFAULT`) without a server.
    fn object_update_flags(&mut self, world: &World, object: ObjHandle) -> u32 {
        let _ = (world, object);
        0
    }

    /// The server part after the object's own update (a ship's jettison
    /// and zone damage; loot, mines and counter-measures).
    fn object_after_update(&mut self, world: &mut World, object: ObjHandle, dt: f32) {
        let _ = (world, object, dt);
    }

    /// The server's per-kind rule for a hit on `object` (decision O46): the
    /// wrapper's `damage_sub_obj`, `munition_hit` or `explosion_hit`.
    fn hit_rule(&mut self, world: &World, object: ObjHandle, hit: Hit) -> HitRule {
        let _ = (world, object, hit);
        HitRule::Common
    }

    /// A root hit an indestructible solar ignored (`Solar::munition_hit`,
    /// `explosion_hit_hull`): the server's `ObjectDamaged` and behaviour
    /// event.
    fn root_hit_ignored(
        &mut self,
        world: &mut World,
        object: ObjHandle,
        list: &crate::object::damage::DamageList,
    ) {
        let _ = (world, object, list);
    }

    /// The server's state the damage computation reads for `object`, hit
    /// by a list whose inflictor belongs to `inflictor_owner` (0 for an
    /// NPC): `is_damage_blocked` reads the player invincibility for a
    /// player's damage and the general one otherwise.
    fn damage_state(
        &mut self,
        world: &World,
        object: ObjHandle,
        inflictor_owner: u32,
    ) -> DamageState {
        let _ = (world, object, inflictor_owner);
        DamageState::default()
    }

    /// `GameObject::apply_damage_list` before the entries are written:
    /// false keeps the list from the object (the damage-callback latch
    /// queues it). The server may change the list.
    fn before_damage_list(
        &mut self,
        world: &mut World,
        object: ObjHandle,
        list: &mut crate::object::damage::DamageList,
    ) -> bool {
        let _ = (world, object, list);
        true
    }

    /// Entry `index` of a list being applied to `object`, before it is
    /// written (`EqObj::apply_root_entry`, `apply_equip_entry`,
    /// `apply_power_entry`, `apply_group_entry`): the server's
    /// `ObjectDamaged`, the behaviour manager's damage notice and the
    /// fuses. The server may change the entry (a burning death fuse
    /// clears a group's fate).
    fn damage_entry(
        &mut self,
        world: &mut World,
        object: ObjHandle,
        list: &mut crate::object::damage::DamageList,
        index: usize,
    ) {
        let _ = (world, object, list, index);
    }

    /// After the entries are written, before a destroyed object is
    /// destroyed: the server's dispatches. Clearing `destroyed` keeps the
    /// object (`is_damage_blocked`).
    fn after_damage_list(
        &mut self,
        world: &mut World,
        object: ObjHandle,
        list: &mut crate::object::damage::DamageList,
    ) {
        let _ = (world, object, list);
    }

    /// The end of `apply_damage_list`, after a destroyed object's
    /// destruction (`set_in_damage_callback(false)`, which applies the
    /// damage queued meanwhile).
    fn damage_list_done(&mut self, world: &mut World, object: ObjHandle) {
        let _ = (world, object);
    }

    /// The server's `destroy(kind, killer)` before the world marks the
    /// object dead: false keeps it alive.
    fn destroy(&mut self, world: &mut World, object: ObjHandle, kind: i32, killer: u32) -> bool {
        let _ = (world, object, kind, killer);
        true
    }

    /// The requests an object's behaviour tick made of the server
    /// (`Ship::slot_5c`, `slot_60`), oldest first, right after the tick.
    /// Without a server they go nowhere, as the bare world's request
    /// command did.
    fn server_requests(&mut self, world: &mut World, requests: Vec<ServerRequest>) {
        let _ = (world, requests);
    }

    /// The answer of the content callback `handle` of `ship`'s manager to
    /// `request_flee_destination` (`None` for false), asked right before
    /// the ship's behaviour tick for the flee that may start in it (record
    /// 0008 point 4): it must not change the server's state, which the
    /// request itself changes after the tick (`ServerRequest::Content`).
    /// `None` without a server.
    fn content_answer(
        &mut self,
        world: &mut World,
        ship: ObjHandle,
        handle: u32,
    ) -> Option<crate::ai::content::FleeAnswer> {
        let _ = (world, ship, handle);
        None
    }

    /// `NotifyDirectiveCallbacks` for a directive callback of the server's
    /// (a handle of the manager's five slots other than the launch
    /// action's, D710): `OnDirectiveComplete(Word11c, Word118)` after the
    /// ship's behaviour tick that reported it (D194). Nothing without a
    /// server.
    fn directive_complete(
        &mut self,
        world: &mut World,
        ship: ObjHandle,
        handle: u32,
        result: (i32, i32),
    ) {
        let _ = (world, ship, handle, result);
    }

    /// A phantom's enter or leave event that no object of the world claims
    /// (`CObjectCB::added`, `deleted` of a server wrapper's phantom: a
    /// mine's). Nothing without a server, which makes no such phantom.
    fn phantom_event(
        &mut self,
        world: &mut World,
        system: u32,
        event: &neuron_core::physics::PhantomEvent,
    ) {
        let _ = (world, system, event);
    }

    /// The server's PvP gate (`GbAllowPvp`) on damage that `inflictor` (the
    /// object the caller found for the inflictor's id, none when it found
    /// none) deals to `target`: `StarSystem::explode`'s damage pass,
    /// `munition_collision` and `collision_damage` skip the target when it
    /// answers true. False without a server.
    fn pvp_blocked(
        &mut self,
        world: &World,
        inflictor: Option<ObjHandle>,
        target: ObjHandle,
    ) -> bool {
        let _ = (world, inflictor, target);
        false
    }

    /// An event `object`'s item `equip` tells its listeners (`INotify`),
    /// which the server's equipment wrappers hear (`ServerEquip::notify`):
    /// a counter-measure dropper's [`crate::equip::EquipEvent::IncomingMissile`]
    /// as it is raised, the events of the equipment pass after it, those
    /// the controllers raise in the physics step after the step. Nothing
    /// without a server, which has no wrappers.
    fn equip_event(
        &mut self,
        world: &mut World,
        object: ObjHandle,
        equip: crate::equip::SubObjId,
        event: crate::equip::EquipEvent,
    ) {
        let _ = (world, object, equip, event);
    }

    /// Whether `EqObj::apply_equip_entry`'s cargo rule applies to
    /// `object`'s stacks: an entry of exactly 0 hit points on a stack of
    /// more than one unit takes one unit off (`RemoveFromStack(1)`, the
    /// next unit whole) instead of setting the hit points. False without
    /// a server.
    fn cargo_stack_rule(&mut self, world: &World, object: ObjHandle) -> bool {
        let _ = (world, object);
        false
    }

    /// `StarSystem_UpdateNearShips`' fill from the system's solar BSP
    /// (`StarSystem::GetBucket`, the partition `StarSystem::load` builds):
    /// for each player of the system, in arrival order, whose ship exists,
    /// the solars the partition's sphere query of 10000 m around the ship
    /// finds, all appended to one list of at most
    /// [`RESULT_CAPACITY`](crate::data::spatial::RESULT_CAPACITY) entries
    /// (a solar near two players is in it twice). `None` when the system
    /// has no partition, which skips the whole pass, the always-updated
    /// list included. Without a server there is neither a partition nor a
    /// player: an empty list, so only the always-updated entries update.
    fn near_solars(&mut self, world: &World, system: u32) -> Option<Vec<ObjHandle>> {
        let _ = (world, system);
        Some(Vec::new())
    }

    /// After a sub-step's deferred deletes: `StarSystem_Update`'s dock
    /// queue.
    fn after_reap(&mut self, world: &mut World, system: u32) {
        let _ = (world, system);
    }

    /// Whether a sub-step with `server_physics` moves the bullets and steps
    /// the physics: `StarSystem_Update` does so only while the system has
    /// players. True without a server.
    fn steps_physics(&mut self, world: &World, system: u32) -> bool {
        let _ = (world, system);
        true
    }

    /// A guided munition's seeker lost its target, which the munition has
    /// already dropped (`Guided::update`'s `set_target(nullptr,
    /// SUBOBJ_ID_ROOT, 0)`): on the server, `Guided::set_target` tells the
    /// players who can see it (`StarSystem::notify_set_target`). Called only
    /// when the target or sub-target changed. Nothing without a server.
    fn guided_target_lost(&mut self, world: &mut World, missile: ObjHandle) {
        let _ = (world, missile);
    }

    /// A ship's `JumpInAction` arrived in `system`: the jumping object's
    /// `IObjAffect::slot_74(system)`, which on the server's `Ship` tells
    /// the bus (`JumpInComplete`) and ends the jump's invincibility.
    /// Nothing without a server.
    fn jump_in_complete(&mut self, world: &mut World, object: ObjHandle, system: u32) {
        let _ = (world, object, system);
    }

    /// A ship's `LaunchAction` completed: the launching object's
    /// `IObjAffect::slot_70(base)` with the id of the object it launched
    /// from, which on the server's `Ship` ends the launch invincibility and
    /// tells the bus (`LaunchComplete`). Nothing without a server.
    fn launch_complete(&mut self, world: &mut World, object: ObjHandle, base: u32) {
        let _ = (world, object, base);
    }
}

/// A sensor of `CEqObj::update`'s list that updates after the list's
/// walk, in the list's order.
#[derive(Clone, Copy)]
enum Sensor {
    /// `CEScanner::Update`.
    Scanner(crate::equip::SubObjId),
    /// `CECounterMeasureDropper::Update`.
    CmDropper(crate::equip::SubObjId),
}

/// No server: every sub-step steps the physics.
#[derive(Clone, Copy, Debug, Default)]
pub struct NoServerHooks;

impl ServerHooks for NoServerHooks {}

/// `ZONE::GetImmersion` (P2.1's `Zone::immersion`), stored to the caller's
/// float.
fn zone_immersion(zone: &crate::data::zone::Zone, pos: Vector) -> f32 {
    zone.immersion(pos) as f32
}

/// `PhySys_SyncObjectFromBody` for every moving body.
fn sync_bodies(objects: &mut Arena<Object>, world: &PhysicsWorld) {
    for &body in world.moving_objects() {
        let Some(key) = world.user_key(body) else {
            continue;
        };
        let h = ObjHandle::from_key(key);
        let Some((position, orientation)) = world.transform(body) else {
            continue;
        };
        if let Some(obj) = objects.get_mut(h) {
            obj.engine.position = position;
            obj.engine.orientation = orientation;
        }
    }
}

impl World {
    /// One server frame of a system (`StarSystem_Update`): the sub-step
    /// loop and, with `server_physics`, the physics, then the global time
    /// (a harness that steps one system per frame).
    pub fn step(&mut self, system: u32, dt: f32) {
        self.step_with(system, dt, &mut NoServerHooks);
        self.update_global_time(dt);
    }

    /// `Timing::UpdateGlobalTime`: once per frame, whatever number of
    /// systems the frame steps (the executables' frame loops).
    pub fn update_global_time(&mut self, dt: f32) {
        self.time += f64::from(dt);
    }

    /// [`World::step`] with the server's hooks (decision record 0007,
    /// point 2), without the global time, which a server advances once per
    /// frame ([`World::update_global_time`]). Returns the number of
    /// sub-steps (`StarSystem_Update`'s result).
    pub fn step_with(&mut self, system: u32, dt: f32, hooks: &mut dyn ServerHooks) -> u32 {
        // `Server_StepSystems`: outside single player `AnimDB::Update`
        // first, which turns the joints the last frame registered.
        if !self.config.single_player {
            self.update_anim(dt);
        }
        let mut step = dt;
        if !self.config.single_player && f64::from(step) > 0.2 {
            step = 0.2;
        }
        let target = f64::from(dt);
        let mut iterations = 0;
        if target > 0.0 {
            let mut accum = 0.0f64;
            let mut current = step;
            loop {
                accum += f64::from(current);
                iterations += 1;
                self.sub_step(system, current, hooks);
                if target - accum < f64::from(current) {
                    current = (target - accum) as f32;
                }
                if !(accum < target) {
                    break;
                }
            }
        }
        iterations
    }

    fn sub_step(&mut self, system: u32, step: f32, hooks: &mut dyn ServerHooks) {
        for list in [TickList::Ships, TickList::Guided] {
            self.tick_list(system, list, step, hooks);
        }
        self.tick_near_solars(system, step, hooks);
        for list in [
            TickList::Loot,
            TickList::Beams,
            TickList::Mines,
            TickList::CounterMeasures,
        ] {
            self.tick_list(system, list, step, hooks);
        }
        // `CmnAsteroid::Update` and `SrvAsteroid_UpdateSystem`, then the
        // asteroid list (`ListC0`).
        self.update_asteroid_fields(system, step);
        self.tick_list(system, TickList::Asteroids, step, hooks);
        self.reap();
        // The dock queue, after the deferred deletes.
        hooks.after_reap(self, system);
        if self.config.server_physics && hooks.steps_physics(self, system) {
            self.move_beams(system, step);
            self.step_physics(system, step);
            self.deliver_controller_events(system, hooks);
            self.deliver_jump_ins(system, hooks);
            // A trade lane that ended in the step (`LaneAction::stop`'s
            // `stop_tradelane` on the traveller, D731).
            let requests = self.take_server_requests();
            if !requests.is_empty() {
                hooks.server_requests(self, requests);
            }
            self.phantom_events(system, hooks);
            self.process_collisions(system, hooks);
            self.reap();
        }
    }

    /// The events an object's equipment queued (`NotifyA`, `NotifyB`), oldest
    /// first: each goes to the world's events and to the server's
    /// listeners ([`ServerHooks::equip_event`]).
    pub(crate) fn deliver_equip_events(&mut self, h: ObjHandle, hooks: &mut dyn ServerHooks) {
        let Some(o) = self.objects.get_mut(h) else {
            return;
        };
        let owner_id = o.simple.id;
        let Some(eq) = o.equipped.as_deref_mut() else {
            return;
        };
        let mut queued = Vec::new();
        for e in eq.equipment.iter_mut() {
            for event in e.events.drain(..) {
                queued.push((e.id, event));
            }
        }
        for &(equip, event) in &queued {
            self.events.push(WorldEvent::Equip {
                owner: owner_id,
                equip,
                event,
            });
        }
        for (equip, event) in queued {
            hooks.equip_event(self, h, equip, event);
        }
    }

    /// After the physics step: the events the equipment's controllers
    /// raised in it (a starved thruster's), to the listeners, ship by ship.
    fn deliver_controller_events(&mut self, system: u32, hooks: &mut dyn ServerHooks) {
        for h in self.list(system, TickList::Ships) {
            let pending = self
                .objects
                .get(h)
                .and_then(|o| o.equipped.as_deref())
                .is_some_and(|eq| eq.equipment.iter().any(|e| !e.events.is_empty()));
            if pending {
                self.deliver_equip_events(h, hooks);
            }
        }
    }

    fn tick_list(&mut self, system: u32, list: TickList, dt: f32, hooks: &mut dyn ServerHooks) {
        for h in self.list(system, list) {
            if hooks.object_update(self, h, dt) {
                let flags = hooks.object_update_flags(self, h);
                self.update_object_hooked(h, dt, flags, hooks);
            }
            hooks.object_after_update(self, h, dt);
        }
    }

    /// `StarSystem_UpdateNearShips`: the scan list the server fills from
    /// the system's solar BSP ([`ServerHooks::near_solars`], once per
    /// player, so a solar near two players is in it twice), then every
    /// entry of the system's always-updated list (`ListD4`, with its
    /// repeats) while the list holds fewer than
    /// [`RESULT_CAPACITY`](crate::data::spatial::RESULT_CAPACITY) entries.
    /// The reference's de-duplication follows: each item drops only the
    /// first later repeat of itself, so an object listed three times
    /// updates twice. Without a partition the pass does nothing.
    fn tick_near_solars(&mut self, system: u32, dt: f32, hooks: &mut dyn ServerHooks) {
        use crate::data::spatial::RESULT_CAPACITY;
        let Some(mut near) = hooks.near_solars(self, system) else {
            return;
        };
        near.truncate(RESULT_CAPACITY);
        let mut scan: Vec<Option<ObjHandle>> = near.into_iter().map(Some).collect();
        for &h in self.always_update() {
            let here = self
                .objects
                .get(h)
                .is_some_and(|o| o.simple.system == Some(system));
            if here && scan.len() < RESULT_CAPACITY {
                scan.push(Some(h));
            }
        }
        let mut chosen = Vec::new();
        if scan.len() > 1 {
            for index in 0..scan.len() {
                let Some(item) = scan[index] else {
                    continue;
                };
                if let Some(later) = (index + 1..scan.len()).find(|&l| scan[l] == Some(item)) {
                    scan[later] = None;
                }
                chosen.push(item);
            }
        } else {
            chosen.extend(scan.into_iter().flatten());
        }
        for h in chosen {
            // The `GameObject` narrowing: an object the world no longer
            // has is not updated.
            if !self.objects.contains(h) {
                continue;
            }
            if hooks.object_update(self, h, dt) {
                let flags = hooks.object_update_flags(self, h);
                self.update_object_hooked(h, dt, flags, hooks);
            }
            hooks.object_after_update(self, h, dt);
        }
    }

    /// The smallest zone of `system` with a physical effect at `pos`: its
    /// nickname, drag modifier and the immersion there (`CSimple::
    /// update_zones`' search).
    pub(crate) fn physical_zone_at(&self, system: u32, pos: Vector) -> Option<(u32, f32, f32)> {
        let mut best: Option<(&crate::data::zone::Zone, f32)> = None;
        for zone in self.data.universe.system_zones(system) {
            if !zone.has_physical_effect() {
                continue;
            }
            let radius = zone.radius();
            if best.is_some_and(|(_, r)| !(radius < r)) {
                continue;
            }
            if zone.in_zone(pos, 0.0) {
                best = Some((zone, radius));
            }
        }
        best.map(|(z, _)| (z.nickname, z.drag_modifier, zone_immersion(z, pos)))
    }

    /// `CSimple::update_zones`: the smallest zone with a physical effect at
    /// the object's position, its drag modifier and the immersion.
    pub(crate) fn update_zones(&mut self, h: ObjHandle) {
        let Some(obj) = self.objects.get(h) else {
            return;
        };
        let Some(system) = obj.simple.system else {
            if let Some(o) = self.objects.get_mut(h) {
                o.simple.zone = None;
                o.simple.zone_drag = None;
                o.simple.scanner_interference = 0.0;
            }
            return;
        };
        let pos = obj.engine.position;
        let found = self.physical_zone_at(system, pos);
        if let Some(o) = self.objects.get_mut(h) {
            match found {
                Some((zone, drag, immersion)) => {
                    o.simple.zone = Some(zone);
                    o.simple.zone_drag = Some(drag);
                    o.simple.scanner_interference = immersion;
                }
                None => {
                    o.simple.zone = None;
                    o.simple.zone_drag = None;
                    o.simple.scanner_interference = 0.0;
                }
            }
        }
    }

    /// The object's `update(dt, flags)`.
    pub fn update_object(&mut self, h: ObjHandle, dt: f32, flags: u32) {
        self.update_object_hooked(h, dt, flags, &mut NoServerHooks);
    }

    /// [`World::update_object`] with the server's hooks on its damage and
    /// destruction paths.
    pub fn update_object_hooked(
        &mut self,
        h: ObjHandle,
        dt: f32,
        flags: u32,
        hooks: &mut dyn ServerHooks,
    ) {
        let Some(obj) = self.objects.get(h) else {
            return;
        };
        if obj.dead {
            return;
        }
        match obj.kind {
            crate::object::ObjectKind::Ship => self.update_ship(h, dt, flags, hooks),
            crate::object::ObjectKind::Solar => {
                self.update_equipped(h, dt, flags, hooks);
                self.update_solar_gate(h, dt);
            }
            crate::object::ObjectKind::Loot => {
                self.update_zones(h);
                let renamed = match self.objects.get_mut(h) {
                    Some(o) => {
                        let id = o.simple.id;
                        match &mut o.data {
                            KindData::Loot(l) => l.update(dt, id),
                            _ => None,
                        }
                    }
                    None => None,
                };
                if let Some(name) = renamed
                    && let Some((system, body)) = self.body(h)
                    && let Some(world) = self.physics.world_mut(system)
                {
                    world.set_collision_group(body, &name);
                }
            }
            crate::object::ObjectKind::Guided => self.update_guided(h, dt, hooks),
            crate::object::ObjectKind::Asteroid => self.update_field_mine(h, dt, hooks),
            crate::object::ObjectKind::Beam
            | crate::object::ObjectKind::Mine
            | crate::object::ObjectKind::CounterMeasure => self.update_projectile(h, dt, hooks),
            _ => {}
        }
    }

    fn update_projectile(&mut self, h: ObjHandle, dt: f32, hooks: &mut dyn ServerHooks) {
        let Some(o) = self.objects.get_mut(h) else {
            return;
        };
        let Some(p) = o.as_projectile_mut() else {
            return;
        };
        let expired = p.update(dt);
        let alive = p.is_alive();
        if expired {
            // `expire_safe_time`: the owner can be hit now.
            match &mut o.data {
                KindData::Beam(b) => b.collision_group.clear(),
                _ => {
                    if let Some((system, body)) = o.physical.body.map(|b| (o.physical.system, b))
                        && let Some(world) = self.physics.world_mut(system)
                    {
                        world.set_collision_group(body, b"");
                    }
                }
            }
        }
        if !alive {
            // The server's projectile wrappers answer false once the
            // lifetime ran out; the step deletes them.
            self.destroy_hooked(h, 0, 0, hooks);
        }
    }

    /// `CEqObj::update`: every item of the first list updates, then the
    /// behaviour tick (unless destroyed with weapons offline, excluded or
    /// flagged off).
    fn update_equipped(&mut self, h: ObjHandle, dt: f32, flags: u32, hooks: &mut dyn ServerHooks) {
        let now = self.time;
        let Some(o) = self.objects.get_mut(h) else {
            return;
        };
        let owner_id = o.simple.id;
        let rel_health = o.simple.relative_health();
        let is_ship = o.kind == crate::object::ObjectKind::Ship;
        let Object { equipped, data, .. } = o;
        let Some(eq) = equipped.as_deref_mut() else {
            return;
        };
        let weapons_offline_destroyed = eq.weapons_offline && rel_health <= 0.0;
        let mut ship = match data {
            KindData::Ship(s) => Some(s.as_mut()),
            _ => None,
        };
        let throttle = ship.as_ref().map(|s| s.flight.throttle);
        let mut cloak = None;
        let mut regen: Vec<(crate::equip::SubObjId, f32)> = Vec::new();
        let mut guns: Vec<crate::equip::SubObjId> = Vec::new();
        // The scanners and the counter-measure droppers, in the list's
        // order: a dropper reads the first scanner's list as the items
        // before it left it.
        let mut sensors: Vec<Sensor> = Vec::new();
        let mut tractors: Vec<crate::equip::SubObjId> = Vec::new();
        let slots = eq.equipment.traverse(types::EXTERNAL_LIST, false);
        for slot in slots {
            let weapons_offline = eq.weapons_offline;
            let max_power = eq.max_power;
            let crate::object::Equipped {
                equipment, power, ..
            } = &mut *eq;
            let shield_view: Vec<(crate::equip::SubObjId, f64, f32)> = equipment
                .iter()
                .filter_map(|e| match &e.kind {
                    EquipKind::Shield(s) => Some((e.id, s.relative_health(), s.hit_pts)),
                    _ => None,
                })
                .collect();
            let Some(equip) = equipment.get_mut(slot) else {
                continue;
            };
            let functioning = equip.is_functioning(weapons_offline);
            let destroyed = equip.is_destroyed();
            let status = super::spawn::shield_status(equip);
            let id = equip.id;
            // `CAttachedEquip::Update`: a destroyed item with debris left
            // still updates.
            let updates = !destroyed || equip.attached.as_ref().is_some_and(|a| a.debris.is_some());
            let crate::equip::Equip { kind, events, .. } = equip;
            match kind {
                EquipKind::Engine(engine) => {
                    let t = if flags & update_flags::NO_DYNAMICS == 0 {
                        throttle
                    } else {
                        None
                    };
                    engine.update(functioning, dt, t, events);
                }
                EquipKind::Power(plant) => {
                    let thrust = ship
                        .as_deref_mut()
                        .map(|s| (&mut s.flight.thrust_power, s.flight.max_thrust_power));
                    plant.recharge(functioning, destroyed, dt, (power, max_power), thrust);
                }
                EquipKind::Gun(g) => {
                    g.launcher.update(dt);
                    if updates {
                        guns.push(id);
                    }
                }
                EquipKind::MineDropper(l) => l.update(dt),
                EquipKind::CmDropper(_) => sensors.push(Sensor::CmDropper(id)),
                EquipKind::Shield(s) => s.update(status, now, events),
                EquipKind::ShieldGenerator(g) => {
                    let real_dt = (now - g.last_update_time) as f32;
                    g.last_update_time = now;
                    if let Some(shield_id) = g.linked_shield
                        && let Some(&(_, rel, hit)) =
                            shield_view.iter().find(|(id, _, _)| *id == shield_id)
                        && let Some(new_hit) = g.regenerate(functioning, real_dt, rel, hit, power)
                    {
                        regen.push((shield_id, new_hit));
                    }
                }
                EquipKind::Scanner(_) => sensors.push(Sensor::Scanner(id)),
                EquipKind::Tractor(_) => tractors.push(id),
                EquipKind::Cloak(c) => {
                    if let Some(p) = c.update(destroyed, dt) {
                        cloak = Some(p);
                    }
                }
                _ => {}
            }
        }
        for (shield_id, hit) in regen {
            if let Some(shield) = eq.equipment.by_id_mut(shield_id) {
                let status = super::spawn::shield_status(shield);
                let crate::equip::Equip { kind, events, .. } = shield;
                if let EquipKind::Shield(s) = kind {
                    s.set_hit_points(hit, status, now, events);
                }
            }
        }
        if let Some(c) = cloak {
            eq.cloak_percent = c;
        }
        let behavior = eq.has_behavior
            && !weapons_offline_destroyed
            && flags & update_flags::NO_BEHAVIOR == 0
            && eq.control_exclusion & 1 == 0;
        let _ = is_ship;
        // `CEGun::Update`'s turret aim and lock clock (each gun's own
        // update; nothing else in the list reads what it changes).
        for id in guns {
            self.update_gun_turret(h, id, dt);
        }
        for sensor in sensors {
            match sensor {
                Sensor::Scanner(id) => self.update_scanner(h, id, flags),
                Sensor::CmDropper(id) => {
                    // `Notify(Event_0x19)` reaches the listeners at once,
                    // before the launcher's own update.
                    if self.cm_dropper_sees_missile(h, id, flags) {
                        let event = crate::equip::EquipEvent::IncomingMissile;
                        self.events.push(WorldEvent::Equip {
                            owner: owner_id,
                            equip: id,
                            event,
                        });
                        hooks.equip_event(self, h, id, event);
                    }
                    self.cm_dropper_launcher_update(h, id, dt);
                }
            }
        }
        for id in tractors {
            self.update_tractor(h, id, dt);
        }
        // Equipment events go to the listeners.
        self.deliver_equip_events(h, hooks);
        self.sync_shield_collisions(h);
        let results = self.directive_results(h);
        if behavior && let Some(mut hook) = self.take_behavior() {
            // The content callback's flee answer, for a flee that starts in
            // the tick (record 0008 point 4).
            let handle = self
                .behavior_managers
                .get(&h)
                .map_or(0, |m| m.state.content_handle);
            if handle != 0 {
                let answer = hooks.content_answer(self, h, handle);
                self.content_answer = Some((h, answer));
            }
            hook.update_behavior_tick(self, h, dt);
            self.content_answer = None;
            self.put_behavior(Some(hook));
        }
        // The requests the tick made, to the server (`Ship::slot_5c` and
        // `slot_60` run inside the tick in the reference).
        let requests = self.take_server_requests();
        if !requests.is_empty() {
            hooks.server_requests(self, requests);
        }
        // The directive callbacks, which the tick's
        // `NotifyDirectiveCallbacks` calls at its end in the reference: the
        // launch action's (D722) and the server's (D194).
        self.directive_callbacks(h, results, hooks);
        self.update_docking_animations(h);
    }

    /// `CShip::update`: the actions (`ActionDB::update`: the jump-in and
    /// the launch), zones, `CEqObj::update`, the wake-up and the damping
    /// (drag over mass and inertia), the target check.
    fn update_ship(&mut self, h: ObjHandle, dt: f32, flags: u32, hooks: &mut dyn ServerHooks) {
        if flags & update_flags::NO_ACTIONS == 0 {
            self.update_actions(h, hooks);
        }
        self.update_zones(h);
        self.update_equipped(h, dt, flags, hooks);
        if flags & update_flags::NO_DYNAMICS == 0 {
            let consts = self.data.constants.engine.clone();
            let Some((system, body)) = self.body(h) else {
                return;
            };
            let Some(world) = self.physics.world_mut(system) else {
                return;
            };
            world.wakeup(body);
            let mass = world.mass(body).unwrap_or(1.0);
            let inertia = world.moment_of_inertia(body).unwrap_or(Vector::ZERO);
            if let Some(o) = self.objects.get(h)
                && let (Some(eq), Some(ship)) = (o.equipped.as_deref(), o.as_ship())
            {
                let (linear, angular) = ship::damping(
                    &ship.flight,
                    &eq.equipment,
                    o.simple.zone_drag,
                    &consts,
                    mass,
                    inertia,
                );
                world.set_damping(body, linear, angular);
            }
        }
        self.update_bay(h);
        // A target that cannot be targeted any more is dropped.
        let target = self
            .objects
            .get(h)
            .and_then(|o| o.as_ship())
            .and_then(|s| s.target);
        if let Some(t) = target {
            let keep = self
                .objects
                .get(t)
                .is_some_and(|o| !o.dead && o.is_targetable());
            if !keep && let Some(s) = self.objects.get_mut(h).and_then(|o| o.as_ship_mut()) {
                s.target = None;
                s.sub_target = 0;
            }
        }
        self.apply_wakeups(h);
    }

    /// The `wakeup()` calls the ship's controllers asked for.
    pub(crate) fn apply_wakeups(&mut self, h: ObjHandle) {
        let Some(o) = self.objects.get_mut(h) else {
            return;
        };
        let mut wake = false;
        if let KindData::Ship(s) = &mut o.data {
            for flag in [
                &mut s.flight.steering.wake_requested,
                &mut s.flight.nudge.wake_requested,
                &mut s.flight.strafe.wake_requested,
            ] {
                wake |= std::mem::take(flag);
            }
        }
        if let Some(eq) = o.equipped.as_deref_mut() {
            for e in eq.equipment.iter_mut() {
                match &mut e.kind {
                    EquipKind::Engine(s) => wake |= std::mem::take(&mut s.wake_requested),
                    EquipKind::Thruster(t) => wake |= std::mem::take(&mut t.wake_requested),
                    _ => {}
                }
            }
        }
        if wake
            && let Some((system, body)) = self.body(h)
            && let Some(world) = self.physics.world_mut(system)
        {
            world.wakeup(body);
        }
    }

    /// `PhySys::Update(system, dt)` with the controller hook and the sync,
    /// then the instance trees of the moved objects.
    pub fn step_physics(&mut self, system: u32, dt: f32) {
        let mode = self.config.step_mode;
        let consts = self.data.constants.engine.clone();
        self.prepare_lanes(system);
        let objects = RefCell::new(&mut self.objects);
        let mut host = Host {
            objects: &objects,
            controllers: &self.controllers,
            consts: &consts,
            weapons_offline_ok: true,
        };
        let mut sync = |world: &PhysicsWorld| {
            sync_bodies(&mut objects.borrow_mut(), world);
        };
        self.physics.update(system, dt, mode, &mut host, &mut sync);
        let moved: Vec<ObjHandle> = self
            .physics
            .world(system)
            .map(|w| {
                w.moving_objects()
                    .iter()
                    .filter_map(|b| w.user_key(*b))
                    .map(ObjHandle::from_key)
                    .collect()
            })
            .unwrap_or_default();
        for h in moved {
            self.sync_instance(h);
        }
        self.finish_jump_ins(system);
        self.finish_lanes(system);
        let _ = StepMode::SinglePlayer;
    }
}
