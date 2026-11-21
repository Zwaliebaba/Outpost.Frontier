//! The server's star systems (work package P3.3, decision record 0007,
//! points 3, 5, 6 and 10): `SysDB::SysMap`'s `StarSystem`s with what the
//! shared world does not hold (the players' observers, the dock queue and
//! the loaded flag), their loading, the per-tick stepping of the systems
//! that have players, and moving ships between systems.
//!
//! The object lists, the physics, the asteroid fields and the solars stay
//! in `game-sim`'s per-system state; this module drives them.
//!
//! - **Loading** (`StarSystem::load`, `Server/server_classes.cpp:6770`):
//!   when a system's first player arrives (`add_player`). Single player
//!   finds the solars and asteroid fields the client side built in the
//!   shared world (record 0007, point 8); multiplayer reads the system
//!   file's `[Object]` sections and its asteroid fields. Then
//!   `SystemLoaded` goes to the bus.
//! - **Stepping** (`Server_StepSystems`, `Server/IServerImpl.cpp:4155`):
//!   each distinct system of the active players, in session order;
//!   `StarSystem_Update` (`:3863`) runs the world's sub-steps with the
//!   server's hooks: the dock queue after each sub-step's deferred
//!   deletes, and the physics only while the system has players.
//! - **Unloading**: single player unloads a system when its player
//!   leaves or lands (`player_switched_out`, `shutdown`); a multiplayer
//!   server keeps every system.
//! - **Transfer** (`remove_ship`, `add_ship`, `arrive_ship`,
//!   `:10286-10417`): the ship leaves its system's lists and body, is in
//!   transit, and enters the next; the other players hear of it
//!   ([`crate::replication`]); arriving through a jump object an NPC's
//!   ship jumps in or comes out of the airlock and the players who can
//!   see it get the switch-in. The callers (a player's gate jump, `pub::
//!   SpaceObj::JumpIn` and `Relocate`) are [`crate::transfer`]'s and
//!   [`crate::api::space_obj`]'s.
//!
//! The observers (`Observer`, their tracking, creates, destroys, fan-outs,
//! the per-frame update and the changed affiliations after it) are
//! [`crate::replication`]'s.

use std::collections::BTreeMap;

use game_sim::object::damage::DamageList;
use game_sim::object::world::{DamageState, Hit, HitRule, ServerHooks, ServerRequest};
use game_sim::object::{ObjHandle, ObjectKind, World};
use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::CreateParms;
use neuron_server::controller::ControllerId;

use crate::events::ControllerMessage;
use crate::server::{Cx, Logic};

pub use crate::replication::Observer;

/// A ship of the dock queue (`FSystemDockEntry`): the ship, what it docks
/// with (none for a forced landing), the base and whether it lands.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DockEntry {
    /// The ship (`Object`, a watcher).
    pub object: Option<ObjHandle>,
    /// What it docked with (`Target`, a watcher).
    pub target: Option<ObjHandle>,
    /// The base the landing packet carries.
    pub base_id: u32,
    /// Land (true) rather than dock without landing.
    pub land: bool,
}

/// A star system's server state (`StarSystem`, `server_sysdb.h:744`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct StarSystem {
    /// The Universe system (`SystemInfo->NicknameId`).
    pub id: u32,
    /// The players, in arrival order (the list at +0x08).
    pub observers: Vec<Observer>,
    /// The dock queue (the list at +0xe4).
    pub dock_queue: Vec<DockEntry>,
    /// +0xfc.
    pub loaded: bool,
}

impl StarSystem {
    /// `count_players`: the players other than `exclude`.
    #[must_use]
    pub fn count_players(&self, exclude: u32) -> u32 {
        self.observers
            .iter()
            .filter(|o| o.player != exclude)
            .count() as u32
    }

    /// Whether `session` is one of the system's players.
    #[must_use]
    pub fn has_player(&self, session: u32) -> bool {
        self.observers.iter().any(|o| o.player == session)
    }
}

/// `SysDB::SysMap`: one entry per Universe system.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ServerWorld {
    /// The systems, by id.
    pub systems: BTreeMap<u32, StarSystem>,
    /// `GDyingShips`: the ships whose death sequence ended this frame, in
    /// order (a ship may be in it more than once), announced after the
    /// frame ([`physics_update`]).
    pub dying_ships: Vec<ObjHandle>,
    /// `!GbAllowPvp`: damage between players' ships is not applied
    /// ([`crate::munitions::pvp_blocked`]). False (PvP allowed) until the
    /// startup sets it, as the C++'s initial 1.
    pub forbid_pvp: bool,
    initialized: bool,
}

