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
//! - **Transfer** (`remove_ship`, `add_ship`, `:10334-10417`): the ship
//!   leaves its system's lists and body, is in transit, and enters the
//!   next. `arrive_ship` (an NPC's jump-in or airlock animation and the
//!   players' switch-in packets) comes with the replication part.
//!
//! The observers' own work (tracking, creates, the per-frame update, the
//! changed affiliations, the creates `add_player` sends) is P3.3's
//! replication part; the wrappers' server state (`GameObject::destroy`,
//! fuses, `on_object_left_system`) is its object part. Their call sites
//! here are marked.

use std::collections::BTreeMap;

use game_sim::object::damage::DamageList;
use game_sim::object::world::{DamageState, ServerHooks};
use game_sim::object::{ObjHandle, ObjectKind, World};
use neuron_core::math::{Matrix, Vector};
use neuron_core::physics::CreateParms;
use neuron_server::controller::ControllerId;

use crate::events::ControllerMessage;
use crate::server::{Cx, Logic};

/// One player's view of a system (`Observer`, `server_sysdb.h:240`). The
/// replication part fills in the tracked objects, the pending damage and
/// the clock (record 0007, point 7).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Observer {
    /// The session (`PlayerId`).
    pub player: u32,
}

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
#[derive(Clone, Debug, Default, PartialEq, Eq)]
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
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ServerWorld {
    /// The systems, by id.
    pub systems: BTreeMap<u32, StarSystem>,
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
    if cx.world.config().single_player {
        // `load_solars` and `SrvAsteroid_MapSystem`: the client side's
        // solars and fields are already in the shared world; the server's
        // side tables come with the object part.
        cx.world.load_asteroid_fields(system);
    } else {
        // `CmnAsteroid::Load`, `SrvAsteroid_LoadSystem`, then every
        // `[Object]` of the system file.
        cx.world.load_asteroid_fields(system);
        cx.world
            .load_system_solars_reported(system, &mut cx.logic.reports);
    }
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
/// joins the list, and on a multiplayer server every session hears the
/// player's system (`PlayerDB::SendSystemID`). The creates the C++ then
/// sends the player (solars, ships, loot) are the replication part's.
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
    s.observers.push(Observer { player: session });
    if !cx.world.config().single_player {
        send_system_id(cx, session, system);
    }
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
/// itself a base's (`DockingBaseId == 0`); a landing ship's message
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
        // The observers' `OnObjectLanded` comes with the replication part.
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
        true
    }

    fn after_damage_list(&mut self, world: &mut World, object: ObjHandle, list: &mut DamageList) {
        // `StarSystem_DamageObject` (the observers) comes with replication.
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
        crate::objects::end_damage_list(&mut cx, object, list);
    }

    fn object_after_update(&mut self, world: &mut World, object: ObjHandle, dt: f32) {
        if world
            .get(object)
            .is_some_and(|o| o.kind == ObjectKind::Loot)
        {
            let mut cx = Cx {
                world,
                logic: self.logic,
            };
            crate::loot::after_update(&mut cx, object, dt);
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
/// hooks. Returns the number of sub-steps. The observers' update and the
/// changed affiliations that follow come with the replication part.
pub fn step_system(cx: &mut Cx<'_>, system: u32, dt: f32) -> u32 {
    let mut hooks = Hooks {
        logic: &mut *cx.logic,
    };
    cx.world.step_with(system, dt, &mut hooks)
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

/// `StarSystem::remove_ship`: the ship out of its system's lists, its body
/// destroyed and its parameters returned (`GameObject::leave_system`).
/// The other players' `OnObjectDestroyed`, `notify_object_left` and the
/// lookup cache are the replication and object parts'.
pub fn remove_ship(cx: &mut Cx<'_>, ship: ObjHandle) -> Option<CreateParms> {
    cx.world.leave_system(ship)
}

/// `StarSystem::add_ship`: the ship at `position` and `orientation` in
/// `system` with a body rebuilt from its saved parameters
/// (`GameObject::enter_system`), appended to the ship list. The creates
/// for the system's players are the replication part's. False when the
/// ship is not in transit or the system is not loaded.
pub fn add_ship(
    cx: &mut Cx<'_>,
    system: u32,
    ship: ObjHandle,
    position: Vector,
    orientation: Matrix,
) -> bool {
    cx.world.enter_system(ship, system, position, orientation)
}