impl ServerWorld {
    /// A system.
    #[must_use]
    pub fn system(&self, id: u32) -> Option<&StarSystem> {
        self.systems.get(&id)
    }
}

/// `SysDB_Initialize`: an entry for every Universe system, made once, on
/// first use (`IServerImpl::Startup`).
pub fn initialize(cx: &mut Cx<'_>) {
    if cx.logic.server_world.initialized {
        return;
    }
    cx.logic.server_world.initialized = true;
    let ids: Vec<u32> = cx.world.data().universe.systems.keys().copied().collect();
    for id in ids {
        cx.logic
            .server_world
            .systems
            .entry(id)
            .or_insert_with(|| StarSystem {
                id,
                ..StarSystem::default()
            });
    }
}

fn system_mut<'c>(cx: &'c mut Cx<'_>, id: u32) -> Option<&'c mut StarSystem> {
    initialize(cx);
    cx.logic.server_world.systems.get_mut(&id)
}

/// Whether a system has the player.
#[must_use]
pub fn has_player(logic: &Logic, system: u32, session: u32) -> bool {
    logic
        .server_world
        .systems
        .get(&system)
        .is_some_and(|s| s.has_player(session))
}

/// `StarSystem::load`: the physics world (`PhySys::Startup`), the solars
/// and asteroid fields, the loaded flag, then `SystemLoaded` to the bus.
/// A loaded or unknown system is left alone. The C++'s solar BSP
/// (`NewSpatial("BSP")`) is not built: nothing reads it until the
/// replication part.
pub fn load(cx: &mut Cx<'_>, system: u32) {
    match system_mut(cx, system) {
        Some(s) if !s.loaded => {}
        _ => return,
    }
    cx.world.load_system(system);
    let solars = if cx.world.config().single_player {
        // `load_solars` and `SrvAsteroid_MapSystem`: the client side's
        // solars and fields are already in the shared world; each solar
        // gets its server entry and `check_fuses`.
        let solars: Vec<ObjHandle> = cx
            .world
            .objects_in(system)
            .into_iter()
            .filter(|&h| cx.world.get(h).is_some_and(|o| o.kind == ObjectKind::Solar))
            .collect();
        cx.world.load_asteroid_fields(system);
        solars
    } else {
        // `CmnAsteroid::Load`, `SrvAsteroid_LoadSystem`, then every
        // `[Object]` of the system file, each with `check_fuses`.
        cx.world.load_asteroid_fields(system);
        cx.world
            .load_system_solars_reported(system, &mut cx.logic.reports)
    };
    for h in solars {
        let _ = cx.logic.objects.entry(cx.world, h);
        crate::fuses::check_fuses(cx, h);
    }
    crate::api::system::build_solar_tree(cx, system);
    if let Some(s) = system_mut(cx, system) {
        s.loaded = true;
    }
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::SystemLoaded { system },
    );
}

/// `StarSystem::add_player`: the system loads first; a session id whose
/// low 30 bits are 0 is then refused; otherwise the player's observer
/// joins the list, on a multiplayer server every session hears the
/// player's system (`PlayerDB::SendSystemID`), and the player gets the
/// system's objects ([`crate::replication::add_player`]).
pub fn add_player(cx: &mut Cx<'_>, system: u32, session: u32) {
    let Some(s) = system_mut(cx, system) else {
        return;
    };
    if !s.loaded {
        load(cx, system);
    }
    if session & 0x3fff_ffff == 0 {
        return;
    }
    let Some(s) = system_mut(cx, system) else {
        return;
    };
    s.observers.push(Observer::new(session));
    let index = s.observers.len() - 1;
    if !cx.world.config().single_player {
        send_system_id(cx, session, system);
    }
    crate::replication::add_player(cx, system, index);
}

/// `PlayerDB::SendSystemID`: every session in use hears the player's
/// system.
pub fn send_system_id(cx: &mut Cx<'_>, player: u32, system: u32) {
    let sessions: Vec<u32> = cx.logic.players.active_ids().collect();
    for to in sessions {
        crate::client::misc_obj_update_system(cx, to, player, system);
    }
}

/// `StarSystem::remove_player`: the player's observer leaves the list
/// (the first match only), then each of the player's ships in the system
/// that is not already destroyed is destroyed. An unknown system does
/// nothing; the C++ faults on a `SysMap` miss (D612).
pub fn remove_player(cx: &mut Cx<'_>, system: u32, session: u32) {
    let Some(s) = system_mut(cx, system) else {
        return;
    };
    if let Some(at) = s.observers.iter().position(|o| o.player == session) {
        s.observers.remove(at);
    }
    let ships: Vec<ObjHandle> = cx
        .world
        .objects_in(system)
        .into_iter()
        .filter(|&h| {
            cx.world.get(h).is_some_and(|o| {
                o.kind == ObjectKind::Ship && o.simple.owner_player == session && !o.dead
            })
        })
        .collect();
    for h in ships {
        // `GameObject::destroy(0, id)`.
        let id = cx.world.get(h).map_or(0, |o| o.simple.id);
        destroy_object(cx, h, 0, id);
    }
}

/// `StarSystem::player_switched_out`: `remove_player`, the player's pending
/// visits flushed, `SystemExit` to the bus when there is a destination,
/// and in single player the system unloaded (`SystemUnloaded`, `unload`).
pub fn player_switched_out(cx: &mut Cx<'_>, system: u32, session: u32, destination: u32) {
    remove_player(cx, system, session);
    if let Some(slot) = cx.logic.players.get_mut(session) {
        crate::player::flush_pending_visits(slot);
    }
    if destination != 0 {
        let _ = cx.dispatch(
            ControllerId::BROADCAST,
            &mut ControllerMessage::SystemExit {
                system,
                destination,
                player: session,
            },
        );
    }
    let loaded = system_mut(cx, system).is_some_and(|s| s.loaded);
    if cx.world.config().single_player && loaded {
        shutdown(cx, system);
    }
}

/// `StarSystem::shutdown`: a loaded system's flag cleared, `SystemUnloaded`
/// to the bus, then `unload`.
pub fn shutdown(cx: &mut Cx<'_>, system: u32) {
    let Some(s) = system_mut(cx, system) else {
        return;
    };
    if !s.loaded {
        return;
    }
    s.loaded = false;
    let _ = cx.dispatch(
        ControllerId::BROADCAST,
        &mut ControllerMessage::SystemUnloaded { system },
    );
    unload(cx, system);
}

/// `StarSystem::unload`: every observer goes, then the world's lists,
/// asteroid fields and physics world (`World::unload_system`). Unlike
/// `shutdown` it does not clear the loaded flag, as in the C++. In single
/// player the C++ deletes only the server's wrappers and the client keeps
/// its objects; the shared world loses them here (D613).
fn unload(cx: &mut Cx<'_>, system: u32) {
    if let Some(s) = system_mut(cx, system) {
        s.observers.clear();
    }
    cx.logic.solar_trees.remove(system);
    cx.world.unload_system(system);
}

/// `StarSystem::queue_dock`: a ship joins the dock queue, which the next
/// sub-step works through after its deferred deletes.
pub fn queue_dock(
    cx: &mut Cx<'_>,
    system: u32,
    object: Option<ObjHandle>,
    target: Option<ObjHandle>,
    base_id: u32,
    land: bool,
) {
    if let Some(s) = system_mut(cx, system) {
        s.dock_queue.push(DockEntry {
            object,
            target,
            base_id,
            land,
        });
    }
}

/// `StarSystem_Update`'s dock queue. Each entry in order, including any
/// queued while the queue is worked through (the C++ list walk sees them):
/// the target's id goes with the message only when the target is not
/// itself a base's (`DockingBaseId == 0`); a landing ship is first
/// announced to every player of the system
/// ([`crate::replication::ship_landed`]); a landing ship's message
/// (`Msg0x27`) is followed by its destruction when it has no owner, or its
/// owner's removal from the system after the walk (pending visits flushed,
/// and in single player the system shut down); a ship that docks without
/// landing only sends the message. Then the queue is emptied. An entry
/// whose ship is gone is skipped; the C++ reads through the empty watcher
/// (D612).
fn work_dock_queue(cx: &mut Cx<'_>, system: u32) {
    let mut landed: Vec<u32> = Vec::new();
    let mut at = 0;
    while let Some(entry) = system_mut(cx, system).and_then(|s| s.dock_queue.get(at).copied()) {
        at += 1;
        let Some(ship) = entry.object.filter(|&h| cx.world.get(h).is_some()) else {
            continue;
        };
        let target = entry
            .target
            .and_then(|h| cx.world.get(h))
            .filter(|t| t.equipped.as_deref().is_some_and(|e| e.docking_base == 0))
            .map_or(0, |t| t.simple.id);
        let (ship_id, owner) = cx
            .world
            .get(ship)
            .map_or((0, 0), |o| (o.simple.id, o.simple.owner_player));
        if entry.land {
            crate::replication::ship_landed(cx, system, ship, target, entry.base_id);
        }
        let _ = cx.dispatch(
            ControllerId::BROADCAST,
            &mut ControllerMessage::Msg0x27 {
                ship: ship_id,
                base: entry.base_id,
                target,
                land: entry.land,
            },
        );
        if entry.land {
            if owner == 0 {
                destroy_object(cx, ship, 0, 0);
            } else {
                landed.push(owner);
            }
        }
    }
    for player in landed {
        remove_player(cx, system, player);
        if let Some(slot) = cx.logic.players.get_mut(player) {
            crate::player::flush_pending_visits(slot);
        }
        if cx.world.config().single_player {
            shutdown(cx, system);
        }
    }
    if let Some(s) = system_mut(cx, system) {
        s.dock_queue.clear();
    }
}

/// The server's hooks in a system's frame and on the world's damage and
/// destruction paths (record 0007 point 2).
pub struct Hooks<'l> {
    /// The server's state.
    pub logic: &'l mut Logic,
}

impl ServerHooks for Hooks<'_> {
    fn damage_state(
        &mut self,
        _world: &World,
        object: ObjHandle,
        inflictor_owner: u32,
    ) -> DamageState {
        crate::objects::damage_state(&self.logic.objects, object, inflictor_owner)
    }

    fn hit_rule(&mut self, world: &World, object: ObjHandle, hit: Hit) -> HitRule {
        crate::objects::hit_rule(world, object, hit)
    }

    fn root_hit_ignored(&mut self, world: &mut World, object: ObjHandle, list: &DamageList) {
        let mut cx = Cx {
            world,
            logic: self.logic,
        };
        crate::objects::root_hit_ignored(&mut cx, object, list);
    }

    fn before_damage_list(
        &mut self,
        world: &mut World,
        object: ObjHandle,
        list: &mut DamageList,
    ) -> bool {
        let mut cx = Cx {
            world,
            logic: self.logic,
        };
        crate::objects::begin_damage_list(&mut cx, object, list);
        crate::ship::stamp_damage_times(&mut cx, object, list);
        crate::fuses::sweep_damage_list(&mut cx, object, list);
        true
    }

    fn object_update(&mut self, world: &mut World, object: ObjHandle, dt: f32) -> bool {
        // `EqObj::update` (the cargo decay comes with its part), then `Ship::update`'s choice; a destroyed
        // object skips its own update.
        let mut cx = Cx {
            world,
            logic: self.logic,
        };
        crate::ship::object_update(&mut cx, object, dt)
    }

    fn object_update_flags(&mut self, world: &World, object: ObjHandle) -> u32 {
        crate::ship::update_flags(world, object)
    }

    fn after_damage_list(&mut self, world: &mut World, object: ObjHandle, list: &mut DamageList) {
        let mut cx = Cx {
            world,
            logic: self.logic,
        };
        if cx
            .world
            .get(object)
            .is_some_and(|o| o.kind == ObjectKind::Loot)
        {
            crate::loot::apply_contents(&mut cx, object, list);
        }
        // `StarSystem_DamageObject`: the players hear of the damage.
        crate::replication::damage_object(&mut cx, object, list);
        crate::objects::end_damage_list(&mut cx, object, list);
    }

    fn object_after_update(&mut self, world: &mut World, object: ObjHandle, dt: f32) {
        let kind = world.get(object).map(|o| o.kind);
        let mut cx = Cx {
            world,
            logic: self.logic,
        };
        match kind {
            Some(ObjectKind::Loot) => crate::loot::after_update(&mut cx, object, dt),
            Some(ObjectKind::Ship) => crate::ship::after_update(&mut cx, object, dt),
            Some(ObjectKind::Mine) => crate::munitions::mine_after_update(&mut cx, object, dt),
            _ => {}
        }
    }

    fn damage_list_done(&mut self, world: &mut World, object: ObjHandle) {
        let pending = {
            let mut cx = Cx {
                world: &mut *world,
                logic: &mut *self.logic,
            };
            crate::objects::take_pending_damage(&mut cx, object)
        };
        if let Some(pending) = pending {
            world.apply_damage_list_hooked(object, &pending, self);
        }
    }

    fn destroy(&mut self, world: &mut World, object: ObjHandle, kind: i32, killer: u32) -> bool {
        let mut cx = Cx {
            world,
            logic: self.logic,
        };
        crate::objects::destroy(&mut cx, object, kind, killer)
    }

    fn server_requests(&mut self, world: &mut World, requests: Vec<ServerRequest>) {
        let mut cx = Cx {
            world,
            logic: self.logic,
        };
        crate::dock::server_requests(&mut cx, requests);
    }

    fn after_reap(&mut self, world: &mut World, system: u32) {
        self.logic.objects.sweep(world);
        let queued = self
            .logic
            .server_world
            .systems
            .get(&system)
            .is_some_and(|s| !s.dock_queue.is_empty());
        if queued {
            let mut cx = Cx {
                world,
                logic: self.logic,
            };
            work_dock_queue(&mut cx, system);
        }
    }

    fn phantom_event(
        &mut self,
        world: &mut World,
        system: u32,
        event: &neuron_core::physics::PhantomEvent,
    ) {
        let mut cx = Cx {
            world,
            logic: self.logic,
        };
        crate::munitions::phantom_event(&mut cx, system, event);
    }

    fn pvp_blocked(&mut self, world: &World, inflictor: u32, target: ObjHandle) -> bool {
        crate::munitions::pvp_blocked(self.logic.server_world.forbid_pvp, world, inflictor, target)
    }

    fn equip_event(
        &mut self,
        world: &mut World,
        object: ObjHandle,
        equip: game_sim::equip::SubObjId,
        event: game_sim::equip::EquipEvent,
    ) {
        let mut cx = Cx {
            world,
            logic: self.logic,
        };
        crate::equip_fire::equip_event(&mut cx, object, equip, event);
    }

    fn cargo_stack_rule(&mut self, world: &World, object: ObjHandle) -> bool {
        // `EqObj::apply_equip_entry`: the server's equipped objects.
        world.get(object).is_some_and(|o| {
            matches!(o.kind, ObjectKind::Ship | ObjectKind::Solar) && o.equipped.is_some()
        })
    }

    fn jump_in_complete(&mut self, world: &mut World, object: ObjHandle, system: u32) {
        let mut cx = Cx {
            world,
            logic: self.logic,
        };
        crate::transfer::slot_74(&mut cx, object, system);
    }

    fn launch_complete(&mut self, world: &mut World, object: ObjHandle, base: u32) {
        let mut cx = Cx {
            world,
            logic: self.logic,
        };
        let _ = crate::ship::launch_complete(&mut cx, object, base);
    }

    fn steps_physics(&mut self, world: &World, system: u32) -> bool {
        !world.config().single_player
            && self
                .logic
                .server_world
                .systems
                .get(&system)
                .is_some_and(|s| !s.observers.is_empty())
    }
}

/// `GameObject::destroy(kind, killer)` through the server's hooks.
pub fn destroy_object(cx: &mut Cx<'_>, object: ObjHandle, kind: i32, killer: u32) {
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.destroy_hooked(object, kind, killer, &mut hooks);
}

/// `StarSystem_Update`: the world's frame of the system with the server's
/// hooks, then each observer's update with the frame's whole time
/// ([`crate::replication::update_observers`]), then the changed
/// affiliations ([`crate::replication::send_changed_affiliations`]).
/// Returns the number of sub-steps.
pub fn step_system(cx: &mut Cx<'_>, system: u32, dt: f32) -> u32 {
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    let iterations = cx.world.step_with(system, dt, &mut hooks);
    crate::replication::update_observers(cx, system, f64::from(dt));
    crate::replication::send_changed_affiliations(cx, system);
    iterations
}

/// `Server_StepSystems`: each distinct system of the active players, in
/// session order, once; a system id with no entry is skipped. A
/// multiplayer server's `AnimDB::Update` runs inside the world's step. The
/// frame loop advances the global time (`Timing::UpdateGlobalTime`,
/// `World::update_global_time`), not this.
pub fn step_systems(cx: &mut Cx<'_>, dt: f32) {
    initialize(cx);
    let mut stepped: Vec<u32> = Vec::new();
    let mut after = None;
    while let Some(session) = cx.logic.players.next_active(after) {
        after = Some(session);
        let system = cx.logic.players.get(session).map_or(0, |p| p.system_id);
        if stepped.contains(&system) {
            continue;
        }
        if cx.logic.server_world.systems.contains_key(&system) {
            step_system(cx, system, dt);
        }
        stepped.push(system);
    }
}

/// `Server_PhysicsUpdate`: [`step_systems`], then each ship of the
/// dying-ship list that still exists is announced (`Msg0x25` with its
/// system, id and death-sequence object, which the update zeroed when it
/// queued the ship), and the list is emptied. The server phantoms' events
/// go to the controllers between them
/// ([`crate::api::phantom::pump_events`]).
pub fn physics_update(cx: &mut Cx<'_>, dt: f32) {
    step_systems(cx, dt);
    crate::api::phantom::pump_events(cx);
    let dying = std::mem::take(&mut cx.logic.server_world.dying_ships);
    for h in dying {
        let Some(o) = cx.world.get(h) else {
            continue;
        };
        let (system, ship) = (o.physical.system, o.simple.id);
        let death_sequence = cx
            .logic
            .objects
            .get(h)
            .and_then(|e| e.ship.as_ref())
            .map_or(0, |s| s.death_sequence_obj_id);
        let _ = cx.dispatch(
            ControllerId::BROADCAST,
            &mut ControllerMessage::Msg0x25 {
                ship,
                death_sequence,
                system,
            },
        );
    }
}

/// `StarSystem::remove_ship`: every other player of the system hears of
/// the ship as destroyed (kind 0, [`crate::replication::ship_removed`]),
/// then the ship leaves its system's lists, a player's ship on a
/// multiplayer server stops following its client (`PhySys::RCEnd`), its
/// body is destroyed and its parameters returned
/// (`GameObject::leave_system`). `notify_object_left`'s behaviour events
/// are the behaviour manager's server link; the lookup cache is not
/// ported (record 0007 point 4).
pub fn remove_ship(cx: &mut Cx<'_>, ship: ObjHandle) -> Option<CreateParms> {
    crate::replication::ship_removed(cx, ship);
    crate::replication::end_remote_control(cx, ship);
    cx.world.leave_system(ship)
}

/// `StarSystem::add_ship`: the ship at `position` and `orientation` in
/// `system` with a body rebuilt from its saved parameters
/// (`Ship::enter_system`, which also ends a death sequence; a player's
/// ship on a multiplayer server follows its client, `PhySys::RCBegin`),
/// appended to
/// the ship list; then every player of the system gets the ship's create
/// ([`crate::replication::ship_arrived`]). False when the ship is not in
/// transit or the system is not loaded.
pub fn add_ship(
    cx: &mut Cx<'_>,
    system: u32,
    ship: ObjHandle,
    position: Vector,
    orientation: Matrix,
) -> bool {
    if !cx.world.enter_system(ship, system, position, orientation) {
        return false;
    }
    crate::replication::begin_remote_control(cx, ship);
    if let Some(s) = cx
        .logic
        .objects
        .entry_mut(ship)
        .and_then(|o| o.ship.as_mut())
    {
        s.death_sequence_obj_id = 0;
    }
    crate::replication::ship_arrived(cx, system, ship);
    true
}

/// The type bits of a jump gate and a jump hole.
const GATE_BITS: u32 = 0x40 | 0x800;
/// The type bits of an airlock.
const AIRLOCK_BITS: u32 = 0x400;

/// `StarSystem::arrive_ship(Obj, JumpObj, Pos, Orient)`: an NPC's ship
/// (no owner) arriving through a jump gate or hole jumps in over 0.5 s
/// (`CShip::jump_in`, [`World::jump_in`]), through an airlock comes out of
/// it (`airlock_in`); then every player of the system who can see the ship
/// gets the switch-in with the jump object's type bits (0 without one,
/// [`crate::replication::ship_switched_in`]). A player's ship only
/// switches in: its client plays the arrival.
pub fn arrive_ship(
    cx: &mut Cx<'_>,
    system: u32,
    ship: ObjHandle,
    jump_object: Option<ObjHandle>,
    position: Vector,
    orientation: Matrix,
) {
    let bits = jump_object
        .and_then(|h| cx.world.get(h))
        .map_or(0, |o| o.simple.type_bits);
    let owner = cx.world.get(ship).map_or(0, |o| o.simple.owner_player);
    if owner == 0 {
        if bits & GATE_BITS != 0 {
            cx.world.jump_in(ship, position, orientation, 0.5);
        } else if bits & AIRLOCK_BITS != 0 {
            cx.world.airlock_in(ship, position, orientation);
        }
    }
    crate::replication::ship_switched_in(cx, system, ship, bits, position, &orientation);
}
